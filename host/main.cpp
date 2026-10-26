#include "psprecomp/elf32.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"
#include "partition_memory.hpp"
#include "thread_manager.hpp"
#include "disc_io.hpp"

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
        bool umd_activated = false;
        std::uint32_t impose_language = 1u;
        std::uint32_t impose_button = 0u;
        std::uint32_t controller_sampling_mode = 0u;
        std::uint32_t controller_sampling_cycle = 0u;
        runtime.register_hle("SysMemUserForUser", 0x91DE343Cu,
            [&compiled_sdk_version](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                // SDK 5.00-5.50 variant of sceKernelSetCompiledSdkVersion.
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
        runtime.register_hle("sceDisplay", 0x210EAB3Au,
            [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                ctx.set_gpr(2u, 0u);
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
                    // The fifth sceIoDevctl argument is the first stack arg.
                    const std::uint32_t output = rt.memory().load32(ctx.gpr[29u] + 16u);
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
