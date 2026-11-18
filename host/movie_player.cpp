// sceMpeg with real playback (2026-11-18). The game feeds PSMF packs through its ring buffer; RingbufferPut calls
// the game's read callback and, when it returns (trampoline at kPutReturn), copies the new packs into a host
// demuxer (movie_demux.hpp). GetAvcAu / GetAtracAu hand out access units with their timestamps; AvcDecodeYCbCr
// decodes H.264 with the platform decoder (movie_video_decoder.cpp, AMediaCodec on Android) and keeps the picture
// on the host, keyed by the game's YCbCr buffer address (the game never reads that buffer itself, it passes it to
// AvcCsc). AvcCsc converts it to the game's pixel format into the destination. AtracDecode decodes ATRAC3plus
// with at3_standalone (LGPL, third_party) into the 2048-sample stereo buffer the game then plays via sceAudio.
// Behaviour of the calls follows PPSSPP's sceMpeg.cpp (GPL; re-implemented from its behaviour, credited in
// psp-native-runtime/CREDITS.md) and PortableKit's hle_mpeg.cpp (MIT).
// PSPRECOMP_MOVIE_SKIP=1 registers the old skip path (movie_skip.cpp) instead; if the decoder can't be opened, the
// calls behave as the skip path did (no data), so the game ends the movie.
// PSPRECOMP_MOVIE_TRACE=1 logs every call with its arguments.
#include "movie_player.hpp"
#include "movie_demux.hpp"
#include "movie_skip.hpp"
#include "movie_video_decoder.hpp"
#include "interrupts.hpp"
#include "psprecomp/runtime.hpp"
#include "at3_decoders.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>

namespace bbs {
extern int g_thread_diag_burst;
namespace {
using movie::EsUnit;
using movie::PsmfDemux;
using movie::VideoDecoder;
using movie::VideoFrame;

constexpr std::uint32_t kNoData = 0x80618001u;
constexpr std::uint32_t kPutReturn = 0x0000000Cu;  // guest address of the RingbufferPut callback trampoline
constexpr std::uint32_t kPacket = 2048u;

bool trace_on() {
    static const bool on = [] { const char *v = std::getenv("PSPRECOMP_MOVIE_TRACE"); return v != nullptr && *v != '0'; }();
    return on;
}
int trace_level() {
    static const int level = [] { const char *v = std::getenv("PSPRECOMP_MOVIE_TRACE"); return v != nullptr ? std::atoi(v) : 0; }();
    return level;
}
#define MTRACE2(x) do { if (trace_level() >= 2) std::cerr << "[mpeg2] t=" << psphost::virtual_time_us() / 1000u << " uid=" \
    << psprecomp::runtime_thread_uid() << " " << x << "\n"; } while (0)
#define MTRACE(x) do { if (trace_on()) std::cerr << "[mpeg] " << x << "\n"; } while (0)

std::string hex(std::uint32_t v) {
    char b[16]; std::snprintf(b, sizeof b, "%08X", v); return b;
}

struct Movie {
    std::uint32_t ring{};
    PsmfDemux demux;
    VideoDecoder video;
    bool video_tried{};
    bool video_failed{};
    int pixel_mode{3};
    EsUnit pending_video;
    bool has_pending_video{};
    EsUnit pending_audio;
    bool has_pending_audio{};
    std::unordered_map<std::uint32_t, VideoFrame> pictures;  // by YCbCr buffer address
    std::vector<std::uint32_t> ycbcr_buffers;                // from AvcInitYCbCr
    VideoFrame last;
    std::uint32_t last_buffer{};
    std::uint32_t frames_shown{};
    std::uint32_t video_aus{};
    std::uint32_t audio_frames{};
    ATRAC3PContext *at3p{};
    int channels{2};
    int block_align{};
    std::vector<float> planes[2]{std::vector<float>(4096), std::vector<float>(4096)};

    ~Movie() { if (at3p != nullptr) atrac3p_free(at3p); }
};
std::unique_ptr<Movie> g_movie;

Movie &current_movie() {
    if (!g_movie) g_movie = std::make_unique<Movie>();
    return *g_movie;
}

void store(psprecomp::Runtime &rt, std::uint32_t address, std::uint32_t value) {
    if (address != 0u && rt.memory().contains(address, 4u)) rt.memory().store32(address, value);
}
std::uint32_t load(psprecomp::Runtime &rt, std::uint32_t address) {
    return address != 0u && rt.memory().contains(address, 4u) ? rt.memory().load32(address) : 0u;
}
std::uint32_t load_be(psprecomp::Runtime &rt, std::uint32_t address) {
    const std::uint32_t v = load(rt, address);
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

// SceMpegRingBuffer: 0 packets, 4 packetsRead, 8 packetsWritten, 12 packetsAvail (filled), 16 packetSize, 20 data,
// 24 callback, 28 callbackArg, 32 dataUpperBound.
void update_ring_fill(psprecomp::Runtime &rt, Movie &m) {
    const std::uint32_t ring = m.ring;
    if (ring == 0u || !rt.memory().contains(ring, 36u)) return;
    const std::uint32_t packets = rt.memory().load32(ring);
    const std::uint32_t old_fill = rt.memory().load32(ring + 12u);
    const std::uint32_t fill = std::min<std::uint32_t>(packets,
        static_cast<std::uint32_t>((m.demux.pending_bytes() + kPacket - 1u) / kPacket));
    if (fill < old_fill) rt.memory().store32(ring + 4u, rt.memory().load32(ring + 4u) + (old_fill - fill));
    rt.memory().store32(ring + 12u, fill);
}

void write_au(psprecomp::Runtime &rt, std::uint32_t au, const EsUnit &unit) {
    // SceMpegAu: 0 pts hi, 4 pts lo, 8 dts hi, 12 dts lo, 16 esBuffer, 20 auSize
    const auto pts = static_cast<std::uint64_t>(unit.pts);
    store(rt, au + 0u, static_cast<std::uint32_t>(pts >> 32));
    store(rt, au + 4u, static_cast<std::uint32_t>(pts));
    store(rt, au + 8u, static_cast<std::uint32_t>(pts >> 32));
    store(rt, au + 12u, static_cast<std::uint32_t>(pts));
    store(rt, au + 20u, static_cast<std::uint32_t>(unit.data.size()));
}

bool ensure_video(Movie &m) {
    if (m.video.is_open()) return true;
    if (m.video_tried) return false;
    m.video_tried = true;
    m.video_failed = !m.video.open(480, 272);
    if (m.video_failed) std::cerr << "[mpeg] no video decoder: movie skipped\n";
    return !m.video_failed;
}

// Picks up decoded pictures; the newest one becomes the picture of `buffer`.
bool collect_picture(Movie &m, std::uint32_t buffer, int timeout_us) {
    m.video.poll(timeout_us);
    bool got = false;
    VideoFrame frame;
    while (m.video.take(frame)) { m.last = std::move(frame); got = true; }
    if (got) {
        m.pictures[buffer] = m.last;
        m.last_buffer = buffer;
        ++m.frames_shown;
    }
    return got;
}

// The YCbCr buffer argument may be the buffer or a pointer to it: use whichever is a buffer from AvcInitYCbCr.
std::uint32_t picture_key(psprecomp::Runtime &rt, const Movie &m, std::uint32_t address) {
    const auto known = [&m](std::uint32_t a) {
        return std::find(m.ycbcr_buffers.begin(), m.ycbcr_buffers.end(), a) != m.ycbcr_buffers.end();
    };
    if (known(address)) return address;
    const std::uint32_t pointed = load(rt, address);
    return known(pointed) ? pointed : address;
}

struct PutState {
    std::uint32_t ring{};
    std::uint32_t remaining{};
    std::uint32_t total{};
    std::uint32_t chunk{};
    std::uint32_t chunk_address{};
    std::uint32_t caller_ra{};
};
std::unordered_map<std::int32_t, PutState> g_puts;  // by PSP thread

// Starts the read callback for the next contiguous chunk of the ring; false when nothing is left to read.
bool start_put_chunk(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx, PutState &s) {
    const std::uint32_t ring = s.ring;
    const std::uint32_t packets = rt.memory().load32(ring);
    const std::uint32_t written = rt.memory().load32(ring + 8u);
    const std::uint32_t at = packets == 0u ? 0u : written % packets;
    s.chunk = std::min(s.remaining, packets - at);
    if (s.chunk == 0u) return false;
    s.chunk_address = rt.memory().load32(ring + 20u) + at * kPacket;
    ctx.set_gpr(4u, s.chunk_address);
    ctx.set_gpr(5u, s.chunk);
    ctx.set_gpr(6u, rt.memory().load32(ring + 28u));
    ctx.set_gpr(31u, kPutReturn);
    ctx.pc = rt.memory().load32(ring + 24u);
    return true;
}

void put_return(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    const auto found = g_puts.find(psprecomp::runtime_thread_uid());
    if (found == g_puts.end()) { rt.stop("sceMpegRingbufferPut callback returned without a pending Put"); return; }
    PutState &s = found->second;
    const auto got = static_cast<std::int32_t>(ctx.gpr[2u]);
    Movie &m = current_movie();
    bool more = false;
    if (got > 0) {
        const std::uint32_t n = std::min<std::uint32_t>(static_cast<std::uint32_t>(got), s.chunk);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n) * kPacket);
        if (rt.memory().contains(s.chunk_address, static_cast<std::uint32_t>(bytes.size()))) {
            rt.memory().copy_out(s.chunk_address, std::span<std::uint8_t>(bytes));
            m.demux.push(bytes.data(), bytes.size());
        }
        rt.memory().store32(s.ring + 8u, rt.memory().load32(s.ring + 8u) + n);
        s.total += n;
        s.remaining -= n;
        update_ring_fill(rt, m);
        if (n == s.chunk && s.remaining > 0u) more = start_put_chunk(rt, ctx, s);
    } else if (got == 0) {
        if (!m.demux.eof()) MTRACE("end of stream after " << m.demux.packs() << " packs");
        m.demux.set_eof();
    }
    if (more) return;
    const std::uint32_t result = got < 0 && s.total == 0u ? static_cast<std::uint32_t>(got) : s.total;
    ctx.pc = s.caller_ra;
    ctx.set_gpr(2u, result);
    g_puts.erase(found);
}

void reset_movie() { g_movie.reset(); g_puts.clear(); }

} // namespace

void movie_after_state_load() {
    // Decoder state isn't saved: a movie playing across a load ends (its next AU fetch reports no data).
    if (g_movie) { g_movie->video.close(); g_movie->video_tried = true; g_movie->video_failed = true; g_movie->demux.set_eof(); }
    g_puts.clear();
}

void register_movie_player(psprecomp::Runtime &runtime) {
    if (const char *skip = std::getenv("PSPRECOMP_MOVIE_SKIP"); skip != nullptr && *skip != '0') {
        register_movie_skip(runtime);
        return;
    }
    runtime.register_function(kPutReturn, &put_return, "psp_mpeg_ringbuffer_put_return");
    using R = psprecomp::Runtime;
    using C = psprecomp::AllegrexContext;

    runtime.register_hle("sceMpeg", 0x682A619Bu, [](R &, C &ctx) { MTRACE("Init"); ctx.set_gpr(2u, 0u); });
    runtime.register_hle("sceMpeg", 0x874624D6u, [](R &, C &ctx) { MTRACE("Finish"); reset_movie(); ctx.set_gpr(2u, 0u); });
    runtime.register_hle("sceMpeg", 0xC132E22Fu, [](R &, C &ctx) {  // QueryMemSize(mode)
        ctx.set_gpr(2u, 0x10000u);
    });
    // Create(mpeg*, data, size, ring, frameWidth, mode, ddrTop)
    runtime.register_hle("sceMpeg", 0xD8C5F121u, [](R &rt, C &ctx) {
        MTRACE("Create mpeg=" << hex(ctx.gpr[4u]) << " data=" << hex(ctx.gpr[5u]) << " size=" << hex(ctx.gpr[6u])
               << " ring=" << hex(ctx.gpr[7u]) << " fw=" << ctx.gpr[8u] << " mode=" << ctx.gpr[9u]);
        reset_movie();
        Movie &m = current_movie();
        m.ring = ctx.gpr[7u];
        store(rt, ctx.gpr[4u], ctx.gpr[5u] + 0x30u);
        if (m.ring != 0u) store(rt, m.ring + 40u, ctx.gpr[5u] + 0x30u);
        ctx.set_gpr(2u, 0u);
    });
    runtime.register_hle("sceMpeg", 0x606A4649u, [](R &, C &ctx) {  // Delete
        MTRACE("Delete shown=" << (g_movie ? g_movie->frames_shown : 0u));
        if (g_movie) std::cerr << "[mpeg] movie done: " << g_movie->frames_shown << " pictures, " << g_movie->video_aus
                               << " video AUs, " << g_movie->audio_frames << " audio frames\n";
        reset_movie();
        ctx.set_gpr(2u, 0u);
    });
    runtime.register_hle("sceMpeg", 0x42560F23u, [](R &, C &ctx) {  // RegistStream(mpeg, type, num) -> handle
        MTRACE("RegistStream type=" << ctx.gpr[5u] << " num=" << ctx.gpr[6u]);
        ctx.set_gpr(2u, 0x00001000u + ctx.gpr[5u]);
    });
    runtime.register_hle("sceMpeg", 0x591A4AA2u, [](R &, C &ctx) { ctx.set_gpr(2u, 0u); });  // UnRegistStream
    runtime.register_hle("sceMpeg", 0x707B7629u, [](R &, C &ctx) {  // FlushAllStream
        MTRACE("FlushAllStream");
        if (g_movie) {
            Movie &m = *g_movie;
            m.demux.reset();
            m.has_pending_video = m.has_pending_audio = false;
            if (m.video.is_open()) { m.video.close(); m.video_tried = false; }
        }
        ctx.set_gpr(2u, 0u);
    });
    runtime.register_hle("sceMpeg", 0x167AFD9Eu, [](R &rt, C &ctx) {  // InitAu(mpeg, esBuffer, au)
        const std::uint32_t au = ctx.gpr[6u];
        store(rt, au + 0u, 0xFFFFFFFFu); store(rt, au + 4u, 0xFFFFFFFFu);
        store(rt, au + 8u, 0xFFFFFFFFu); store(rt, au + 12u, 0xFFFFFFFFu);
        store(rt, au + 16u, ctx.gpr[5u]); store(rt, au + 20u, 0u);
        ctx.set_gpr(2u, 0u);
    });
    runtime.register_hle("sceMpeg", 0xA780CF7Eu, [](R &, C &ctx) { ctx.set_gpr(2u, 1u); });   // MallocAvcEsBuf
    runtime.register_hle("sceMpeg", 0xCEB870B1u, [](R &, C &ctx) { ctx.set_gpr(2u, 0u); });   // FreeAvcEsBuf
    runtime.register_hle("sceMpeg", 0xA11C7026u, [](R &rt, C &ctx) {  // AvcDecodeMode(mpeg, mode*): {unk, pixelMode}
        const std::uint32_t mode = ctx.gpr[5u];
        if (mode != 0u) {
            const auto pixel = static_cast<std::int32_t>(load(rt, mode + 4u));
            if (pixel >= 0 && pixel <= 3) current_movie().pixel_mode = pixel;
            MTRACE("AvcDecodeMode " << static_cast<std::int32_t>(load(rt, mode)) << " pixel=" << pixel);
        }
        ctx.set_gpr(2u, 0u);
    });
    runtime.register_hle("sceMpeg", 0x67179B1Bu, [](R &, C &ctx) {  // AvcInitYCbCr(mpeg, mode, w, h, buffer)
        current_movie().ycbcr_buffers.push_back(ctx.gpr[8u]);
        MTRACE("AvcInitYCbCr mode=" << ctx.gpr[5u] << " " << ctx.gpr[6u] << "x" << ctx.gpr[7u] << " buf=" << hex(ctx.gpr[8u]));
        ctx.set_gpr(2u, 0u);
    });
    runtime.register_hle("sceMpeg", 0x211A057Cu, [](R &rt, C &ctx) {  // AvcQueryYCbCrSize(mpeg, mode, w, h, out*)
        store(rt, ctx.gpr[8u], (ctx.gpr[6u] / 2u) * (ctx.gpr[7u] / 2u) * 6u + 128u);
        ctx.set_gpr(2u, 0u);
    });
    runtime.register_hle("sceMpeg", 0xF8DCB679u, [](R &rt, C &ctx) {  // QueryAtracEsSize(mpeg, esSize*, outSize*)
        store(rt, ctx.gpr[5u], 2112u);
        store(rt, ctx.gpr[6u], 8192u);
        ctx.set_gpr(2u, 0u);
    });
    runtime.register_hle("sceMpeg", 0x21FF80E4u, [](R &rt, C &ctx) {  // QueryStreamOffset(mpeg, header, offset*)
        const std::uint32_t header = ctx.gpr[5u];
        if (load(rt, header) != 0x464D5350u) { ctx.set_gpr(2u, 0x806101FEu); return; }  // "PSMF"
        store(rt, ctx.gpr[6u], load_be(rt, header + 8u));
        MTRACE("QueryStreamOffset " << load_be(rt, header + 8u));
        ctx.set_gpr(2u, 0u);
    });
    runtime.register_hle("sceMpeg", 0x611E9E11u, [](R &rt, C &ctx) {  // QueryStreamSize(header, size*)
        const std::uint32_t size = load_be(rt, ctx.gpr[4u] + 12u);
        store(rt, ctx.gpr[5u], size);
        MTRACE("QueryStreamSize " << size);
        ctx.set_gpr(2u, 0u);
    });
    runtime.register_hle("sceMpeg", 0xD7A29F46u, [](R &, C &ctx) {  // RingbufferQueryMemSize(packets)
        ctx.set_gpr(2u, ctx.gpr[4u] * (kPacket + 104u));
    });
    runtime.register_hle("sceMpeg", 0x37295ED8u, [](R &rt, C &ctx) {  // RingbufferConstruct(ring, packets, data, size, cb, arg)
        const std::uint32_t ring = ctx.gpr[4u];
        MTRACE("RingbufferConstruct ring=" << hex(ring) << " packets=" << ctx.gpr[5u] << " data=" << hex(ctx.gpr[6u])
               << " size=" << hex(ctx.gpr[7u]) << " cb=" << hex(ctx.gpr[8u]));
        if (ring != 0u && rt.memory().contains(ring, 44u)) {
            rt.memory().store32(ring + 0u, ctx.gpr[5u]);
            rt.memory().store32(ring + 4u, 0u);
            rt.memory().store32(ring + 8u, 0u);
            rt.memory().store32(ring + 12u, 0u);
            rt.memory().store32(ring + 16u, kPacket);
            rt.memory().store32(ring + 20u, ctx.gpr[6u]);
            rt.memory().store32(ring + 24u, ctx.gpr[8u]);
            rt.memory().store32(ring + 28u, ctx.gpr[9u]);
            rt.memory().store32(ring + 32u, ctx.gpr[6u] + ctx.gpr[5u] * kPacket);
            rt.memory().store32(ring + 36u, 0u);
            rt.memory().store32(ring + 40u, 0u);
        }
        ctx.set_gpr(2u, 0u);
    });
    runtime.register_hle("sceMpeg", 0x13407F13u, [](R &, C &ctx) { ctx.set_gpr(2u, 0u); });  // RingbufferDestruct
    runtime.register_hle("sceMpeg", 0xB5F6DC87u, [](R &rt, C &ctx) {  // RingbufferAvailableSize(ring) -> free packets
        const std::uint32_t ring = ctx.gpr[4u];
        if (ring == 0u || !rt.memory().contains(ring, 16u)) { ctx.set_gpr(2u, 0u); return; }
        if (g_movie && g_movie->ring == ring) update_ring_fill(rt, *g_movie);
        const std::uint32_t packets = rt.memory().load32(ring), fill = rt.memory().load32(ring + 12u);
        ctx.set_gpr(2u, packets > fill ? packets - fill : 0u);
    });
    // RingbufferPut(ring, packets, available) -> packets added. Calls the game's callback(data, count, arg) once per
    // contiguous chunk; put_return copies what it read into the demuxer.
    runtime.register_hle("sceMpeg", 0xB240A59Eu, [](R &rt, C &ctx) {
        const std::uint32_t ring = ctx.gpr[4u];
        const std::uint32_t packets = std::min(ctx.gpr[5u], ctx.gpr[6u]);
        MTRACE2("RingbufferPut " << ctx.gpr[5u] << "/" << ctx.gpr[6u] << " pending=" << (g_movie ? g_movie->demux.pending_bytes() : 0u));
        if (packets == 0u || ring == 0u || !rt.memory().contains(ring, 36u) || rt.memory().load32(ring + 24u) == 0u) {
            ctx.set_gpr(2u, 0u);
            return;
        }
        Movie &m = current_movie();
        if (m.ring == 0u) m.ring = ring;
        PutState s{ring, packets, 0u, 0u, 0u, ctx.gpr[31u]};
        if (!start_put_chunk(rt, ctx, s)) { ctx.set_gpr(2u, 0u); return; }
        g_puts[psprecomp::runtime_thread_uid()] = s;
    });

    // GetAvcAu(mpeg, stream, au, attr*)
    runtime.register_hle("sceMpeg", 0xFE246728u, [](R &rt, C &ctx) {
        Movie &m = current_movie();
        if (!ensure_video(m)) { ctx.set_gpr(2u, kNoData); return; }
        if (!m.has_pending_video) m.has_pending_video = m.demux.next_video_au(m.pending_video);
        update_ring_fill(rt, m);
        if (!m.has_pending_video) {
            MTRACE("GetAvcAu: no data (eof=" << m.demux.eof() << ")");
            ctx.set_gpr(2u, kNoData);
            return;
        }
        write_au(rt, ctx.gpr[6u], m.pending_video);
        store(rt, ctx.gpr[7u], 1u);
        MTRACE2("GetAvcAu pts=" << m.pending_video.pts << " au=" << hex(ctx.gpr[6u]));
        ++m.video_aus;
        if (m.video_aus == 50u && trace_level() >= 3) g_thread_diag_burst = 600;
        if (m.video_aus <= 3u || trace_on() && m.video_aus % 60u == 0u)
            MTRACE("GetAvcAu #" << m.video_aus << " pts=" << m.pending_video.pts << " size=" << m.pending_video.data.size());
        ctx.set_gpr(2u, 0u);
    });
    // AvcDecodeYCbCr(mpeg, au, buffer, init*): decodes the AU from GetAvcAu; *init = 1 when a picture came out.
    runtime.register_hle("sceMpeg", 0xF0EB1125u, [](R &rt, C &ctx) {
        Movie &m = current_movie();
        const std::uint32_t buffer = picture_key(rt, m, ctx.gpr[6u]);
        bool got = false;
        if (m.video.is_open() && m.has_pending_video) {
            m.video.queue(m.pending_video.data.data(), m.pending_video.data.size(), m.pending_video.pts * 100 / 9);
            m.has_pending_video = false;
            // The decoder holds a few pictures back at the start; afterwards one comes out per AU.
            const bool primed = m.video.queued() > m.video.decoded() + 2u;
            got = collect_picture(m, buffer, primed ? 30000 : 4000);
        }
        store(rt, ctx.gpr[7u], got ? 1u : 0u);
        MTRACE2("AvcDecodeYCbCr arg=" << hex(ctx.gpr[6u]) << " key=" << hex(buffer) << " got=" << got
                << " q=" << m.video.queued() << " d=" << m.video.decoded());
        if (m.frames_shown <= 3u && got) MTRACE("AvcDecodeYCbCr buf=" << hex(buffer) << " picture #" << m.frames_shown);
        ctx.set_gpr(2u, 0u);
    });
    // AvcDecodeStopYCbCr(mpeg, buffer, status*): flushes the pictures still in the decoder, one per call.
    runtime.register_hle("sceMpeg", 0xF2930C9Cu, [](R &rt, C &ctx) {
        Movie &m = current_movie();
        bool got = false;
        if (m.video.is_open()) {
            m.video.signal_end();
            got = collect_picture(m, picture_key(rt, m, ctx.gpr[5u]), 20000);
        }
        MTRACE("AvcDecodeStopYCbCr got=" << got);
        store(rt, ctx.gpr[6u], got ? 1u : 0u);
        ctx.set_gpr(2u, 0u);
    });
    // AvcDecodeDetail(mpeg, detail*)
    runtime.register_hle("sceMpeg", 0x0F6C18D7u, [](R &rt, C &ctx) {
        Movie &m = current_movie();
        const std::uint32_t d = ctx.gpr[5u];
        store(rt, d + 0u, 1u);
        store(rt, d + 4u, m.frames_shown);
        store(rt, d + 8u, m.last.width > 0 ? m.last.width : 480u);
        store(rt, d + 12u, m.last.height > 0 ? m.last.height : 272u);
        for (std::uint32_t o = 16u; o < 32u; o += 4u) store(rt, d + o, 0u);
        store(rt, d + 32u, m.frames_shown > 0u ? 1u : 0u);
        ctx.set_gpr(2u, 0u);
    });
    // AvcCsc(mpeg, source, range*, frameWidth, dest): YCbCr picture -> pixels at dest (stride frameWidth).
    runtime.register_hle("sceMpeg", 0x31BD0272u, [](R &rt, C &ctx) {
        Movie &m = current_movie();
        const std::uint32_t source = ctx.gpr[5u], range = ctx.gpr[6u], dest = ctx.gpr[8u];
        const std::uint32_t frame_width = ctx.gpr[7u];
        auto rx = static_cast<std::int32_t>(load(rt, range)), ry = static_cast<std::int32_t>(load(rt, range + 4u));
        auto rw = static_cast<std::int32_t>(load(rt, range + 8u)), rh = static_cast<std::int32_t>(load(rt, range + 12u));
        static int logged = 0;
        if (logged < 2) {
            ++logged;
            std::cerr << "[mpeg] AvcCsc src=" << hex(source) << " range=" << rx << "," << ry << "," << rw << "," << rh
                      << " fw=" << frame_width << " dest=" << hex(dest) << " pixel=" << m.pixel_mode << "\n";
        }
        if (rw > 0 && rh > 0 && rw <= 64 && rh <= 64) { rx *= 16; ry *= 16; rw *= 16; rh *= 16; }  // macroblock units
        auto found = m.pictures.find(picture_key(rt, m, source));
        MTRACE2("AvcCsc src=" << hex(source) << " key=" << hex(picture_key(rt, m, source)) << " dest=" << hex(dest)
                << " found=" << (found != m.pictures.end()));
        const VideoFrame *f = found != m.pictures.end() ? &found->second : (m.last.width > 0 ? &m.last : nullptr);
        if (f == nullptr || frame_width == 0u) { ctx.set_gpr(2u, 0u); return; }
        const int bpp = m.pixel_mode == 3 ? 4 : 2;
        rw = std::min(rw, f->width - rx);
        rh = std::min(rh, f->height - ry);
        if (rx < 0 || ry < 0 || rw <= 0 || rh <= 0) { ctx.set_gpr(2u, 0u); return; }
        const std::uint32_t bytes = (static_cast<std::uint32_t>(rh - 1) * frame_width + static_cast<std::uint32_t>(rw)) * bpp;
        if (!rt.memory().contains(dest, bytes)) { ctx.set_gpr(2u, 0u); return; }
        std::vector<std::uint8_t> row(static_cast<std::size_t>(rw) * bpp);
        for (int y = 0; y < rh; ++y) {
            const int sy = ry + y;
            const std::uint8_t *yp = &f->y[static_cast<std::size_t>(sy) * f->width];
            const std::uint8_t *up = &f->u[static_cast<std::size_t>(sy / 2) * (f->width / 2)];
            const std::uint8_t *vp = &f->v[static_cast<std::size_t>(sy / 2) * (f->width / 2)];
            for (int x = 0; x < rw; ++x) {
                const int sx = rx + x;
                const int Y = (yp[sx] - 16) * 298, U = up[sx / 2] - 128, V = vp[sx / 2] - 128;
                const int r = std::clamp((Y + 409 * V + 128) >> 8, 0, 255);
                const int g = std::clamp((Y - 100 * U - 208 * V + 128) >> 8, 0, 255);
                const int b = std::clamp((Y + 516 * U + 128) >> 8, 0, 255);
                std::uint8_t *o = &row[static_cast<std::size_t>(x) * bpp];
                std::uint16_t p = 0;
                switch (m.pixel_mode) {
                case 3: o[0] = static_cast<std::uint8_t>(r); o[1] = static_cast<std::uint8_t>(g);
                        o[2] = static_cast<std::uint8_t>(b); o[3] = 0xFFu; continue;
                case 0: p = static_cast<std::uint16_t>((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)); break;
                case 1: p = static_cast<std::uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000u); break;
                default: p = static_cast<std::uint16_t>((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | 0xF000u); break;
                }
                o[0] = static_cast<std::uint8_t>(p); o[1] = static_cast<std::uint8_t>(p >> 8);
            }
            rt.memory().copy_in(dest + static_cast<std::uint32_t>(y) * frame_width * bpp,
                                std::span<const std::uint8_t>(row));
        }
        ctx.set_gpr(2u, 0u);
    });

    // GetAtracAu(mpeg, stream, au, attr*)
    runtime.register_hle("sceMpeg", 0xE1CE83A7u, [](R &rt, C &ctx) {
        Movie &m = current_movie();
        if (m.video_failed) { ctx.set_gpr(2u, kNoData); return; }
        if (!m.has_pending_audio) m.has_pending_audio = m.demux.next_audio_frame(m.pending_audio);
        update_ring_fill(rt, m);
        if (!m.has_pending_audio) { ctx.set_gpr(2u, kNoData); return; }
        write_au(rt, ctx.gpr[6u], m.pending_audio);
        MTRACE2("GetAtracAu pts=" << m.pending_audio.pts);
        store(rt, ctx.gpr[7u], 0u);
        ++m.audio_frames;
        if (m.audio_frames <= 2u) MTRACE("GetAtracAu #" << m.audio_frames << " pts=" << m.pending_audio.pts);
        ctx.set_gpr(2u, 0u);
    });
    // AtracDecode(mpeg, au, buffer, init): one ATRAC3plus frame -> 2048 stereo s16 samples at buffer.
    runtime.register_hle("sceMpeg", 0x800C44DFu, [](R &rt, C &ctx) {
        Movie &m = current_movie();
        const std::uint32_t buffer = ctx.gpr[6u];
        MTRACE2("AtracDecode buf=" << hex(buffer) << " pending=" << m.has_pending_audio << " init=" << ctx.gpr[7u]);
        std::vector<std::int16_t> pcm(2048u * 2u, 0);
        if (m.has_pending_audio && m.pending_audio.data.size() > 8u) {
            const auto &d = m.pending_audio.data;
            if (m.at3p == nullptr) {
                m.channels = ((d[2] >> 2) & 7u) == 1u ? 1 : 2;
                m.block_align = static_cast<int>(d.size()) - 8;
                m.at3p = atrac3p_alloc(m.channels, &m.block_align);
                MTRACE("atrac3plus ch=" << m.channels << " align=" << m.block_align);
            }
            if (m.at3p != nullptr && static_cast<int>(d.size()) - 8 == m.block_align) {
                float *out[2]{m.planes[0].data(), m.planes[1].data()};
                int samples = 0;
                if (atrac3p_decode_frame(m.at3p, out, &samples, d.data() + 8, m.block_align) >= 0) {
                    samples = std::clamp(samples, 0, 2048);
                    const float *l = m.planes[0].data(), *r = m.channels > 1 ? m.planes[1].data() : l;
                    for (int i = 0; i < samples; ++i) {
                        pcm[2u * i] = static_cast<std::int16_t>(std::clamp(l[i], -1.0f, 1.0f) * 32767.0f);
                        pcm[2u * i + 1u] = static_cast<std::int16_t>(std::clamp(r[i], -1.0f, 1.0f) * 32767.0f);
                    }
                }
            }
            m.has_pending_audio = false;
        }
        if (buffer != 0u && rt.memory().contains(buffer, 8192u))
            rt.memory().copy_in(buffer, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(pcm.data()), 8192u));
        ctx.set_gpr(2u, 0u);
    });
}
} // namespace bbs
