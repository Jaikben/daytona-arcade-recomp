#pragma once

#include <SDL.h>
#include <cstdint>
#include <exception>
#ifdef __vita__
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace vita {

// A single in-flight sound frame. Main owns all submission/completion calls;
// the worker owns only execute_deferred_sound until finish joins that work.
// Keep this object alive until after finish, and finish before destroying the
// submitted game. The SDL playback callback still consumes converted samples.
class SoundWorker {
public:
    SoundWorker() = default;
    SoundWorker(const SoundWorker &) = delete;
    SoundWorker &operator=(const SoundWorker &) = delete;
    ~SoundWorker() { close(); }

    bool open() {
        close();
        mutex_ = SDL_CreateMutex();
        ready_ = SDL_CreateCond();
        completed_ = SDL_CreateCond();
        if (!mutex_ || !ready_ || !completed_) { close(); return false; }
        stopping_ = initialized_ = false;
        affinity_before_ = affinity_mask_ = affinity_result_ = 0;
        thread_ = SDL_CreateThreadWithStackSize(entry, "Daytona sound", 512u * 1024u, this);
        if (!thread_) { close(); return false; }
        SDL_LockMutex(mutex_);
        while (!initialized_) SDL_CondWait(completed_, mutex_);
        SDL_UnlockMutex(mutex_);
        return true;
    }
    bool threaded() const { return thread_ != nullptr; }
    int affinity_before() const { return affinity_before_; }
    int affinity_mask() const { return affinity_mask_; }
    int affinity_result() const { return affinity_result_; }

    template<class Game>
    void dispatch(Game &game) {
        dispatch(&game,
            [](void *p) { return static_cast<Game *>(p)->execute_deferred_sound(); },
            [](void *p, uint64_t ticks) { static_cast<Game *>(p)->complete_deferred_sound(ticks); });
    }

    // Completes profiling on the owner thread and rethrows worker failures
    // there, so the frontend's normal runtime-error path remains effective.
    void finish() {
        if (!thread_) return;
        SDL_LockMutex(mutex_);
        if (!busy_) { SDL_UnlockMutex(mutex_); return; }
        while (!done_) SDL_CondWait(completed_, mutex_);
        void *context = context_;
        Complete complete = complete_;
        const uint64_t ticks = ticks_;
        std::exception_ptr error = error_;
        context_ = nullptr;
        error_ = nullptr;
        busy_ = false;
        SDL_UnlockMutex(mutex_);
        complete(context, ticks);
        if (error) std::rethrow_exception(error);
    }

    void close() noexcept {
        // Runtime callers normally use finish so failures reach the UI.
        // Destruction must nevertheless join safely during stack unwinding.
        try { finish(); } catch (...) {}
        if (thread_) {
            SDL_LockMutex(mutex_);
            stopping_ = true;
            SDL_CondSignal(ready_);
            SDL_UnlockMutex(mutex_);
            SDL_WaitThread(thread_, nullptr);
            thread_ = nullptr;
        }
        if (completed_) SDL_DestroyCond(completed_);
        if (ready_) SDL_DestroyCond(ready_);
        if (mutex_) SDL_DestroyMutex(mutex_);
        completed_ = ready_ = nullptr;
        mutex_ = nullptr;
    }

private:
    using Execute = uint64_t (*)(void *);
    using Complete = void (*)(void *, uint64_t);
    SDL_Thread *thread_ = nullptr;
    SDL_mutex *mutex_ = nullptr;
    SDL_cond *ready_ = nullptr, *completed_ = nullptr;
    void *context_ = nullptr;
    Execute execute_ = nullptr;
    Complete complete_ = nullptr;
    uint64_t ticks_ = 0;
    std::exception_ptr error_;
    bool queued_ = false, busy_ = false, done_ = false, stopping_ = false, initialized_ = false;
    int affinity_before_ = 0, affinity_mask_ = 0, affinity_result_ = 0;

    void dispatch(void *context, Execute execute, Complete complete) {
        finish(); // also makes accidental re-submission safe
        if (!thread_) {
            uint64_t ticks = 0;
            std::exception_ptr error;
            try { ticks = execute(context); } catch (...) { error = std::current_exception(); }
            complete(context, ticks);
            if (error) std::rethrow_exception(error);
            return;
        }
        SDL_LockMutex(mutex_);
        context_ = context;
        execute_ = execute;
        complete_ = complete;
        done_ = false;
        queued_ = busy_ = true;
        SDL_CondSignal(ready_);
        SDL_UnlockMutex(mutex_);
    }

    static int SDLCALL entry(void *opaque) {
        auto &self = *static_cast<SoundWorker *>(opaque);
#ifdef __vita__
        const SceUID thread = sceKernelGetThreadId();
        self.affinity_before_ = sceKernelGetThreadCpuAffinityMask(thread);
        // Permit all application CPUs rather than inheriting a single CPU
        // from the rendering thread. Do not force either job onto one core.
        if (self.affinity_before_ < 0 ||
            (self.affinity_before_ & SCE_KERNEL_CPU_MASK_USER_ALL) != SCE_KERNEL_CPU_MASK_USER_ALL)
            self.affinity_result_ = sceKernelChangeThreadCpuAffinityMask(thread, SCE_KERNEL_CPU_MASK_USER_ALL);
        self.affinity_mask_ = sceKernelGetThreadCpuAffinityMask(thread);
#endif
        SDL_LockMutex(self.mutex_);
        self.initialized_ = true;
        SDL_CondSignal(self.completed_);
        for (;;) {
            while (!self.queued_ && !self.stopping_) SDL_CondWait(self.ready_, self.mutex_);
            if (self.stopping_) break;
            void *context = self.context_;
            Execute execute = self.execute_;
            self.queued_ = false;
            SDL_UnlockMutex(self.mutex_);
            uint64_t ticks = 0;
            std::exception_ptr error;
            try { ticks = execute(context); } catch (...) { error = std::current_exception(); }
            SDL_LockMutex(self.mutex_);
            self.ticks_ = ticks;
            self.error_ = error;
            self.done_ = true;
            SDL_CondSignal(self.completed_);
        }
        SDL_UnlockMutex(self.mutex_);
        return 0;
    }
};

} // namespace vita
