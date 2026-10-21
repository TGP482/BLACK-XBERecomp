// the mixer pulled every 10 ms, its output dropped: the SDL3 sink lands in milestone 3
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "audio/output.h"

namespace audio {

namespace {

std::atomic<uint32_t> g_mode{kOutputAuto};
std::atomic<bool> g_paused{false};

}  // namespace

void StartOutput(std::function<void(float* out, uint32_t frames)> render) {
    std::thread([render] {
        constexpr uint32_t kFrames = kRate / 100;  // 10 ms
        std::vector<float> b(kFrames * kChannels);
        for (;;) {
            if (!g_paused.load(std::memory_order_relaxed)) render(b.data(), kFrames);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }).detach();
}

bool IsSurround() { return g_mode.load(std::memory_order_relaxed) == kOutputSurround; }
bool IsMono() { return g_mode.load(std::memory_order_relaxed) == kOutputMono; }
void SetOutputMode(uint32_t mode) { g_mode.store(mode, std::memory_order_relaxed); }
void SetPaused(bool paused) { g_paused.store(paused, std::memory_order_relaxed); }

}  // namespace audio
