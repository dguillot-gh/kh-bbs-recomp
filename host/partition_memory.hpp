#pragma once

#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace bbs {

class PartitionMemory {
public:
    void initialize(psprecomp::Runtime &runtime, std::uint32_t arena_start);
    void install_hles(psprecomp::Runtime &runtime);
    void set_stack_floor_source(std::function<std::uint32_t()> source) { stack_floor_source_ = std::move(source); }
    [[nodiscard]] std::uint32_t high_watermark() const noexcept { return next_address_; }

private:
    struct Block { std::string name; std::uint32_t address{}; std::uint32_t size{}; };
    struct FreeBlock { std::uint32_t address{}; std::uint32_t size{}; };
    struct FixedPool {
        std::string name;
        std::uint32_t base{};
        std::uint32_t block_size{};
        std::vector<bool> used;
    };

    void allocate(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void free(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void create_fpl(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void allocate_fpl(psprecomp::AllegrexContext &, bool wait);
    void free_fpl(psprecomp::AllegrexContext &);
    void delete_fpl(psprecomp::AllegrexContext &);
    std::uint32_t allocate_range(std::uint32_t size);
    void release_range(std::uint32_t address, std::uint32_t size);

    psprecomp::Runtime *runtime_{};
    std::uint32_t next_address_{};
    std::function<std::uint32_t()> stack_floor_source_;
    static constexpr std::uint32_t kUpperLimit = 0x09FF0000u;
    std::int32_t next_uid_{0x100};
    std::map<std::int32_t, Block> blocks_;
    std::vector<FreeBlock> free_blocks_;
    std::int32_t next_fpl_uid_{0x400};
    std::map<std::int32_t, FixedPool> fixed_pools_;
};

} // namespace bbs
