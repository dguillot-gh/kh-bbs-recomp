#pragma once

#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>

namespace bbs {

class DiscIo {
public:
    void install_hles(psprecomp::Runtime &runtime);
    // The game's ISO, for raw UMD access: "umd0:"/"umd1:" block reads and the
    // starting sector (LBN) that sceIoGetstat reports in st_private[0]. BBS
    // finds BBS0-4.DAT that way. Without an ISO those reads have nothing behind them.
    void set_iso(const std::filesystem::path &path);

private:
    // Raw = none: a host file. Block: the whole disc, seek/read counted in
    // 2048-byte sectors. Range: a byte view of the ISO ("disc0:/sce_lbn..._size...").
    enum class Raw { None, Block, Range };
    struct OpenFile {
        std::fstream stream;
        bool readable{};
        bool writable{};
        Raw raw{Raw::None};
        std::uint64_t base{};     // Range: byte offset in the ISO
        std::uint64_t size{};     // Block: sectors; Range: bytes
        std::uint64_t position{}; // Block: sectors; Range: bytes
    };
    struct IsoEntry {
        std::uint32_t lbn{};
        std::uint32_t size{};
        bool directory{};
    };

    void open(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void close(psprecomp::AllegrexContext &);
    void read(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void write(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    // sceIoLseek/LseekAsync take a 64-bit offset in a2:a3 and whence in t0 and
    // return 64 bits in v0:v1; the Lseek32 pair takes a1/a2 and returns v0.
    void seek(psprecomp::AllegrexContext &, bool wide);
    std::int64_t seek_to(std::int32_t fd, std::int64_t offset, std::uint32_t whence);
    void mkdir(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void getstat(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void load_module_by_id(psprecomp::AllegrexContext &);
    void start_module(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void unload_module(psprecomp::AllegrexContext &);
    // The async variants (open/close/read/write/seek-Async) complete inline on
    // this cooperative host; their result is kept per-fd for sceIoWaitAsync /
    // sceIoWaitAsyncCB / sceIoPollAsync (all: int (SceUID fd, SceInt64 *res)).
    void wait_async(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void note_async_result(psprecomp::AllegrexContext &, bool fd_is_result);

    bool read_iso(std::uint64_t offset, char *out, std::size_t length);
    // ISO 9660 lookup of a path inside the disc ("/PSP_GAME/USRDIR/BBS0.DAT").
    std::optional<IsoEntry> iso_lookup(const std::string &path);

    std::int32_t next_fd_{3};
    std::map<std::int32_t, OpenFile> files_;
    std::map<std::int32_t, std::uint32_t> async_results_;
    std::int32_t next_module_uid_{0x600};
    std::unordered_set<std::int32_t> modules_;
    std::ifstream iso_;
    std::uint64_t iso_bytes_{};
    std::optional<IsoEntry> iso_root_;
    std::map<std::string, IsoEntry> iso_cache_;
};

} // namespace bbs
