#pragma once

namespace psprecomp { class Runtime; class GuestMemory; }
namespace psphost { class StateIO; }

namespace bbs {

// Registers the sceAtrac3plus HLEs (ATRAC3/3+ decoded from guest memory: atrac_hle.cpp).
void install_atrac_hle(psprecomp::Runtime &runtime);
void serialize_atrac_state(psphost::StateIO &io, const psprecomp::GuestMemory &memory);

} // namespace bbs
