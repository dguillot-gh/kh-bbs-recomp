#pragma once

#include <cstdint>

namespace bbs {

// Display state shared by main.cpp's sceDisplay handlers and
// thread_manager.cpp's vblank waits. Copied from
// mgs-peace-walker\host\display_state.hpp (S3).
struct DisplayState {
    std::uint32_t mode{};
    std::uint32_t width{480u};
    std::uint32_t height{272u};
    std::uint32_t frame_buffer{};
    std::uint32_t buffer_width{512u};
    std::uint32_t pixel_format{3u};
    std::uint32_t sync_mode{};
};

inline DisplayState display_state{};
inline std::uint64_t vblank_count{};

} // namespace bbs
