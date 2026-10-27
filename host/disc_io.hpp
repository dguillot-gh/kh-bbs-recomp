#pragma once

#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_set>

namespace bbs {

class DiscIo {
public:
    void install_hles(psprecomp::Runtime &runtime);

private:
    struct OpenFile {
        std::fstream stream;
        bool readable{};
        bool writable{};
    };

    void open(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void close(psprecomp::AllegrexContext &);
    void read(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void write(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void seek(psprecomp::AllegrexContext &);
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

    std::int32_t next_fd_{3};
    std::map<std::int32_t, OpenFile> files_;
    std::map<std::int32_t, std::uint32_t> async_results_;
    std::int32_t next_module_uid_{0x600};
    std::unordered_set<std::int32_t> modules_;
};

} // namespace bbs
