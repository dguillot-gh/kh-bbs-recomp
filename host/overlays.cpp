// Overlay swapping for BBS (2026-11-16). The nine MODULE/*.ELF files all load into one window at 0x08B8BF08 (the
// game's own loader copies them there and flushes the instruction cache). Each is recompiled ahead of time with
// its own symbols; when dispatch reaches the window with nothing registered, we match guest memory against the
// module files and register the one that is loaded. Same scheme as PortableKit's overlay levers
// (portable-ops-native docs/ARCHITECTURE.md, "Code that changes at run time"), written for this runtime.
#include "overlays.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace bbs {
namespace {
struct Module {
    const OverlayInfo *info;
    std::uint32_t vaddr{};
    std::uint32_t end{};              // vaddr + memsz
    std::vector<std::uint8_t> bytes;  // the loadable segment's file bytes
};
std::vector<Module> g_modules;
std::uint32_t g_window_start = 0xFFFFFFFFu, g_window_end = 0u;
// Overlays registered right now. The US disc loads one at a time (all share 0x08B8BF08); Final Mix can hold two side
// by side (MENU_CAMP at 0x08B6EE80 ends where CAMP_MAIN starts, 0x08B799D0), so any set of non-overlapping ones.
std::vector<const Module *> g_loaded;

bool overlaps(const Module &m, std::uint32_t start, std::uint32_t end) { return m.vaddr < end && start < m.end; }

bool trace() { return std::getenv("PSPRECOMP_OVERLAY_TRACE") != nullptr; }

bool read_module(const std::filesystem::path &path, Module &m) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(in)), {});
    auto u16 = [&](std::size_t o) { return static_cast<std::uint32_t>(d[o] | d[o + 1] << 8); };
    auto u32 = [&](std::size_t o) { return u16(o) | u16(o + 2) << 16; };
    if (d.size() < 52 || std::memcmp(d.data(), "\x7f" "ELF", 4) != 0) return false;
    const std::uint32_t phoff = u32(28), phnum = u16(44);
    for (std::uint32_t i = 0; i < phnum; ++i) {
        const std::size_t p = phoff + 32u * i;
        if (p + 32 > d.size() || u32(p) != 1u) continue;  // PT_LOAD
        const std::uint32_t off = u32(p + 4), vaddr = u32(p + 8), filesz = u32(p + 16), memsz = u32(p + 20);
        if (off + filesz > d.size()) return false;
        m.vaddr = vaddr;
        m.end = vaddr + memsz;
        m.bytes.assign(d.begin() + off, d.begin() + off + filesz);
        g_window_start = std::min(g_window_start, vaddr);
        g_window_end = std::max(g_window_end, vaddr + memsz);
        return true;
    }
    return false;
}

// Share of the first 16 KiB of the module's words that guest memory holds right now.
double match(psprecomp::Runtime &rt, const Module &m) {
    const std::size_t words = std::min<std::size_t>(m.bytes.size(), 0x4000u) / 4u;
    if (words == 0 || !rt.memory().contains(m.vaddr, static_cast<std::uint32_t>(words * 4u))) return 0.0;
    std::size_t same = 0;
    for (std::size_t i = 0; i < words; ++i) {
        std::uint32_t w;
        std::memcpy(&w, m.bytes.data() + i * 4u, 4u);
        if (rt.memory().load32(m.vaddr + static_cast<std::uint32_t>(i * 4u)) == w) ++same;
    }
    return static_cast<double>(same) / static_cast<double>(words);
}

// Unregisters every loaded overlay that overlaps [start, end).
void unload(psprecomp::Runtime &rt, std::uint32_t start, std::uint32_t end) {
    for (auto it = g_loaded.begin(); it != g_loaded.end();) {
        if (!overlaps(**it, start, end)) { ++it; continue; }
        rt.unregister_functions((*it)->vaddr, (*it)->end);
        if (trace()) std::cerr << "[overlay] unloaded " << (*it)->info->name << "\n";
        it = g_loaded.erase(it);
    }
}

bool on_missing(psprecomp::Runtime &rt, std::uint32_t pc) {
    if (pc < g_window_start || pc >= g_window_end) return false;
    const Module *best = nullptr;
    double best_score = 0.0;
    for (const Module &m : g_modules) {
        if (pc < m.vaddr || pc >= m.end) continue;   // only modules that cover this address
        const double s = match(rt, m);
        if (s > best_score) { best_score = s; best = &m; }
    }
    if (best == nullptr || best_score < 0.9) {
        std::cerr << "[overlay] no module matches the code at " << psprecomp::hex32(pc) << " (best "
                  << (best ? best->info->name : "none") << " " << best_score << ")\n";
        return false;
    }
    if (std::find(g_loaded.begin(), g_loaded.end(), best) != g_loaded.end()) {  // loaded and still missing: a real gap
        bool listed = false;
        for (std::size_t i = 0; i < best->info->address_count; ++i) listed |= best->info->addresses[i] == pc;
        std::cerr << "[overlay] " << best->info->name << " is loaded but has nothing at " << psprecomp::hex32(pc)
                  << " (in its address list: " << (listed ? "yes" : "no") << ")\n";
        return false;
    }
    unload(rt, best->vaddr, best->end);
    best->info->register_functions(rt);
    g_loaded.push_back(best);
    std::cerr << "[overlay] loaded " << best->info->name << " (match " << best_score << ", at "
              << psprecomp::hex32(pc) << ")\n";
    return true;
}
} // namespace

void install_overlays(psprecomp::Runtime &runtime) {
    for (std::size_t i = 0; i < kOverlayCount; ++i) {
        Module m{&kOverlays[i]};
        const auto path = runtime.translate_path(std::string("disc0:/PSP_GAME/USRDIR/MODULE/") + kOverlays[i].name + ".ELF");
        if (read_module(path, m)) g_modules.push_back(std::move(m));
        else std::cerr << "[overlay] cannot read " << path.string() << "\n";
    }
    psprecomp::set_runtime_missing_function_hook(&on_missing);
    // The main program's generated units are 0x8000-byte buckets, and the last one can reach into the window
    // (Final Mix: unit 109 = 0x08B6C000-0x08B74000, window from 0x08B6EE80). The runtime's dense unit table would
    // hand window addresses to that main unit, which has no entry there ("invalid internal function entry", seen
    // 2026-11-19 at 0x08B6F8A4) and never asks on_missing. Registering a non-generated function in a bucket poisons
    // it (exact per-PC lookup from then on, also after unregistering), so do that for every bucket the window
    // touches; overlay and main functions in those buckets still chain through the exact table.
    if (g_window_start < g_window_end) {
        const auto guard = [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            rt.stop("overlay window guard reached at " + psprecomp::hex32(ctx.pc));
        };
        const std::uint32_t first = g_window_start & ~0x3FFFu;   // smallest bucket size the runtime uses (16 KiB)
        for (std::uint32_t a = first; a < g_window_end; a += 0x4000u) {
            const std::uint32_t at = a < g_window_start ? g_window_start : a;
            // Nothing lives here yet: main code ends below the window and overlays register on first use.
            runtime.register_function(at, guard, "overlay-window-guard");
            runtime.unregister_functions(at, at + 4u);
        }
    }
    std::cerr << "[overlay] " << g_modules.size() << " modules, window " << psprecomp::hex32(g_window_start) << "-"
              << psprecomp::hex32(g_window_end) << "\n";
}

void overlays_code_changed(psprecomp::Runtime &runtime, std::uint32_t address, std::uint32_t size) {
    if (address < g_window_end && address + size > g_window_start) unload(runtime, address, address + size);
}
} // namespace bbs
