#pragma once
namespace psprecomp { class Runtime; }
namespace bbs {
// sceMpeg stand-in that skips movies (host/movie_skip.cpp).
void register_movie_skip(psprecomp::Runtime &runtime);
}
