#include "psprecomp/elf32.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"
#include "partition_memory.hpp"
#include "thread_manager.hpp"
#include "disc_io.hpp"
#include "display_state.hpp"

#include <cstdint>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
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

int main(int argc, char **argv) {
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
        std::int32_t ge_next_callback_uid = 0xA00;
        bool umd_activated = false;
        std::uint32_t impose_language = 1u;
        std::uint32_t impose_button = 0u;
        std::uint32_t controller_sampling_mode = 0u;
        std::uint32_t controller_sampling_cycle = 0u;
        std::uint32_t ge_next_list_id = 1u;
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
        // sceGeEdramGetAddr(): return the guest physical address where the runtime
        // maps EDRAM (kVramPhysicalBase), matching the VCS profile.
        runtime.register_hle("sceGe_user", 0xE47E40E4u,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2u, psprecomp::GuestMemory::kVramPhysicalBase);
            });
        // sceGeSetCallback(struct *cb): registers a GE completion callback.
        // Bringup has no GE execution, so validate the struct (VCS profile
        // does the same) and hand out an id without ever firing the callback.
        runtime.register_hle("sceGe_user", 0xA4FC06A4u,
            [&ge_next_callback_uid](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                const std::uint32_t callback_data = ctx.gpr[4u];
                if (callback_data == 0u || !rt.memory().contains(callback_data, 16u)) {
                    ctx.set_gpr(2u, 0x800200D3u); // SCE_KERNEL_ERROR_ILLEGAL_ADDR
                    return;
                }
                ctx.set_gpr(2u, static_cast<std::uint32_t>(ge_next_callback_uid++));
            });
        // BRINGUP SHIM: no GE execution; replace with the VCS GE port (S7/S8).
        // Enqueue/EnQueueHead hand out a list id and raise any SIGNAL event flag
        // in the list (Sony's GE init waits on the "SceGuSignal" flag).
        auto ge_shim_enqueue = [&ge_next_list_id](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t list = ctx.gpr[4u];
            if (list != 0u && rt.memory().contains(list, 4u)) {
                for (std::uint32_t offset = 0u; offset < 16384u; offset += 4u) {
                    const std::uint32_t address = list + offset;
                    if (!rt.memory().contains(address, 4u)) break;
                    const std::uint32_t word = rt.memory().load32(address);
                    const std::uint32_t cmd = word >> 24u;
                    if (cmd == 0x0Fu || cmd == 0x0Cu) break; // FINISH / END
                    if (cmd != 0x0Eu) continue;              // SIGNAL: raise the flag
                    psprecomp::AllegrexContext flag_ctx{};
                    flag_ctx.gpr[4u] = word & 0xFFFFFFu;     // event-flag uid
                    flag_ctx.gpr[5u] = 1u;                    // pattern |= mask
                    rt.invoke_import("ThreadManForUser", 0x1FB15A32u, flag_ctx);
                }
            }
            ctx.set_gpr(2u, ge_next_list_id++);
        };
        runtime.register_hle("sceGe_user", 0xAB49E76Au,   // sceGeListEnQueue
            [ge_shim_enqueue](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { ge_shim_enqueue(rt, ctx); });
        runtime.register_hle("sceGe_user", 0x1C0D95A6u,   // sceGeListEnQueueHead
            [ge_shim_enqueue](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { ge_shim_enqueue(rt, ctx); });
        runtime.register_hle("sceGe_user", 0xE0D68148u,   // sceGeListUpdateStallAddr
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); });
        runtime.register_hle("sceGe_user", 0x03444EB4u,   // sceGeListSync: COMPLETED
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); });
        runtime.register_hle("sceGe_user", 0xB287BD61u,   // sceGeDrawSync
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); });
        runtime.register_hle("sceGe_user", 0xB448EC0Du,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); });
        runtime.register_hle("sceGe_user", 0x4C06E472u,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); });
        runtime.register_hle("sceGe_user", 0x05DB22CEu,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); });
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
                localtime_s(&local, &time);
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
        // with the bring-up constants: no buttons, centered analog sticks.
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
            const std::uint32_t buttons = negative ? ~0u : 0u;
            for (std::uint32_t index = 0u; index < count; ++index) {
                const std::uint32_t sample = destination + index * sample_size;
                rt.memory().store32(sample, timestamp);
                rt.memory().store32(sample + 4u, buttons);
                rt.memory().store8(sample + 8u, 128u);
                rt.memory().store8(sample + 9u, 128u);
                rt.memory().store8(sample + 10u, 128u);
                rt.memory().store8(sample + 11u, 128u);
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
                ctx.set_gpr(2u, static_cast<std::uint32_t>(bbs::vblank_count));
            });
        runtime.register_hle("sceDisplay", 0xDBA6C4C4u,   // sceDisplayGetFramePerSec
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.fpr[0u] = 59.94f;
            });
        runtime.register_hle("sceDisplay", 0x4D4E10ECu,   // sceDisplayIsVblank
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); });
        // InterruptManager, copied from the VCS profile (vcs_profile.cpp
        // 1285-1294, 6739-6803). Bring-up never fires the handlers; the state
        // exists so S8's GE completion callback path has something to use.
        struct SubInterruptRecord {
            std::uint32_t handler{};
            std::uint32_t argument{};
            bool enabled{};
            bool occurred{};
        };
        std::unordered_map<std::uint64_t, SubInterruptRecord> sub_interrupts;
        auto sub_interrupt_key = [](std::uint32_t interrupt_number, std::uint32_t sub_number) -> std::uint64_t {
            return (static_cast<std::uint64_t>(interrupt_number) << 32u) | sub_number;
        };
        runtime.register_hle("InterruptManager", 0xCA04A2B9u,   // sceInterruptSetHandler
            [&sub_interrupts, &sub_interrupt_key](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                const std::uint32_t interrupt_number = ctx.gpr[4u];
                const std::uint32_t sub_number = ctx.gpr[5u];
                const std::uint32_t handler = ctx.gpr[6u];
                const std::uint32_t argument = ctx.gpr[7u];
                if (interrupt_number >= 67u || handler == 0u) {
                    ctx.set_gpr(2u, 0x80020064u);
                    return;
                }
                const std::uint64_t key = sub_interrupt_key(interrupt_number, sub_number);
                if (sub_interrupts.contains(key)) {
                    ctx.set_gpr(2u, 0x80020067u);  // handler already present
                    return;
                }
                sub_interrupts.emplace(key, SubInterruptRecord{handler, argument, false, false});
                if (std::getenv("PSPRECOMP_GE_DIAG") != nullptr) {
                    std::cerr << "[intr] register int=" << interrupt_number << " sub=" << sub_number
                              << " handler=" << psprecomp::hex32(handler)
                              << " arg=" << psprecomp::hex32(argument) << "\n";
                }
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("InterruptManager", 0xD61E6961u,   // sceInterruptRemoveHandler
            [&sub_interrupts, &sub_interrupt_key](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                const std::uint64_t key = sub_interrupt_key(ctx.gpr[4u], ctx.gpr[5u]);
                ctx.set_gpr(2u, sub_interrupts.erase(key) == 1u ? 0u : 0x80020068u);
            });
        runtime.register_hle("InterruptManager", 0xFB8E22ECu,   // sceInterruptEnable
            [&sub_interrupts, &sub_interrupt_key](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                const auto found = sub_interrupts.find(sub_interrupt_key(ctx.gpr[4u], ctx.gpr[5u]));
                if (found == sub_interrupts.end()) { ctx.set_gpr(2u, 0x80020068u); return; }
                found->second.enabled = true;
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("InterruptManager", 0x8A389411u,   // sceInterruptDisable
            [&sub_interrupts, &sub_interrupt_key](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                const auto found = sub_interrupts.find(sub_interrupt_key(ctx.gpr[4u], ctx.gpr[5u]));
                if (found == sub_interrupts.end()) { ctx.set_gpr(2u, 0x80020068u); return; }
                found->second.enabled = false;
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("InterruptManager", 0x5CB5A78Bu,   // sceInterruptClear
            [&sub_interrupts, &sub_interrupt_key](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                const auto found = sub_interrupts.find(sub_interrupt_key(ctx.gpr[4u], ctx.gpr[5u]));
                if (found == sub_interrupts.end()) { ctx.set_gpr(2u, 0x80020068u); return; }
                if (ctx.gpr[6u] != 0u) {
                    if (!rt.memory().contains(ctx.gpr[6u], 4u)) { ctx.set_gpr(2u, 0x800200D3u); return; }
                    rt.memory().store32(ctx.gpr[6u], found->second.enabled ? 1u : 0u);
                }
                found->second.enabled = false;
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("InterruptManager", 0x7860E0DCu,   // sceInterruptSetGlobalPriority (enable-state variant)
            [&sub_interrupts, &sub_interrupt_key](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                const auto found = sub_interrupts.find(sub_interrupt_key(ctx.gpr[4u], ctx.gpr[5u]));
                if (found == sub_interrupts.end()) { ctx.set_gpr(2u, 0x80020068u); return; }
                found->second.enabled = ctx.gpr[6u] != 0u;
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("InterruptManager", 0xFC4374B8u,   // sceInterruptHasOccurred
            [&sub_interrupts, &sub_interrupt_key](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                const auto found = sub_interrupts.find(sub_interrupt_key(ctx.gpr[4u], ctx.gpr[5u]));
                ctx.set_gpr(2u, found != sub_interrupts.end() && found->second.occurred ? 1u : 0u);
            });
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
        runtime.register_hle("sceSuspendForUser", 0xEADB1BD7u,   // sceKernelVolatileMemIsLocked
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2u, ctx.gpr[4u] == 0u ? 0u : 0x80000107u);
            });
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

        // Unimplemented PSP imports stop the runtime and report their library/NID.
        runtime.run(elf.runtime_entry(), max_dispatches);
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
