// sceAtrac3plus for BBS (2026-11-17): music and streamed sounds.
//
// BBS hands every ATRAC file over whole, in guest memory (it imports neither
// AddStreamData nor GetStreamDataInfo), so each DecodeData decodes the next
// frame straight from the guest buffer. The RIFF parsing follows
// PSPRecomp-VCS-Android (vcs_media_decoder_android.cpp, MIT); the decoders are
// PPSSPP's at3_standalone (third_party/at3_standalone, LGPL 2.1+, from FFmpeg).
//
// Sample positions are "output samples": after the encoder delay in the fact
// chunk (first_offset), which is decoded and dropped, as on a PSP. End and loop
// points from the fact/smpl chunks are in the same space. Output is always
// interleaved stereo s16 (a mono file is duplicated), as SetData sets it up.

#include "atrac_hle.hpp"
#include "state_io.hpp"
#include "perf_stats.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"
#include "at3_decoders.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

namespace bbs {
namespace {

constexpr std::uint32_t kErrorBadId = 0x80630005u;
constexpr std::uint32_t kErrorNoId = 0x80630006u;
constexpr std::uint32_t kErrorUnknownFormat = 0x80630003u;
constexpr std::uint32_t kErrorAllDataDecoded = 0x80630024u;
constexpr std::uint32_t kErrorNoData = 0x80630010u;
constexpr int kMaxFrameSamples = 2048;

bool diag() {
    static const bool enabled = std::getenv("PSPRECOMP_ATRAC_DIAG") != nullptr ||
                                std::getenv("PSPRECOMP_AUDIO_DIAG") != nullptr;
    return enabled;
}

struct Track {
    // Where the file sits in guest memory.
    std::uint32_t buffer{};
    std::uint32_t available{};      // bytes of the file that are loaded
    std::uint32_t data_offset{};    // "data" chunk start, from buffer
    std::uint32_t data_size{};
    int block_align{};
    int channels{2};
    bool plus{};
    std::int32_t first_offset{};    // encoder delay (fact[1])
    std::int32_t total{};           // output samples (fact[0]); 0 = until the data ends
    std::int32_t loop_start{-1};
    std::int32_t loop_end{-1};      // inclusive
    std::int32_t loop_num{};        // -1 = forever

    ATRAC3Context *at3{};
    ATRAC3PContext *at3p{};
    std::array<std::vector<float>, 2> planes{std::vector<float>(kMaxFrameSamples),
                                             std::vector<float>(kMaxFrameSamples)};
    std::vector<std::int16_t> pending;   // decoded stereo samples not yet handed out
    std::size_t pending_read{};
    std::uint32_t next_frame{};
    std::int64_t decoded_abs{};          // absolute samples decoded so far (incl. first_offset)
    std::int32_t out_pos{};              // output samples handed out so far
    bool finished{};
    std::vector<std::uint8_t> extra;     // codec extradata (fmt cb), kept to re-create the decoder on a state load

    ~Track() {
        if (at3 != nullptr) atrac3_free(at3);
        if (at3p != nullptr) atrac3p_free(at3p);
    }
    [[nodiscard]] int frame_samples() const { return plus ? 2048 : 1024; }
    [[nodiscard]] std::uint32_t frame_count() const {
        return block_align > 0 ? data_size / static_cast<std::uint32_t>(block_align) : 0u;
    }
    void flush() {
        if (at3 != nullptr) atrac3_flush_buffers(at3);
        if (at3p != nullptr) atrac3p_flush_buffers(at3p);
        pending.clear();
        pending_read = 0u;
    }

    // Decodes the next frame into `pending`, dropping samples before first_offset.
    bool decode_next(const psprecomp::GuestMemory &memory) {
        psphost::perf::Scope perf_scope(psphost::perf::Atrac);
        if (next_frame >= frame_count()) return false;
        const std::uint32_t at = data_offset + next_frame * static_cast<std::uint32_t>(block_align);
        if (at + static_cast<std::uint32_t>(block_align) > available) return false;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(block_align));
        memory.copy_out(buffer + at, std::span<std::uint8_t>(bytes));
        ++next_frame;
        float *out[2]{planes[0].data(), planes[1].data()};
        int samples = 0;
        const int result = plus ? atrac3p_decode_frame(at3p, out, &samples, bytes.data(), block_align)
                                : atrac3_decode_frame(at3, out, &samples, bytes.data(), block_align);
        if (result < 0) samples = 0;
        samples = std::clamp(samples, 0, kMaxFrameSamples);
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(pending_read));
        pending_read = 0u;
        const float *left = planes[0].data();
        const float *right = channels > 1 ? planes[1].data() : left;
        const auto s16 = [](float v) { return static_cast<std::int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f); };
        for (int i = 0; i < samples; ++i, ++decoded_abs) {
            if (decoded_abs < first_offset) continue;
            pending.push_back(s16(left[i]));
            pending.push_back(s16(right[i]));
        }
        return true;
    }

    // Repositions so the next output sample is `target` (output space).
    void seek(const psprecomp::GuestMemory &memory, std::int32_t target) {
        const std::int64_t abs = static_cast<std::int64_t>(target) + first_offset;
        const std::int64_t frame = abs / frame_samples();
        // Start one frame early: the decoder's overlap needs the previous frame.
        const std::int64_t start = std::max<std::int64_t>(0, frame - 1);
        flush();
        next_frame = static_cast<std::uint32_t>(start);
        decoded_abs = start * frame_samples();
        const std::int32_t saved_offset = first_offset;
        first_offset = static_cast<std::int32_t>(abs);   // drop everything before the target
        while (decoded_abs < abs && decode_next(memory)) {}
        first_offset = saved_offset;
        out_pos = target;
    }
};

std::array<std::unique_ptr<Track>, 6> tracks;

std::uint16_t le16(const std::vector<std::uint8_t> &b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1u] << 8u));
}
std::uint32_t le32(const std::vector<std::uint8_t> &b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1u]) << 8u) |
           (static_cast<std::uint32_t>(b[at + 2u]) << 16u) | (static_cast<std::uint32_t>(b[at + 3u]) << 24u);
}

// Parses the RIFF header at `buffer` (the first `available` bytes are loaded).
std::uint32_t open_track(const psprecomp::GuestMemory &memory, std::uint32_t buffer, std::uint32_t available,
                         Track &t) {
    if (available < 12u || !memory.contains(buffer, available)) return kErrorNoData;
    // The header is small; read up to 4 KiB of it.
    std::vector<std::uint8_t> h(std::min<std::uint32_t>(available, 4096u));
    memory.copy_out(buffer, std::span<std::uint8_t>(h));
    if (std::memcmp(h.data(), "RIFF", 4) != 0 || std::memcmp(h.data() + 8, "WAVE", 4) != 0) return kErrorUnknownFormat;
    std::uint16_t format_tag = 0u;
    const std::uint8_t *extra = nullptr;
    int extra_size = 0;
    for (std::size_t offset = 12u; offset + 8u <= h.size();) {
        const std::uint32_t size = le32(h, offset + 4u);
        const std::size_t body = offset + 8u;
        const char *id = reinterpret_cast<const char *>(h.data() + offset);
        if (std::memcmp(id, "data", 4) == 0) {
            t.data_offset = static_cast<std::uint32_t>(body);
            t.data_size = std::min<std::uint32_t>(size, available - static_cast<std::uint32_t>(body));
            break;
        }
        if (body + size > h.size()) break;
        if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16u) {
            format_tag = le16(h, body);
            t.channels = std::max<int>(1, le16(h, body + 2u));
            t.block_align = le16(h, body + 12u);
            const std::uint16_t cb = size >= 18u ? le16(h, body + 16u) : 0u;
            if (cb > 0u && body + 18u + cb <= h.size()) { extra = h.data() + body + 18u; extra_size = cb; }
            t.plus = format_tag == 0xFFFEu;
        } else if (std::memcmp(id, "fact", 4) == 0 && size >= 4u) {
            t.total = static_cast<std::int32_t>(le32(h, body));
            if (size >= 8u) t.first_offset = static_cast<std::int32_t>(le32(h, body + 4u));
        } else if (std::memcmp(id, "smpl", 4) == 0 && size >= 36u + 24u) {
            if (le32(h, body + 28u) > 0u) {
                t.loop_start = static_cast<std::int32_t>(le32(h, body + 36u + 8u));
                t.loop_end = static_cast<std::int32_t>(le32(h, body + 36u + 12u));
            }
        }
        offset = body + size + (size & 1u);
    }
    if (t.data_offset == 0u || t.block_align <= 0 || (!t.plus && format_tag != 0x0270u)) return kErrorUnknownFormat;
    // The decoder adds its own delay on top of the encoder's (fact[1]): 184 samples
    // for ATRAC3+, 69 for ATRAC3. smpl loop points count from before both -- BBS's
    // menu theme: total 7491585, offset 3727, loop 3911..7495495 = 0..total-1 once
    // shifted -- so drop that much at the start and move the loops into output space.
    t.first_offset += t.plus ? 184 : 69;
    if (t.loop_start >= 0) {
        t.loop_start = std::max(0, t.loop_start - t.first_offset);
        t.loop_end = std::max(t.loop_start, t.loop_end - t.first_offset);
        if (t.total > 0) t.loop_end = std::min(t.loop_end, t.total - 1);
    }
    t.buffer = buffer;
    t.available = available;
    int align = t.block_align;
    if (t.plus) t.at3p = atrac3p_alloc(t.channels, &align);
    else t.at3 = atrac3_alloc(t.channels, &align, extra, extra_size);
    if (extra != nullptr && extra_size > 0) t.extra.assign(extra, extra + extra_size);
    if (t.at3 == nullptr && t.at3p == nullptr) return kErrorUnknownFormat;
    if (diag())
        std::cerr << "[atrac] open buf=" << psprecomp::hex32(buffer) << " bytes=" << available
                  << (t.plus ? " at3+" : " at3") << " ch=" << t.channels << " align=" << t.block_align
                  << " total=" << t.total << " offset=" << t.first_offset << " loop=" << t.loop_start << ".."
                  << t.loop_end << " frames=" << t.frame_count() << "\n";
    return 0u;
}

void store(psprecomp::Runtime &rt, std::uint32_t address, std::uint32_t value) {
    if (address != 0u && rt.memory().contains(address, 4u)) rt.memory().store32(address, value);
}

Track *track_for(psprecomp::AllegrexContext &ctx) {
    const std::uint32_t id = ctx.gpr[4u];
    if (id >= tracks.size() || !tracks[id]) { ctx.set_gpr(2u, kErrorBadId); return nullptr; }
    return tracks[id].get();
}

void set_data(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx, std::uint32_t buffer, std::uint32_t available) {
    std::size_t id = 0u;
    while (id < tracks.size() && tracks[id]) ++id;
    if (id == tracks.size()) { ctx.set_gpr(2u, kErrorNoId); return; }
    auto track = std::make_unique<Track>();
    if (const std::uint32_t error = open_track(rt.memory(), buffer, available, *track); error != 0u) {
        if (diag()) std::cerr << "[atrac] open failed buf=" << psprecomp::hex32(buffer) << " error=" << psprecomp::hex32(error) << "\n";
        ctx.set_gpr(2u, error);
        return;
    }
    tracks[id] = std::move(track);
    ctx.set_gpr(2u, static_cast<std::uint32_t>(id));
}

} // namespace

void install_atrac_hle(psprecomp::Runtime &runtime) {
    const auto ok = [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); };
    runtime.register_hle("sceAtrac3plus", 0x132F1ECAu, ok);   // sceAtracReinit(at3, at3plus)
    // sceAtracSetDataAndGetID(buf, size)
    runtime.register_hle("sceAtrac3plus", 0x7A20E7AFu, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        set_data(rt, ctx, ctx.gpr[4u], ctx.gpr[5u]);
    });
    // sceAtracSetHalfwayBufferAndGetID(buf, read_size, buf_size)
    runtime.register_hle("sceAtrac3plus", 0x0FAE370Eu, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        set_data(rt, ctx, ctx.gpr[4u], std::min(ctx.gpr[5u], ctx.gpr[6u]));
    });
    // sceAtracReleaseAtracID(id)
    runtime.register_hle("sceAtrac3plus", 0x61EB33F5u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        const std::uint32_t id = ctx.gpr[4u];
        if (id >= tracks.size() || !tracks[id]) { ctx.set_gpr(2u, kErrorBadId); return; }
        tracks[id].reset();
        ctx.set_gpr(2u, 0u);
    });
    // sceAtracDecodeData(id, out, *samples, *finish, *remain)
    runtime.register_hle("sceAtrac3plus", 0x6A8C3CD5u, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        Track *t = track_for(ctx);
        if (t == nullptr) return;
        const std::uint32_t out = ctx.gpr[5u];
        if (t->finished) {
            store(rt, ctx.gpr[6u], 0u);
            store(rt, ctx.gpr[7u], 1u);
            store(rt, ctx.gpr[8u], 0xFFFFFFFFu);
            ctx.set_gpr(2u, kErrorAllDataDecoded);
            return;
        }
        const int want = t->frame_samples();
        std::vector<std::int16_t> pcm;
        pcm.reserve(static_cast<std::size_t>(want) * 2u);
        int produced = 0;
        while (produced < want) {
            const bool looping = t->loop_start >= 0 && t->loop_end >= t->loop_start && t->loop_num != 0;
            if (looping && t->out_pos > t->loop_end) {
                if (t->loop_num > 0) --t->loop_num;
                t->seek(rt.memory(), t->loop_start);
                // Hand out a loop boundary on its own call, as the PSP does.
                if (produced > 0) break;
                continue;
            }
            if (t->total > 0 && t->out_pos >= t->total) { t->finished = true; break; }
            if (t->pending_read >= t->pending.size() && !t->decode_next(rt.memory())) { t->finished = true; break; }
            if (t->pending_read >= t->pending.size()) continue;   // a frame wholly inside the encoder delay
            pcm.push_back(t->pending[t->pending_read++]);
            pcm.push_back(t->pending[t->pending_read++]);
            ++t->out_pos;
            ++produced;
        }
        // The PSP reports the end with the last samples, not a call later.
        if (!t->finished && t->total > 0 && t->out_pos >= t->total &&
            !(t->loop_start >= 0 && t->loop_num != 0))
            t->finished = true;
        if (out != 0u && produced > 0 && rt.memory().contains(out, static_cast<std::size_t>(produced) * 4u))
            rt.memory().copy_in(out, std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t *>(pcm.data()), static_cast<std::size_t>(produced) * 4u));
        store(rt, ctx.gpr[6u], static_cast<std::uint32_t>(produced));
        store(rt, ctx.gpr[7u], t->finished ? 1u : 0u);
        store(rt, ctx.gpr[8u], 0xFFFFFFFFu);   // all data is on memory
        ctx.set_gpr(2u, 0u);
    });
    // sceAtracGetSoundSample(id, *end, *loop_start, *loop_end)
    runtime.register_hle("sceAtrac3plus", 0xA2BBA8BEu, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        Track *t = track_for(ctx);
        if (t == nullptr) return;
        const std::int32_t total = t->total > 0 ? t->total
            : static_cast<std::int32_t>(t->frame_count()) * t->frame_samples() - t->first_offset;
        store(rt, ctx.gpr[5u], static_cast<std::uint32_t>(std::max(0, total - 1)));
        store(rt, ctx.gpr[6u], static_cast<std::uint32_t>(t->loop_start));
        store(rt, ctx.gpr[7u], static_cast<std::uint32_t>(t->loop_end));
        ctx.set_gpr(2u, 0u);
    });
    // sceAtracGetMaxSample(id, *max)
    runtime.register_hle("sceAtrac3plus", 0xD6A5F2F7u, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        Track *t = track_for(ctx);
        if (t == nullptr) return;
        store(rt, ctx.gpr[5u], static_cast<std::uint32_t>(t->frame_samples()));
        ctx.set_gpr(2u, 0u);
    });
    // sceAtracGetInternalErrorInfo(id, *error)
    runtime.register_hle("sceAtrac3plus", 0xE88F759Bu, [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
        if (track_for(ctx) == nullptr) return;
        store(rt, ctx.gpr[5u], 0u);
        ctx.set_gpr(2u, 0u);
    });
    // sceAtracSetLoopNum(id, n): -1 forever
    runtime.register_hle("sceAtrac3plus", 0x868120B5u, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
        Track *t = track_for(ctx);
        if (t == nullptr) return;
        t->loop_num = static_cast<std::int32_t>(ctx.gpr[5u]);
        ctx.set_gpr(2u, 0u);
    });
}

// Save states (2026-11-18): track fields are saved; decoders are re-created and re-seeked to out_pos on load
// (guest RAM, which holds the file, is restored before this runs).
void serialize_atrac_state(psphost::StateIO &io, const psprecomp::GuestMemory &memory) {
    io.section("atrac");
    for (auto &slot : tracks) {
        std::uint8_t present = slot != nullptr ? 1u : 0u;
        io(present);
        if (io.loading()) slot.reset(present != 0u ? new Track() : nullptr);
        if (present == 0u) continue;
        Track &t = *slot;
        io(t.buffer); io(t.available); io(t.data_offset); io(t.data_size); io(t.block_align); io(t.channels);
        io(t.plus); io(t.first_offset); io(t.total); io(t.loop_start); io(t.loop_end); io(t.loop_num);
        io(t.next_frame); io(t.decoded_abs); io(t.out_pos); io(t.finished); io(t.extra);
        if (!io.loading()) continue;
        int align = t.block_align;
        if (t.plus) t.at3p = atrac3p_alloc(t.channels, &align);
        else t.at3 = atrac3_alloc(t.channels, &align, t.extra.empty() ? nullptr : t.extra.data(),
                                  static_cast<int>(t.extra.size()));
        const bool finished = t.finished;
        if (t.at3 != nullptr || t.at3p != nullptr) t.seek(memory, t.out_pos);
        t.finished = finished;
    }
}

} // namespace bbs
