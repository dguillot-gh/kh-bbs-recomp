#include "movie_video_decoder.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>

#if defined(__ANDROID__)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

namespace bbs::movie {

#if defined(__ANDROID__)

struct VideoDecoder::Impl {
    AMediaCodec *codec{};
    int width{}, height{};
    // Output layout (from the codec's output format).
    int color_format{21};
    int stride{}, slice_height{};
    int crop_left{}, crop_top{}, crop_right{-1}, crop_bottom{-1};
    bool ended{};

    void read_format(std::string &describe) {
        AMediaFormat *format = AMediaCodec_getOutputFormat(codec);
        if (format == nullptr) return;
        std::int32_t value = 0;
        if (AMediaFormat_getInt32(format, "color-format", &value)) color_format = value;
        if (AMediaFormat_getInt32(format, "width", &value)) width = value;
        if (AMediaFormat_getInt32(format, "height", &value)) height = value;
        stride = width; slice_height = height;
        if (AMediaFormat_getInt32(format, "stride", &value) && value > 0) stride = value;
        if (AMediaFormat_getInt32(format, "slice-height", &value) && value > 0) slice_height = value;
        if (AMediaFormat_getInt32(format, "crop-left", &value)) crop_left = value;
        if (AMediaFormat_getInt32(format, "crop-top", &value)) crop_top = value;
        if (AMediaFormat_getInt32(format, "crop-right", &value)) crop_right = value;
        if (AMediaFormat_getInt32(format, "crop-bottom", &value)) crop_bottom = value;
        describe = AMediaFormat_toString(format);
        std::cerr << "[mpeg] video output format: " << describe << "\n";
        AMediaFormat_delete(format);
    }
};

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::open(int width, int height) {
    close();
    auto impl = std::make_unique<Impl>();
    impl->codec = AMediaCodec_createDecoderByType("video/avc");
    if (impl->codec == nullptr) { std::cerr << "[mpeg] no video/avc decoder\n"; return false; }
    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, "mime", "video/avc");
    AMediaFormat_setInt32(format, "width", width);
    AMediaFormat_setInt32(format, "height", height);
    AMediaFormat_setInt32(format, "max-input-size", 512 * 1024);
    AMediaFormat_setInt32(format, "low-latency", 1);  // API 30+, ignored before
    AMediaFormat_setInt32(format, "priority", 0);     // real time
    media_status_t status = AMediaCodec_configure(impl->codec, format, nullptr, nullptr, 0);
    AMediaFormat_delete(format);
    if (status == AMEDIA_OK) status = AMediaCodec_start(impl->codec);
    if (status != AMEDIA_OK) {
        std::cerr << "[mpeg] video decoder configure/start failed: " << status << "\n";
        AMediaCodec_delete(impl->codec);
        return false;
    }
#if __ANDROID_API__ >= 28
    char *name = nullptr;
    if (AMediaCodec_getName(impl->codec, &name) == AMEDIA_OK && name != nullptr) {
        std::cerr << "[mpeg] video decoder " << name << " " << width << "x" << height << "\n";
        AMediaCodec_releaseName(impl->codec, name);
    }
#else
    std::cerr << "[mpeg] video decoder open " << width << "x" << height << "\n";
#endif
    impl->width = width; impl->height = height;
    impl->stride = width; impl->slice_height = height;
    impl_ = std::move(impl);
    frames_.clear();
    queued_ = decoded_ = 0;
    return true;
}

void VideoDecoder::close() {
    if (impl_ && impl_->codec != nullptr) {
        AMediaCodec_stop(impl_->codec);
        AMediaCodec_delete(impl_->codec);
    }
    impl_.reset();
    frames_.clear();
}

bool VideoDecoder::is_open() const { return impl_ != nullptr; }

bool VideoDecoder::queue(const std::uint8_t *data, std::size_t size, std::int64_t pts_us) {
    if (!impl_ || impl_->ended) return false;
    for (int attempt = 0; attempt < 50; ++attempt) {
        const ssize_t index = AMediaCodec_dequeueInputBuffer(impl_->codec, 2000);
        if (index >= 0) {
            std::size_t capacity = 0;
            std::uint8_t *buffer = AMediaCodec_getInputBuffer(impl_->codec, static_cast<std::size_t>(index), &capacity);
            if (buffer == nullptr) return false;
            const std::size_t n = std::min(size, capacity);
            std::memcpy(buffer, data, n);
            if (AMediaCodec_queueInputBuffer(impl_->codec, static_cast<std::size_t>(index), 0, n,
                                             static_cast<std::uint64_t>(pts_us), 0) != AMEDIA_OK)
                return false;
            ++queued_;
            return true;
        }
        poll(0);  // all input buffers busy: drain outputs to free them
    }
    std::cerr << "[mpeg] video decoder input stalled\n";
    return false;
}

void VideoDecoder::signal_end() {
    if (!impl_ || impl_->ended) return;
    const ssize_t index = AMediaCodec_dequeueInputBuffer(impl_->codec, 10000);
    if (index < 0) return;
    AMediaCodec_queueInputBuffer(impl_->codec, static_cast<std::size_t>(index), 0, 0, 0,
                                 AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    impl_->ended = true;
}

void VideoDecoder::poll(int timeout_us) {
    if (!impl_) return;
    Impl &d = *impl_;
    for (int guard = 0; guard < 16; ++guard) {
        AMediaCodecBufferInfo info{};
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(d.codec, &info, timeout_us);
        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { d.read_format(describe_); continue; }
        if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (index < 0) return;  // try again later
        timeout_us = 0;
        std::size_t capacity = 0;
        const std::uint8_t *buffer = AMediaCodec_getOutputBuffer(d.codec, static_cast<std::size_t>(index), &capacity);
        if (buffer != nullptr && info.size > 0) {
            const int left = std::max(0, d.crop_left), top = std::max(0, d.crop_top);
            const int w = (d.crop_right >= left ? d.crop_right - left + 1 : d.width) & ~1;
            const int h = (d.crop_bottom >= top ? d.crop_bottom - top + 1 : d.height) & ~1;
            const std::uint8_t *base = buffer + info.offset;
            const std::size_t luma_size = static_cast<std::size_t>(d.stride) * static_cast<std::size_t>(d.slice_height);
            VideoFrame frame;
            frame.width = w; frame.height = h;
            frame.pts_us = info.presentationTimeUs;
            frame.y.resize(static_cast<std::size_t>(w) * h);
            frame.u.resize(static_cast<std::size_t>(w / 2) * (h / 2));
            frame.v.resize(frame.u.size());
            const std::size_t limit = static_cast<std::size_t>(info.size);
            const bool planar = d.color_format == 19;  // COLOR_FormatYUV420Planar (I420); anything else as NV12
            const std::size_t needed = planar ? luma_size + luma_size / 2u : luma_size + luma_size / 2u;
            if (needed <= limit + static_cast<std::size_t>(d.stride)) {
                for (int row = 0; row < h; ++row)
                    std::memcpy(&frame.y[static_cast<std::size_t>(row) * w],
                                base + static_cast<std::size_t>(row + top) * d.stride + left, static_cast<std::size_t>(w));
                const std::uint8_t *chroma = base + luma_size;
                for (int row = 0; row < h / 2; ++row) {
                    std::uint8_t *u = &frame.u[static_cast<std::size_t>(row) * (w / 2)];
                    std::uint8_t *v = &frame.v[static_cast<std::size_t>(row) * (w / 2)];
                    const std::size_t crow = static_cast<std::size_t>(row + top / 2);
                    if (planar) {
                        const std::size_t cstride = static_cast<std::size_t>(d.stride) / 2u;
                        const std::uint8_t *us = chroma + crow * cstride + left / 2;
                        const std::uint8_t *vs = chroma + luma_size / 4u + crow * cstride + left / 2;
                        std::memcpy(u, us, static_cast<std::size_t>(w / 2));
                        std::memcpy(v, vs, static_cast<std::size_t>(w / 2));
                    } else {
                        const std::uint8_t *uv = chroma + crow * d.stride + (left & ~1);
                        for (int x = 0; x < w / 2; ++x) { u[x] = uv[2 * x]; v[x] = uv[2 * x + 1]; }
                    }
                }
                frames_.push_back(std::move(frame));
                ++decoded_;
            } else if (decoded_ == 0) {
                std::cerr << "[mpeg] video output smaller than expected: " << info.size << " < " << needed << "\n";
            }
        }
        AMediaCodec_releaseOutputBuffer(d.codec, static_cast<std::size_t>(index), false);
        if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0u) return;
    }
}

#else  // no decoder on this platform yet: movies are skipped

struct VideoDecoder::Impl {};
VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() = default;
bool VideoDecoder::open(int, int) { return false; }
void VideoDecoder::close() { impl_.reset(); frames_.clear(); }
bool VideoDecoder::is_open() const { return false; }
bool VideoDecoder::queue(const std::uint8_t *, std::size_t, std::int64_t) { return false; }
void VideoDecoder::signal_end() {}
void VideoDecoder::poll(int) {}

#endif

bool VideoDecoder::take(VideoFrame &out) {
    if (frames_.empty()) return false;
    out = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

} // namespace bbs::movie
