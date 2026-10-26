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

    std::int32_t next_fd_{3};
    std::map<std::int32_t, OpenFile> files_;
    std::int32_t next_module_uid_{0x600};
    std::unordered_set<std::int32_t> modules_;
};

} // namespace bbs
