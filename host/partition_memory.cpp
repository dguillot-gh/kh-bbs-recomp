#include "partition_memory.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace bbs {
namespace {
void set_error(psprecomp::AllegrexContext &ctx, std::uint32_t error) { ctx.set_gpr(2u, error); }
}

void PartitionMemory::initialize(psprecomp::Runtime &runtime, std::uint32_t arena_start) {
    runtime_ = &runtime;
    next_address_ = (arena_start + 0xFFu) & ~0xFFu;
    next_uid_ = 0x100;
    blocks_.clear();
    free_blocks_.clear();
    next_fpl_uid_ = 0x400;
    fixed_pools_.clear();
}

void PartitionMemory::install_hles(psprecomp::Runtime &runtime) {
    runtime.register_hle("SysMemUserForUser", 0x237DBD4Fu,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { allocate(rt, ctx); });
    runtime.register_hle("SysMemUserForUser", 0x9D9A5BA1u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto found = blocks_.find(uid);
            ctx.set_gpr(2u, found != blocks_.end() ? found->second.address : 0u);
        });
    runtime.register_hle("SysMemUserForUser", 0xB6D61D02u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { free(rt, ctx); });
    runtime.register_hle("SysMemUserForUser", 0xA291F107u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t low = (next_address_ + 0xFFu) & ~0xFFu;
            const std::uint32_t limit = stack_floor_source_ ? std::min(kUpperLimit, stack_floor_source_()) : kUpperLimit;
            ctx.set_gpr(2u, limit > low ? limit - low : 0u);
        });
    runtime.register_hle("ThreadManForUser", 0xC07BB470u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { create_fpl(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0xD979E9BFu,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { allocate_fpl(ctx, true); });
    runtime.register_hle("ThreadManForUser", 0x623AE665u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { allocate_fpl(ctx, false); });
    runtime.register_hle("ThreadManForUser", 0xF6414A71u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { free_fpl(ctx); });
    runtime.register_hle("ThreadManForUser", 0xED1410E0u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { delete_fpl(ctx); });
}

void PartitionMemory::create_fpl(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t name_ptr = ctx.gpr[4u];
    const std::uint32_t block_size = ctx.gpr[7u];
    const std::uint32_t block_count = ctx.gpr[8u];
    const std::uint32_t option = ctx.gpr[9u];
    if (std::getenv("PSPRECOMP_PARTITION_DIAG") != nullptr)
        std::cerr << "[pw-memory] create-fpl args name=" << psprecomp::hex32(name_ptr)
                  << " part=" << ctx.gpr[5u] << " attr=" << psprecomp::hex32(ctx.gpr[6u])
                  << " size=" << psprecomp::hex32(block_size) << " count=" << block_count
                  << " opt=" << psprecomp::hex32(option) << "\n";
    if (block_size == 0u || block_count == 0u || block_count > 4096u ||
        block_size > std::numeric_limits<std::uint32_t>::max() / block_count) {
        set_error(ctx, 0x80020190u);
        return;
    }
    const std::uint32_t stride = (block_size + 0xFu) & ~0xFu;
    if (stride < block_size || stride > std::numeric_limits<std::uint32_t>::max() / block_count) {
        set_error(ctx, 0x80020190u);
        return;
    }
    const std::uint32_t total = stride * block_count;
    const std::uint32_t base = allocate_range(total);
    if (base == 0u) {
        set_error(ctx, 0x80020190u);
        return;
    }
    runtime.memory().zero(base, total);
    const std::string name = name_ptr != 0u ? runtime.memory().read_c_string(name_ptr, 128u) : "fpl";
    const std::int32_t uid = next_fpl_uid_++;
    FixedPool pool{name, base, stride, std::vector<bool>(block_count, false)};
    fixed_pools_.emplace(uid, std::move(pool));
    ctx.set_gpr(2u, static_cast<std::uint32_t>(uid));
    if (std::getenv("PSPRECOMP_PARTITION_DIAG") != nullptr ||
        std::getenv("PSPRECOMP_THREAD_DIAG") != nullptr) {
        std::cerr << "[pw-memory] create-fpl uid=" << uid << " name=\"" << name
                  << "\" base=" << psprecomp::hex32(base) << " block="
                  << psprecomp::hex32(stride) << " count=" << block_count << " total="
                  << psprecomp::hex32(total) << "\n";
    }
}

void PartitionMemory::allocate_fpl(psprecomp::AllegrexContext &ctx, bool wait) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
    const std::uint32_t output = ctx.gpr[5u];
    const auto found = fixed_pools_.find(uid);
    if (found == fixed_pools_.end()) { set_error(ctx, 0x800201A1u); return; }
    if (output == 0u || !runtime_->memory().contains(output, 4u)) {
        set_error(ctx, 0x80020190u); return;
    }
    for (std::size_t i = 0; i < found->second.used.size(); ++i) {
        if (found->second.used[i]) continue;
        found->second.used[i] = true;
        const std::uint32_t address = found->second.base +
            static_cast<std::uint32_t>(i) * found->second.block_size;
        runtime_->memory().store32(output, address);
        ctx.set_gpr(2u, 0u);
        return;
    }
    set_error(ctx, wait ? 0x800201A8u : 0x800201A8u);
}

void PartitionMemory::free_fpl(psprecomp::AllegrexContext &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
    const std::uint32_t address = ctx.gpr[5u];
    const auto found = fixed_pools_.find(uid);
    if (found == fixed_pools_.end()) { set_error(ctx, 0x800201A1u); return; }
    if (address < found->second.base) { set_error(ctx, 0x80020190u); return; }
    const std::uint32_t offset = address - found->second.base;
    if (offset % found->second.block_size != 0u) { set_error(ctx, 0x80020190u); return; }
    const std::size_t index = offset / found->second.block_size;
    if (index >= found->second.used.size() || !found->second.used[index]) {
        set_error(ctx, 0x80020190u); return;
    }
    found->second.used[index] = false;
    ctx.set_gpr(2u, 0u);
}

void PartitionMemory::delete_fpl(psprecomp::AllegrexContext &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
    const auto found = fixed_pools_.find(uid);
    if (found == fixed_pools_.end()) { set_error(ctx, 0x800201A1u); return; }
    const std::uint32_t total = found->second.block_size *
        static_cast<std::uint32_t>(found->second.used.size());
    release_range(found->second.base, total);
    fixed_pools_.erase(found);
    ctx.set_gpr(2u, 0u);
}

std::uint32_t PartitionMemory::allocate_range(std::uint32_t size) {
    for (auto it = free_blocks_.begin(); it != free_blocks_.end(); ++it) {
        if (it->size < size) continue;
        const std::uint32_t address = it->address;
        it->address += size;
        it->size -= size;
        if (it->size == 0u) free_blocks_.erase(it);
        return address;
    }
    const std::uint64_t aligned = (static_cast<std::uint64_t>(next_address_) + 0xFFu) & ~0xFFull;
    const std::uint64_t end = aligned + size;
    const std::uint32_t limit = stack_floor_source_ ? std::min(kUpperLimit, stack_floor_source_()) : kUpperLimit;
    if (end > limit || end > std::numeric_limits<std::uint32_t>::max() ||
        !runtime_->memory().contains(static_cast<std::uint32_t>(aligned), size)) return 0u;
    next_address_ = static_cast<std::uint32_t>(end);
    return static_cast<std::uint32_t>(aligned);
}

void PartitionMemory::release_range(std::uint32_t address, std::uint32_t size) {
    free_blocks_.push_back({address, size});
    std::sort(free_blocks_.begin(), free_blocks_.end(), [](const FreeBlock &a, const FreeBlock &b) {
        return a.address < b.address;
    });
    std::vector<FreeBlock> merged;
    for (const FreeBlock block : free_blocks_) {
        if (!merged.empty() && merged.back().address + merged.back().size == block.address)
            merged.back().size += block.size;
        else
            merged.push_back(block);
    }
    free_blocks_ = std::move(merged);
    while (!free_blocks_.empty() &&
           free_blocks_.back().address + free_blocks_.back().size == next_address_) {
        next_address_ = free_blocks_.back().address;
        free_blocks_.pop_back();
    }
}

void PartitionMemory::allocate(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t name_ptr = ctx.gpr[5u];
    const std::uint32_t type = ctx.gpr[6u];
    const std::uint32_t requested = ctx.gpr[7u];
    const std::uint32_t requested_address = ctx.gpr[8u];
    const std::uint64_t aligned64 = (static_cast<std::uint64_t>(requested) + 0xFFu) & ~0xFFull;
    if (requested == 0u || aligned64 > std::numeric_limits<std::uint32_t>::max()) {
        set_error(ctx, 0x80020190u);
        return;
    }
    const std::uint32_t size = static_cast<std::uint32_t>(aligned64);
    std::uint32_t address = 0u;
    const std::uint32_t limit = stack_floor_source_ ? std::min(kUpperLimit, stack_floor_source_()) : kUpperLimit;
    if (type == 2u) {
        if ((requested_address & 0xFFu) != 0u ||
            !runtime.memory().contains(requested_address, size) ||
            requested_address < next_address_ ||
            static_cast<std::uint64_t>(requested_address) + size > limit) {
            set_error(ctx, 0x80020190u);
            return;
        }
        address = requested_address;
        next_address_ = std::max(next_address_, address + size);
    } else {
        address = allocate_range(size);
        if (address == 0u) {
            set_error(ctx, 0x80020190u);
            return;
        }
    }
    const std::string name = name_ptr != 0u ? runtime.memory().read_c_string(name_ptr, 128u) : "partition";
    const std::int32_t uid = next_uid_++;
    runtime.memory().zero(address, size);
    blocks_.emplace(uid, Block{name, address, size});
    ctx.set_gpr(2u, static_cast<std::uint32_t>(uid));
    if (std::getenv("PSPRECOMP_PARTITION_DIAG") != nullptr ||
        std::getenv("PSPRECOMP_THREAD_DIAG") != nullptr) {
        std::cerr << "[pw-memory] alloc uid=" << uid << " partition=" << ctx.gpr[4u]
                  << " type=" << type << " name=\"" << name << "\" addr="
                  << psprecomp::hex32(address) << " size=" << psprecomp::hex32(size) << "\n";
    }
}

void PartitionMemory::free(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
    const auto found = blocks_.find(uid);
    if (found == blocks_.end()) {
        set_error(ctx, 0x800200CBu);
        return;
    }
    release_range(found->second.address, found->second.size);
    blocks_.erase(found);
    ctx.set_gpr(2u, 0u);
}

} // namespace bbs
