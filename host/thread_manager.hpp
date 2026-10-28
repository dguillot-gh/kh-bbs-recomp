#pragma once

#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <deque>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bbs {

class ThreadManager {
public:
    void initialize(psprecomp::Runtime &runtime, std::uint32_t initial_stack_pointer);
    void install_hles(psprecomp::Runtime &runtime);
    void set_heap_end_source(std::function<std::uint32_t()> source) { heap_end_source_ = std::move(source); }
    [[nodiscard]] std::uint32_t stack_floor() const noexcept { return next_stack_top_; }

private:
    enum class State { Created, Ready, Running, Sleeping, Completed };

    struct Thread {
        std::string name;
        std::uint32_t entry{};
        std::uint32_t priority{};
        std::uint32_t stack_bottom{};
        std::uint32_t stack_top{};
        State state{State::Created};
        std::uint64_t ready_sequence{};
        psprecomp::AllegrexContext context{};
        std::uint32_t wakeup{0u}; // sceKernelWakeupThread markers (PW S2b).
    };

    struct ReadyItem {
        std::int32_t uid{};
        std::uint64_t sequence{};
    };

    static void thread_return_trampoline(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void on_thread_return(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void create_thread(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void start_thread(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void exit_delete_thread(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void wait_thread_end(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void delete_thread(psprecomp::Runtime &, psprecomp::AllegrexContext &);
    void enqueue(std::int32_t uid, const psprecomp::AllegrexContext &context);
    bool activate_next(psprecomp::Runtime &, psprecomp::AllegrexContext &, const char *reason);
    void wake_thread_end_waiters(std::int32_t completed_uid);
    bool allocate_stack(std::uint32_t size, std::uint32_t &bottom, std::uint32_t &top);

    psprecomp::Runtime *runtime_{};
    std::int32_t current_uid_{};
    std::int32_t next_uid_{1};
    std::int32_t next_lw_mutex_uid_{0x500};
    std::int32_t next_semaphore_uid_{0x600};
    std::int32_t next_message_pipe_uid_{0x700};
    std::int32_t next_mutex_uid_{0x800};
    std::int32_t next_event_flag_uid_{0x900};
    std::uint32_t next_stack_top_{0x09FF0000u};
    std::uint64_t next_ready_sequence_{1u};
    std::function<std::uint32_t()> heap_end_source_;
    std::unordered_map<std::int32_t, Thread> threads_;
    std::deque<ReadyItem> ready_;
    std::unordered_map<std::int32_t, std::vector<std::int32_t>> end_waiters_;
    struct Semaphore { std::int32_t count{}; std::int32_t maximum{}; std::vector<std::int32_t> waiting; };
    std::unordered_map<std::int32_t, Semaphore> semaphores_;
    struct MessagePipe {
        std::string name;
        std::uint32_t capacity{};
        std::deque<std::uint8_t> bytes;
        std::vector<std::int32_t> waiting_readers;
        std::vector<std::int32_t> waiting_writers;
    };
    std::unordered_map<std::int32_t, MessagePipe> message_pipes_;
    // Event flags: waiters are sleeping thread uids; on wake they re-enter
    // the sceKernelWaitEventFlag import (context kept at the import pc, like
    // the message-pipe waiters) and re-check the pattern there.
    struct EventFlag {
        std::string name;
        std::uint32_t attributes{};
        std::uint32_t pattern{};
        std::vector<std::int32_t> waiters;
    };
    std::unordered_map<std::int32_t, EventFlag> event_flags_;
};

} // namespace bbs
