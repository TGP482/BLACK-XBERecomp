#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace host {

// A host panel pauses guest execution and its clocks, while the window and
// renderer keep running. Never wait here while holding a host subsystem lock.
class PauseState {
public:
    using Clock = std::chrono::steady_clock;
    bool Paused() const { return paused_.load(std::memory_order_acquire); }
    void Set(bool paused) {
        {
            std::lock_guard lock(lock_);
            ChangeLocked(paused);
        }
        wake_.notify_all();
    }
    void Menu(bool requested) {
        {
            std::lock_guard lock(lock_);
            requested_ = requested;
            // An early R3 press must allow the GPU and its first frame to boot.
            if (!requested || frame_ready_) ChangeLocked(requested);
        }
        wake_.notify_all();
    }
    void FrameReady() {
        {
            std::lock_guard lock(lock_);
            if (frame_ready_) return;
            frame_ready_ = true;
            ChangeLocked(requested_);
        }
        wake_.notify_all();
    }
    void Wait() {
        if (!Paused()) return;
        std::unique_lock lock(lock_);
        wake_.wait(lock, [&] { return !Paused(); });
    }
    Clock::duration Excluded() const {
        std::lock_guard lock(lock_);
        return excluded_ + (Paused() ? Clock::now() - since_ : Clock::duration{});
    }
    Clock::time_point Now() const {
        std::lock_guard lock(lock_);
        return (Paused() ? since_ : Clock::now()) - excluded_;
    }
private:
    void ChangeLocked(bool paused) {
        if (Paused() == paused) return;
        const auto now = Clock::now();
        if (paused) since_ = now;
        else excluded_ += now - since_;
        paused_.store(paused, std::memory_order_release);
    }
    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::atomic<bool> paused_{false};
    Clock::time_point since_{};
    Clock::duration excluded_{};
    bool requested_ = false, frame_ready_ = false;
};

inline PauseState& Pause() { static PauseState state; return state; }
inline auto GameNow() { return Pause().Now(); }
inline int64_t GameNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(GameNow().time_since_epoch()).count();
}
}  // namespace host
