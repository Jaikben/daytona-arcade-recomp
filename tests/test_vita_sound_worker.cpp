// Runs the production sound worker against real host threads and SDL-shaped
// primitives. Checks ownership, overlap, lifecycle, failures, and exact tasks.
#include "../platform/vita/sound_worker.h"
#include "../src/runtime/frame_profile.h"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

struct SDL_mutex { std::mutex value; };
struct SDL_cond { std::condition_variable value; };
struct SDL_Thread { std::thread value; int result = 0; };

namespace mock {
int live_mutex = 0, live_cond = 0, live_thread = 0;
int allocation = 0, fail_at = 0;
bool fail() { return ++allocation == fail_at; }
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void reset(int fail_at = 0) {
    require(!live_mutex && !live_cond && !live_thread, "all worker resources released");
    allocation = 0; mock::fail_at = fail_at;
}
}
SDL_mutex *SDL_CreateMutex() {
    if (mock::fail()) return nullptr;
    ++mock::live_mutex; return new SDL_mutex;
}
void SDL_DestroyMutex(SDL_mutex *p) { --mock::live_mutex; delete p; }
int SDL_LockMutex(SDL_mutex *p) { p->value.lock(); return 0; }
int SDL_UnlockMutex(SDL_mutex *p) { p->value.unlock(); return 0; }
SDL_cond *SDL_CreateCond() {
    if (mock::fail()) return nullptr;
    ++mock::live_cond; return new SDL_cond;
}
void SDL_DestroyCond(SDL_cond *p) { --mock::live_cond; delete p; }
int SDL_CondWait(SDL_cond *c, SDL_mutex *m) {
    std::unique_lock<std::mutex> lock(m->value, std::adopt_lock);
    c->value.wait(lock); lock.release(); return 0;
}
int SDL_CondSignal(SDL_cond *p) { p->value.notify_one(); return 0; }
SDL_Thread *SDL_CreateThreadWithStackSize(int (*fn)(void *), const char *, size_t stack, void *p) {
    mock::require(stack == 512u * 1024u, "native sound worker has explicit 512 KiB stack");
    if (mock::fail()) return nullptr;
    auto *thread = new SDL_Thread;
    thread->value = std::thread([=] { thread->result = fn(p); });
    ++mock::live_thread; return thread;
}
void SDL_WaitThread(SDL_Thread *thread, int *result) {
    thread->value.join();
    if (result) *result = thread->result;
    --mock::live_thread; delete thread;
}

struct Job {
    const std::thread::id owner = std::this_thread::get_id();
    bool expect_worker = true, fail = false, released = true, started = false;
    std::mutex gate;
    std::condition_variable changed;
    int executed = 0, completed = 0;
    uint32_t state = 1;
    std::vector<uint32_t> samples;
    rt::FrameProfile profile{40, 7, 3, 0};

    uint64_t execute_deferred_sound() {
        mock::require((std::this_thread::get_id() != owner) == expect_worker, "execute on expected thread");
        {
            std::unique_lock<std::mutex> lock(gate);
            started = true; changed.notify_all();
            changed.wait(lock, [&] { return released; });
        }
        ++executed;
        if (fail) throw std::runtime_error("sound job failure");
        for (int i = 0; i < 1536; ++i) {
            state = state * 1664525u + 1013904223u;
            samples.push_back(state);
        }
        return 17;
    }
    void complete_deferred_sound(uint64_t ticks) {
        mock::require(std::this_thread::get_id() == owner, "completion on owner thread");
        mock::require(ticks == (fail ? 0u : 17u), "sound timing passed to owner");
        profile.sound += ticks; profile.total += ticks;
        mock::require(profile.core() == 30, "overlap preserves core profile");
        ++completed;
    }
    void wait_started() {
        std::unique_lock<std::mutex> lock(gate);
        changed.wait(lock, [&] { return started; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(gate);
        released = true; changed.notify_all();
    }
};

int main() {
    try {
        // Dispatch returns while work is blocked: independent rendering can
        // proceed before finish. Completion must remain on the owner thread.
        mock::reset();
        {
            vita::SoundWorker worker;
            mock::require(worker.open() && worker.threaded(), "thread created");
            Job job; job.released = false;
            worker.dispatch(job);
            job.wait_started();
            mock::require(job.completed == 0, "not completed before owner joins");
            job.release(); worker.finish();
            mock::require(job.executed == 1 && job.completed == 1, "exactly one sound frame");
            worker.finish(); // idle joins do not replay the frame
            mock::require(job.completed == 1, "repeated finish is idempotent");

            Job reference; reference.expect_worker = false;
            Job threaded;
            for (int frame = 0; frame < 200; ++frame) {
                worker.dispatch(threaded);
                reference.complete_deferred_sound(reference.execute_deferred_sound());
                worker.finish();
            }
            mock::require(threaded.samples == reference.samples, "all samples identical after overlapped frames");
            mock::require(threaded.executed == 200 && threaded.completed == 200, "no skipped or extra sound frames");

            Job failed; failed.fail = true;
            worker.dispatch(failed);
            bool caught = false;
            try { worker.finish(); } catch (const std::runtime_error &) { caught = true; }
            mock::require(caught && failed.completed == 1, "worker exception propagates after owner completion");
            Job recovered;
            worker.dispatch(recovered); worker.finish();
            mock::require(recovered.completed == 1, "worker remains usable after exception");
        }
        mock::reset();

        // Resource failures preserve audio via the identical synchronous task.
        for (int fail = 1; fail <= 4; ++fail) {
            mock::reset(fail);
            vita::SoundWorker worker;
            mock::require(!worker.open() && !worker.threaded(), "allocation failure falls back");
            Job job; job.expect_worker = false;
            worker.dispatch(job); worker.finish();
            mock::require(job.executed == 1 && job.completed == 1, "fallback executes full sound frame");
            worker.close();
        }
        mock::reset();

        // Destruction waits for in-flight work, then destroys synchronization.
        Job draining; draining.released = false;
        std::thread release;
        {
            vita::SoundWorker worker;
            mock::require(worker.open(), "reopen worker");
            worker.dispatch(draining);
            release = std::thread([&] { draining.wait_started(); draining.release(); });
        }
        release.join();
        mock::require(draining.executed == 1 && draining.completed == 1, "shutdown drains pending frame");
        mock::reset();
        std::puts("PASS: sound worker overlap, exact tasks/samples, profile ownership, fallback, exception propagation and joined shutdown");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
