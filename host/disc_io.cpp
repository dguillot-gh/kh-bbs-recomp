#include "disc_io.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace bbs {
namespace {
constexpr std::uint32_t kIoErrorInvalidArgument = 0x80010016u;
constexpr std::uint32_t kIoErrorBadDescriptor = 0x80010009u;
constexpr std::uint32_t kIoErrorNotFound = 0x80010002u;
constexpr std::uint32_t kIoErrorNoSpace = 0x8001001Cu;
}

void DiscIo::install_hles(psprecomp::Runtime &runtime) {
    runtime.register_hle("IoFileMgrForUser", 0x109F50BCu,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { open(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x810C4BC3u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { close(ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x6A638D83u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { read(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x42EC03ACu,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { write(rt, ctx); });
    // The PSP "Async" I/O variants (KJFS and the EBOOT call these) complete inline
    // on this single-threaded cooperative host, so they share the same handlers:
    // open-async/close-async/read-async/write-async/seek-async are the exact same
    // operations, just without a callback. (sceIoCloseAsync NID 0xFF5940B6 was the
    // KJFS-worker blocker on 10/22: it opened libfont/TITLEID and closed the last fd.)
    // Each Async variant completes inline; note_async_result keeps the per-fd
    // outcome so sceIoWaitAsync/WaitAsyncCB below can answer with it.
    // (fd is the open result itself; the others take it in a0.)
    runtime.register_hle("IoFileMgrForUser", 0x89AA9906u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            open(rt, ctx);
            note_async_result(ctx, true);
        });
    runtime.register_hle("IoFileMgrForUser", 0xFF5940B6u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            close(ctx);
            note_async_result(ctx, false);
        });
    runtime.register_hle("IoFileMgrForUser", 0xA0B5A7C2u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            read(rt, ctx);
            note_async_result(ctx, false);
        });
    runtime.register_hle("IoFileMgrForUser", 0x0FACAB19u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            write(rt, ctx);
            note_async_result(ctx, false);
        });
    // sceIoLseekAsync / sceIoLseek32Async (the plain Lseek/Lseek32 pair below is sync).
    runtime.register_hle("IoFileMgrForUser", 0x71B19E77u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            seek(ctx, true);
            note_async_result(ctx, false);
        });
    runtime.register_hle("IoFileMgrForUser", 0x1B385D8Fu,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            seek(ctx, false);
            note_async_result(ctx, false);
        });
    // PSP SDK form for all three: int (SceUID fd, SceInt64 *res). Ops complete
    // inline on this cooperative host, so the stored per-fd result is final:
    // wait_async writes it as a sign-extended 64-bit value to *res and returns 0.
    // WaitAsync/WaitAsyncCB block (async_wait_hook_); PollAsync never does.
    runtime.register_hle("IoFileMgrForUser", 0xE23EEC33u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { wait_async(rt, ctx, true); });
    runtime.register_hle("IoFileMgrForUser", 0x35DBD746u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { wait_async(rt, ctx, true); });
    runtime.register_hle("IoFileMgrForUser", 0x3251EA56u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { wait_async(rt, ctx, false); });
    runtime.register_hle("IoFileMgrForUser", 0x27EB27B8u,   // sceIoLseek (64-bit)
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { seek(ctx, true); });
    runtime.register_hle("IoFileMgrForUser", 0x68963324u,   // sceIoLseek32
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { seek(ctx, false); });
    runtime.register_hle("IoFileMgrForUser", 0x06A70004u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { mkdir(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0xACE946E8u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { getstat(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x63632449u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { ioctl(rt, ctx); });
    runtime.register_hle("ModuleMgrForUser", 0xB7F46618u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { load_module_by_id(ctx); });
    runtime.register_hle("ModuleMgrForUser", 0x50F0C1ECu,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { start_module(rt, ctx); });
    runtime.register_hle("ModuleMgrForUser", 0xD1FF982Au,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { start_module(rt, ctx); });
    runtime.register_hle("ModuleMgrForUser", 0x2E0911AAu,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { unload_module(ctx); });
}

void DiscIo::load_module_by_id(psprecomp::AllegrexContext &ctx) {
    // Call sites (EBOOT AOT L_08806A88, KJFS AOT L_09E06C34) pass either an open
    // PRX fd (EBOOT: IoOpen -> load -> IoClose -> StartModule) or a bare module
    // number with no fd (KJFS worker job). Contract for both: return 0 = pending
    // (caller retries), > 0 = assigned module id, < 0 = error. Module services
    // are provided through HLEs, so "loading" just assigns and registers a uid;
    // it must not depend on a pre-opened descriptor.
    const std::int32_t uid = next_module_uid_++;
    modules_.insert(uid);
    ctx.set_gpr(2u, static_cast<std::uint32_t>(uid));
}

void DiscIo::start_module(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
    if (!modules_.contains(uid)) { ctx.set_gpr(2u, 0x800201A1u); return; }
    const std::uint32_t status = ctx.gpr[7u];
    if (status != 0u && runtime.memory().contains(status, 4u))
        runtime.memory().store32(status, 0u);
    // Profiles provide module services through HLEs instead of mapping PSP
    // PRX code into the generated game image.
    ctx.set_gpr(2u, 0u);
}

void DiscIo::unload_module(psprecomp::AllegrexContext &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
    ctx.set_gpr(2u, modules_.erase(uid) != 0u ? 0u : 0x800201A1u);
}

void DiscIo::open(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    try {
        static std::uint32_t trace_count = 0u;
        const bool trace = std::getenv("PSPRECOMP_IO_TRACE") != nullptr && trace_count < 32u;
        const std::string psp_path = runtime.memory().read_c_string(ctx.gpr[4u], 1024u);
        const std::uint32_t flags = ctx.gpr[5u];
        const bool read_access = (flags & 3u) != 2u;
        const bool write_access = (flags & 3u) != 1u;
        const bool create = (flags & 0x0200u) != 0u;
        const bool truncate = (flags & 0x0400u) != 0u;
        const bool append = (flags & 0x0100u) != 0u;
        if (!read_access && !write_access) { ctx.set_gpr(2u, kIoErrorInvalidArgument); return; }
        const std::filesystem::path native = runtime.translate_path(psp_path);
        std::error_code error;
        if (trace) {
            ++trace_count;
            std::cerr << "[io-open] path=\"" << psp_path << "\" native=\""
                      << native.string() << "\" create=" << create << "\n";
        }
        // Raw UMD access, served from the ISO. "umd0:"/"umd1:"/"umd:" with no file
        // part is the whole disc as a block device (seek/read in 2048-byte sectors);
        // BBS opens "umd1:" and reads BBS0-4.DAT at the LBN that getstat reported.
        // "disc0:/sce_lbn0x<lbn>_size0x<bytes>" is a byte view of one sector range.
        if (const auto colon = psp_path.find(':'); colon != std::string::npos && !write_access) {
            std::string device = psp_path.substr(0u, colon);
            std::transform(device.begin(), device.end(), device.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::string rest = psp_path.substr(colon + 1u);
            const bool block = (device == "umd" || device == "umd0" || device == "umd1") &&
                (rest.empty() || rest == "/");
            const auto lbn_at = rest.find("sce_lbn0x");
            const auto size_at = rest.find("_size0x");
            const bool range = lbn_at != std::string::npos && size_at != std::string::npos && size_at > lbn_at;
            if ((block || range) && iso_.is_open()) {
                OpenFile raw{};
                raw.readable = true;
                if (block) {
                    raw.raw = Raw::Block;
                    raw.size = iso_bytes_ / 2048u;
                } else {
                    const auto lbn = std::stoull(rest.substr(lbn_at + 9u, size_at - lbn_at - 9u), nullptr, 16);
                    raw.raw = Raw::Range;
                    raw.base = lbn * 2048u;
                    raw.size = std::stoull(rest.substr(size_at + 7u), nullptr, 16);
                }
                const std::int32_t fd = next_fd_++;
                files_.emplace(fd, std::move(raw));
                if (trace) std::cerr << "[io-open] raw ISO fd=" << fd << (block ? " block" : " range") << "\n";
                ctx.set_gpr(2u, static_cast<std::uint32_t>(fd));
                return;
            }
        }
        if (create) std::filesystem::create_directories(native.parent_path(), error);
        if (!create && !std::filesystem::exists(native, error)) {
            ctx.set_gpr(2u, kIoErrorNotFound); return;
        }
        // A bare drive / pseudo-path probe (e.g. "umd1:") maps to a directory on the host.
        // The PSP treats this as a valid (empty) device handle and games check only the
        // returned fd >= 0 to confirm the medium is present before loading its archives.
        // Opening the directory with std::fstream fails on Windows, so hand back a valid
        // synthetic fd instead — VCS's HLE does the same for disc0:/sce_lbn0x0 probes.
        if (!create && std::filesystem::is_directory(native, error)) {
            OpenFile probe{};
            probe.readable = read_access;
            probe.writable = write_access;
            const std::int32_t fd = next_fd_++;
            files_.emplace(fd, std::move(probe));
            ctx.set_gpr(2u, static_cast<std::uint32_t>(fd));
            return;
        }
        std::ios::openmode mode = std::ios::binary;
        if (read_access) mode |= std::ios::in;
        if (write_access) mode |= std::ios::out;
        if (truncate) mode |= std::ios::trunc;
        if (append) mode |= std::ios::app;
        OpenFile file{};
        file.native = native;
        file.stream.open(native, mode);
        if (!file.stream) { ctx.set_gpr(2u, create ? kIoErrorNoSpace : kIoErrorNotFound); return; }
        file.readable = read_access;
        file.writable = write_access;
        const std::int32_t fd = next_fd_++;
        files_.emplace(fd, std::move(file));
        ctx.set_gpr(2u, static_cast<std::uint32_t>(fd));
    } catch (...) {
        ctx.set_gpr(2u, kIoErrorInvalidArgument);
    }
}

void DiscIo::close(psprecomp::AllegrexContext &ctx) {
    const auto fd = static_cast<std::int32_t>(ctx.gpr[4u]);
    const auto found = files_.find(fd);
    if (found == files_.end()) { ctx.set_gpr(2u, kIoErrorBadDescriptor); return; }
    found->second.stream.close();
    files_.erase(found);
    ctx.set_gpr(2u, 0u);
}

void DiscIo::read(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const auto fd = static_cast<std::int32_t>(ctx.gpr[4u]);
    const std::uint32_t output = ctx.gpr[5u];
    const std::uint32_t size = ctx.gpr[6u];
    const auto found = files_.find(fd);
    if (found == files_.end()) { ctx.set_gpr(2u, kIoErrorBadDescriptor); return; }
    if (!found->second.readable || !runtime.memory().contains(output, size) ||
        size > static_cast<std::uint32_t>(std::numeric_limits<std::streamsize>::max())) {
        ctx.set_gpr(2u, kIoErrorInvalidArgument); return;
    }
    OpenFile &file = found->second;
    if (file.raw != Raw::None) {
        // Block: size and the result are sectors. Range: bytes, clamped to the range.
        const std::uint64_t unit = file.raw == Raw::Block ? 2048u : 1u;
        const std::uint64_t left = file.position < file.size ? file.size - file.position : 0u;
        const std::uint64_t units = std::min<std::uint64_t>(size, left);
        const std::uint64_t bytes = units * unit;
        if (bytes > 0x7FFFFFFFu || !runtime.memory().contains(output, static_cast<std::uint32_t>(bytes))) {
            ctx.set_gpr(2u, kIoErrorInvalidArgument); return;
        }
        std::vector<char> data(static_cast<std::size_t>(bytes));
        if (bytes != 0u && !read_iso(file.base + file.position * unit, data.data(), data.size())) {
            ctx.set_gpr(2u, kIoErrorInvalidArgument); return;
        }
        for (std::size_t i = 0; i < data.size(); ++i)
            runtime.memory().store8(output + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(data[i]));
        if (std::getenv("PSPRECOMP_IO_TRACE") != nullptr)
            std::cerr << "[io-read] raw fd=" << fd << " units=" << units << " at=" << file.position << "\n";
        file.position += units;
        ctx.set_gpr(2u, static_cast<std::uint32_t>(units));
        return;
    }
    std::vector<char> buffer(size);
    found->second.stream.read(buffer.data(), static_cast<std::streamsize>(size));
    const std::streamsize count = found->second.stream.gcount();
    if (count > 0) {
        for (std::streamsize i = 0; i < count; ++i)
            runtime.memory().store8(output + static_cast<std::uint32_t>(i),
                static_cast<std::uint8_t>(buffer[static_cast<std::size_t>(i)]));
    }
    found->second.stream.clear();
    ctx.set_gpr(2u, static_cast<std::uint32_t>(count));
}

void DiscIo::write(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const auto fd = static_cast<std::int32_t>(ctx.gpr[4u]);
    const std::uint32_t input = ctx.gpr[5u];
    const std::uint32_t size = ctx.gpr[6u];
    const auto found = files_.find(fd);
    if (found == files_.end()) { ctx.set_gpr(2u, kIoErrorBadDescriptor); return; }
    if (!found->second.writable || !runtime.memory().contains(input, size)) {
        ctx.set_gpr(2u, kIoErrorInvalidArgument); return;
    }
    std::vector<char> buffer(size);
    for (std::uint32_t i = 0; i < size; ++i)
        buffer[i] = static_cast<char>(runtime.memory().load8(input + i));
    found->second.stream.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (!found->second.stream) { ctx.set_gpr(2u, kIoErrorNoSpace); return; }
    ctx.set_gpr(2u, size);
}

void DiscIo::seek(psprecomp::AllegrexContext &ctx, bool wide) {
    const auto fd = static_cast<std::int32_t>(ctx.gpr[4u]);
    const std::int64_t offset = wide
        ? static_cast<std::int64_t>((static_cast<std::uint64_t>(ctx.gpr[7u]) << 32u) | ctx.gpr[6u])
        : static_cast<std::int64_t>(static_cast<std::int32_t>(ctx.gpr[5u]));
    const std::uint32_t whence = wide ? ctx.gpr[8u] : ctx.gpr[6u];
    const std::int64_t result = seek_to(fd, offset, whence);
    if (std::getenv("PSPRECOMP_IO_TRACE") != nullptr)
        std::cerr << "[io-seek] fd=" << fd << " offset=" << offset << " whence=" << whence << " wide=" << wide
                  << " ret=0x" << std::hex << static_cast<std::uint64_t>(result) << std::dec << "\n";
    ctx.set_gpr(2u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(result)));
    if (wide) ctx.set_gpr(3u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(result) >> 32u));
}

// The new position, or a PSP error code sign-extended to 64 bits.
std::int64_t DiscIo::seek_to(std::int32_t fd, std::int64_t offset, std::uint32_t whence) {
    const auto error = [](std::uint32_t code) {
        return static_cast<std::int64_t>(static_cast<std::int32_t>(code));
    };
    const auto found = files_.find(fd);
    if (found == files_.end()) return error(kIoErrorBadDescriptor);
    if (whence > 2u) return error(kIoErrorInvalidArgument);
    OpenFile &file = found->second;
    if (file.raw != Raw::None) {
        const std::int64_t origin = whence == 0u ? 0 :
            static_cast<std::int64_t>(whence == 1u ? file.position : file.size);
        const std::int64_t target = origin + offset;
        if (target < 0) return error(kIoErrorInvalidArgument);
        file.position = static_cast<std::uint64_t>(target);
        return target;
    }
    const std::ios::seekdir direction = whence == 0u ? std::ios::beg :
        (whence == 1u ? std::ios::cur : std::ios::end);
    file.stream.clear();
    if (file.readable) file.stream.seekg(offset, direction);
    if (file.writable) file.stream.seekp(offset, direction);
    if (!file.stream) return error(kIoErrorInvalidArgument);
    const auto position = file.readable ? file.stream.tellg() : file.stream.tellp();
    if (position < 0) return error(kIoErrorInvalidArgument);
    return static_cast<std::int64_t>(position);
}

namespace {
bool io_trace_on() { return std::getenv("PSPRECOMP_IO_TRACE") != nullptr; }
}

void DiscIo::note_async_result(psprecomp::AllegrexContext &ctx, bool fd_is_result) {
    const std::uint32_t result = ctx.gpr[2u];
    const std::int32_t fd = fd_is_result
        ? (result >= 0x80000000u ? -1 : static_cast<std::int32_t>(result))
        : static_cast<std::int32_t>(ctx.gpr[4u]);
    if (fd < 0) return;
    async_results_[fd] = result;
    if (io_trace_on()) {
        std::cerr << "[io-async] fd=" << fd << " result=0x" << std::hex << result << std::dec << "\n";
    }
}

void DiscIo::wait_async(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx, bool blocking) {
    // PSP SDK: int (SceUID fd, SceInt64 *res). Same contract for sceIoWaitAsync,
    // sceIoWaitAsyncCB and sceIoPollAsync: ops complete inline on this host, so
    // the stored per-fd result is final — write it as a sign-extended 64-bit
    // value to *res and return 0. No pending op for that fd: 0x80020329.
    const auto fd = static_cast<std::int32_t>(ctx.gpr[4u]);
    const std::uint32_t res = ctx.gpr[5u];
    const auto found = async_results_.find(fd);
    if (found == async_results_.end()) {
        if (io_trace_on())
            std::cerr << "[io-waitasync] fd=" << fd << " ret=0x80020329 (no async result)\n";
        ctx.set_gpr(2u, 0x80020329u);
        return;
    }
    if (!runtime.memory().contains(res, 8u)) {
        if (io_trace_on())
            std::cerr << "[io-waitasync] fd=" << fd << " res=0x" << std::hex << res << std::dec
                      << " ret=0x80010016 (bad res)\n";
        ctx.set_gpr(2u, kIoErrorInvalidArgument);
        return;
    }
    const std::uint32_t result = found->second;
    runtime.memory().store32(res, result);
    runtime.memory().store32(res + 4u, (result & 0x80000000u) ? 0xFFFFFFFFu : 0u);
    if (io_trace_on()) {
        std::cerr << "[io-waitasync] fd=" << fd << " result=0x" << std::hex << result
                  << std::dec << " ret=0\n";
    }
    ctx.set_gpr(2u, 0u);
    // A real disc read takes time and the waiting thread blocks meanwhile, so
    // lower-priority threads run before the result comes back. BBS depends on
    // it: RsrcBuilder (prio 21) wakes AsyncFileAccess (prio 20) and only then
    // marks the resource "requested" (state 3). Finishing the read inline let
    // Async set "loaded" (7) first; RsrcBuilder overwrote it with 3 and the
    // "common" lookup before 0x08A8BC6C returned null (+0x48 read). 2026-11-16.
    if (blocking && async_wait_hook_) async_wait_hook_(runtime, ctx);
}

namespace {
// SceIoStat, 0x58 bytes: st_mode, st_attr, 64-bit st_size, ctime/atime/mtime
// as 16-byte ScePspDateTime (u16 year, month, day, hour, minute, second; u32 us)
// at 0x10/0x20/0x30, then st_private[6] at 0x40. st_private[0] is the file's
// starting sector on the UMD. Clear exactly 0x58 bytes: the buffer is usually on
// the guest stack, and the old 280-byte clear wiped the caller's saved registers
// and return address (BBS's DiscInfoBind thread then "returned" to 0).
const std::uint32_t kFstatSize = 0x58u;
void install_stat_buffer(psprecomp::Runtime &runtime, std::uint32_t output, std::int64_t file_size,
                         std::uint32_t lbn, bool directory) {
    runtime.memory().zero(output, kFstatSize);
    // FIO_S_IFDIR/FIO_S_IFREG with r-x for all; FIO_SO_IFDIR/FIO_SO_IFREG.
    runtime.memory().store32(output + 0x00u, (directory ? 0x1000u : 0x2000u) | 0x016Du);
    runtime.memory().store32(output + 0x04u, directory ? 0x0010u : 0x0020u);
    const auto size = static_cast<std::uint64_t>(file_size < 0 ? 0 : file_size);
    runtime.memory().store32(output + 0x08u, static_cast<std::uint32_t>(size));
    runtime.memory().store32(output + 0x0Cu, static_cast<std::uint32_t>(size >> 32u));
    std::tm local{};
    const auto now = std::time(nullptr);
    const auto *host_local = std::localtime(&now);
    if (host_local) local = *host_local;
    for (std::uint32_t stamp = 0x10u; stamp <= 0x30u; stamp += 0x10u) {
        runtime.memory().store16(output + stamp + 0u, static_cast<std::uint16_t>(local.tm_year + 1900));
        runtime.memory().store16(output + stamp + 2u, static_cast<std::uint16_t>(local.tm_mon + 1));
        runtime.memory().store16(output + stamp + 4u, static_cast<std::uint16_t>(local.tm_mday));
        runtime.memory().store16(output + stamp + 6u, static_cast<std::uint16_t>(local.tm_hour));
        runtime.memory().store16(output + stamp + 8u, static_cast<std::uint16_t>(local.tm_min));
        runtime.memory().store16(output + stamp + 10u, static_cast<std::uint16_t>(local.tm_sec));
    }
    runtime.memory().store32(output + 0x40u, lbn);
}
} // namespace

void DiscIo::mkdir(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    try {
        const std::string psp_path = runtime.memory().read_c_string(ctx.gpr[4u], 1024u);
        const std::filesystem::path native = runtime.translate_path(psp_path);
        std::error_code error;
        if (std::filesystem::exists(native, error) && !error) { ctx.set_gpr(2u, 0u); return; }
        std::filesystem::create_directories(native, error);
        if (error && error != std::errc::file_exists) {
            ctx.set_gpr(2u, error == std::errc::no_such_file_or_directory
                ? kIoErrorNotFound : kIoErrorInvalidArgument);
            return;
        }
        ctx.set_gpr(2u, 0u);
    } catch (...) {
        ctx.set_gpr(2u, kIoErrorInvalidArgument);
    }
}

void DiscIo::getstat(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    std::int64_t file_size = 0;
    std::string psp_path;
    try {
        psp_path = runtime.memory().read_c_string(ctx.gpr[4u], 1024u);
        const std::filesystem::path native = runtime.translate_path(psp_path);
        std::error_code error;
        if (!std::filesystem::is_regular_file(native, error) || error) {
            ctx.set_gpr(2u, kIoErrorNotFound); return;
        }
        const auto size = std::filesystem::file_size(native, error);
        if (!error) file_size = static_cast<std::int64_t>(size);
    } catch (...) {
        ctx.set_gpr(2u, kIoErrorInvalidArgument); return;
    }
    if (!runtime.memory().contains(ctx.gpr[5u], kFstatSize)) {
        ctx.set_gpr(2u, kIoErrorInvalidArgument); return;
    }
    std::uint32_t lbn = 0u;
    if (const auto colon = psp_path.find(':'); colon != std::string::npos) {
        if (const auto entry = iso_lookup(psp_path.substr(colon + 1u))) lbn = entry->lbn;
    }
    if (std::getenv("PSPRECOMP_IO_TRACE") != nullptr)
        std::cerr << "[io-getstat] \"" << psp_path << "\" size=" << file_size << " lbn=" << lbn << "\n";
    install_stat_buffer(runtime, ctx.gpr[5u], file_size, lbn, false);
    ctx.set_gpr(2u, 0u);
}


namespace {
std::uint32_t le32(const unsigned char *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
}
std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}
}

void DiscIo::set_iso(const std::filesystem::path &path) {
    iso_.open(path, std::ios::binary);
    if (!iso_) { std::cerr << "ISO:        cannot open " << path.string() << "\n"; return; }
    std::error_code error;
    iso_bytes_ = std::filesystem::file_size(path, error);
    char pvd[2048];
    if (error || !read_iso(16u * 2048u, pvd, sizeof pvd) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) {
        std::cerr << "ISO:        no ISO 9660 volume descriptor in " << path.string() << "\n";
        iso_.close();
        return;
    }
    // Primary volume descriptor: the root directory record is at offset 156.
    const auto *root = reinterpret_cast<const unsigned char *>(pvd + 156);
    iso_root_ = IsoEntry{le32(root + 2), le32(root + 10), true};
    std::cerr << "ISO:        " << path.string() << " (" << iso_bytes_ / 2048u << " sectors)\n";
}

bool DiscIo::read_iso(std::uint64_t offset, char *out, std::size_t length) {
    if (!iso_.is_open() || offset + length > iso_bytes_) return false;
    iso_.clear();
    iso_.seekg(static_cast<std::streamoff>(offset));
    iso_.read(out, static_cast<std::streamsize>(length));
    return static_cast<std::size_t>(iso_.gcount()) == length;
}

std::optional<DiscIo::IsoEntry> DiscIo::iso_lookup(const std::string &path) {
    if (!iso_root_) return std::nullopt;
    std::string key = upper(path);
    std::replace(key.begin(), key.end(), '\\', '/');
    while (!key.empty() && key.front() == '/') key.erase(key.begin());
    while (!key.empty() && key.back() == '/') key.pop_back();
    if (key.empty()) return iso_root_;
    if (const auto cached = iso_cache_.find(key); cached != iso_cache_.end()) return cached->second;

    IsoEntry current = *iso_root_;
    std::size_t start = 0u;
    while (true) {
        const std::size_t slash = key.find('/', start);
        const std::string part = key.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!current.directory || current.size == 0u) return std::nullopt;
        std::vector<char> directory(current.size);
        if (!read_iso(static_cast<std::uint64_t>(current.lbn) * 2048u, directory.data(), directory.size()))
            return std::nullopt;
        std::optional<IsoEntry> next;
        for (std::size_t pos = 0u; pos < directory.size();) {
            const auto length = static_cast<unsigned char>(directory[pos]);
            if (length == 0u) { pos = (pos / 2048u + 1u) * 2048u; continue; } // records never cross sectors
            if (length < 34u || pos + length > directory.size()) break;
            const auto *record = reinterpret_cast<const unsigned char *>(directory.data() + pos);
            std::string name(reinterpret_cast<const char *>(record + 33), record[32]);
            if (const auto version = name.find(';'); version != std::string::npos) name.resize(version);
            if (!name.empty() && name.back() == '.') name.pop_back();
            if (upper(name) == part) {
                next = IsoEntry{le32(record + 2), le32(record + 10), (record[25] & 2u) != 0u};
                break;
            }
            pos += length;
        }
        if (!next) return std::nullopt;
        current = *next;
        if (slash == std::string::npos) break;
        start = slash + 1u;
    }
    iso_cache_.emplace(key, current);
    return current;
}


void DiscIo::ioctl(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const auto fd = static_cast<std::int32_t>(ctx.gpr[4u]);
    const std::uint32_t command = ctx.gpr[5u];
    const bool trace = std::getenv("PSPRECOMP_IO_TRACE") != nullptr;
    const auto found = files_.find(fd);
    if (found == files_.end()) { ctx.set_gpr(2u, kIoErrorBadDescriptor); return; }
    OpenFile &file = found->second;
    if (command != 0x04100001u) {
        // Other commands are not needed yet: acknowledge, leave outdata alone.
        if (trace) std::cerr << "[io-ioctl] fd=" << fd << " cmd=0x" << std::hex << command << std::dec << " ret=0 (ignored)\n";
        ctx.set_gpr(2u, 0u);
        return;
    }
    // 0x04100001: define the PGD (amctrl DRM) key for this file; reads from now
    // on return the decrypted data, starting at its offset 0. Plain files have
    // no "\0PGD" header and are left as they are.
    char magic[4]{};
    if (file.raw == Raw::None && file.stream.is_open()) {
        file.stream.clear();
        file.stream.seekg(0, std::ios::beg);
        file.stream.read(magic, 4);
        file.stream.clear();
        file.stream.seekg(0, std::ios::beg);
    }
    if (std::memcmp(magic, "\0PGD", 4) != 0) {
        if (trace) std::cerr << "[io-ioctl] fd=" << fd << " pgd-key: not a PGD file, ret=0\n";
        ctx.set_gpr(2u, 0u);
        return;
    }
    const std::filesystem::path plain = decrypted_dir_ / file.native.filename();
    std::error_code error;
    if (decrypted_dir_.empty() || !std::filesystem::is_regular_file(plain, error)) {
        std::string key_hex;
        static const char *digits = "0123456789abcdef";
        for (std::uint32_t i = 0; i < 16u && runtime.memory().contains(ctx.gpr[6u] + i, 1u); ++i) {
            const auto byte = runtime.memory().load8(ctx.gpr[6u] + i);
            key_hex += digits[byte >> 4u];
            key_hex += digits[byte & 15u];
        }
        runtime.stop("PGD file " + file.native.filename().string() + " needs an offline-decrypted copy at " +
                     plain.string() + ". Run: pgddec \"" + file.native.string() + "\" \"" + plain.string() +
                     "\" " + key_hex);
        return;
    }
    file.stream.close();
    file.stream.open(plain, std::ios::binary | std::ios::in);
    if (!file.stream) { ctx.set_gpr(2u, kIoErrorNotFound); return; }
    file.native = plain;
    file.writable = false;
    if (trace) std::cerr << "[io-ioctl] fd=" << fd << " pgd-key: now reading " << plain.string() << "\n";
    ctx.set_gpr(2u, 0u);
}

} // namespace bbs
