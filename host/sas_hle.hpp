#pragma once

namespace psprecomp { class Runtime; }
namespace psphost { class StateIO; }

namespace bbs {

// Registers the sceSasCore HLEs (a real mixer: see sas_hle.cpp).
void install_sas_hle(psprecomp::Runtime &runtime);
void serialize_sas_state(psphost::StateIO &io);

} // namespace bbs
