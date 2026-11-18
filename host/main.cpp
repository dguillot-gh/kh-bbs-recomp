#include "psprecomp/elf32.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"
#include "partition_memory.hpp"
#include "thread_manager.hpp"
#include "disc_io.hpp"
#include "savedata.hpp"
#include "overlays.hpp"
#include "movie_skip.hpp"
#include "display_state.hpp"
#include "ge_display.hpp"
#include "interrupts.hpp"
#include "ge_gpu_backend.hpp"
#include "vcs_config.hpp"
#include "audio_output.hpp"
#include "vcs_fps_overlay.hpp"
#include "sas_hle.hpp"
#include "atrac_hle.hpp"
#include "guest_calls.hpp"
#include "state_io.hpp"
#include "perf_stats.hpp"
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

#include <cstdint>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <span>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <vector>

namespace psprecomp {
void register_generated_functions(Runtime &runtime);
#if defined(PSPRECOMP_HAS_KJFS_MODULE)
void register_kjfs_generated_functions(Runtime &runtime);
#endif
#if defined(PSPRECOMP_HAS_ZLIBDEC_MODULE)
void register_zlibdec_generated_functions(Runtime &runtime);
#endif
}

// Host pad state (PSP button bits, sceCtrl layout). Set by a front end (Android touch, 2026-11-16);
// stays 0 in the headless Windows bring-up, so sceCtrl reports no buttons as before.
std::atomic<std::uint32_t> g_bbs_pad_buttons{0u};
// Analog sticks packed lx | ly<<8 | rx<<16 | ry<<24 (128 = centre). Right stick goes in sample bytes 10/11,
// the slot PPSSPP uses for right-stick mods. Set by the Android front end (nativeSetAnalog).
std::atomic<std::uint32_t> g_bbs_pad_analog{0x80808080u};

// Save states + pause (2026-11-18). The front end (bbs_jni.cpp) sets a request; the game thread carries it out
// at the next vblank-wait safe point (ThreadManager safe_point_hook): every guest thread is parked in an HLE
// there, so guest RAM + the host modules' state is the whole machine. Result: 1 ok, -1 failed, 0 pending.
std::atomic<int> g_bbs_state_request{0};   // 0 none, 1 save, 2 load
std::atomic<int> g_bbs_state_result{0};
std::mutex g_bbs_state_mutex;
std::string g_bbs_state_path;              // guarded by g_bbs_state_mutex
std::atomic<bool> g_bbs_paused{false};
std::atomic<std::uint64_t> g_bbs_flip_count{0u};  // sceDisplaySetFrameBuf calls (FPS counter in the app)
// 60 FPS mode (2026-11-18, experimental). BBS's vblank sub-interrupt handler 0x0880919C reads a frame-mode word
// at 0x09F22E08 (getter 0x088072A4): 1 in gameplay = wake FlipThread only every second vblank (30 FPS), 0 = every
// vblank. Like the Final Mix CWCheat (_L 0x21725EC8 0 writes the same kind of word, at the FM address), keep it at
// 0 while the option is on. Only 1 is rewritten; other modes are left alone.
std::atomic<bool> g_bbs_60fps{false};
constexpr std::uint32_t kBbsFrameModeAddress = 0x09F22E08u;
// Set where the audio channels live (a nested scope in main), so the state code can reach them.
std::function<void(psphost::StateIO &)> g_bbs_audio_state;
// [bbs-camera-mod] Right-stick camera, the idea of RemasteredControls KHBBS by TheFloW (GPL-3; re-implemented
// here, no code copied, credited in psp-native-runtime/CREDITS.md). Hooks in generated-v4 units 0039/0048 call
// these: while the right stick is off centre the game takes its "L held, control type B" camera path and its
// two stick getters (0x088162F0 X, 0x08816320 Y) read the right stick. Centred stick = vanilla game.
// PSPRECOMP_BBS_NO_CAMERA_MOD=1 turns it off. 2026-11-18.
extern "C" bool bbs_camera_mod_active() noexcept {
    static const bool disabled = std::getenv("PSPRECOMP_BBS_NO_CAMERA_MOD") != nullptr;
    if (disabled) return false;
    const std::uint32_t a = g_bbs_pad_analog.load(std::memory_order_relaxed);
    return (a >> 16u) != 0x8080u;
}
extern "C" float bbs_right_stick_axis(int axis) noexcept {
    const std::uint32_t a = g_bbs_pad_analog.load(std::memory_order_relaxed);
    const int v = static_cast<int>((a >> (axis == 0 ? 16u : 24u)) & 0xFFu);
    return static_cast<float>(v - 128) / 128.0f;
}

namespace {
struct KjfsPendingReturn {
    std::uint32_t return_pc{};
    std::uint32_t caller_gp{};
    std::uint32_t buffer{};
    std::uint32_t length{};
    bool trace_path{};
    bool trace_call{};
    std::uint32_t a0{}, a1{}, a2{}, a3{};
};
std::unordered_map<std::int32_t, std::vector<KjfsPendingReturn>> g_kjfs_pending_returns;
bool g_trace_kjfs_calls{};
std::uint32_t g_kjfs_trace_count{};

void kjfs_return_trampoline_function(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    auto &pending = g_kjfs_pending_returns[psprecomp::runtime_thread_uid()];
    if (pending.empty()) {
        runtime.stop("KJFS return trampoline has no pending call");
        return;
    }
    const KjfsPendingReturn call = pending.back();
    pending.pop_back();
    if (call.trace_call && g_trace_kjfs_calls && g_kjfs_trace_count < 8u) {
        ++g_kjfs_trace_count;
        std::cerr << "[kjfs-return] result=" << psprecomp::hex32(ctx.gpr[2u])
                  << " caller=" << psprecomp::hex32(call.return_pc - 8u)
                  << " args=[" << psprecomp::hex32(call.a0) << ","
                  << psprecomp::hex32(call.a1) << "," << psprecomp::hex32(call.a2)
                  << "," << psprecomp::hex32(call.a3) << "]";
        if (call.trace_path) {
            std::cerr << " path=\"" << runtime.memory().read_c_string(call.buffer, 256u) << "\" data=";
            for (std::uint32_t offset = 0; offset < std::min(16u, call.length); offset += 4u)
                std::cerr << (offset == 0u ? "" : ",")
                          << psprecomp::hex32(runtime.memory().load32(call.buffer + offset));
        }
        std::cerr << "\n";
    }
    ctx.set_gpr(28u, call.caller_gp);
    ctx.set_gpr(31u, call.return_pc);
    ctx.pc = call.return_pc;
}
}

namespace {
// setenv/unsetenv for MinGW (Windows) and POSIX.
void setenv_compat(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
void unsetenv_compat(const char *name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}
} // namespace

int main(int argc, char **argv) {
    // BBS's screen-copy passes (draws that sample a render target: projected shadows from the 128x128 buffer at
    // 0x040CC000, the full-screen glow that copies the frame at half size and adds it back) are drawn by default
    // since 2026-11-18 (fixed: GE alpha = stencil in the Vulkan backend, texture matrix 0x40/0x41 in ge_display,
    // UV clamp inside the sampled target rectangle). PSPRECOMP_GE_SKIP_FEEDBACK_DRAWS=1 still skips them.
    if (const char *v = std::getenv("PSPRECOMP_GE_SKIP_FEEDBACK_DRAWS"); v == nullptr || v[0] != '1')
        unsetenv_compat("PSPRECOMP_GE_SKIP_FEEDBACK_DRAWS");
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: BBSBringup <decrypted-elf> <disc-root> [max-dispatches]\n";
        return 2;
    }

    try {
        const std::filesystem::path elf_path = argv[1];
        const std::filesystem::path disc_root = argv[2];
        const std::uint64_t max_dispatches = argc == 4
            ? std::stoull(argv[3], nullptr, 0)
            : 10'000'000u;

        const psprecomp::Elf32Image elf = psprecomp::Elf32Image::from_file(elf_path);
        psprecomp::Runtime runtime;
        runtime.set_game_root(disc_root);
        std::uint32_t compiled_sdk_version = 0u;
        std::uint32_t compiler_version = 0u;
        std::uint32_t interrupt_state = 0u;
        std::int32_t next_callback_uid = 1;
        std::unordered_map<std::int32_t, std::pair<std::uint32_t, std::uint32_t>> callbacks;
        std::uint32_t exit_callback_uid = 0u;
        std::unordered_set<std::int32_t> umd_callbacks;
        bool umd_activated = false;
        std::uint32_t impose_language = 1u;
        std::uint32_t impose_button = 0u;
        std::uint32_t controller_sampling_mode = 0u;
        std::uint32_t controller_sampling_cycle = 0u;
        std::uint32_t general_purpose_io = 0u;
        runtime.register_hle("SysMemUserForUser", 0x91DE343Cu,
            [&compiled_sdk_version](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                // SDK 5.00-5.50 variant of sceKernelSetCompiledSdkVersion.
                compiled_sdk_version = ctx.gpr[4u];
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("SysMemUserForUser", 0x35669D4Cu,
            [&compiled_sdk_version](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                // sceKernelSetCompiledSdkVersion600_602 (SDK 6.00-6.02 variant).
                compiled_sdk_version = ctx.gpr[4u];
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("SysMemUserForUser", 0xF77D77CBu,
            [&compiler_version](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                compiler_version = ctx.gpr[4u];
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("Kernel_Library", 0xA089ECA4u,   // sceKernelMemset(dest, byte, size) -> dest
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                const std::uint32_t dest = ctx.gpr[4u], size = ctx.gpr[6u];
                const auto value = static_cast<std::uint8_t>(ctx.gpr[5u]);
                if (size != 0u && rt.memory().contains(dest, size))
                    for (std::uint32_t i = 0; i < size; ++i) rt.memory().store8(dest + i, value);
                ctx.set_gpr(2u, dest);
            });
        runtime.register_hle("Kernel_Library", 0x092968F4u,
            [&interrupt_state](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                // sceKernelCpuSuspendIntr returns the prior interrupt state.
                ctx.set_gpr(2u, interrupt_state);
                interrupt_state = 1u;
            });
        runtime.register_hle("Kernel_Library", 0x5F10D406u,
            [&interrupt_state](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                // sceKernelCpuResumeIntr restores the state returned by suspend.
                interrupt_state = ctx.gpr[4u] != 0u ? 1u : 0u;
                ctx.set_gpr(2u, 0u);
            });
        for (const std::uint32_t nid : {0x79D1C3FAu, 0xB435DEC5u, 0x3EE30821u,
                                        0x34B9FA9Eu, 0xBFA98062u}) {
            runtime.register_hle("UtilsForUser", nid,
                [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                    // The interpreter and generated code share coherent host memory.
                    ctx.set_gpr(2u, 0u);
                });
        }
        // VCS vcs_profile.cpp:6380-6388: debug LEDs/switches, harmless.
        runtime.register_hle("UtilsForUser", 0x37FB5C42u,
            [&general_purpose_io](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                // sceKernelGetGPI: return the stored value in v0.
                ctx.set_gpr(2u, general_purpose_io);
            });
        runtime.register_hle("UtilsForUser", 0x6AD345D7u,
            [&general_purpose_io](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                // sceKernelSetGPO: store the value in a0.
                general_purpose_io = ctx.gpr[4u];
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("ThreadManForUser", 0xE81CAF8Fu,
            [&next_callback_uid, &callbacks](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                const std::int32_t uid = next_callback_uid++;
                callbacks.emplace(uid, std::pair{ctx.gpr[5u], ctx.gpr[6u]});
                ctx.set_gpr(2u, static_cast<std::uint32_t>(uid));
            });
        runtime.register_hle("ThreadManForUser", 0xEDBA5844u,
            [&callbacks](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
                ctx.set_gpr(2u, callbacks.erase(uid) == 1u ? 0u : 0x800201A1u);
            });
        runtime.register_hle("LoadExecForUser", 0x4AC57943u,
            [&exit_callback_uid](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                exit_callback_uid = ctx.gpr[4u];
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("sceUmdUser", 0xAEE7404Du,
            [&umd_callbacks](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                umd_callbacks.insert(static_cast<std::int32_t>(ctx.gpr[4u]));
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("sceUmdUser", 0xBD2BDE07u,
            [&umd_callbacks](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2u, umd_callbacks.erase(static_cast<std::int32_t>(ctx.gpr[4u])) ? 0u : 0x800201A1u);
            });
        runtime.register_hle("sceUmdUser", 0xC6183D47u,
            [&umd_activated](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                umd_activated = std::filesystem::exists(rt.game_root());
                ctx.set_gpr(2u, umd_activated ? 0u : 0x80010013u);
            });
        runtime.register_hle("sceUmdUser", 0x6B4A146Cu,
            [&umd_activated](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2u, umd_activated ? 0x32u : 0x02u);
            });
        runtime.register_hle("sceUmdUser", 0x46EBB729u,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 1u); });
        runtime.register_hle("sceUmdUser", 0x8EF08FCEu,
            [&umd_activated](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                const std::uint32_t requested = ctx.gpr[4u];
                const std::uint32_t status = umd_activated ? 0x32u : 0x02u;
                ctx.set_gpr(2u, (status & requested) == requested ? 0u : 0x800201A8u);
            });
        // sceUmdWaitDriveStatCB(wantedStat, timeout) and its no-callback/timer/
        // cancel siblings. wantedStat is a UMD status bit-mask (e.g. 0x02 PRESENT,
        // 0x20 READABLE); timeout is microseconds. The host runtime is
        // single-threaded, so a drive already in the wanted state succeeds and a
        // missing one reports the wait as timed out, like the sibling above.
        auto umd_wait_drive_stat = [&umd_activated](std::uint32_t wanted) -> std::uint32_t {
            static constexpr std::uint32_t valid = 0x01u | 0x02u | 0x08u | 0x10u | 0x20u;
            if ((wanted & valid) == 0u) {
                return 0x80010013u; // ERROR_ERRNO_INVALID_ARGUMENT
            }
            const std::uint32_t status = umd_activated ? 0x32u : 0x02u;
            return (status & wanted) != 0u ? 0u : 0x800201A8u; // ERROR_KERNEL_WAIT_TIMEOUT
        };
        runtime.register_hle("sceUmdUser", 0x4A9E5E29u,
            [&](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2u, umd_wait_drive_stat(ctx.gpr[4u]));
            });
        runtime.register_hle("sceUmdUser", 0x56202973u,
            [&](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2u, umd_wait_drive_stat(ctx.gpr[4u]));
            });
        runtime.register_hle("sceUmdUser", 0x6AF9B50Au,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); });
        // GE: the real display-list path (S7), shared with the other game:
        // psp-native-runtime/host-shared/ge_display.cpp. Lists run inside
        // sceGeListEnQueue through the VCS software rasterizer into VRAM, and
        // the game's GE signal/finish callbacks run as guest code (0x4
        // trampoline). Replaces the 10/28 BRINGUP SHIM. PSPRECOMP_GE_DIAG=1 logs it.
        psphost::install_ge_display(runtime, psphost::GeDisplayOptions{
            [&compiled_sdk_version] { return compiled_sdk_version; }});
        // BRINGUP SHIM: sceNetGetLocalEtherAddr(mac[6]): a fixed, locally administered MAC.
        runtime.register_hle("sceNet", 0x0BF0A3AEu,
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                static constexpr std::uint8_t mac[6] = {0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u};
                const std::uint32_t out = ctx.gpr[4u];
                if (!rt.memory().contains(out, 6u)) { ctx.set_gpr(2u, 0x800200D3u); return; }
                for (std::uint32_t i = 0u; i < 6u; ++i) rt.memory().store8(out + i, mac[i]);
                ctx.set_gpr(2u, 0u);
            });
        // sceNetEtherNtostr(mac[6], str): "xx:xx:xx:xx:xx:xx" + NUL.
        runtime.register_hle("sceNet", 0x89360950u,
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                const std::uint32_t mac = ctx.gpr[4u];
                const std::uint32_t out = ctx.gpr[5u];
                if (!rt.memory().contains(mac, 6u) || !rt.memory().contains(out, 18u)) { ctx.set_gpr(2u, 0x800200D3u); return; }
                static constexpr char hex[] = "0123456789abcdef";
                for (std::uint32_t i = 0u; i < 6u; ++i) {
                    const std::uint8_t byte = rt.memory().load8(mac + i);
                    rt.memory().store8(out + i * 3u, static_cast<std::uint8_t>(hex[byte >> 4u]));
                    rt.memory().store8(out + i * 3u + 1u, static_cast<std::uint8_t>(hex[byte & 15u]));
                    rt.memory().store8(out + i * 3u + 2u, i == 5u ? 0u : static_cast<std::uint8_t>(':'));
                }
                ctx.set_gpr(2u, 0u);
            });
        // sceUtilityGetSystemParamString(id, buf, len): only id 1, the nickname.
        runtime.register_hle("sceUtility", 0x34B78343u,
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                static constexpr char nickname[] = "PSP";
                const std::uint32_t buffer = ctx.gpr[5u];
                const std::uint32_t length = ctx.gpr[6u];
                if (ctx.gpr[4u] != 1u) { ctx.set_gpr(2u, 0x80110103u); return; }   // invalid id
                if (length < sizeof(nickname) || !rt.memory().contains(buffer, sizeof(nickname))) {
                    ctx.set_gpr(2u, 0x80110104u); return;                         // buffer too small
                }
                for (std::uint32_t i = 0u; i < sizeof(nickname); ++i)
                    rt.memory().store8(buffer + i, static_cast<std::uint8_t>(nickname[i]));
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("sceUtility", 0xA5DA2406u,
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                // PSP SystemParam integer IDs: defaults for a US/English system.
                static constexpr std::uint32_t values[] = {
                    1u, 0u, 0u, 0u, 0u, 0u, 1u, 0u, 1u, 0u
                };
                const std::uint32_t id = ctx.gpr[4u];
                const std::uint32_t output = ctx.gpr[5u];
                if (id >= 10u || output == 0u || !rt.memory().contains(output, 4u)) {
                    ctx.set_gpr(2u, 0x80110002u);
                    return;
                }
                rt.memory().store32(output, values[id]);
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("sceImpose", 0x36AA6E91u,
            [&impose_language, &impose_button](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                impose_language = ctx.gpr[4u];
                impose_button = ctx.gpr[5u];
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("sceImpose", 0x24FD7BCFu,
            [&impose_language](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                // Echo the value SetLanguageMode recorded; the caller passes
                // two adjacent output words (observed at 0x09FEFA80/0x09FEFA84).
                const std::uint32_t first = ctx.gpr[4u];
                if (first != 0u && rt.memory().contains(first, 4u)) {
                    rt.memory().store32(first, impose_language);
                }
                const std::uint32_t second = ctx.gpr[5u];
                if (second != 0u && rt.memory().contains(second, 4u)) {
                    rt.memory().store32(second, 0u);
                }
                ctx.set_gpr(2u, 0u);
            });
        // Power callback registration: accepted but never fired (VCS does the same).
        runtime.register_hle("scePower", 0x04B7766Eu,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); });
        // AC power: report online (1) so the game never takes battery-saving paths.
        runtime.register_hle("scePower", 0x87440F5Eu,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 1u); });
        // BRINGUP: sceKernelGetSystemTimeLow — return the low 32 bits of the
        // monotonic microsecond clock (VCS vcs_profile.cpp:5810-5813 does the
        // same; the game's pointer arguments go unused).
        runtime.register_hle("ThreadManForUser", 0x369ED59Du,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                const auto usec = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                ctx.set_gpr(2u, static_cast<std::uint32_t>(usec));
            });
        // sceKernelGetSystemTimeWide: the same clock, 64 bits in v0:v1.
        runtime.register_hle("ThreadManForUser", 0x82BC5777u,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                const auto usec = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                ctx.set_gpr(2u, static_cast<std::uint32_t>(usec));
                ctx.set_gpr(3u, static_cast<std::uint32_t>(usec >> 32u));
            });
        // sceKernelGetSystemTime(SceKernelSysClock *): the same clock, stored as low, high.
        runtime.register_hle("ThreadManForUser", 0xDB738F35u,
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                const auto usec = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                const std::uint32_t clock = ctx.gpr[4u];
                if (!rt.memory().contains(clock, 8u)) { ctx.set_gpr(2u, 0x800200D3u); return; }
                rt.memory().store32(clock, static_cast<std::uint32_t>(usec));
                rt.memory().store32(clock + 4u, static_cast<std::uint32_t>(usec >> 32u));
                ctx.set_gpr(2u, 0u);
            });
        // BRINGUP: scePowerGetPllClockFrequencyInt — the BBS caller
        // (generated_unit_0000.cpp L_0880745C) wants the low-power 222 MHz
        // mode and compares v0 to it, calling
        // scePowerSetClockFrequency350 (0xEBD177D6) on mismatch. Return 222
        // so the query takes the "already set" fast path; a0/a3 are unused
        // (third-birthday's h_PowerPll does the same with 222).
        runtime.register_hle("scePower", 0x34F9C463u,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 222u); });
        // sceKernelReferThreadStatus is in thread_manager.cpp (fills the real
        // SceKernelThreadInfo: the game's heap setup depends on it).
        runtime.register_hle("sceRtc", 0xE7C27D1Bu,
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                const std::uint32_t output = ctx.gpr[4u];
                if (output == 0u || !rt.memory().contains(output, 12u)) {
                    ctx.set_gpr(2u, 0x80000102u);
                    return;
                }
                const auto now = std::chrono::system_clock::now();
                const auto time = std::chrono::system_clock::to_time_t(now);
                std::tm local{};
#if defined(_WIN32)
                localtime_s(&local, &time);
#else
                localtime_r(&time, &local);
#endif
                const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                    now.time_since_epoch()).count() % 1'000'000;
                rt.memory().store16(output, static_cast<std::uint16_t>(local.tm_year + 1900));
                rt.memory().store8(output + 2u, static_cast<std::uint8_t>(local.tm_mon + 1));
                rt.memory().store8(output + 3u, static_cast<std::uint8_t>(local.tm_mday));
                rt.memory().store8(output + 4u, static_cast<std::uint8_t>(local.tm_hour));
                rt.memory().store8(output + 5u, static_cast<std::uint8_t>(local.tm_min));
                rt.memory().store8(output + 6u, static_cast<std::uint8_t>(local.tm_sec));
                rt.memory().store8(output + 7u, 0u);
                rt.memory().store32(output + 8u, static_cast<std::uint32_t>(micros));
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("sceOpenPSID", 0xC69BEBCEu,
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                const std::uint32_t output = ctx.gpr[4u];
                if (output == 0u || !rt.memory().contains(output, 16u)) {
                    ctx.set_gpr(2u, 0x80000102u);
                    return;
                }
                // Stable virtual console identity; this is not a real console PSID.
                static constexpr std::uint8_t virtual_id[16] = {
                    0x50u, 0x53u, 0x50u, 0x52u, 0x45u, 0x43u, 0x4Du, 0x50u,
                    0x57u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u
                };
                for (std::uint32_t i = 0; i < 16u; ++i)
                    rt.memory().store8(output + i, virtual_id[i]);
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("sceCtrl", 0x1F4011E6u,
            [&controller_sampling_mode](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                controller_sampling_mode = ctx.gpr[4u];
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("sceCtrl", 0xDA6B76A1u,
            [&controller_sampling_mode](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2u, controller_sampling_mode);
            });
        runtime.register_hle("sceCtrl", 0x6A2774F3u,
            [&controller_sampling_cycle](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                controller_sampling_cycle = ctx.gpr[4u];
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("sceCtrl", 0x02BAAD91u,
            [&controller_sampling_cycle](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2u, controller_sampling_cycle);
            });
        // sceCtrlReadBuffer*, copied from the VCS profile (vcs_profile.cpp 6449-6482)
        // with centered analog sticks; buttons from g_bbs_pad_buttons (none unless a front end sets them).
        auto write_controller_samples = [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx, bool negative) {
            const std::uint32_t destination = ctx.gpr[4u];
            const std::uint32_t count = ctx.gpr[5u];
            constexpr std::uint32_t sample_size = 16u;
            if (count == 0u) { ctx.set_gpr(2u, 0u); return; }
            if (count > 64u || !rt.memory().contains(destination, static_cast<std::size_t>(count) * sample_size)) {
                ctx.set_gpr(2u, 0x80000103u);
                return;
            }
            const std::uint32_t timestamp = static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            std::uint32_t pressed = g_bbs_pad_buttons.load(std::memory_order_relaxed);
            // PSPRECOMP_AUTOPAD=1 (testing without hands): every 2 s press LEFT, then CROSS, which answers the
            // boot notices ("No system data... Proceed?" defaults to No) and starts the title screen.
            static const bool autopad = std::getenv("PSPRECOMP_AUTOPAD") != nullptr;
            if (autopad) {
                const std::uint32_t t = psphost::vblank_count() % 120u;
                if (t < 8u) pressed |= 0x0080u;                   // LEFT
                else if (t >= 40u && t < 48u) pressed |= 0x4000u;  // CROSS
            }
            const std::uint32_t buttons = negative ? ~pressed : pressed;
            const std::uint32_t analog = g_bbs_pad_analog.load(std::memory_order_relaxed);
            for (std::uint32_t index = 0u; index < count; ++index) {
                const std::uint32_t sample = destination + index * sample_size;
                rt.memory().store32(sample, timestamp);
                rt.memory().store32(sample + 4u, buttons);
                rt.memory().store8(sample + 8u, static_cast<std::uint8_t>(analog >> 0u));
                rt.memory().store8(sample + 9u, static_cast<std::uint8_t>(analog >> 8u));
                rt.memory().store8(sample + 10u, static_cast<std::uint8_t>(analog >> 16u));
                rt.memory().store8(sample + 11u, static_cast<std::uint8_t>(analog >> 24u));
                rt.memory().zero(sample + 12u, 4u);
            }
            ctx.set_gpr(2u, count);
        };
        runtime.register_hle("sceCtrl", 0x3A622550u,
            [write_controller_samples](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { write_controller_samples(rt, ctx, false); });
        runtime.register_hle("sceCtrl", 0x1F803938u,
            [write_controller_samples](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { write_controller_samples(rt, ctx, false); });
        runtime.register_hle("sceCtrl", 0xC152080Au,
            [write_controller_samples](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { write_controller_samples(rt, ctx, true); });
        runtime.register_hle("sceCtrl", 0x60B81F86u,
            [write_controller_samples](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { write_controller_samples(rt, ctx, true); });
        runtime.register_hle("sceDisplay", 0x210EAB3Au,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2u, 0u);
            });
        // Display state + handlers, copied from the VCS profile
        // (vcs_profile.cpp 1275-1284, 6805-6873). S4 adds frame capture on top.
        runtime.register_hle("sceDisplay", 0x0E20F177u,   // sceDisplaySetMode
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                const std::uint32_t mode = ctx.gpr[4u];
                const std::uint32_t width = ctx.gpr[5u];
                const std::uint32_t height = ctx.gpr[6u];
                if (mode != 0u || width == 0u || width > 480u || height == 0u || height > 272u) {
                    ctx.set_gpr(2u, 0x80000107u);
                    return;
                }
                bbs::display_state.mode = mode;
                bbs::display_state.width = width;
                bbs::display_state.height = height;
                if (std::getenv("PSPRECOMP_DISPLAY_DIAG") != nullptr)
                    std::cerr << "[display] set-mode " << width << "x" << height << "\n";
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("sceDisplay", 0xDEA197D4u,   // sceDisplayGetMode
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                const std::uint32_t mode = ctx.gpr[4u];
                const std::uint32_t width = ctx.gpr[5u];
                const std::uint32_t height = ctx.gpr[6u];
                if ((mode != 0u && !rt.memory().contains(mode, 4u)) ||
                    (width != 0u && !rt.memory().contains(width, 4u)) ||
                    (height != 0u && !rt.memory().contains(height, 4u))) {
                    ctx.set_gpr(2u, 0x800200D3u);
                    return;
                }
                if (mode != 0u) rt.memory().store32(mode, bbs::display_state.mode);
                if (width != 0u) rt.memory().store32(width, bbs::display_state.width);
                if (height != 0u) rt.memory().store32(height, bbs::display_state.height);
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("sceDisplay", 0x289D82FEu,   // sceDisplaySetFrameBuf
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                auto &state = bbs::display_state;
                const std::uint32_t buffer = ctx.gpr[4u];
                const std::uint32_t stride = ctx.gpr[5u];
                const std::uint32_t format = ctx.gpr[6u];
                const std::uint32_t sync = ctx.gpr[7u];
                if (buffer == 0u || !rt.memory().contains(buffer, 4u) ||
                    (stride != 0u && (stride < state.width || stride > 2048u)) ||
                    format > 3u || sync > 1u) {
                    ctx.set_gpr(2u, 0x80000107u);
                    return;
                }
                state.frame_buffer = buffer;
                state.buffer_width = stride;
                state.pixel_format = format;
                state.sync_mode = sync;
                if (std::getenv("PSPRECOMP_DISPLAY_DIAG") != nullptr)
                    std::cerr << "[display] set-framebuf buffer=" << psprecomp::hex32(buffer)
                              << " stride=" << stride << " format=" << format << "\n";
                // GPU GE backend: finish its frame and put the picture in guest VRAM
                // (no-op on the software path). 2026-11-16.
                psphost::ge_display_present_gpu_frame(rt, buffer, stride, format, psphost::vblank_count());
                g_bbs_flip_count.fetch_add(1u, std::memory_order_relaxed);
                psphost::perf::frame_tick();   // [perf] frame breakdown every 2 s (PSPRECOMP_PERF=1)
                if (g_bbs_60fps.load(std::memory_order_relaxed) && rt.memory().load32(kBbsFrameModeAddress) == 1u)
                    rt.memory().store32(kBbsFrameModeAddress, 0u);
                // Seal audio up to now even on frames nothing was submitted (silence, not underrun).
                vcs::audio_output_advance(psphost::virtual_time_us());
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("sceDisplay", 0xEEDA2E54u,   // sceDisplayGetFrameBuf
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                const std::uint32_t buffer = ctx.gpr[4u];
                const std::uint32_t stride = ctx.gpr[5u];
                const std::uint32_t format = ctx.gpr[6u];
                const std::uint32_t sync = ctx.gpr[7u];
                if ((buffer != 0u && !rt.memory().contains(buffer, 4u)) ||
                    (stride != 0u && !rt.memory().contains(stride, 4u)) ||
                    (format != 0u && !rt.memory().contains(format, 4u))) {
                    ctx.set_gpr(2u, 0x800200D3u);
                    return;
                }
                if (sync > 1u) { ctx.set_gpr(2u, 0x80000107u); return; }
                const auto &state = bbs::display_state;
                if (buffer != 0u) rt.memory().store32(buffer, state.frame_buffer);
                if (stride != 0u) rt.memory().store32(stride, state.buffer_width);
                if (format != 0u) rt.memory().store32(format, state.pixel_format);
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("sceDisplay", 0x9C6EAAD7u,   // sceDisplayGetVcount
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                // Every delivered vblank (virtual clock too), not only the vblank
                // waits (bbs::vblank_count): see psphost::vblank_count. 2026-11-16.
                ctx.set_gpr(2u, static_cast<std::uint32_t>(psphost::vblank_count()));
            });
        runtime.register_hle("sceDisplay", 0xDBA6C4C4u,   // sceDisplayGetFramePerSec
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.fpr[0u] = 59.94f;
            });
        runtime.register_hle("sceDisplay", 0x4D4E10ECu,   // sceDisplayIsVblank
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); });
        // InterruptManager (sub-interrupt handlers): shared with the other game,
        // psp-native-runtime/host-shared/interrupts.cpp. Vblank (interrupt 30)
        // handlers run as guest code at each vblank wait (thread_manager.cpp).
        psphost::install_interrupts(runtime);
        // Pace the virtual clock to real time unless PSPRECOMP_NO_PACING is set (audio latency, game speed).
        psphost::set_real_time_pacing(std::getenv("PSPRECOMP_NO_PACING") == nullptr);
        runtime.register_hle("scePower", 0xDFA8BAF8u,   // scePowerUnregisterCallback
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); });
        // BRINGUP SHIM: volatile memory lock — VCS vcs_profile.cpp:6332-6368
        // minus the zeroing (bring-up never suspends).
        auto volatile_mem_lock = [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (ctx.gpr[4u] != 0u) { ctx.set_gpr(2u, 0x80000107u); return; }
            if (!rt.memory().contains(ctx.gpr[5u], 4u) || !rt.memory().contains(ctx.gpr[6u], 4u)) {
                ctx.set_gpr(2u, 0x800200D3u); return;
            }
            rt.memory().store32(ctx.gpr[5u], 0x08400000u);
            rt.memory().store32(ctx.gpr[6u], 0x00400000u);
            ctx.set_gpr(2u, 0u);
        };
        runtime.register_hle("sceSuspendForUser", 0x3E0271D3u, volatile_mem_lock);
        runtime.register_hle("sceSuspendForUser", 0xA14F40B2u, volatile_mem_lock);
        runtime.register_hle("sceSuspendForUser", 0xA569E425u,   // sceKernelVolatileMemUnlock
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2u, ctx.gpr[4u] == 0u ? 0u : 0x80000107u);
            });
        runtime.register_hle("sceSuspendForUser", 0x090CCB3Fu,   // sceKernelPowerTick: keeps the PSP awake (movies)
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); });
        runtime.register_hle("sceSuspendForUser", 0xEADB1BD7u,   // sceKernelVolatileMemIsLocked
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2u, ctx.gpr[4u] == 0u ? 0u : 0x80000107u);
            });
        bbs::register_savedata_hle(runtime);
        bbs::register_movie_skip(runtime);   // sceMpeg (host/movie_skip.cpp)
        runtime.register_hle("UtilsForUser", 0xC2DF770Eu,   // sceKernelIcacheInvalidateRange
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                // The game copied code (an overlay): drop the functions compiled for what was there before.
                bbs::overlays_code_changed(rt, ctx.gpr[4u], ctx.gpr[5u]);
                ctx.set_gpr(2u, 0u);
            });   // sceUtilitySavedata* (host/savedata.cpp)
        for (const std::uint32_t nid : {0xEBD177D6u /* scePowerSetClockFrequency */,
                                        0x737486F2u /* scePowerSetClockFrequency (alias) */,
                                        0x843FBF43u /* scePowerSetCpuClockFrequency */,
                                        0xB8D7B3FBu /* scePowerSetBusClockFrequency */}) {
            runtime.register_hle("scePower", nid,   // CPU/bus clock: the host runs at its own speed
                [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); });
        }
        runtime.register_hle("sceUtility", 0xE49BFE92u,   // sceUtilityUnloadModule: pairs with LoadModule below
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); });
        runtime.register_hle("sceUtility", 0x2A2B3DE0u,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                // The bring-up runtime provides the game executable's imports
                // directly; acknowledge utility PRX requests without loading
                // firmware modules into the host process.
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("IoFileMgrForUser", 0x54F5FB11u,
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                const std::string device = rt.memory().read_c_string(ctx.gpr[4u], 32u);
                if (ctx.gpr[5u] == 0x02425823u && device == "fatms0:") {
                    // The fifth sceIoDevctl argument (outdata) is in t0 (args 5-8 are t0-t3).
                    const std::uint32_t output = ctx.gpr[8u];
                    if (output == 0u || !rt.memory().contains(output, 4u)) {
                        ctx.set_gpr(2u, 0x80010016u);
                        return;
                    }
                    rt.memory().store32(output, 1u);
                }
                ctx.set_gpr(2u, 0u);
            });
        const auto relocations = elf.load_and_relocate(
            runtime.memory(), psprecomp::kDefaultPspUserLoadBase);
        const auto module = elf.find_module_info(
            runtime.memory(), psprecomp::kDefaultPspUserLoadBase);
        if (!module) throw psprecomp::Error("PSP module info not found after relocation");

#if defined(PSPRECOMP_HAS_ZLIBDEC_MODULE)
        constexpr std::uint32_t zlibdec_load_base = 0x09D00000u;
        constexpr std::uint32_t zlibdec_return_trampoline = 0x09D0F000u;
        const auto zlibdec_path = disc_root / "PSP_GAME" / "USRDIR" / "modules" / "zlibdec.prx";
        const auto zlibdec_elf = psprecomp::Elf32Image::from_file(zlibdec_path);
        const auto zlibdec_relocations = zlibdec_elf.load_and_relocate(runtime.memory(), zlibdec_load_base);
        const auto zlibdec_module = zlibdec_elf.find_module_info(runtime.memory(), zlibdec_load_base);
        if (!zlibdec_module) throw psprecomp::Error("zlibdec module info not found after relocation");
        psprecomp::register_zlibdec_generated_functions(runtime);
        for (std::uint32_t entry = zlibdec_module->ent_top; entry < zlibdec_module->ent_end;) {
            if (!runtime.memory().contains(entry, 16u))
                throw psprecomp::Error("zlibdec export entry is outside guest memory");
            const std::uint8_t entry_words = runtime.memory().load8(entry + 8u);
            const std::uint8_t variable_count = runtime.memory().load8(entry + 9u);
            const std::uint8_t function_count = runtime.memory().load8(entry + 10u);
            const std::uint32_t table = runtime.memory().load32(entry + 12u);
            const std::uint32_t export_count = static_cast<std::uint32_t>(variable_count) + function_count;
            const std::uint32_t entry_bytes = static_cast<std::uint32_t>(entry_words) * 4u;
            if (entry_words < 4u || entry + entry_bytes > zlibdec_module->ent_end ||
                !runtime.memory().contains(table, export_count * 8u))
                throw psprecomp::Error("Malformed zlibdec export table");
            for (std::uint32_t index = 0; index < function_count; ++index) {
                const std::uint32_t export_index = static_cast<std::uint32_t>(variable_count) + index;
                const std::uint32_t nid = runtime.memory().load32(table + export_index * 4u);
                const std::uint32_t target = runtime.memory().load32(table + export_count * 4u + export_index * 4u);
                runtime.register_hle("zlibdec", nid,
                    [=](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                        const std::uint32_t return_pc = ctx.gpr[31u];
                        g_kjfs_pending_returns[psprecomp::runtime_thread_uid()].push_back(
                            {return_pc, ctx.gpr[28u], 0u, 0u, false, false,
                             ctx.gpr[4u], ctx.gpr[5u], ctx.gpr[6u], ctx.gpr[7u]});
                        ctx.set_gpr(31u, zlibdec_return_trampoline);
                        ctx.set_gpr(28u, zlibdec_module->gp);
                        ctx.pc = target;
                    });
            }
            entry += entry_bytes;
        }
        runtime.register_function(zlibdec_return_trampoline,
            &kjfs_return_trampoline_function, "zlibdec return trampoline");
#endif

        std::uint64_t arena_start = psprecomp::kDefaultPspUserLoadBase;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> main_module_ranges;
        for (std::size_t index = 0; index < elf.segments().size(); ++index) {
            const auto &segment = elf.segments()[index];
            if (segment.type != 1u) continue;
            const std::uint32_t segment_base = elf.segment_runtime_address(index);
            const std::uint64_t end = static_cast<std::uint64_t>(segment_base) + segment.memory_size;
            if (end <= std::numeric_limits<std::uint32_t>::max())
                main_module_ranges.emplace_back(segment_base, static_cast<std::uint32_t>(end));
            arena_start = std::max(arena_start, end);
        }
        if (arena_start >= 0x09FF0000u || arena_start > std::numeric_limits<std::uint32_t>::max())
            throw psprecomp::Error("BBS ELF leaves no safe user-memory arena");

        psprecomp::register_generated_functions(runtime);
        bbs::install_overlays(runtime);
#if defined(PSPRECOMP_HAS_KJFS_MODULE)
        // Generated AOT functions replace PRX instruction execution, but the
        // PRX's relocated data/BSS still has to live in guest memory. KJFS
        // keeps its filesystem state in GP-relative globals.
        constexpr std::uint32_t kjfs_load_base = 0x09E00000u;
        const auto kjfs_path = disc_root / "PSP_GAME" / "USRDIR" / "modules" / "kjfs.prx";
        const auto kjfs_elf = psprecomp::Elf32Image::from_file(kjfs_path);
        const auto kjfs_relocations = kjfs_elf.load_and_relocate(runtime.memory(), kjfs_load_base);
        const auto kjfs_module = kjfs_elf.find_module_info(runtime.memory(), kjfs_load_base);
        if (!kjfs_module) throw psprecomp::Error("KJFS module info not found after relocation");
        psprecomp::register_kjfs_generated_functions(runtime);
        // The disc's KCEJ_FS module is AOT-generated at 0x09E00000. Export
        // NID 0x87AE0285 resolves to module-relative entry 0x3DBC.
        constexpr std::uint32_t kjfs_entry_87ae0285 = kjfs_load_base + 0x3DBCu;
        const std::uint32_t kjfs_gp = kjfs_module->gp;
        constexpr std::uint32_t kjfs_return_trampoline = kjfs_load_base + 0x16000u;
        const bool trace_kjfs = std::getenv("PSPRECOMP_KJFS_TRACE") != nullptr;
        g_trace_kjfs_calls = trace_kjfs;
        g_kjfs_trace_count = 0u;
        runtime.register_function(kjfs_return_trampoline,
            &kjfs_return_trampoline_function, "KJFS return trampoline");
        // Resolve every KJFS function export to its relocated PRX address so
        // generated EBOOT import stubs can enter the matching AOT function.
        // Export entries contain an 8-byte header followed by NID and address
        // arrays. Variable exports precede function exports in the NID list.
        for (std::uint32_t entry = kjfs_module->ent_top; entry < kjfs_module->ent_end;) {
            if (!runtime.memory().contains(entry, 16u))
                throw psprecomp::Error("KJFS export entry is outside guest memory");
            const std::uint8_t entry_words = runtime.memory().load8(entry + 8u);
            const std::uint8_t variable_count = runtime.memory().load8(entry + 9u);
            const std::uint8_t function_count = runtime.memory().load8(entry + 10u);
            const std::uint32_t table = runtime.memory().load32(entry + 12u);
            const std::uint32_t export_count = static_cast<std::uint32_t>(variable_count) + function_count;
            const std::uint32_t entry_bytes = static_cast<std::uint32_t>(entry_words) * 4u;
            if (entry_words < 4u || entry + entry_bytes > kjfs_module->ent_end ||
                !runtime.memory().contains(table, export_count * 8u))
                throw psprecomp::Error("Malformed KJFS export table");
            for (std::uint32_t index = 0; index < function_count; ++index) {
                const std::uint32_t export_index = static_cast<std::uint32_t>(variable_count) + index;
                const std::uint32_t nid = runtime.memory().load32(table + export_index * 4u);
                const std::uint32_t target = runtime.memory().load32(table + export_count * 4u + export_index * 4u);
                if (trace_kjfs && (nid == 0x9995DE66u || nid == 0x0B4EDE89u))
                    std::cerr << "[kjfs-export] nid=" << psprecomp::hex32(nid)
                              << " target=" << psprecomp::hex32(target) << "\n";
                runtime.register_hle("kjfs", nid,
                    [=](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                        const std::uint32_t return_pc = ctx.gpr[31u];
                        if (trace_kjfs && nid == 0x0B4EDE89u && g_kjfs_trace_count < 2u) {
                            std::cerr << "[kjfs-call] nid=" << psprecomp::hex32(nid)
                                      << " args=[" << psprecomp::hex32(ctx.gpr[4u]) << ","
                                      << psprecomp::hex32(ctx.gpr[5u]) << ","
                                      << psprecomp::hex32(ctx.gpr[6u]) << ","
                                      << psprecomp::hex32(ctx.gpr[7u]) << "]";
                            if (rt.memory().contains(ctx.gpr[4u], 1u))
                                std::cerr << " a0str=\"" << rt.memory().read_c_string(ctx.gpr[4u], 48u) << "\"";
                            std::cerr << "\n";
                        }
                        if (trace_kjfs && nid == 0x9995DE66u && g_kjfs_trace_count < 2u) {
                            std::cerr << "[kjfs-call] nid=" << psprecomp::hex32(nid) << " a0 words=";
                            for (std::uint32_t i = 0; i < 8u; ++i)
                                std::cerr << (i ? "," : "") << psprecomp::hex32(
                                    rt.memory().load32(ctx.gpr[4u] + i * 4u));
                            std::cerr << " a1 words=";
                            for (std::uint32_t i = 0; i < 8u; ++i)
                                std::cerr << (i ? "," : "") << psprecomp::hex32(
                                    rt.memory().load32(ctx.gpr[5u] + i * 4u));
                            std::cerr << "\n";
                        }
                        g_kjfs_pending_returns[psprecomp::runtime_thread_uid()].push_back(
                            {return_pc, ctx.gpr[28u], ctx.gpr[4u], ctx.gpr[7u],
                             nid == 0x87AE0285u, trace_kjfs && nid == 0x9995DE66u,
                             ctx.gpr[4u], ctx.gpr[5u], ctx.gpr[6u], ctx.gpr[7u]});
                        ctx.set_gpr(31u, kjfs_return_trampoline);
                        ctx.set_gpr(28u, kjfs_gp);
                        ctx.pc = target;
                    });
            }
            entry += entry_bytes;
        }
        (void)kjfs_entry_87ae0285;
#endif
        runtime.cpu().set_gpr(28u, module->gp);
        runtime.cpu().set_gpr(29u, 0x09FFF000u);
        runtime.cpu().set_gpr(31u, 0u);
        bbs::ThreadManager threads;
        threads.initialize(runtime, runtime.cpu().gpr[29u]);
        threads.install_hles(runtime);
        // sceAudio with sound (2026-11-17). Return values follow the VCS profile;
        // the PCM goes to vcs::audio_output (AAudio on Android, waveOut on
        // Windows), scheduled on the virtual clock the way PSPRecomp-VCS-Android
        // does it (vcs_profile.cpp audio_queue_buffer, MIT): buffer N starts
        // where buffer N-1 ends, and a blocking output waits until its buffer
        // starts. The wait moves virtual time on (deliver_alarms), so alarms and
        // vblanks keep ticking, then yields like DelayThread.
        {
            struct AudioChannel {
                bool reserved = false;
                std::uint32_t samples = 0u;
                std::uint32_t format = 0u;   // 0x10 = mono
                bool queue_active = false;
                std::uint64_t queue_anchor_us = 0u;
                std::uint64_t queued_frames = 0u;
            };
            auto channels = std::make_shared<std::array<AudioChannel, 9>>(); // 0-7 + Output2 (8)
            // Save states: reservations and formats are game state; the host queue restarts on load.
            g_bbs_audio_state = [channels](psphost::StateIO &io) {
                io.section("audio");
                io(*channels);
                if (io.loading()) {
                    for (std::uint32_t i = 0; i < channels->size(); ++i) {
                        (*channels)[i].queue_active = false;
                        (*channels)[i].queued_frames = 0u;
                        vcs::audio_output_reset_channel(i);
                    }
                }
            };
            // `threads` lives until main returns, after the runtime stops: the
            // pointer is captured by value, never a reference to a local.
            bbs::ThreadManager *thread_manager = &threads;
            const auto ok = [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); };
            const auto frames_us = [](std::uint64_t frames) { return frames * 1'000'000u / 44'100u; };
            // Where the next buffer of `c` starts. A stream that fell more than
            // 250 ms behind the clock (or a new one) restarts at the clock.
            const auto next_start = [frames_us](const AudioChannel &c, std::uint64_t now) {
                const std::uint64_t start = c.queue_anchor_us + frames_us(c.queued_frames);
                return (!c.queue_active || start + 250'000u < now) ? now : start;
            };
            const auto queue = [frames_us](AudioChannel &c, std::uint32_t frames) {
                const std::uint64_t now = psphost::virtual_time_us();
                std::uint64_t start = c.queue_anchor_us + frames_us(c.queued_frames);
                if (!c.queue_active || start + 250'000u < now) {
                    c.queue_active = true;
                    c.queue_anchor_us = now;
                    c.queued_frames = 0u;
                    start = now;
                }
                c.queued_frames += frames;
                return start;
            };
            // One output: wait (first entry only moves the clock; deliver_alarms
            // re-enters the import when it starts guest handlers), then queue
            // and submit the buffer, then yield.
            const auto output = [channels, thread_manager, next_start, queue](
                                    psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx, std::uint32_t ch,
                                    std::uint32_t left, std::uint32_t right, std::uint32_t buffer, bool blocking) {
                AudioChannel &c = (*channels)[ch];
                const std::uint64_t now = psphost::virtual_time_us();
                const std::uint64_t start = next_start(c, now);
                if (blocking && psphost::deliver_alarms(ctx, start > now ? start - now : 0u)) return;
                const bool stereo = c.format != 0x10u;
                const std::size_t bytes = static_cast<std::size_t>(c.samples) * (stereo ? 4u : 2u);
                const std::uint64_t at = queue(c, c.samples);
                if (buffer != 0u && c.samples != 0u && rt.memory().contains(buffer, bytes) &&
                    vcs::audio_output_enabled()) {
                    std::vector<std::int16_t> pcm(bytes / 2u);
                    rt.memory().copy_out(buffer, std::span<std::uint8_t>(
                        reinterpret_cast<std::uint8_t *>(pcm.data()), bytes));
                    vcs::audio_output_submit(pcm, c.samples, stereo, left, right, 44'100u, ch, at);
                    vcs::audio_output_advance(psphost::virtual_time_us());
                    // PSPRECOMP_AUDIO_DIAG: one line per channel every 200 buffers.
                    static const bool diag = std::getenv("PSPRECOMP_AUDIO_DIAG") != nullptr;
                    if (diag) {
                        static std::array<std::uint32_t, 9> calls{};
                        static std::array<int, 9> peak{};
                        for (const std::int16_t s : pcm) peak[ch] = std::max(peak[ch], std::abs(static_cast<int>(s)));
                        if (++calls[ch] % 200u == 1u) {
                            std::cerr << "[audio] ch=" << ch << " calls=" << calls[ch] << " samples=" << c.samples
                                      << " stereo=" << stereo << " vol=" << left << "/" << right
                                      << " peak=" << peak[ch] << " at_us=" << at
                                      << " now_us=" << psphost::virtual_time_us() << "\n";
                            peak[ch] = 0;
                        }
                    }
                }
                ctx.set_gpr(2u, c.samples);
                if (blocking) thread_manager->wait_like_delay(rt, ctx);
            };
            // sceAudioOutputBlocking(ch, vol, buf) / OutputPannedBlocking(ch, l, r, buf)
            const auto block = [output](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                const std::uint32_t ch = ctx.gpr[4u];
                if (ch > 7u) { ctx.set_gpr(2u, 0x80260003u); return; }
                output(rt, ctx, ch, ctx.gpr[5u], ctx.gpr[5u], ctx.gpr[6u], true);
            };
            const auto block_panned = [output](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                const std::uint32_t ch = ctx.gpr[4u];
                if (ch > 7u) { ctx.set_gpr(2u, 0x80260003u); return; }
                output(rt, ctx, ch, ctx.gpr[5u], ctx.gpr[6u], ctx.gpr[7u], true);
            };
            // sceAudioOutput2OutputBlocking(vol, buf): always stereo.
            const auto block_output2 = [output](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                output(rt, ctx, 8u, ctx.gpr[4u], ctx.gpr[4u], ctx.gpr[5u], true);
            };
            runtime.register_hle("sceAudio", 0x5EC81C55u,   // sceAudioChReserve(ch, samples, format)
                [channels](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                    auto ch = static_cast<std::int32_t>(ctx.gpr[4u]);
                    const std::uint32_t samples = ctx.gpr[5u];
                    if (samples == 0u || samples > 0xFFC0u || (samples & 63u) != 0u) { ctx.set_gpr(2u, 0x80260005u); return; }
                    if (ch < 0) {
                        for (std::int32_t i = 7; i >= 0; --i) if (!(*channels)[i].reserved) { ch = i; break; }
                        if (ch < 0) { ctx.set_gpr(2u, 0x80260002u); return; }
                    }
                    if (ch > 7) { ctx.set_gpr(2u, 0x80260003u); return; }
                    if ((*channels)[ch].reserved) { ctx.set_gpr(2u, 0x80260002u); return; }
                    (*channels)[ch] = AudioChannel{true, samples, ctx.gpr[6u]};
                    ctx.set_gpr(2u, static_cast<std::uint32_t>(ch));
                });
            runtime.register_hle("sceAudio", 0x6FC46853u,   // sceAudioChRelease(ch)
                [channels](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                    const std::uint32_t ch = ctx.gpr[4u];
                    if (ch > 7u) { ctx.set_gpr(2u, 0x80260003u); return; }
                    if (!(*channels)[ch].reserved) { ctx.set_gpr(2u, 0x80260008u); return; }
                    (*channels)[ch] = AudioChannel{};
                    ctx.set_gpr(2u, 0u);
                });
            runtime.register_hle("sceAudio", 0xCB2E439Eu,   // sceAudioSetChannelDataLen(ch, samples)
                [channels](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                    const std::uint32_t ch = ctx.gpr[4u];
                    if (ch > 7u) { ctx.set_gpr(2u, 0x80260003u); return; }
                    (*channels)[ch].samples = ctx.gpr[5u];
                    ctx.set_gpr(2u, 0u);
                });
            runtime.register_hle("sceAudio", 0xE2D56B2Du,   // sceAudioOutputPanned(ch, l, r, buf), non-blocking
                [output](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                    const std::uint32_t ch = ctx.gpr[4u];
                    if (ch > 7u) { ctx.set_gpr(2u, 0x80260003u); return; }
                    output(rt, ctx, ch, ctx.gpr[5u], ctx.gpr[6u], ctx.gpr[7u], false);
                });
            runtime.register_hle("sceAudio", 0x136CAF51u, block);          // sceAudioOutputBlocking
            runtime.register_hle("sceAudio", 0x13F592BCu, block_panned);   // sceAudioOutputPannedBlocking
            runtime.register_hle("sceAudio", 0x2D53F36Eu, block_output2);   // sceAudioOutput2OutputBlocking
            runtime.register_hle("sceAudio", 0xB011922Fu, ok);      // sceAudioGetChannelRestLength: 0 = drained
            runtime.register_hle("sceAudio", 0x95FD0C2Du, ok);      // sceAudioChangeChannelConfig
            runtime.register_hle("sceAudio", 0xB7E1D8E7u, ok);      // sceAudioChangeChannelVolume
            runtime.register_hle("sceAudio", 0x01562BA3u,   // sceAudioOutput2Reserve(samples): one argument
                [channels](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                    const std::uint32_t samples = ctx.gpr[4u] & 0x7FFFFFFFu;
                    if (samples < 17u || samples > 4111u) { ctx.set_gpr(2u, 0x80000104u); return; }
                    if ((*channels)[8].reserved) { ctx.set_gpr(2u, 0x80268002u); return; }
                    (*channels)[8] = AudioChannel{true, samples};
                    ctx.set_gpr(2u, 0u);
                });
            runtime.register_hle("sceAudio", 0x43196845u,   // sceAudioOutput2Release
                [channels](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                    (*channels)[8] = AudioChannel{};
                    ctx.set_gpr(2u, 0u);
                });
            runtime.register_hle("sceAudio", 0x647CEF33u, ok);      // sceAudioOutput2GetRestSample: 0 = drained
            runtime.register_hle("sceAudio", 0x63F2889Cu,   // sceAudioOutput2ChangeLength(samples)
                [channels](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                    (*channels)[8].samples = ctx.gpr[4u] & 0x7FFFFFFFu;
                    ctx.set_gpr(2u, 0u);
                });
        }
        // sceSasCore: the real voice mixer (sas_hle.cpp, from PSPRecomp-VCS-Android). 2026-11-17.
        bbs::install_sas_hle(runtime);
        // sceAtrac3plus: ATRAC3/3+ decoded from guest memory (atrac_hle.cpp). 2026-11-17.
        bbs::install_atrac_hle(runtime);
        bbs::PartitionMemory partition_memory;
        bbs::DiscIo disc_io;
        // Raw UMD reads (umd1: + the LBN from sceIoGetstat) need the real disc image.
        // PSPRECOMP_ISO, else the first *.iso next to the disc-files folder.
        {
            std::filesystem::path iso;
            if (const char *forced = std::getenv("PSPRECOMP_ISO"); forced != nullptr && *forced != '\0') {
                iso = forced;
            } else {
                std::error_code iso_error;
                for (const auto &entry : std::filesystem::directory_iterator(disc_root.parent_path(), iso_error)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".iso") { iso = entry.path(); break; }
                }
            }
            if (iso.empty()) std::cerr << "ISO:        none found; raw umd0:/umd1: reads will fail\n";
            else disc_io.set_iso(iso);
            // PGD-encrypted data (BBS1-3.DAT) is read from copies that
            // psp-native-runtime\tools\pspdec\pgddec decrypted offline.
            disc_io.set_decrypted_dir(disc_root.parent_path() / "decrypted");
        }
        partition_memory.initialize(runtime, static_cast<std::uint32_t>(arena_start));
        partition_memory.set_stack_floor_source([&threads] { return threads.stack_floor(); });
        threads.set_heap_end_source([&partition_memory] { return partition_memory.high_watermark(); });
        partition_memory.install_hles(runtime);
        disc_io.install_hles(runtime);
        // sceIoWaitAsync(CB) blocks like DelayThread (see DiscIo::wait_async).
        // `threads` outlives the runtime: capture its address, not a local ref.
        disc_io.set_async_wait_hook([thread_manager = &threads](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            thread_manager->wait_like_delay(rt, ctx);
        });

        // Save states (2026-11-18). The whole machine = this context, guest RAM/VRAM/scratchpad and every host
        // module's state, in a fixed order; each module serializes both ways (psphost::StateIO). The locals
        // captured by reference live in main() for the whole run.
        const auto serialize_machine = [&](psphost::StateIO &io, psprecomp::Runtime &rt,
                                           psprecomp::AllegrexContext &ctx) {
            io.section("cpu");
            io(ctx);
            io.section("memory");
            io.bytes(rt.memory().raw_pointer(0x08000000u, 32u * 1024u * 1024u), 32u * 1024u * 1024u);
            io.bytes(rt.memory().raw_pointer(0x04000000u, psprecomp::GuestMemory::kVramSize),
                     psprecomp::GuestMemory::kVramSize);
            io.bytes(rt.memory().raw_pointer(0x00010000u, 16u * 1024u), 16u * 1024u);
            threads.serialize_state(io);
            io.section("kernel");
            io(compiled_sdk_version); io(compiler_version); io(interrupt_state); io(next_callback_uid);
            io(callbacks); io(exit_callback_uid); io(umd_callbacks); io(umd_activated); io(impose_language);
            io(impose_button); io(controller_sampling_mode); io(controller_sampling_cycle); io(general_purpose_io);
            io(g_kjfs_pending_returns);
            io(bbs::display_state);
            io(bbs::vblank_count);
            partition_memory.serialize_state(io);
            disc_io.serialize_state(io);
            bbs::serialize_sas_state(io);
            bbs::serialize_atrac_state(io, rt.memory());
            psphost::serialize_interrupt_state(io);
            psphost::serialize_guest_call_state(io);
            psphost::serialize_ge_display_state(io);
            if (g_bbs_audio_state) g_bbs_audio_state(io);
            io.section("end");
        };
        static constexpr char kStateMagic[8] = {'B', 'B', 'S', 'S', 'T', 'A', 'T', '1'};
        static constexpr std::uint32_t kStateVersion = 2u;   // 2: captured at a thread switch (1 was mid-HLE)
        const auto save_state_to = [&](const std::string &path, psprecomp::Runtime &rt,
                                       psprecomp::AllegrexContext &ctx) -> bool {
            psphost::StateIO io(false);
            char magic[8];
            std::memcpy(magic, kStateMagic, 8);
            io(magic);
            std::uint32_t version = kStateVersion;
            io(version);
            serialize_machine(io, rt, ctx);
            const std::string tmp = path + ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out) return false;
                out.write(reinterpret_cast<const char *>(io.data().data()),
                          static_cast<std::streamsize>(io.data().size()));
                if (!out) return false;
            }
            std::error_code error;
            std::filesystem::rename(tmp, path, error);
            return !error;
        };
        const auto load_state_from = [&](const std::string &path, psprecomp::Runtime &rt,
                                         psprecomp::AllegrexContext &ctx) -> bool {
            std::vector<std::uint8_t> data;
            {
                std::ifstream in(path, std::ios::binary);
                if (!in) return false;
                data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            if (data.size() < 12u || std::memcmp(data.data(), kStateMagic, 8) != 0) {
                std::cerr << "[state] " << path << ": not a BBS save state\n";
                return false;
            }
            std::uint32_t version{};
            std::memcpy(&version, data.data() + 8, 4);
            if (version != kStateVersion) {
                std::cerr << "[state] " << path << ": version " << version << ", this build reads "
                          << kStateVersion << "\n";
                return false;
            }
            // Rollback copy of the live machine: a state that fails halfway is undone.
            psphost::StateIO rollback(false);
            serialize_machine(rollback, rt, ctx);
            try {
                psphost::StateIO io(true, std::move(data));
                char magic[8];
                io(magic);
                io(version);
                serialize_machine(io, rt, ctx);
            } catch (const std::exception &e) {
                std::cerr << "[state] load failed (" << e.what() << "), restoring the running game\n";
                psphost::StateIO back(true, rollback.data());
                serialize_machine(back, rt, ctx);
                return false;
            }
            return true;
        };
        // After a load: the runtime's thread identity, chained-call tokens and any loaded overlay code follow
        // the restored machine.
        const auto after_load = [&](psprecomp::Runtime &rt) {
            ++psprecomp::g_runtime_thread_switch_generation_fast;
            psprecomp::set_runtime_thread_identity(threads.current_uid(), threads.current_name());
            bbs::overlays_code_changed(rt, 0x08000000u, 0x02000000u);
            vcs::audio_output_rebase(psphost::virtual_time_us());   // sound after a load (11-18)
        };
        threads.set_safe_point_hook([&, save_state_to, load_state_from, after_load](
                                        psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto handle_request = [&]() {
                const int request = g_bbs_state_request.exchange(0);
                if (request == 0) return;
                std::cerr << "[state] " << (request == 1 ? "save" : "load") << " request reached the safe point\n";
                std::string path;
                {
                    std::lock_guard<std::mutex> lock(g_bbs_state_mutex);
                    path = g_bbs_state_path;
                }
                const auto started = std::chrono::steady_clock::now();
                bool ok = false;
                try {
                    ok = request == 1 ? save_state_to(path, rt, ctx) : load_state_from(path, rt, ctx);
                } catch (const std::exception &e) {
                    std::cerr << "[state] " << e.what() << "\n";
                    ok = false;
                }
                if (ok && request == 2) after_load(rt);
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count();
                std::cerr << "[state] " << (request == 1 ? "save " : "load ") << path << (ok ? " ok" : " FAILED")
                          << " (" << ms << " ms, thread " << threads.current_name() << ")\n";
                g_bbs_state_result.store(ok ? 1 : -1);
            };
            handle_request();
            while (g_bbs_paused.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                handle_request();
            }
        });
        runtime.register_hle("ModuleMgrForUser", 0xD8B73127u,
            [main_module_ranges](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                const std::uint32_t address = ctx.gpr[4u];
                const bool belongs_to_main = std::any_of(main_module_ranges.begin(), main_module_ranges.end(),
                    [address](const auto &range) { return address >= range.first && address < range.second; });
                ctx.set_gpr(2u, belongs_to_main ? 0x1000u : 0x8002012Fu);
            });

#if defined(PSPRECOMP_HAS_ZLIBDEC_MODULE)
        constexpr std::uint32_t zlibdec_module_start_return = 0x09D0F100u;
        runtime.register_function(zlibdec_module_start_return,
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &) {
                rt.stop("zlibdec module_start returned");
            }, "zlibdec module_start return trampoline");
        runtime.cpu() = psprecomp::AllegrexContext{};
        runtime.cpu().set_gpr(28u, zlibdec_module->gp);
        runtime.cpu().set_gpr(29u, 0x09FFF000u);
        runtime.cpu().set_gpr(31u, zlibdec_module_start_return);
        runtime.run(zlibdec_elf.runtime_entry(zlibdec_load_base), 1'000'000u);
        if (runtime.stop_reason() != "zlibdec module_start returned") {
            std::cerr << "zlibdec module_start stopped unexpectedly: " << runtime.stop_reason() << "\n";
            runtime.report_hle_histogram();
            return 3;
        }
        std::cout << "zlibdec module_start: returned\n";
        std::cout << "zlibdec relocations: " << zlibdec_relocations.total << " (invalid "
                  << zlibdec_relocations.invalid << ", unsupported " << zlibdec_relocations.unsupported << ")\n";
        runtime.cpu() = psprecomp::AllegrexContext{};
        runtime.cpu().set_gpr(28u, module->gp);
        runtime.cpu().set_gpr(29u, 0x09FFF000u);
#endif

#if defined(PSPRECOMP_HAS_KJFS_MODULE)
        // PSP's loader calls the PRX entry point before its exports are used.
        // The previous bring-up stub skipped this initialization, causing the
        // game's first 32 KiB read to retry forever.
        constexpr std::uint32_t kjfs_module_start_return = 0x09FFEF00u;
        runtime.register_function(kjfs_module_start_return,
            [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &) {
                rt.stop("KJFS module_start returned");
            }, "KJFS module_start return trampoline");
        runtime.cpu().set_gpr(28u, kjfs_gp);
        runtime.cpu().set_gpr(4u, 0u);
        runtime.cpu().set_gpr(5u, 0u);
        runtime.cpu().set_gpr(31u, kjfs_module_start_return);
        runtime.run(kjfs_load_base + 0x70C8u, 1'000'000u);
        if (runtime.stop_reason() != "KJFS module_start returned") {
            std::cerr << "KJFS module_start stopped unexpectedly: " << runtime.stop_reason() << "\n";
            runtime.report_hle_histogram();
            return 3;
        }
        std::cout << "KJFS module_start: returned\n";
        std::cout << "KJFS relocations: " << kjfs_relocations.total << " (invalid "
                  << kjfs_relocations.invalid << ", unsupported " << kjfs_relocations.unsupported << ")\n";
        runtime.cpu() = psprecomp::AllegrexContext{};
        runtime.cpu().set_gpr(28u, module->gp);
        runtime.cpu().set_gpr(29u, 0x09FFF000u);
        runtime.cpu().set_gpr(31u, 0u);
#endif

        std::cout << "BBS bring-up harness\n"
                  << "ELF:        " << elf_path.string() << "\n"
                  << "Disc root:  " << disc_root.string() << "\n"
                  << "Module:     " << module->name << "\n"
                  << "Entry:      " << psprecomp::hex32(elf.runtime_entry()) << "\n"
                  << "GP:         " << psprecomp::hex32(module->gp) << "\n"
                  << "Relocations:" << relocations.total << " (invalid "
                  << relocations.invalid << ", unsupported " << relocations.unsupported << ")\n"
                  << "Functions:  " << runtime.function_count() << "\n"
                  << "Dispatch cap: " << max_dispatches << "\n";

        // Optional GPU GE backend (2026-11-16): VCSNative.ini beside the exe, or PSPRECOMP_CONFIG,
        // [Rendering] Backend=Vulkan (or DirectX12) + NativeGE=true. Without it the bit-exact software
        // rasterizer draws everything, as before; ge_display's renderer feeds whichever backend is active.
        vcs::initialize_vcs_configuration(std::filesystem::absolute(argv[0]).parent_path());
        {
            std::string gpu_error;
            if (!vcs::initialize_ge_gpu_backend(gpu_error))
                throw psprecomp::Error("GE GPU backend: " + gpu_error);
            const vcs::GeGpuBackendReport gpu = vcs::ge_gpu_backend_report();
            std::cout << "GE backend: requested=" << vcs::ge_gpu_backend_name(gpu.requested)
                      << " active=" << vcs::ge_gpu_backend_name(gpu.active) << " (" << gpu.message << ")\n";
        }

        // Unimplemented PSP imports stop the runtime and report their library/NID.
        runtime.run(elf.runtime_entry(), max_dispatches);
        {
            const vcs::GeGpuBackendReport gpu = vcs::ge_gpu_backend_report();
            std::cout << "GE backend " << vcs::ge_gpu_backend_name(gpu.active) << ": draws=" << gpu.game_draw_calls
                      << " frames=" << gpu.game_frames << " textured=" << gpu.textured_game_draw_calls
                      << " targets=" << gpu.framebuffer_targets_observed << "\n";
            vcs::shutdown_ge_gpu_backend();
        }
        std::cout << "Compiled SDK version: " << compiled_sdk_version << "\n";
        std::cout << "Compiler version: " << compiler_version << "\n";
        std::cout << "Runtime stopped: " << runtime.stop_reason() << "\n";
        runtime.report_hle_histogram(100u);
        return runtime.stop_reason().starts_with("Missing HLE import ") ? 3 : 0;
    } catch (const std::exception &error) {
        std::cerr << "BBS bring-up failed: " << error.what() << "\n";
        return 1;
    }
}
