#include "disc_io.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
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
            seek(ctx);
            note_async_result(ctx, false);
        });
    runtime.register_hle("IoFileMgrForUser", 0x1B385D8Fu,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            seek(ctx);
            note_async_result(ctx, false);
        });
    // PSP SDK form for all three: int (SceUID fd, SceInt64 *res). Ops complete
    // inline on this cooperative host, so the stored per-fd result is final:
    // wait_async writes it as a sign-extended 64-bit value to *res and returns 0.
    runtime.register_hle("IoFileMgrForUser", 0xE23EEC33u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { wait_async(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x35DBD746u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { wait_async(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x3251EA56u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { wait_async(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x27EB27B8u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { seek(ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x68963324u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { seek(ctx); });
    runtime.register_hle("IoFileMgrForUser", 0x06A70004u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { mkdir(rt, ctx); });
    runtime.register_hle("IoFileMgrForUser", 0xACE946E8u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { getstat(rt, ctx); });
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
        if (create) std::filesystem::create_directories(native.parent_path(), error);
        if (!create && !std::filesystem::exists(native, error)) {
            ctx.set_gpr(2u, kIoErrorNotFound); return;
        }
        std::ios::openmode mode = std::ios::binary;
        if (read_access) mode |= std::ios::in;
        if (write_access) mode |= std::ios::out;
        if (truncate) mode |= std::ios::trunc;
        if (append) mode |= std::ios::app;
        OpenFile file{};
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

void DiscIo::seek(psprecomp::AllegrexContext &ctx) {
    const auto fd = static_cast<std::int32_t>(ctx.gpr[4u]);
    const auto found = files_.find(fd);
    if (found == files_.end()) { ctx.set_gpr(2u, kIoErrorBadDescriptor); return; }
    const std::int32_t offset = static_cast<std::int32_t>(ctx.gpr[5u]);
    const std::uint32_t whence = ctx.gpr[6u];
    const std::ios::seekdir direction = whence == 0u ? std::ios::beg :
        (whence == 1u ? std::ios::cur : std::ios::end);
    if (whence > 2u) { ctx.set_gpr(2u, kIoErrorInvalidArgument); return; }
    found->second.stream.clear();
    if (found->second.readable) found->second.stream.seekg(offset, direction);
    if (found->second.writable) found->second.stream.seekp(offset, direction);
    if (!found->second.stream) { ctx.set_gpr(2u, kIoErrorInvalidArgument); return; }
    const auto position = found->second.readable
        ? found->second.stream.tellg() : found->second.stream.tellp();
    if (position < 0 || static_cast<std::uint64_t>(position) > 0x7FFFFFFFu) {
        ctx.set_gpr(2u, kIoErrorInvalidArgument); return;
    }
    ctx.set_gpr(2u, static_cast<std::uint32_t>(position));
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

void DiscIo::wait_async(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
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
}

namespace {
// SCE IoFstat buffer (SCE_IO_FSTATBUFLEN): mode u16, dev u16, uid u16, gid u16,
// size u32, then ctime/atime/mtime as local date-times, zero-filled padding.
const std::uint32_t kFstatSize = 280u;
void install_stat_buffer(psprecomp::Runtime &runtime, std::uint32_t output, std::int64_t file_size) {
    runtime.memory().zero(output, kFstatSize);
    runtime.memory().store16(output + 0u, 0x81B3u);  // mode: regular file, rwx r-x r-x
    runtime.memory().store16(output + 2u, 0u);       // dev
    runtime.memory().store16(output + 4u, 0u);       // uid
    runtime.memory().store16(output + 6u, 0u);       // gid
    runtime.memory().store32(output + 8u,
        file_size >= 0u && file_size <= 0x7FFFFFFFll ? static_cast<std::uint32_t>(file_size) : 0u);
    std::tm local{};
    const auto now = std::time(nullptr);
    const auto *host_local = std::localtime(&now);
    if (host_local) local = *host_local;
    for (std::uint32_t stamp = 0x0Cu; stamp <= 0x24u; stamp += 0x0Cu) {
        runtime.memory().store16(output + stamp + 0u, static_cast<std::uint16_t>(local.tm_year + 1900));
        runtime.memory().store16(output + stamp + 2u, static_cast<std::uint16_t>(local.tm_mon + 1));
        runtime.memory().store16(output + stamp + 4u, static_cast<std::uint16_t>(local.tm_mday));
        runtime.memory().store16(output + stamp + 6u, static_cast<std::uint16_t>(local.tm_hour));
        runtime.memory().store16(output + stamp + 8u, static_cast<std::uint16_t>(local.tm_min));
        runtime.memory().store16(output + stamp + 10u, static_cast<std::uint16_t>(local.tm_sec));
    }
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
    try {
        const std::string psp_path = runtime.memory().read_c_string(ctx.gpr[4u], 1024u);
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
    install_stat_buffer(runtime, ctx.gpr[5u], file_size);
    ctx.set_gpr(2u, 0u);
}

} // namespace bbs
