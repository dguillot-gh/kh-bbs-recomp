#pragma once
// PSMF stream demuxer for the sceMpeg HLE (2026-11-18). A PSMF movie is an MPEG-2 program stream cut in 2048-byte
// packs: H.264 video in PES stream 0xE0 (Annex B, one access-unit delimiter per picture) and ATRAC3plus audio in
// private stream 1 (0xBD, sub-stream 0x00, 4-byte sub-header). Audio frames start with an 8-byte 0x0F 0xD0 header
// whose bytes 2-3 give the block size. Packs are pushed as the game feeds its ring buffer; complete video access
// units and audio frames come out with their 90 kHz timestamps. Written from the MPEG-2 PS / H.264 Annex B specs
// and the PSMF layout as documented by PPSSPP (GPL, behaviour only, see CREDITS.md).
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace bbs::movie {

struct EsUnit {
    std::vector<std::uint8_t> data;
    std::int64_t pts{-1};  // 90 kHz
};

class PsmfDemux {
public:
    static constexpr std::int64_t kVideoTicks = 3003;  // 29.97 fps
    static constexpr std::int64_t kAudioTicks = 4180;  // 2048 samples at 44.1 kHz (90000*2048/44100)

    void reset() { *this = PsmfDemux{}; }
    void set_eof() { eof_ = true; }
    [[nodiscard]] bool eof() const { return eof_; }
    [[nodiscard]] std::size_t pending_bytes() const { return video_.size() + audio_.size(); }
    [[nodiscard]] std::uint64_t packs() const { return packs_; }

    // One or more whole 2048-byte packs.
    void push(const std::uint8_t *data, std::size_t size) {
        for (std::size_t at = 0; at + 2048u <= size; at += 2048u) push_pack(data + at);
    }

    bool next_video_au(EsUnit &out) {
        scan_video();
        if (au_starts_.empty()) return false;
        std::size_t end = 0;
        if (au_starts_.size() >= 2u) end = au_starts_[1];
        else if (eof_ && video_.size() > au_starts_[0]) end = video_.size();
        else return false;
        const std::size_t start = au_starts_[0];
        out.data.assign(video_.begin() + static_cast<std::ptrdiff_t>(start), video_.begin() + static_cast<std::ptrdiff_t>(end));
        out.pts = take_pts(video_marks_, start);
        if (out.pts < 0) out.pts = last_video_pts_ < 0 ? 0 : last_video_pts_ + kVideoTicks;
        last_video_pts_ = out.pts;
        au_starts_.pop_front();
        consume(video_, video_marks_, end);
        for (auto &s : au_starts_) s -= end;
        scan_pos_ = scan_pos_ >= end ? scan_pos_ - end : 0;
        return true;
    }

    bool next_audio_frame(EsUnit &out) {
        // Resync on the 0x0F 0xD0 frame header.
        std::size_t start = 0;
        while (start + 1u < audio_.size() && !(audio_[start] == 0x0Fu && audio_[start + 1u] == 0xD0u)) ++start;
        if (start > 0u) consume(audio_, audio_marks_, start);
        if (audio_.size() < 8u) return false;
        const std::size_t size = 8u + (((static_cast<std::size_t>(audio_[2]) & 3u) << 8) | audio_[3]) * 8u + 8u;
        if (audio_.size() < size) return false;
        out.data.assign(audio_.begin(), audio_.begin() + static_cast<std::ptrdiff_t>(size));
        out.pts = take_pts(audio_marks_, 0);
        if (out.pts < 0) out.pts = last_audio_pts_ < 0 ? 0 : last_audio_pts_ + kAudioTicks;
        last_audio_pts_ = out.pts;
        consume(audio_, audio_marks_, size);
        return true;
    }

private:
    using Marks = std::deque<std::pair<std::size_t, std::int64_t>>;  // (ES offset of a PES payload, its PTS)

    static std::uint32_t be16(const std::uint8_t *p) { return (static_cast<std::uint32_t>(p[0]) << 8) | p[1]; }

    static std::int64_t read_pts(const std::uint8_t *p) {
        return (static_cast<std::int64_t>(p[0] & 0x0Eu) << 29) | (static_cast<std::int64_t>(be16(p + 1) >> 1) << 15) |
               static_cast<std::int64_t>(be16(p + 3) >> 1);
    }

    void push_pack(const std::uint8_t *p) {
        ++packs_;
        std::size_t at = 0;
        while (at + 6u <= 2048u) {
            if (p[at] != 0u || p[at + 1u] != 0u || p[at + 2u] != 1u) break;
            const std::uint8_t id = p[at + 3u];
            if (id == 0xBAu) {  // pack header (MPEG-2): 14 bytes + stuffing
                if (at + 14u > 2048u) break;
                at += 14u + (p[at + 13u] & 7u);
                continue;
            }
            const std::size_t length = be16(p + at + 4u);
            const std::size_t end = at + 6u + length;
            if (end > 2048u) break;
            if ((id == 0xE0u || id == 0xBDu) && length >= 3u) {
                const std::uint8_t *h = p + at + 6u;
                const std::size_t header = 3u + h[2];
                std::int64_t pts = -1;
                if ((h[1] & 0x80u) != 0u && header >= 8u) pts = read_pts(h + 3);
                if (header <= length) {
                    const std::uint8_t *payload = h + header;
                    std::size_t size = length - header;
                    if (id == 0xE0u) {
                        if (pts >= 0) video_marks_.emplace_back(video_.size(), pts);
                        video_.insert(video_.end(), payload, payload + size);
                    } else if (size > 4u && payload[0] == 0x00u) {  // ATRAC sub-stream 0
                        if (pts >= 0) audio_marks_.emplace_back(audio_.size(), pts);
                        audio_.insert(audio_.end(), payload + 4, payload + size);
                    }
                }
            }
            at = end;
        }
    }

    // Finds access-unit starts: an access-unit delimiter (NAL 9), or, in a stream without them, an SPS or the first
    // slice of a picture (first_mb_in_slice == 0) that doesn't follow an SPS/PPS/SEI of the same unit.
    void scan_video() {
        while (scan_pos_ + 4u < video_.size()) {
            const std::uint8_t *v = video_.data();
            if (v[scan_pos_] == 0u && v[scan_pos_ + 1u] == 0u && v[scan_pos_ + 2u] == 1u) {
                const std::size_t nal_at = scan_pos_ + 3u;
                std::size_t start = scan_pos_;
                if (start > 0u && v[start - 1u] == 0u) --start;  // 4-byte start code
                const std::uint8_t type = v[nal_at] & 0x1Fu;
                bool begins = false;
                if (type == 9u) { begins = true; seen_aud_ = true; }
                else if (!seen_aud_) {
                    if (type == 7u || type == 6u) begins = !in_prefix_;
                    else if ((type == 1u || type == 5u) && nal_at + 1u < video_.size())
                        begins = !in_prefix_ && (v[nal_at + 1u] & 0x80u) != 0u;
                    in_prefix_ = (type == 7u || type == 8u || type == 6u);
                }
                if (begins && (au_starts_.empty() || au_starts_.back() != start)) au_starts_.push_back(start);
                scan_pos_ += 3u;
                continue;
            }
            ++scan_pos_;
        }
    }

    // A PES timestamp belongs to the first unit that starts at or after the PES payload start, so the unit
    // starting at `start` takes the last mark at or before it.
    static std::int64_t take_pts(Marks &marks, std::size_t start) {
        std::int64_t pts = -1;
        while (!marks.empty() && marks.front().first <= start) { pts = marks.front().second; marks.pop_front(); }
        return pts;
    }

    static void consume(std::vector<std::uint8_t> &buffer, Marks &marks, std::size_t count) {
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
        for (auto &m : marks) m.first = m.first >= count ? m.first - count : 0u;
    }

    std::vector<std::uint8_t> video_;
    std::vector<std::uint8_t> audio_;
    Marks video_marks_;
    Marks audio_marks_;
    std::deque<std::size_t> au_starts_;
    std::size_t scan_pos_{};
    bool seen_aud_{};
    bool in_prefix_{};
    bool eof_{};
    std::int64_t last_video_pts_{-1};
    std::int64_t last_audio_pts_{-1};
    std::uint64_t packs_{};
};

} // namespace bbs::movie
