#pragma once
// sceMpeg with real movie playback (movie_player.cpp). Falls back to movie_skip.cpp with PSPRECOMP_MOVIE_SKIP=1.
#include "psprecomp/runtime.hpp"

namespace bbs {
void register_movie_player(psprecomp::Runtime &runtime);
// Called after a save-state load: a movie playing at that point ends.
void movie_after_state_load();
} // namespace bbs
