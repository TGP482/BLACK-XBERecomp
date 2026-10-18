// SDL3 audio output implementing audio/output.h
#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <future>
#include <thread>
#include <vector>

#include "audio/output.h"
#include "core/log.h"

namespace audio {

namespace {

constexpr float kCentreFold = 0.707f;

std::atomic<bool> g_surround{false};
std::atomic<bool> g_mono{false};
std::atomic<uint32_t> g_mode{kOutputAuto};
std::atomic<bool> g_mode_changed{false};
std::atomic<bool> g_paused{false};

void UpdateMode(int dev_channels) {
    uint32_t mode = g_mode;
    if (mode == kOutputAuto) {
        g_surround = dev_channels >= 6;
        g_mono = dev_channels == 1;
    } else {
        g_surround = mode == kOutputSurround;
        g_mono = mode == kOutputMono;
    }
}

void Pump(SDL_AudioStream* stream, int dev_channels, const std::function<void(float*, uint32_t)>& render) {
    constexpr uint32_t kFrames = kRate / 100; // 10ms = 480 frames
    constexpr uint32_t kSlots = 4;
    std::vector<float> bufs(kSlots * kFrames * kChannels);
    std::vector<float> processed(kFrames * kChannels);

    bool stopped = false;
    for (uint32_t n = 0;;) {
        if (g_mode_changed.exchange(false)) {
            UpdateMode(dev_channels);
        }

        if (g_paused != stopped) {
            stopped = !stopped;
            if (stopped) SDL_PauseAudioStreamDevice(stream);
            else SDL_ResumeAudioStreamDevice(stream);
        }

        if (stopped) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int queued = SDL_GetAudioStreamQueued(stream);
        if (queued >= int(3 * kFrames * kChannels * sizeof(float))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        float* b = &bufs[(n % kSlots) * kFrames * kChannels];
        render(b, kFrames);

        // Process audio according to selected mode:
        // kChannels order: FrontLeft(0), FrontRight(1), Centre(2), Sub(3), BackLeft(4), BackRight(5)
        if (g_mono) {
            for (uint32_t i = 0; i < kFrames; i++) {
                float* s = b + i * kChannels;
                float left = s[0] + kCentreFold * s[2] + s[4];
                float right = s[1] + kCentreFold * s[2] + s[5];
                float m = (left + right) * 0.5f;
                processed[i * kChannels + 0] = m;
                processed[i * kChannels + 1] = m;
                processed[i * kChannels + 2] = 0.0f;
                processed[i * kChannels + 3] = 0.0f;
                processed[i * kChannels + 4] = 0.0f;
                processed[i * kChannels + 5] = 0.0f;
            }
            SDL_PutAudioStreamData(stream, processed.data(), kFrames * kChannels * sizeof(float));
        } else if (!g_surround) {
            // Fold to stereo
            for (uint32_t i = 0; i < kFrames; i++) {
                float* s = b + i * kChannels;
                float left = s[0] + kCentreFold * s[2] + s[4];
                float right = s[1] + kCentreFold * s[2] + s[5];
                processed[i * kChannels + 0] = left;
                processed[i * kChannels + 1] = right;
                processed[i * kChannels + 2] = 0.0f;
                processed[i * kChannels + 3] = 0.0f;
                processed[i * kChannels + 4] = 0.0f;
                processed[i * kChannels + 5] = 0.0f;
            }
            SDL_PutAudioStreamData(stream, processed.data(), kFrames * kChannels * sizeof(float));
        } else {
            // Native 5.1 surround
            SDL_PutAudioStreamData(stream, b, kFrames * kChannels * sizeof(float));
        }

        n++;
    }
}

}  // namespace

bool IsSurround() { return g_surround; }

bool IsMono() { return g_mono; }

void SetOutputMode(uint32_t mode) {
    if (g_mode.exchange(mode) != mode) g_mode_changed = true;
}

void SetPaused(bool paused) { g_paused = paused; }

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::promise<void> opened;
    std::future<void> known = opened.get_future();
    std::thread([render, opened = std::move(opened)]() mutable {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            LOGE("audio: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
            opened.set_value();
            return;
        }

        SDL_AudioSpec src_spec{};
        src_spec.format = SDL_AUDIO_F32;
        src_spec.channels = kChannels;
        src_spec.freq = kRate;

        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &src_spec, nullptr, nullptr);
        if (!stream) {
            LOGE("audio: failed to open default playback audio stream: %s", SDL_GetError());
            opened.set_value();
            return;
        }

        SDL_AudioSpec dev_spec{};
        int dev_channels = 2;
        if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dev_spec, nullptr)) {
            dev_channels = dev_spec.channels;
        }
        UpdateMode(dev_channels);
        opened.set_value();

        SDL_ResumeAudioStreamDevice(stream);
        LOGI("audio: SDL3 audio stream opened (%d Hz, %d ch input -> %d ch device, mode: %s)",
             kRate, kChannels, dev_channels, g_mono ? "mono" : (g_surround ? "5.1" : "stereo"));

        Pump(stream, dev_channels, render);
    }).detach();
    known.wait();
}

}  // namespace audio
