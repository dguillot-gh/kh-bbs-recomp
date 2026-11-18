// Offline check of the movie path (2026-11-18): demux a PSMF file, decode its video with AMediaCodec and its
// audio with at3_standalone, print counts/timestamps, and write a few frames (PPM) and the audio (WAV).
// Build: see HANDOFF.md (NDK clang++, run from adb shell in /data/local/tmp). Not part of the app.
#include "movie_demux.hpp"
#include "movie_video_decoder.hpp"
#include "at3_decoders.h"

#include <dlfcn.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace bbs::movie;

static void write_ppm(const VideoFrame &f, const char *path) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << f.width << " " << f.height << "\n255\n";
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x) {
            const int Y = f.y[y * f.width + x] - 16, U = f.u[(y / 2) * (f.width / 2) + x / 2] - 128,
                      V = f.v[(y / 2) * (f.width / 2) + x / 2] - 128;
            auto c = [](int v) { return static_cast<char>(std::clamp(v >> 8, 0, 255)); };
            const char rgb[3]{c(298 * Y + 409 * V + 128), c(298 * Y - 100 * U - 208 * V + 128), c(298 * Y + 516 * U + 128)};
            out.write(rgb, 3);
        }
}

int main(int argc, char **argv) {
    if (void *lib = dlopen("libbinder_ndk.so", RTLD_NOW)) {  // a shell binary has no binder threads for codec callbacks
        if (auto start = reinterpret_cast<void (*)()>(dlsym(lib, "ABinderProcess_startThreadPool"))) start();
    }
    if (argc < 2) { std::printf("usage: movie_test file.pmf [offset]\n"); return 1; }
    std::ifstream in(argv[1], std::ios::binary);
    if (argc > 2) in.seekg(std::strtoll(argv[2], nullptr, 0));
    std::vector<std::uint8_t> header(2048);
    in.read(reinterpret_cast<char *>(header.data()), 2048);
    auto be32 = [](const std::uint8_t *p) { return (std::uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; };
    const std::uint32_t offset = be32(&header[8]), size = be32(&header[12]);
    std::printf("magic %.8s offset %u size %u\n", reinterpret_cast<char *>(header.data()), offset, size);
    in.seekg(static_cast<std::streamoff>(offset) - 2048, std::ios::cur);
    std::vector<std::uint8_t> stream(size);
    in.read(reinterpret_cast<char *>(stream.data()), size);

    PsmfDemux demux;
    VideoDecoder video;
    const bool opened = video.open(480, 272);
    std::printf("video decoder open: %d\n", opened);
    int channels = 2, align = 0;
    ATRAC3PContext *at3p = nullptr;
    std::vector<std::int16_t> pcm;
    std::vector<float> planes[2]{std::vector<float>(4096), std::vector<float>(4096)};
    int aus = 0, afr = 0, frames = 0;
    std::size_t fed = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto take_frames = [&] {
        VideoFrame f;
        while (video.take(f)) {
            if (frames < 3 || frames % 300 == 0)
                std::printf("frame %d pts_us %lld %dx%d\n", frames, (long long)f.pts_us, f.width, f.height);
            if (frames == 0 || frames == 100 || frames == 400) {
                char path[64]; std::snprintf(path, sizeof path, "/data/local/tmp/movie_f%d.ppm", frames);
                write_ppm(f, path);
            }
            ++frames;
        }
    };
    while (true) {
        if (fed < stream.size() && demux.pending_bytes() < 256 * 1024) {
            const std::size_t n = std::min<std::size_t>(64 * 2048, stream.size() - fed);
            demux.push(&stream[fed], n);
            fed += n;
            if (fed >= stream.size()) demux.set_eof();
            continue;
        }
        EsUnit u;
        bool any = false;
        if (demux.next_video_au(u)) {
            any = true;
            if (aus < 3) std::printf("au %d size %zu pts %lld nal %02x %02x %02x %02x %02x\n", aus, u.data.size(),
                                     (long long)u.pts, u.data[0], u.data[1], u.data[2], u.data[3], u.data[4]);
            ++aus;
            if (opened) { video.queue(u.data.data(), u.data.size(), u.pts * 100 / 9); video.poll(0); take_frames(); }
        }
        if (demux.next_audio_frame(u)) {
            any = true;
            if (at3p == nullptr) {
                channels = ((u.data[2] >> 2) & 7) == 1 ? 1 : 2;
                align = static_cast<int>(u.data.size()) - 8;
                at3p = atrac3p_alloc(channels, &align);
                std::printf("audio hdr %02x %02x %02x %02x ch %d align %d pts %lld\n", u.data[0], u.data[1], u.data[2],
                            u.data[3], channels, align, (long long)u.pts);
            }
            float *out[2]{planes[0].data(), planes[1].data()};
            int n = 0;
            const int r = atrac3p_decode_frame(at3p, out, &n, u.data.data() + 8, align);
            if (afr < 2) std::printf("audio frame %d result %d samples %d pts %lld\n", afr, r, n, (long long)u.pts);
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < 2; ++c)
                    pcm.push_back(static_cast<std::int16_t>(std::clamp(planes[channels > 1 ? c : 0][i], -1.f, 1.f) * 32767));
            ++afr;
        }
        if (!any && fed >= stream.size()) break;
    }
    if (opened) {
        video.signal_end();
        for (int i = 0; i < 20; ++i) { video.poll(20000); take_frames(); }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("packs %llu aus %d frames %d audio frames %d (%.1f s audio) in %.2f s\n",
                (unsigned long long)demux.packs(), aus, frames, afr, pcm.size() / 2 / 44100.0, secs);
    std::ofstream wav("/data/local/tmp/movie.wav", std::ios::binary);
    const std::uint32_t bytes = static_cast<std::uint32_t>(pcm.size() * 2), rate = 44100, brate = rate * 4;
    const std::uint32_t riff = 36 + bytes, fmt_size = 16; const std::uint16_t pcm_fmt = 1, ch = 2, balign = 4, bits = 16;
    wav.write("RIFF", 4); wav.write((const char *)&riff, 4); wav.write("WAVEfmt ", 8); wav.write((const char *)&fmt_size, 4);
    wav.write((const char *)&pcm_fmt, 2); wav.write((const char *)&ch, 2); wav.write((const char *)&rate, 4);
    wav.write((const char *)&brate, 4); wav.write((const char *)&balign, 2); wav.write((const char *)&bits, 2);
    wav.write("data", 4); wav.write((const char *)&bytes, 4); wav.write((const char *)pcm.data(), bytes);
    return 0;
}
