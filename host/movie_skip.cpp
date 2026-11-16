// sceMpeg without a decoder (2026-11-16): movies are skipped. Setup calls succeed, size queries answer with
// plausible values, and the calls that fetch or decode access units report "no data" (0x80618001), which the game
// treats as the end of the movie. The approach is PortableKit's register_movie_skip (portable-ops-native,
// portablekit/host/hle/hle_mpeg.cpp, MIT); this file is written for this runtime and covers every sceMpeg import
// BBS has, since an unregistered call stops this runtime. Real playback needs FFmpeg (h264 + atrac3p) later.
#include "movie_skip.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace bbs {
namespace {
constexpr std::uint32_t kNoData = 0x80618001u;

void store(psprecomp::Runtime &rt, std::uint32_t address, std::uint32_t value) {
    if (address != 0u && rt.memory().contains(address, 4u)) rt.memory().store32(address, value);
}
} // namespace

void register_movie_skip(psprecomp::Runtime &runtime) {
    auto ok = [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); };
    auto no_data = [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        static bool logged = false;
        if (!logged) { logged = true; std::cerr << "[mpeg] no decoder: movie skipped\n"; }
        ctx.set_gpr(2u, kNoData);
    };
    for (std::uint32_t nid : {0x682A619Bu /*Init*/, 0x874624D6u /*Finish*/, 0x606A4649u /*Delete*/,
                              0x591A4AA2u /*UnRegistStream*/, 0x707B7629u /*FlushAllStream*/, 0x167AFD9Eu /*InitAu*/,
                              0x67179B1Bu /*AvcInitYCbCr*/, 0x13407F13u /*RingbufferDestruct*/,
                              0xCEB870B1u /*FreeAvcEsBuf*/, 0xA11C7026u /*AvcDecodeMode*/,
                              0xD8C5F121u /*Create*/})
        runtime.register_hle("sceMpeg", nid, ok);
    for (std::uint32_t nid : {0xFE246728u /*GetAvcAu*/, 0xE1CE83A7u /*GetAtracAu*/, 0x800C44DFu /*AtracDecode*/,
                              0xF0EB1125u /*AvcDecodeYCbCr*/, 0xF2930C9Cu /*AvcDecodeStopYCbCr*/,
                              0x0F6C18D7u /*AvcDecodeDetail*/, 0x31BD0272u /*AvcCsc*/})
        runtime.register_hle("sceMpeg", nid, no_data);
    runtime.register_hle("sceMpeg", 0x42560F23u,  // RegistStream -> a stream handle
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0x00001000u + ctx.gpr[5u]); });
    runtime.register_hle("sceMpeg", 0xA780CF7Eu,  // MallocAvcEsBuf -> a buffer handle
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 1u); });
    runtime.register_hle("sceMpeg", 0xC132E22Fu,  // QueryMemSize(mode)
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0x10000u); });
    runtime.register_hle("sceMpeg", 0xD7A29F46u,  // RingbufferQueryMemSize(packets)
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, ctx.gpr[4u] * 2048u); });
    runtime.register_hle("sceMpeg", 0x211A057Cu,  // AvcQueryYCbCrSize(mpeg, mode, width, height, out)
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            store(rt, ctx.gpr[8u], (ctx.gpr[6u] * ctx.gpr[7u] * 3u) / 2u + 128u);
            ctx.set_gpr(2u, 0u);
        });
    runtime.register_hle("sceMpeg", 0xF8DCB679u,  // QueryAtracEsSize(mpeg, esSize*, outSize*)
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            store(rt, ctx.gpr[5u], 2112u);
            store(rt, ctx.gpr[6u], 8192u);
            ctx.set_gpr(2u, 0u);
        });
    runtime.register_hle("sceMpeg", 0x21FF80E4u,  // QueryStreamOffset(mpeg, header, offset*)
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            store(rt, ctx.gpr[6u], 2048u);
            ctx.set_gpr(2u, 0u);
        });
    runtime.register_hle("sceMpeg", 0x611E9E11u,  // QueryStreamSize(header, size*)
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            store(rt, ctx.gpr[5u], 2048u);
            ctx.set_gpr(2u, 0u);
        });
    // Ring buffer, laid out as the PSP's SceMpegRingBuffer (PPSSPP's layout): 0 packets, 4 packetsRead,
    // 8 packetsWritten, 12 packetsAvail, 16 packetSize, 20 data, 24 callback, 28 callbackArg.
    // RingbufferConstruct(ring, packets, data, size, callback, callbackArg) fills it in.
    runtime.register_hle("sceMpeg", 0x37295ED8u,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t ring = ctx.gpr[4u];
            if (ring != 0u && rt.memory().contains(ring, 32u)) {
                rt.memory().store32(ring + 0u, ctx.gpr[5u]);
                rt.memory().store32(ring + 4u, 0u);
                rt.memory().store32(ring + 8u, 0u);
                rt.memory().store32(ring + 12u, ctx.gpr[5u]);
                rt.memory().store32(ring + 16u, 2048u);
                rt.memory().store32(ring + 20u, ctx.gpr[6u]);
                rt.memory().store32(ring + 24u, ctx.gpr[8u]);
                rt.memory().store32(ring + 28u, ctx.gpr[9u]);
            }
            ctx.set_gpr(2u, 0u);
        });
    // RingbufferPut(ring, packets, available) -> packets read. As on a PSP it calls the game's read callback
    // (data, packets, callbackArg), whose return (0 at the end of the file) becomes Put's own: a tail call into
    // the callback with the caller's return address. The game's feeder sees end of file through it and ends the
    // movie; the packets are never decoded.
    runtime.register_hle("sceMpeg", 0xB240A59Eu,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t ring = ctx.gpr[4u];
            const std::uint32_t packets = std::min(ctx.gpr[5u], ctx.gpr[6u]);
            if (packets == 0u || ring == 0u || !rt.memory().contains(ring, 32u)) { ctx.set_gpr(2u, 0u); return; }
            const std::uint32_t callback = rt.memory().load32(ring + 24u);
            if (callback == 0u) { ctx.set_gpr(2u, packets); return; }
            ctx.set_gpr(4u, rt.memory().load32(ring + 20u));
            ctx.set_gpr(5u, packets);
            ctx.set_gpr(6u, rt.memory().load32(ring + 28u));
            ctx.pc = callback;  // ra still holds Put's caller
        });
    runtime.register_hle("sceMpeg", 0xB5F6DC87u,  // RingbufferAvailableSize(ring) -> free packets (all)
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t ring = ctx.gpr[4u];
            ctx.set_gpr(2u, ring != 0u && rt.memory().contains(ring, 4u) ? rt.memory().load32(ring) : 0u);
        });
}
} // namespace bbs
