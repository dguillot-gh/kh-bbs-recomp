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
    std::vector<std::uint8_t> bytes;  // the loadable segment's file bytes
};
std::vector<Module> g_modules;
std::uint32_t g_window_start = 0xFFFFFFFFu, g_window_end = 0u;
const Module *g_loaded = nullptr;

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

void unload(psprecomp::Runtime &rt) {
    if (g_loaded == nullptr) return;
    rt.unregister_functions(g_window_start, g_window_end);
    if (trace()) std::cerr << "[overlay] unloaded " << g_loaded->info->name << "\n";
    g_loaded = nullptr;
}

bool on_missing(psprecomp::Runtime &rt, std::uint32_t pc) {
    if (pc < g_window_start || pc >= g_window_end) return false;
    const Module *best = nullptr;
    double best_score = 0.0;
    for (const Module &m : g_modules) {
        const double s = match(rt, m);
        if (s > best_score) { best_score = s; best = &m; }
    }
    if (best == nullptr || best_score < 0.9) {
        std::cerr << "[overlay] no module matches the code at " << psprecomp::hex32(pc) << " (best "
                  << (best ? best->info->name : "none") << " " << best_score << ")\n";
        return false;
    }
    if (best == g_loaded) {  // loaded and still missing: a real gap in its corpus
        bool listed = false;
        for (std::size_t i = 0; i < best->info->address_count; ++i) listed |= best->info->addresses[i] == pc;
        std::cerr << "[overlay] " << best->info->name << " is loaded but has nothing at " << psprecomp::hex32(pc)
                  << " (in its address list: " << (listed ? "yes" : "no") << ")\n";
        return false;
    }
    unload(rt);
    best->info->register_functions(rt);
    g_loaded = best;
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
    std::cerr << "[overlay] " << g_modules.size() << " modules, window " << psprecomp::hex32(g_window_start) << "-"
              << psprecomp::hex32(g_window_end) << "\n";
}

void overlays_code_changed(psprecomp::Runtime &runtime, std::uint32_t address, std::uint32_t size) {
    if (address < g_window_end && address + size > g_window_start) unload(runtime);
}
} // namespace bbs
