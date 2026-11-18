#pragma once
// H.264 decoding for movies (2026-11-18). On Android it is the platform decoder through the NDK's AMediaCodec
// (hardware on the phone, no FFmpeg). Elsewhere there is no decoder yet: open() fails and the movie is skipped.
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace bbs::movie {

// One decoded picture as tight I420 planes (width x height luma, half-size chroma).
struct VideoFrame {
    int width{};
    int height{};
    std::int64_t pts_us{};
    std::vector<std::uint8_t> y, u, v;
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    bool open(int width, int height);
    void close();
    [[nodiscard]] bool is_open() const;
    // Queues one access unit (Annex B). Returns false on a codec error.
    bool queue(const std::uint8_t *data, std::size_t size, std::int64_t pts_us);
    // Tells the codec no more input follows, so it releases the pictures it holds back for reordering.
    void signal_end();
    // Collects decoded pictures, waiting up to timeout_us for the first one.
    void poll(int timeout_us);
    // Takes the oldest decoded picture, if any.
    bool take(VideoFrame &out);
    [[nodiscard]] std::size_t ready() const { return frames_.size(); }
    [[nodiscard]] std::uint64_t queued() const { return queued_; }
    [[nodiscard]] std::uint64_t decoded() const { return decoded_; }
    [[nodiscard]] const std::string &describe() const { return describe_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::deque<VideoFrame> frames_;
    std::uint64_t queued_{};
    std::uint64_t decoded_{};
    std::string describe_;
};

} // namespace bbs::movie
