#include "thread_manager.hpp"

#include "display_state.hpp"
#include "framebuffer_capture.hpp"
#include "guest_calls.hpp"
#include "interrupts.hpp"
#include "psprecomp/common.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace bbs {
namespace {
ThreadManager *g_thread_manager{};

void set_success(psprecomp::AllegrexContext &ctx) { ctx.set_gpr(2u, 0u); }
void set_error(psprecomp::AllegrexContext &ctx, std::uint32_t error) { ctx.set_gpr(2u, error); }
bool thread_diag_enabled() {
    return std::getenv("PSPRECOMP_THREAD_DIAG") != nullptr ||
        std::getenv("PSPRECOMP_TRACE") != nullptr;
}
// PSP event-flag wait matching: mode bit 0 = OR (any bit set), else AND (all set).
bool event_flag_matches(std::uint32_t pattern, std::uint32_t requested, std::uint32_t mode) {
    if ((mode & 1u) != 0u) return (pattern & requested) != 0u;
    return (pattern & requested) == requested;
}
// Wait-consume modes: 0x20 clears the waited bits, 0x10 clears everything.
void consume_event_flag(std::uint32_t &pattern, std::uint32_t requested, std::uint32_t mode) {
    if ((mode & 0x20u) != 0u) pattern &= ~requested;
    if ((mode & 0x10u) != 0u) pattern = 0u;
}
}

void ThreadManager::initialize(psprecomp::Runtime &runtime,
                               std::uint32_t initial_stack_pointer) {
    runtime_ = &runtime;
    current_uid_ = 0;
    next_uid_ = 1;
    next_lw_mutex_uid_ = 0x500;
    next_semaphore_uid_ = 0x600;
    next_message_pipe_uid_ = 0x700;
    next_event_flag_uid_ = 0x900;
    next_stack_top_ = 0x09FF0000u;
    next_ready_sequence_ = 1u;
    threads_.clear();
    ready_.clear();
    delayed_.clear();
    switches_ = 0u;
    end_waiters_.clear();
    semaphores_.clear();
    message_pipes_.clear();
    event_flags_.clear();

    Thread main{};
    main.name = "module_start";
    main.priority = 32u;
    main.state = State::Running;
    main.stack_top = 0x0A000000u;
    main.stack_bottom = 0x09FF0000u;
    main.context = runtime.cpu();
    threads_.emplace(0, std::move(main));
    runtime.memory().zero(0x09FF0000u, 0x10000u);
    runtime.cpu().set_gpr(26u, 0x09FFFF00u);
    runtime.cpu().set_gpr(29u, initial_stack_pointer);

    g_thread_manager = this;
    runtime.register_function(0u, &ThreadManager::thread_return_trampoline,
                              "psp_thread_return");
    psprecomp::set_runtime_thread_identity(0, "module_start");
}

void ThreadManager::install_hles(psprecomp::Runtime &runtime) {
    runtime.register_hle("ThreadManForUser", 0x446D8DE6u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { create_thread(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0xF475845Du,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { start_thread(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x809CE29Bu,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { exit_delete_thread(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x278C0DF5u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { wait_thread_end(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x840E8133u,   // sceKernelWaitThreadEndCB: same wait, callback-interruptible == same under cooperative HLE
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { wait_thread_end(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x9FA03CD3u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { delete_thread(rt, ctx); });
    runtime.register_hle("ThreadManForUser", 0x293B45B8u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2u, static_cast<std::uint32_t>(current_uid_));
        });
    // The same sceKernelGetThreadId, imported through Kernel_Library too.
    runtime.register_hle("Kernel_Library", 0x293B45B8u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            ctx.set_gpr(2u, static_cast<std::uint32_t>(current_uid_));
        });
    // sceKernelReferThreadStatus(uid, SceKernelThreadInfo *): uid 0 = the caller.
    // The game fills info->size (108) first. Its heap setup (unit_0001
    // L_0880D6CC) reads info->stack (+48) and builds the allocator only on
    // success; the old UNKNOWN_THID stub left that allocator null (11/03).
    runtime.register_hle("ThreadManForUser", 0x17C1684Eu,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::int32_t uid = ctx.gpr[4u] == 0u ? current_uid_ : static_cast<std::int32_t>(ctx.gpr[4u]);
            const std::uint32_t info = ctx.gpr[5u];
            const auto found = threads_.find(uid);
            if (found == threads_.end()) { set_error(ctx, 0x80020198u); return; }   // UNKNOWN_THID
            if (!rt.memory().contains(info, 4u)) { set_error(ctx, 0x800200D3u); return; }
            const std::uint32_t size = rt.memory().load32(info);
            if (size < 104u || !rt.memory().contains(info, size)) { set_error(ctx, 0x800200D2u); return; }
            const Thread &thread = found->second;
            rt.memory().zero(info + 4u, size - 4u);
            for (std::uint32_t i = 0u; i < 31u && i < thread.name.size(); ++i)
                rt.memory().store8(info + 4u + i, static_cast<std::uint8_t>(thread.name[i]));
            std::uint32_t status = 0x02u;                                             // ready
            switch (thread.state) {
            case State::Running: status = 0x01u; break;
            case State::Sleeping: case State::Asleep: case State::Delayed: status = 0x04u; break;
            case State::Created: case State::Completed: status = 0x10u; break;     // dormant
            default: break;
            }
            rt.memory().store32(info + 36u, 0x80000000u);                             // attr: user mode
            rt.memory().store32(info + 40u, status);
            rt.memory().store32(info + 44u, thread.entry);
            rt.memory().store32(info + 48u, thread.stack_bottom);
            rt.memory().store32(info + 52u, thread.stack_top - thread.stack_bottom);
            rt.memory().store32(info + 56u, thread.context.gpr[28u]);
            rt.memory().store32(info + 60u, thread.priority);
            rt.memory().store32(info + 64u, thread.priority);
            set_success(ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x19CFF145u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t work = ctx.gpr[4u];
            if (work == 0u || !rt.memory().contains(work, 32u)) {
                set_error(ctx, 0x80020190u); return;
            }
            const std::uint32_t initial_count = ctx.gpr[7u];
            rt.memory().zero(work, 32u);
            rt.memory().store32(work, initial_count);
            rt.memory().store32(work + 4u, initial_count != 0u ? static_cast<std::uint32_t>(current_uid_) : 0u);
            rt.memory().store32(work + 8u, ctx.gpr[6u]);
            rt.memory().store32(work + 12u, 0u);
            rt.memory().store32(work + 16u, static_cast<std::uint32_t>(next_lw_mutex_uid_++));
            set_success(ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x60107536u,
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t work = ctx.gpr[4u];
            if (work == 0u || !rt.memory().contains(work, 20u)) {
                set_error(ctx, 0x80020190u); return;
            }
            rt.memory().zero(work, 20u);
            set_success(ctx);
        });
    runtime.register_hle("ThreadManForUser", 0xD6DA4BA1u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto initial = static_cast<std::int32_t>(ctx.gpr[6u]);
            const auto maximum = static_cast<std::int32_t>(ctx.gpr[7u]);
            if (maximum <= 0 || initial < 0 || initial > maximum) {
                set_error(ctx, 0x800201B0u); return;
            }
            const std::int32_t uid = next_semaphore_uid_++;
            semaphores_.emplace(uid, Semaphore{initial, maximum});
            if (thread_diag_enabled()) {
                const std::string name = ctx.gpr[4u] != 0u
                    ? rt.memory().read_c_string(ctx.gpr[4u], 64u) : "semaphore";
                std::cerr << "[pw-thread] create-sema uid=" << uid << " name=\"" << name
                          << "\" initial=" << initial << " max=" << maximum << "\n";
            }
            ctx.set_gpr(2u, static_cast<std::uint32_t>(uid));
        });
    runtime.register_hle("ThreadManForUser", 0x3F53E640u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto amount = static_cast<std::int32_t>(ctx.gpr[5u]);
            const auto it = semaphores_.find(uid);
            if (it == semaphores_.end()) { set_error(ctx, 0x80020199u); return; }
            if (amount <= 0 || static_cast<std::int64_t>(it->second.count) + amount > it->second.maximum) {
                set_error(ctx, 0x800201B0u); return;
            }
            it->second.count += amount;
            // Wake sleeping waiters in FIFO order, one per posted count (PW S2).
            while (!it->second.waiting.empty() && it->second.count > 0) {
                const std::int32_t waiter = it->second.waiting.front();
                it->second.waiting.erase(it->second.waiting.begin());
                it->second.count--;
                const auto found = threads_.find(waiter);
                if (found != threads_.end() && found->second.state == State::Sleeping)
                    enqueue(waiter, found->second.context);
            }
            set_success(ctx);
        });
    // sceKernelWaitSema (0x4E3A1105) and WaitSemaCB (0x6D212BAC): take the
    // count, or sleep until Post resumes us past the import with success
    // (PW S2, VCS vcs_profile.cpp:6022-6040). The plain one used to return
    // 0x800201A8 instead of waiting, which a PSP never does (review 10/28).
    for (const std::uint32_t nid : {0x4E3A1105u, 0x6D212BACu})
    runtime.register_hle("ThreadManForUser", nid,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto amount = static_cast<std::int32_t>(ctx.gpr[5u]);
            const auto it = semaphores_.find(uid);
            if (it == semaphores_.end()) { set_error(ctx, 0x80020199u); return; }
            if (amount <= 0 || amount > it->second.maximum) { set_error(ctx, 0x800201B0u); return; }
            if (it->second.count >= amount) {
                it->second.count -= amount;
                set_success(ctx);
                return;
            }
            const std::int32_t self = current_uid_;
            const auto current = threads_.find(self);
            if (current == threads_.end()) { rt.stop("BBS semaphore wait without a PSP thread"); return; }
            psprecomp::AllegrexContext suspended = ctx;
            suspended.set_gpr(2u, 0u);
            suspended.pc = ctx.gpr[31u]; // Post resumes us past the import.
            current->second.state = State::Sleeping;
            current->second.context = suspended;
            if (std::find(it->second.waiting.begin(), it->second.waiting.end(), self) == it->second.waiting.end())
                it->second.waiting.push_back(self);
            const std::string reason = "semaphore wait " + std::to_string(uid);
            if (!activate_next(rt, ctx, reason.c_str())) {
                dump_threads("semaphore wait");
                rt.stop("BBS semaphore deadlock waiting on " + std::to_string(uid));
            }
        });
    runtime.register_hle("ThreadManForUser", 0x28B6489Cu,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            if (semaphores_.erase(uid) == 0u) { set_error(ctx, 0x80020199u); return; }
            set_success(ctx);
        });
    // PSP mutex = semaphore with maximum 1. BBS passes initialCount=768 in $a1 and
    // stores the handle unconditionally, so create is lenient: any nonzero initial frees it.
    runtime.register_hle("ThreadManForUser", 0xB7D098C6u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto initial = static_cast<std::int32_t>(ctx.gpr[5u]);
            const std::int32_t uid = next_mutex_uid_++;
            semaphores_.emplace(uid, Semaphore{initial != 0 ? 1 : 0, 1});
            if (thread_diag_enabled()) {
                const std::string name = ctx.gpr[4u] != 0u
                    ? rt.memory().read_c_string(ctx.gpr[4u], 64u) : "mutex";
                std::cerr << "[bbs-thread] create-mutex uid=" << uid << " name=\"" << name
                          << "\" initial=" << initial << "\n";
            }
            ctx.set_gpr(2u, static_cast<std::uint32_t>(uid));
        });
    runtime.register_hle("ThreadManForUser", 0xB011B11Fu,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto it = semaphores_.find(uid);
            if (it == semaphores_.end()) { set_error(ctx, 0x80020199u); return; }
            if (it->second.count < 1) { set_error(ctx, 0x800201A8u); return; }
            it->second.count = 0;
            set_success(ctx);
        });
    // LockMutexCB: same as Lock (callbacks are not run here).
    runtime.register_hle("ThreadManForUser", 0x5BF4DD27u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto it = semaphores_.find(uid);
            if (it == semaphores_.end()) { set_error(ctx, 0x80020199u); return; }
            if (it->second.count < 1) { set_error(ctx, 0x800201A8u); return; }
            it->second.count = 0;
            set_success(ctx);
        });
    // TryLock: same non-blocking behavior as Lock; timeout arg ignored.
    runtime.register_hle("ThreadManForUser", 0x0DDCD2C9u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto it = semaphores_.find(uid);
            if (it == semaphores_.end()) { set_error(ctx, 0x80020199u); return; }
            if (it->second.count < 1) { set_error(ctx, 0x800201A8u); return; }
            it->second.count = 0;
            set_success(ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x6B30100Fu,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto it = semaphores_.find(uid);
            if (it == semaphores_.end()) { set_error(ctx, 0x80020199u); return; }
            if (it->second.count != 0) { set_error(ctx, 0x800201B0u); return; }
            it->second.count = 1;
            set_success(ctx);
        });
    runtime.register_hle("ThreadManForUser", 0xF8170FBEu,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            if (semaphores_.erase(uid) == 0u) { set_error(ctx, 0x80020199u); return; }
            set_success(ctx);
        });
    // sceKernelChangeThreadPriority(uid, priority) (VCS vcs_profile.cpp:5693-5745).
    // uid 0 = the caller, priority 0 = the caller's priority. A scheduling point:
    // if a ready thread now outranks the caller, the caller yields to it.
    runtime.register_hle("ThreadManForUser", 0x71BC9871u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::int32_t uid = ctx.gpr[4u] == 0u ? current_uid_ : static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto current = threads_.find(current_uid_);
            std::uint32_t priority = ctx.gpr[5u];
            if (priority == 0u && current != threads_.end()) priority = current->second.priority;
            const auto found = threads_.find(uid);
            if (found == threads_.end()) { set_error(ctx, 0x80020198u); return; }   // UNKNOWN_THID
            if (found->second.state == State::Created || found->second.state == State::Completed) {
                set_error(ctx, 0x800201A2u);                                       // DORMANT
                return;
            }
            if (priority < 0x08u || priority > 0x77u) { set_error(ctx, 0x80020193u); return; } // ILLEGAL_PRIORITY
            found->second.priority = priority;
            set_success(ctx);
            if (thread_diag_enabled())
                std::cerr << "[pw-thread] priority uid=" << uid << " value=" << priority << "\n";
            if (current == threads_.end()) return;
            const bool outranked = std::any_of(ready_.begin(), ready_.end(), [this, &current](const ReadyItem &item) {
                const auto t = threads_.find(item.uid);
                return t != threads_.end() && t->second.state == State::Ready &&
                       t->second.priority < current->second.priority;
            });
            if (!outranked) return;
            psprecomp::AllegrexContext caller = ctx;
            caller.pc = ctx.gpr[31u];
            enqueue(current_uid_, caller);
            (void)activate_next(rt, ctx, "change-priority");
        });
    runtime.register_hle("ThreadManForUser", 0x912354A7u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto current = threads_.find(current_uid_);
            // Priority 0 means "the caller's own priority".
            const std::uint32_t priority = ctx.gpr[4u] != 0u || current == threads_.end()
                ? ctx.gpr[4u] : current->second.priority;
            if (current == threads_.end() || current->second.priority != priority) {
                set_success(ctx);
                return;
            }
            const bool peer_ready = std::any_of(ready_.begin(), ready_.end(), [this, priority](const ReadyItem &item) {
                const auto found = threads_.find(item.uid);
                return found != threads_.end() && found->second.state == State::Ready &&
                    found->second.priority == priority;
            });
            if (!peer_ready) {
                // A spin-wait on rotate: let time pass so delayed threads can run.
                if (!delayed_.empty()) delay_yield(rt, ctx);
                else set_success(ctx);
                return;
            }
            psprecomp::AllegrexContext yielded = ctx;
            yielded.set_gpr(2u, 0u);
            yielded.pc = ctx.gpr[31u];
            enqueue(current_uid_, yielded);
            (void)activate_next(rt, ctx, "rotate-ready-queue");
        });
    const auto wake_pipe_waiters = [this](std::vector<std::int32_t> &waiters) {
        for (const std::int32_t uid : waiters) {
            const auto found = threads_.find(uid);
            if (found != threads_.end() && found->second.state == State::Sleeping)
                enqueue(uid, found->second.context);
        }
        waiters.clear();
    };
    const auto trace_pipe_io = [](psprecomp::Runtime &rt, const char *op, const MessagePipe &pipe,
                                  std::int32_t uid, std::uint32_t requested, std::uint32_t amount,
                                  std::uint32_t mode, std::uint32_t buffer) {
        static std::uint32_t traces = 0u;
        if (thread_diag_enabled() && traces++ < 160u) {
            std::cerr << "[pw-msgpipe] " << op << " uid=" << uid << " name=\"" << pipe.name
                      << "\" request=" << requested << " moved=" << amount << " queued="
                      << pipe.bytes.size() << "/" << pipe.capacity << " mode=" << mode << " data=";
            const auto shown = std::min(amount, 16u);
            for (std::uint32_t i = 0; i < shown; ++i)
                std::cerr << (i ? " " : "") << std::hex << static_cast<unsigned>(rt.memory().load8(buffer + i));
            std::cerr << std::dec << "\n";
        }
    };
    const auto suspend_pipe_waiter = [this](psprecomp::Runtime &rt,
                                            psprecomp::AllegrexContext &ctx,
                                            std::vector<std::int32_t> &waiters,
                                            const char *reason) {
        const std::int32_t uid = current_uid_;
        const auto current = threads_.find(uid);
        if (current == threads_.end()) { rt.stop("Message-pipe wait without a PSP thread"); return; }
        psprecomp::AllegrexContext suspended = ctx;
        suspended.pc = ctx.pc; // Re-enter the import once the pipe can satisfy it.
        current->second.state = State::Sleeping;
        current->second.context = suspended;
        if (std::find(waiters.begin(), waiters.end(), uid) == waiters.end()) waiters.push_back(uid);
        if (!activate_next(rt, ctx, reason))
            rt.stop(std::string("BBS message-pipe deadlock waiting on ") + reason);
    };
    runtime.register_hle("ThreadManForUser", 0x7C0DC2A0u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t size = ctx.gpr[7u];
            if (size == 0u || size > 0x10000000u) { set_error(ctx, 0x80020190u); return; }
            const std::int32_t uid = next_message_pipe_uid_++;
            const std::string name = ctx.gpr[4u] != 0u
                ? rt.memory().read_c_string(ctx.gpr[4u], 64u) : "message-pipe";
            message_pipes_.emplace(uid, MessagePipe{name, size, {}});
            if (thread_diag_enabled())
                std::cerr << "[pw-thread] create-msgpipe uid=" << uid << " name=\"" << name
                          << "\" part=" << ctx.gpr[5u] << " attr=0x" << std::hex << ctx.gpr[6u]
                          << " size=0x" << size << std::dec << "\n";
            ctx.set_gpr(2u, static_cast<std::uint32_t>(uid));
        });
    runtime.register_hle("ThreadManForUser", 0xF0B7DA1Cu,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            if (message_pipes_.erase(uid) == 0u) { set_error(ctx, 0x80020199u); return; }
            set_success(ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x876DBFADu,
        [this, wake_pipe_waiters, suspend_pipe_waiter, trace_pipe_io](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto found = message_pipes_.find(uid);
            const std::uint32_t source = ctx.gpr[5u];
            const std::uint32_t size = ctx.gpr[6u];
            if (found == message_pipes_.end() || size == 0u || !rt.memory().contains(source, size)) {
                set_error(ctx, found == message_pipes_.end() ? 0x80020199u : 0x80020190u); return;
            }
            // Arg 5 (int *result) is in t0; args 5-8 are t0-t3, not the stack.
            // Reading [sp+4] wrote the byte count through a stale stack word:
            // that was PW's "fd 4" in KJFS's device structure (10/27).
            const std::uint32_t result_ptr = ctx.gpr[8u];
            const auto available = found->second.capacity - static_cast<std::uint32_t>(found->second.bytes.size());
            const std::uint32_t amount = std::min(size, available);
            trace_pipe_io(rt, "send", found->second, uid, size, amount, ctx.gpr[7u], source);
            if (amount == 0u && ctx.gpr[7u] == 0u) {
                suspend_pipe_waiter(rt, ctx, found->second.waiting_writers, "message-pipe-send");
                return;
            }
            for (std::uint32_t i = 0; i < amount; ++i) found->second.bytes.push_back(rt.memory().load8(source + i));
            if (result_ptr != 0u && rt.memory().contains(result_ptr, 4u)) rt.memory().store32(result_ptr, amount);
            if (amount == 0u) { set_error(ctx, 0x800201A8u); return; }
            set_success(ctx);
            wake_pipe_waiters(found->second.waiting_readers);
        });
    runtime.register_hle("ThreadManForUser", 0x7C41F2C2u,
        [this, wake_pipe_waiters, suspend_pipe_waiter, trace_pipe_io](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto found = message_pipes_.find(uid);
            const std::uint32_t source = ctx.gpr[5u];
            const std::uint32_t size = ctx.gpr[6u];
            if (found == message_pipes_.end() || size == 0u || !rt.memory().contains(source, size)) {
                set_error(ctx, found == message_pipes_.end() ? 0x80020199u : 0x80020190u); return;
            }
            // Arg 5 (int *result) is in t0; args 5-8 are t0-t3, not the stack.
            // Reading [sp+4] wrote the byte count through a stale stack word:
            // that was PW's "fd 4" in KJFS's device structure (10/27).
            const std::uint32_t result_ptr = ctx.gpr[8u];
            const auto available = found->second.capacity - static_cast<std::uint32_t>(found->second.bytes.size());
            const std::uint32_t amount = std::min(size, available);
            trace_pipe_io(rt, "send-cb", found->second, uid, size, amount, ctx.gpr[7u], source);
            if (amount == 0u && ctx.gpr[7u] == 0u) {
                suspend_pipe_waiter(rt, ctx, found->second.waiting_writers, "message-pipe-send-cb");
                return;
            }
            for (std::uint32_t i = 0; i < amount; ++i) found->second.bytes.push_back(rt.memory().load8(source + i));
            if (result_ptr != 0u && rt.memory().contains(result_ptr, 4u)) rt.memory().store32(result_ptr, amount);
            if (amount == 0u) { set_error(ctx, 0x800201A8u); return; }
            set_success(ctx);
            wake_pipe_waiters(found->second.waiting_readers);
        });
    runtime.register_hle("ThreadManForUser", 0x884C9F90u,
        [this, wake_pipe_waiters, trace_pipe_io](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto found = message_pipes_.find(uid);
            const std::uint32_t source = ctx.gpr[5u];
            const std::uint32_t size = ctx.gpr[6u];
            if (found == message_pipes_.end() || size == 0u || !rt.memory().contains(source, size)) {
                set_error(ctx, found == message_pipes_.end() ? 0x80020199u : 0x80020190u); return;
            }
            // Arg 5 (int *result) is in t0; args 5-8 are t0-t3, not the stack.
            // Reading [sp+4] wrote the byte count through a stale stack word:
            // that was PW's "fd 4" in KJFS's device structure (10/27).
            const std::uint32_t result_ptr = ctx.gpr[8u];
            const auto available = found->second.capacity - static_cast<std::uint32_t>(found->second.bytes.size());
            const std::uint32_t amount = std::min(size, available);
            trace_pipe_io(rt, "try-send", found->second, uid, size, amount, ctx.gpr[7u], source);
            for (std::uint32_t i = 0; i < amount; ++i) found->second.bytes.push_back(rt.memory().load8(source + i));
            if (result_ptr != 0u && rt.memory().contains(result_ptr, 4u)) rt.memory().store32(result_ptr, amount);
            if (amount == 0u) { set_error(ctx, 0x800201A8u); return; }
            set_success(ctx);
            wake_pipe_waiters(found->second.waiting_readers);
        });
    runtime.register_hle("ThreadManForUser", 0x74829B76u,
        [this, wake_pipe_waiters, suspend_pipe_waiter, trace_pipe_io](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto found = message_pipes_.find(uid);
            const std::uint32_t destination = ctx.gpr[5u];
            const std::uint32_t size = ctx.gpr[6u];
            if (found == message_pipes_.end() || size == 0u || !rt.memory().contains(destination, size)) {
                set_error(ctx, found == message_pipes_.end() ? 0x80020199u : 0x80020190u); return;
            }
            // Arg 5 (int *result) is in t0; args 5-8 are t0-t3, not the stack.
            // Reading [sp+4] wrote the byte count through a stale stack word:
            // that was PW's "fd 4" in KJFS's device structure (10/27).
            const std::uint32_t result_ptr = ctx.gpr[8u];
            const std::uint32_t amount = std::min(size, static_cast<std::uint32_t>(found->second.bytes.size()));
            trace_pipe_io(rt, "receive", found->second, uid, size, amount, ctx.gpr[7u], destination);
            if (amount == 0u && ctx.gpr[7u] == 0u) {
                suspend_pipe_waiter(rt, ctx, found->second.waiting_readers, "message-pipe-receive");
                return;
            }
            for (std::uint32_t i = 0; i < amount; ++i) {
                rt.memory().store8(destination + i, found->second.bytes.front());
                found->second.bytes.pop_front();
            }
            if (result_ptr != 0u && rt.memory().contains(result_ptr, 4u)) rt.memory().store32(result_ptr, amount);
            // (KJFS's wrappers pass t0 = sp, so the result slot at [sp+0] is
            // filled through result_ptr above; no separate stack write.)
            if (amount == 0u) { set_error(ctx, 0x800201A8u); return; }
            ctx.set_gpr(2u, amount);
            wake_pipe_waiters(found->second.waiting_writers);
        });
    runtime.register_hle("ThreadManForUser", 0xFBFA697Du,
        [this, wake_pipe_waiters, suspend_pipe_waiter, trace_pipe_io](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto found = message_pipes_.find(uid);
            const std::uint32_t destination = ctx.gpr[5u];
            const std::uint32_t size = ctx.gpr[6u];
            if (found == message_pipes_.end() || size == 0u || !rt.memory().contains(destination, size)) {
                set_error(ctx, found == message_pipes_.end() ? 0x80020199u : 0x80020190u); return;
            }
            // Arg 5 (int *result) is in t0; args 5-8 are t0-t3, not the stack.
            // Reading [sp+4] wrote the byte count through a stale stack word:
            // that was PW's "fd 4" in KJFS's device structure (10/27).
            const std::uint32_t result_ptr = ctx.gpr[8u];
            const std::uint32_t amount = std::min(size, static_cast<std::uint32_t>(found->second.bytes.size()));
            trace_pipe_io(rt, "receive-cb", found->second, uid, size, amount, ctx.gpr[7u], destination);
            if (amount == 0u && ctx.gpr[7u] == 0u) {
                suspend_pipe_waiter(rt, ctx, found->second.waiting_readers, "message-pipe-receive-cb");
                return;
            }
            for (std::uint32_t i = 0; i < amount; ++i) {
                rt.memory().store8(destination + i, found->second.bytes.front());
                found->second.bytes.pop_front();
            }
            if (result_ptr != 0u && rt.memory().contains(result_ptr, 4u)) rt.memory().store32(result_ptr, amount);
            // (KJFS's wrappers pass t0 = sp, so the result slot at [sp+0] is
            // filled through result_ptr above; no separate stack write.)
            if (amount == 0u) { set_error(ctx, 0x800201A8u); return; }
            ctx.set_gpr(2u, amount);
            wake_pipe_waiters(found->second.waiting_writers);
        });
    runtime.register_hle("ThreadManForUser", 0xDF52098Fu,
        [this, wake_pipe_waiters, trace_pipe_io](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto found = message_pipes_.find(uid);
            const std::uint32_t destination = ctx.gpr[5u];
            const std::uint32_t size = ctx.gpr[6u];
            if (found == message_pipes_.end() || size == 0u || !rt.memory().contains(destination, size)) {
                set_error(ctx, found == message_pipes_.end() ? 0x80020199u : 0x80020190u); return;
            }
            // Arg 5 (int *result) is in t0; args 5-8 are t0-t3, not the stack.
            // Reading [sp+4] wrote the byte count through a stale stack word:
            // that was PW's "fd 4" in KJFS's device structure (10/27).
            const std::uint32_t result_ptr = ctx.gpr[8u];
            const std::uint32_t amount = std::min(size, static_cast<std::uint32_t>(found->second.bytes.size()));
            trace_pipe_io(rt, "try-receive", found->second, uid, size, amount, ctx.gpr[7u], destination);
            for (std::uint32_t i = 0; i < amount; ++i) {
                rt.memory().store8(destination + i, found->second.bytes.front());
                found->second.bytes.pop_front();
            }
            if (result_ptr != 0u && rt.memory().contains(result_ptr, 4u)) rt.memory().store32(result_ptr, amount);
            // (KJFS's wrappers pass t0 = sp, so the result slot at [sp+0] is
            // filled through result_ptr above; no separate stack write.)
            if (amount == 0u) { set_error(ctx, 0x800201A8u); return; }
            ctx.set_gpr(2u, amount);
            wake_pipe_waiters(found->second.waiting_writers);
        });
    const auto lock_lw_mutex = [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx,
                                      bool blocking) {
        const std::uint32_t work = ctx.gpr[4u];
        const std::uint32_t count = ctx.gpr[5u];
        if (work == 0u || count == 0u || !rt.memory().contains(work, 20u)) {
            set_error(ctx, 0x80020190u); return;
        }
        const std::uint32_t level = rt.memory().load32(work);
        const std::uint32_t owner = rt.memory().load32(work + 4u);
        const std::uint32_t attributes = rt.memory().load32(work + 8u);
        if (level == 0u) {
            rt.memory().store32(work, count);
            rt.memory().store32(work + 4u, static_cast<std::uint32_t>(current_uid_));
            set_success(ctx); return;
        }
        if (owner == static_cast<std::uint32_t>(current_uid_) && (attributes & 0x0200u) != 0u) {
            rt.memory().store32(work, level + count);
            set_success(ctx); return;
        }
        // Contended locking requires a scheduler wait queue; expose a PSP busy
        // error rather than corrupting the work area or claiming ownership.
        (void)blocking;
        set_error(ctx, 0x800201A8u);
    };
    runtime.register_hle("Kernel_Library", 0xBEA46419u,
        [lock_lw_mutex](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            lock_lw_mutex(rt, ctx, true);
        });
    runtime.register_hle("Kernel_Library", 0xDC692EE3u,
        [lock_lw_mutex](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            lock_lw_mutex(rt, ctx, false);
        });
    // LockLwMutexCB: same (work, count) shape; the callback arg (a2) is
    // ignored — if the mutex is free the lock succeeds and the callback is
    // never due; if contended, lock_lw_mutex reports the PSP busy error.
    runtime.register_hle("Kernel_Library", 0x1FC64E09u,
        [lock_lw_mutex](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            lock_lw_mutex(rt, ctx, true);
        });
    runtime.register_hle("Kernel_Library", 0x15B6446Bu,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t work = ctx.gpr[4u];
            const std::uint32_t count = ctx.gpr[5u];
            if (work == 0u || count == 0u || !rt.memory().contains(work, 20u)) {
                set_error(ctx, 0x80020190u); return;
            }
            const std::uint32_t level = rt.memory().load32(work);
            const std::uint32_t owner = rt.memory().load32(work + 4u);
            if (owner != static_cast<std::uint32_t>(current_uid_) || level < count) {
                set_error(ctx, 0x800201A8u); return;
            }
            const std::uint32_t updated = level - count;
            rt.memory().store32(work, updated);
            if (updated == 0u) rt.memory().store32(work + 4u, 0u);
            set_success(ctx);
        });
    // Event flags (VCS profile reference, adapted to BBS's re-entry suspend):
    // a sleeping waiter keeps its context at the import pc and re-enters the
    // wait handler on wake, re-checking the pattern there (like pipe waiters).
    const auto wake_event_flag_waiters = [this](EventFlag &flag) {
        if (flag.waiters.empty()) return;
        for (const std::int32_t waiter : flag.waiters) {
            const auto thread = threads_.find(waiter);
            if (thread != threads_.end() && thread->second.state == State::Sleeping)
                enqueue(waiter, thread->second.context);
        }
        flag.waiters.clear();
    };
    runtime.register_hle("ThreadManForUser", 0x55C20A00u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::string name = ctx.gpr[4u] != 0u
                ? rt.memory().read_c_string(ctx.gpr[4u], 64u) : "event-flag";
            const std::uint32_t attributes = ctx.gpr[5u];
            const std::uint32_t initial = ctx.gpr[6u];
            const std::int32_t uid = next_event_flag_uid_++;
            event_flags_.emplace(uid, EventFlag{name, attributes, initial, {}});
            if (thread_diag_enabled())
                std::cerr << "[bbs-thread] create-event-flag uid=" << uid << " name=\"" << name
                          << "\" attr=0x" << std::hex << attributes << " init=0x" << initial
                          << std::dec << "\n";
            ctx.set_gpr(2u, static_cast<std::uint32_t>(uid));
        });
    runtime.register_hle("ThreadManForUser", 0xEF9E4C70u,
        [this, wake_event_flag_waiters](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto found = event_flags_.find(uid);
            if (found == event_flags_.end()) { set_error(ctx, 0x80020199u); return; }
            wake_event_flag_waiters(found->second);
            // Woken waiters re-enter their wait and receive an invalid-uid error.
            event_flags_.erase(found);
            set_success(ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x1FB15A32u,
        [this, wake_event_flag_waiters](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto found = event_flags_.find(uid);
            if (found == event_flags_.end()) { set_error(ctx, 0x80020199u); return; }
            EventFlag &flag = found->second;
            flag.pattern |= ctx.gpr[5u];
            wake_event_flag_waiters(flag);
            set_success(ctx);
        });
    // PSP clear semantics: clear the bits present in the mask (pattern &= ~mask).
    runtime.register_hle("ThreadManForUser", 0x812346E4u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto found = event_flags_.find(uid);
            if (found == event_flags_.end()) { set_error(ctx, 0x80020199u); return; }
            found->second.pattern &= ~ctx.gpr[5u];
            set_success(ctx);
        });
    runtime.register_hle("ThreadManForUser", 0x30FD48F0u,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto found = event_flags_.find(uid);
            if (found == event_flags_.end()) { set_error(ctx, 0x80020199u); return; }
            const std::uint32_t requested = ctx.gpr[5u];
            const std::uint32_t mode = ctx.gpr[6u];
            const std::uint32_t output = ctx.gpr[7u];
            if (requested == 0u || (mode & ~0x31u) != 0u) { set_error(ctx, 0x800201B0u); return; }
            EventFlag &flag = found->second;
            if (!event_flag_matches(flag.pattern, requested, mode)) {
                if (output != 0u && rt.memory().contains(output, 4u))
                    rt.memory().store32(output, flag.pattern);
                set_error(ctx, 0x800201A8u);
                return;
            }
            if (output != 0u && rt.memory().contains(output, 4u))
                rt.memory().store32(output, flag.pattern);
            consume_event_flag(flag.pattern, requested, mode);
            set_success(ctx);
        });
    const auto event_flag_wait = [this](psprecomp::Runtime &rt,
                                        psprecomp::AllegrexContext &ctx,
                                        const char *reason) {
        const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
        const auto found = event_flags_.find(uid);
        if (found == event_flags_.end()) { set_error(ctx, 0x80020199u); return; }
        const std::uint32_t requested = ctx.gpr[5u];
        const std::uint32_t mode = ctx.gpr[6u];
        const std::uint32_t output = ctx.gpr[7u];
        if (requested == 0u || (mode & ~0x31u) != 0u) { set_error(ctx, 0x800201B0u); return; }
        EventFlag &flag = found->second;
        if (event_flag_matches(flag.pattern, requested, mode)) {
            if (output != 0u && rt.memory().contains(output, 4u))
                rt.memory().store32(output, flag.pattern);
            consume_event_flag(flag.pattern, requested, mode);
            set_success(ctx);
            return;
        }
        const auto current = threads_.find(current_uid_);
        if (current == threads_.end()) { rt.stop("Event-flag wait without a PSP thread"); return; }
        current->second.state = State::Sleeping;
        current->second.context = ctx; // Re-enter the import once the flag is set.
        if (std::find(flag.waiters.begin(), flag.waiters.end(), current_uid_) == flag.waiters.end())
            flag.waiters.push_back(current_uid_);
        if (!activate_next(rt, ctx, reason))
            rt.stop("BBS event-flag deadlock waiting on uid " + std::to_string(uid));
    };
    runtime.register_hle("ThreadManForUser", 0x402FCF22u,
        [event_flag_wait](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            event_flag_wait(rt, ctx, "event-flag-wait");
        });
    // WaitCB: callback arg ignored; the game's event-flag waits are blocking.
    runtime.register_hle("ThreadManForUser", 0x328C546Au,
        [event_flag_wait](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            event_flag_wait(rt, ctx, "event-flag-wait-cb");
        });
    // sceKernelDelayThread(CB): the duration moves virtual time on (due alarm
    // handlers run first); then the caller waits until no other thread is
    // ready (delay_yield). The CB callback argument is ignored.
    for (const std::uint32_t nid : {0xCEADEB47u, 0x68DA9E36u}) {
        runtime.register_hle("ThreadManForUser", nid,
            [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                if (psphost::deliver_alarms(ctx, ctx.gpr[4u])) return;
                delay_yield(rt, ctx);
            });
    }
    // sceDisplay vblank waits (bring-up): count the frame and wait like a
    // DelayThread (delay_yield: other ready threads run first), return 0.
    // Real 60 fps pacing and frame capture land later (S4/S11).
    for (const std::uint32_t nid : {0x984C27E7u, 0x46F186C3u, 0x36CDFADEu, 0x8EB9EC49u}) {
        runtime.register_hle("sceDisplay", nid,
            [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                // First the game's vblank interrupt handlers (guest code); they
                // resume this same import, which then counts the frame and waits.
                // (PW's frame limiter waits for a counter its handler increments.)
                if (psphost::deliver_vblank_interrupt(ctx)) return;
                ++vblank_count;
                // Frame dumps (roadmap phase 1): PPM files of changed frames
                // when PSPRECOMP_FRAME_DUMP_DIR is set (VCS's capture code,
                // as in vcs_profile.cpp ~7037).
                if (display_state.frame_buffer != 0u && display_state.buffer_width != 0u)
                    vcs::capture_frame_if_requested(rt.memory(), vcs::FramebufferDescription{
                        display_state.frame_buffer, display_state.width, display_state.height,
                        display_state.buffer_width, display_state.pixel_format});
                delay_yield(rt, ctx);
            });
    }
    // sceKernelSleepThread(CB): block until sceKernelWakeupThread (sleep_thread).
    // The CB variant doesn't run callbacks yet.
    for (const std::uint32_t nid : {0x9ACE131Eu, 0x82826F70u}) {
        runtime.register_hle("ThreadManForUser", nid,
            [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) { sleep_thread(rt, ctx); });
    }
    // sceKernelWakeupThread: resume a sleeping thread, or count the wakeup for
    // its next Sleep (wake_thread).
    runtime.register_hle("ThreadManForUser", 0xD59EAD2Fu,
        [this](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            wake_thread(rt, static_cast<std::int32_t>(ctx.gpr[4u]), ctx);
        });
    // sceKernelCancelWakeupThread(uid): drop pending wakeups, return how many there were.
    runtime.register_hle("ThreadManForUser", 0xFCCFAD26u,
        [this](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            const std::int32_t uid = ctx.gpr[4u] == 0u ? current_uid_ : static_cast<std::int32_t>(ctx.gpr[4u]);
            const auto found = threads_.find(uid);
            if (found == threads_.end()) { set_error(ctx, 0x80020198u); return; }   // UNKNOWN_THID
            ctx.set_gpr(2u, found->second.wakeup);
            found->second.wakeup = 0u;
        });
}

void ThreadManager::delay_yield(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    set_success(ctx);
    // Nothing else could run: the delay just passes.
    if (ready_.empty() && delayed_.empty()) return;
    const auto self = threads_.find(current_uid_);
    if (self == threads_.end()) {
        runtime.stop("BBS delay: delaying thread unknown");
        return;
    }
    // Park until the CPU is idle, so every ready peer runs first, even a
    // lower-priority one (user_main 19 vs DiscInfoBind 20, 10/27). A plain
    // yield put the thread straight back in ready_, and two delaying threads
    // then took turns forever above a lower-priority one (PW S2b, review 10/28).
    self->second.context = ctx;
    self->second.context.pc = ctx.gpr[31u]; // Resume past the import.
    self->second.state = State::Delayed;
    self->second.delay_due = switches_ + kDelaySwitches;
    delayed_.push_back(current_uid_);
    if (!activate_next(runtime, ctx, "delay")) {
        dump_threads("no thread to run after a delay");
        runtime.stop("BBS deadlock: no thread to run after a delay");
    }
}

void ThreadManager::dump_threads(const char *why) const {
    const auto state_name = [](State state) {
        switch (state) {
        case State::Created: return "created";
        case State::Ready: return "ready";
        case State::Running: return "running";
        case State::Sleeping: return "waiting (sema/pipe/flag/thread-end)";
        case State::Asleep: return "asleep (SleepThread, needs WakeupThread)";
        case State::Delayed: return "delayed";
        case State::Completed: return "completed";
        }
        return "?";
    };
    std::cerr << "[pw-thread] thread states (" << why << "):\n";
    for (const auto &[uid, thread] : threads_)
        std::cerr << "[pw-thread]   uid=" << uid << " name=" << thread.name
                  << " prio=" << thread.priority << " state=" << state_name(thread.state)
                  << " pc=" << psprecomp::hex32(thread.context.pc) << "\n";
}

void ThreadManager::sleep_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    set_success(ctx);
    const auto self = threads_.find(current_uid_);
    if (self == threads_.end()) {
        runtime.stop("BBS sleep: sleeping thread unknown");
        return;
    }
    if (self->second.wakeup > 0u) { // A WakeupThread came first: don't sleep.
        self->second.wakeup -= 1u;
        return;
    }
    self->second.context = ctx;
    self->second.context.pc = ctx.gpr[31u]; // WakeupThread resumes us past the import.
    self->second.state = State::Asleep;
    // Nothing emulated wakes a sleeper yet (no interrupts or GE callbacks run):
    // only the game's own WakeupThread calls do. Log it, so a thread that
    // never comes back is easy to find.
    if (thread_diag_enabled())
        std::cerr << "[pw-thread] asleep uid=" << current_uid_ << " name=" << self->second.name
                  << " ra=" << psprecomp::hex32(ctx.gpr[31u]) << "\n";
    if (!activate_next(runtime, ctx, "sleep")) {
        dump_threads("every thread waits");
        runtime.stop("BBS deadlock: every thread waits (SleepThread with no other thread to run)");
    }
}

void ThreadManager::wake_thread(psprecomp::Runtime &runtime, std::int32_t uid,
                                psprecomp::AllegrexContext &ctx) {
    const auto found = threads_.find(uid);
    if (found == threads_.end()) { set_error(ctx, 0x80020198u); return; } // INVALID_THREAD
    set_success(ctx);
    if (found->second.state != State::Asleep) {
        found->second.wakeup += 1u; // Its next Sleep returns at once.
        return;
    }
    enqueue(uid, found->second.context);
    // A woken thread that outranks the caller runs now (like StartThread),
    // except from an interrupt handler/callback, which runs to completion first
    // (psphost::in_guest_calls; the woken thread runs at the next switch).
    const auto caller = threads_.find(current_uid_);
    if (caller != threads_.end() && uid != current_uid_ && !psphost::in_guest_calls() &&
        found->second.priority < caller->second.priority) {
        psprecomp::AllegrexContext caller_context = ctx;
        caller_context.pc = ctx.gpr[31u];
        enqueue(current_uid_, caller_context);
        (void)activate_next(runtime, ctx, "wakeup");
    }
}

void ThreadManager::thread_return_trampoline(psprecomp::Runtime &runtime,
                                             psprecomp::AllegrexContext &ctx) {
    if (g_thread_manager == nullptr) {
        runtime.stop("PSP thread returned without an installed thread manager");
        return;
    }
    g_thread_manager->on_thread_return(runtime, ctx);
}

bool ThreadManager::allocate_stack(std::uint32_t size, std::uint32_t &bottom,
                                   std::uint32_t &top) {
    top = next_stack_top_ & ~0xFFu;
    if (size == 0u || top < size) return false;
    bottom = top - size;
    const std::uint32_t heap_end = heap_end_source_ ? heap_end_source_() : 0x08800000u;
    if (!runtime_->memory().contains(bottom, size) || bottom < heap_end) return false;
    next_stack_top_ = bottom;
    return true;
}

void ThreadManager::create_thread(psprecomp::Runtime &runtime,
                                  psprecomp::AllegrexContext &ctx) {
    const std::uint32_t name_ptr = ctx.gpr[4u];
    const std::uint32_t entry = ctx.gpr[5u];
    const std::uint32_t priority = ctx.gpr[6u];
    const std::uint32_t requested_stack = ctx.gpr[7u];
    if (requested_stack < 0x200u || requested_stack > 0x01000000u ||
        !runtime.has_function(entry)) {
        set_error(ctx, 0x80020194u);
        return;
    }
    const std::uint32_t stack_size = (requested_stack + 0xFFu) & ~0xFFu;
    std::uint32_t bottom = 0u;
    std::uint32_t top = 0u;
    if (!allocate_stack(stack_size, bottom, top)) {
        set_error(ctx, 0x80020190u);
        return;
    }

    Thread thread{};
    thread.name = name_ptr != 0u ? runtime.memory().read_c_string(name_ptr, 128u) : "unnamed";
    thread.entry = entry;
    thread.priority = priority;
    thread.stack_bottom = bottom;
    thread.stack_top = top;
    const std::int32_t uid = next_uid_++;
    runtime.memory().zero(bottom, stack_size);
    runtime.memory().store32(bottom, static_cast<std::uint32_t>(uid));
    threads_.emplace(uid, std::move(thread));
    ctx.set_gpr(2u, static_cast<std::uint32_t>(uid));
    if (thread_diag_enabled()) {
        const Thread &created = threads_.at(uid);
        std::cerr << "[pw-thread] create uid=" << uid << " name=" << created.name
                  << " entry=" << psprecomp::hex32(entry) << " priority=" << priority
                  << " stack=" << psprecomp::hex32(bottom) << "-" << psprecomp::hex32(top) << "\n";
    }
}

void ThreadManager::enqueue(std::int32_t uid,
                            const psprecomp::AllegrexContext &context) {
    const auto found = threads_.find(uid);
    if (found == threads_.end()) return;
    found->second.state = State::Ready;
    found->second.context = context;
    const auto existing = std::find_if(ready_.begin(), ready_.end(),
        [uid](const ReadyItem &item) { return item.uid == uid; });
    if (existing != ready_.end()) existing->sequence = next_ready_sequence_++;
    else ready_.push_back(ReadyItem{uid, next_ready_sequence_++});
}

bool ThreadManager::activate_next(psprecomp::Runtime &runtime,
                                  psprecomp::AllegrexContext &ctx,
                                  const char *reason) {
    ready_.erase(std::remove_if(ready_.begin(), ready_.end(), [this](const ReadyItem &item) {
        const auto found = threads_.find(item.uid);
        return found == threads_.end() || found->second.state != State::Ready;
    }), ready_.end());
    ++switches_;
    // Delays also end while other threads keep handing work to each other
    // (pipes, semas: ready_ never empties), after kDelaySwitches switches.
    if (!delayed_.empty()) {
        std::vector<std::int32_t> waiting;
        for (const std::int32_t uid : delayed_) {
            const auto found = threads_.find(uid);
            if (found == threads_.end() || found->second.state != State::Delayed) continue;
            if (found->second.delay_due <= switches_) enqueue(uid, found->second.context);
            else waiting.push_back(uid);
        }
        delayed_ = std::move(waiting);
    }
    if (ready_.empty() && !delayed_.empty()) {
        // Nothing can run, so time passes: the delayed threads' waits are over.
        // The thread that delayed last (the back) waits one more round when
        // others delayed before it, so two delaying threads alternate instead
        // of the higher-priority one winning every time.
        std::vector<std::int32_t> keep;
        if (delayed_.size() > 1u) keep.push_back(delayed_.back());
        for (std::size_t i = 0; i < delayed_.size() - keep.size(); ++i) {
            const auto found = threads_.find(delayed_[i]);
            if (found != threads_.end() && found->second.state == State::Delayed)
                enqueue(delayed_[i], found->second.context);
        }
        delayed_ = std::move(keep);
        if (ready_.empty() && !delayed_.empty()) { // Every earlier one was gone.
            const auto found = threads_.find(delayed_.front());
            if (found != threads_.end() && found->second.state == State::Delayed)
                enqueue(delayed_.front(), found->second.context);
            delayed_.clear();
        }
    }
    if (ready_.empty()) return false;

    const auto selected = std::min_element(ready_.begin(), ready_.end(), [this](const auto &a, const auto &b) {
        const std::uint32_t pa = threads_.at(a.uid).priority;
        const std::uint32_t pb = threads_.at(b.uid).priority;
        return pa != pb ? pa < pb : a.sequence < b.sequence;
    });
    const ReadyItem item = *selected;
    ready_.erase(selected);
    current_uid_ = item.uid;
    Thread &thread = threads_.at(item.uid);
    thread.state = State::Running;
    ctx = thread.context;
    psprecomp::set_runtime_thread_identity(item.uid, thread.name);
    if (thread_diag_enabled())
        std::cerr << "[pw-thread] switch reason=" << reason << " uid=" << item.uid
                  << " name=" << thread.name << " pc=" << psprecomp::hex32(ctx.pc) << "\n";
    (void)runtime;
    return true;
}

void ThreadManager::start_thread(psprecomp::Runtime &runtime,
                                 psprecomp::AllegrexContext &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
    const auto found = threads_.find(uid);
    if (uid <= 0 || found == threads_.end()) {
        set_error(ctx, 0x80020198u);
        return;
    }
    Thread &thread = found->second;
    if (thread.state != State::Created) {
        set_error(ctx, 0x800201A4u);
        return;
    }
    thread.wakeup = 0u; // A WakeupThread before the start doesn't count.

    const std::uint32_t argument_size = ctx.gpr[5u];
    const std::uint32_t argument_ptr = ctx.gpr[6u];
    std::uint32_t sp = thread.stack_top - 0x100u;
    psprecomp::AllegrexContext next{};
    if (argument_ptr != 0u && argument_size != 0u) {
        if (argument_size > 0x10000u || !runtime.memory().contains(argument_ptr, argument_size)) {
            set_error(ctx, 0x800200D3u);
            return;
        }
        const std::uint32_t aligned_size = (argument_size + 0xFu) & ~0xFu;
        if (sp < thread.stack_bottom + aligned_size + 64u) {
            set_error(ctx, 0x800200D3u);
            return;
        }
        sp -= aligned_size;
        std::vector<std::uint8_t> args(argument_size);
        runtime.memory().copy_out(argument_ptr, args);
        runtime.memory().copy_in(sp, args);
        next.set_gpr(4u, argument_size);
        next.set_gpr(5u, sp);
    }
    sp -= 64u;
    next.set_gpr(26u, thread.stack_top - 0x100u);
    next.set_gpr(28u, ctx.gpr[28u]);
    next.set_gpr(29u, sp);
    next.set_gpr(30u, sp);
    next.set_gpr(31u, 0u);
    next.pc = thread.entry;
    enqueue(uid, next);

    const std::int32_t caller_uid = current_uid_;
    const auto caller = threads_.find(caller_uid);
    const std::uint32_t caller_priority = caller != threads_.end() ? caller->second.priority : 0xFFFFFFFFu;
    if (thread.priority < caller_priority) {
        psprecomp::AllegrexContext caller_context = ctx;
        caller_context.set_gpr(2u, 0u);
        caller_context.pc = ctx.gpr[31u];
        enqueue(caller_uid, caller_context);
        (void)activate_next(runtime, ctx, "start-thread");
    } else {
        set_success(ctx);
    }
}

void ThreadManager::wake_thread_end_waiters(std::int32_t completed_uid) {
    const auto found = end_waiters_.find(completed_uid);
    if (found == end_waiters_.end()) return;
    for (const std::int32_t waiter_uid : found->second) {
        const auto waiter = threads_.find(waiter_uid);
        if (waiter != threads_.end() && waiter->second.state == State::Sleeping)
            enqueue(waiter_uid, waiter->second.context);
    }
    end_waiters_.erase(found);
}

void ThreadManager::on_thread_return(psprecomp::Runtime &runtime,
                                     psprecomp::AllegrexContext &ctx) {
    const std::int32_t finished_uid = current_uid_;
    if (const auto found = threads_.find(finished_uid); found != threads_.end())
        found->second.state = State::Completed;
    wake_thread_end_waiters(finished_uid);
    if (!activate_next(runtime, ctx, "thread-return")) {
        set_success(ctx);
        const bool all_done = std::all_of(threads_.begin(), threads_.end(),
            [](const auto &entry) {
                return entry.second.state == State::Completed || entry.second.state == State::Created;
            });
        if (!all_done) {
            dump_threads("thread returned, nothing left to run");
            runtime.stop("BBS deadlock: a thread returned and every other thread waits");
            return;
        }
        runtime.stop("All PSP threads completed");
    }
}

void ThreadManager::exit_delete_thread(psprecomp::Runtime &runtime,
                                       psprecomp::AllegrexContext &ctx) {
    if (const auto found = threads_.find(current_uid_); found != threads_.end())
        found->second.context.set_gpr(2u, ctx.gpr[4u]);
    on_thread_return(runtime, ctx);
}

void ThreadManager::wait_thread_end(psprecomp::Runtime &runtime,
                                    psprecomp::AllegrexContext &ctx) {
    const auto target_uid = static_cast<std::int32_t>(ctx.gpr[4u]);
    const auto target = threads_.find(target_uid);
    if (target_uid <= 0 || target == threads_.end()) {
        set_error(ctx, 0x80020198u);
        return;
    }
    if (target->second.state == State::Completed) {
        set_success(ctx);
        return;
    }
    const std::int32_t waiter_uid = current_uid_;
    psprecomp::AllegrexContext waiter = ctx;
    waiter.set_gpr(2u, 0u);
    waiter.pc = ctx.gpr[31u];
    end_waiters_[target_uid].push_back(waiter_uid);
    if (auto current = threads_.find(waiter_uid); current != threads_.end()) {
        current->second.state = State::Sleeping;
        current->second.context = waiter;
    }
    if (!activate_next(runtime, ctx, "wait-thread-end"))
        runtime.stop("BBS thread wait deadlock on uid " + std::to_string(target_uid));
}

void ThreadManager::delete_thread(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    const auto uid = static_cast<std::int32_t>(ctx.gpr[4u]);
    const auto found = threads_.find(uid);
    if (found == threads_.end()) {
        set_error(ctx, 0x80020198u);   // SCE_KERNEL_ERROR_INVALID_THREAD
        return;
    }
    if (found->second.state != State::Created && found->second.state != State::Completed) {
        set_error(ctx, 0x800201A4u);   // SCE_KERNEL_ERROR_NOT_DORMANT
        return;
    }
    end_waiters_.erase(uid);
    threads_.erase(found);
    set_success(ctx);
}

} // namespace bbs
