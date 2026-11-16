#pragma once
// BBS code overlays (PSP_GAME/USRDIR/MODULE/*.ELF), recompiled by scripts/gen-overlays.sh. 2026-11-16.
#include <cstddef>
#include <cstdint>

namespace psprecomp { class Runtime; }

namespace bbs {
struct OverlayInfo {
    const char *name;                         // file name without .ELF
    void (*register_functions)(psprecomp::Runtime &);
    const std::uint32_t *addresses;           // every address register_functions registers
    std::size_t address_count;
};
extern const OverlayInfo kOverlays[];         // generated overlays_registry.cpp
extern const std::size_t kOverlayCount;

// Reads the module files, installs the runtime's missing-function hook. Call after the main corpus is registered.
void install_overlays(psprecomp::Runtime &runtime);
// sceKernelIcacheInvalidateRange: the game just copied code; drop a swapped-out overlay's functions.
void overlays_code_changed(psprecomp::Runtime &runtime, std::uint32_t address, std::uint32_t size);
}
