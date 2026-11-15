// mixer output through an SDL3 audio stream, in place of XAudio2 (xaudio2.cpp)
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "platform/sdl_audio.h"
#include "audio/output.h"
#include "core/log.h"

namespace audio {

namespace {

// the console's stereo downmix in the DOLBY image: centre and back pair at -3 dB, no sub (xaudio2.cpp:25-27)
constexpr float kCentreFold = 0.707f;
constexpr float kBackFold = 0.707f;
// 10 ms blocks, the size the XAudio2 sink submits (xaudio2.cpp:166)
constexpr uint32_t kFrames = kRate / 100;

std::atomic<bool> g_surround{false};
std::atomic<bool> g_mono{false};
std::atomic<uint32_t> g_mode{kOutputAuto};
uint32_t g_diag_starved = 0;  // DIAG(audio): starves since the last line
uint64_t g_diag_at = 0;
std::atomic<bool> g_mode_changed{false};
std::atomic<bool> g_paused{false};

// what the device and the setting agree on: how many channels go to the stream
struct Plan {
    uint32_t channels = 2;
    bool surround = false;
    bool mono = false;
};

struct Output {
    SDL_AudioStream* stream = nullptr;
    Plan plan;
    // output channel per source channel, plan.channels rows of kChannels
    std::vector<float> matrix;
    std::vector<float> mix;
    std::vector<float> block;
};

const char* OutputName(const Plan& plan) {
    if (plan.mono) return "mono";
    return plan.surround ? "5.1" : "stereo";
}

// a device's channels before it is opened: macOS hands out stereo unless the output really is 5.1
uint32_t DeviceChannels() {
    SDL_AudioSpec spec{};
    if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr)) {
        LOGW("audio: no default device format: %s", SDL_GetError());
        return 2;
    }
    return spec.channels > 0 ? uint32_t(spec.channels) : 2u;
}

// only a device with the centre, the sub and the back pair hears the mix as 5.1 (xaudio2.cpp:47-57)
Plan ChoosePlan(uint32_t mode, uint32_t device_channels) {
    Plan plan;
    plan.mono = mode == kOutputMono;
    bool room = device_channels >= kChannels;
    bool want = mode == kOutputSurround || (mode == kOutputAuto && room);
    plan.surround = want && room;
    if (device_channels == 1) plan.channels = 1;
    else if (plan.surround) plan.channels = kChannels;
    else plan.channels = 2;
    return plan;
}

// every source channel on its own speaker: SDL's six channel layout is FL, FR, FC, LFE, BL, BR, the Speaker order
// (xaudio2.cpp:86-97 without the mask search)
void BuildMatrix(const Plan& plan, std::vector<float>& m) {
    m.assign(size_t(plan.channels) * kChannels, 0.0f);
    if (plan.surround) {
        for (uint32_t s = 0; s < kChannels; s++) m[size_t(s) * kChannels + s] = 1.0f;
        return;
    }
    // L = FL + kCentreFold C + kBackFold BL and R = FR + kCentreFold C + kBackFold BR without the sub; mono and a
    // single output hear the average of the two (xaudio2.cpp:99-117)
    static const float kLeft[kChannels] = {1, 0, kCentreFold, 0, kBackFold, 0};
    static const float kRight[kChannels] = {0, 1, kCentreFold, 0, 0, kBackFold};
    bool average = plan.mono || plan.channels == 1;
    for (uint32_t s = 0; s < kChannels; s++) {
        float left = average ? (kLeft[s] + kRight[s]) * 0.5f : kLeft[s];
        float right = average ? left : kRight[s];
        m[s] = left;
        if (plan.channels > 1) m[size_t(kChannels) + s] = right;
    }
}

// the open, the driver name log and the paused start are the shape of vendor/crownpark/audio/sdl_audio.cpp:37-51
bool Open(Output& out, Plan plan) {
    for (int attempt = 0; attempt < 2; attempt++) {
        SDL_AudioSpec spec{SDL_AUDIO_F32, int(plan.channels), int(kRate)};
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) {
            LOGE("audio: no output device, no sound: %s", SDL_GetError());
            return false;
        }
        SDL_AudioSpec device{};
        if (plan.surround && SDL_GetAudioStreamFormat(stream, nullptr, &device) && device.channels > 0 &&
            uint32_t(device.channels) < kChannels) {
            // the device took fewer channels than asked: fold here rather than let SDL claim a 5.1 mix
            SDL_DestroyAudioStream(stream);
            plan.surround = false;
            plan.channels = device.channels == 1 ? 1 : 2;
            continue;
        }
        out.stream = stream;
        out.plan = plan;
        BuildMatrix(plan, out.matrix);
        out.mix.assign(kFrames * kChannels, 0.0f);
        out.block.assign(kFrames * plan.channels, 0.0f);
        g_surround = plan.surround;
        g_mono = plan.mono;
        LOGI("audio: SDL %s, %s output, %u device channels", SDL_GetCurrentAudioDriver(), OutputName(plan),
             plan.channels);
        return true;
    }
    return false;
}

void Pump(Output& out, const std::function<void(float*, uint32_t)>& render) {
    // the XAudio2 pacing: 3 blocks queued, one more each time the queue runs dry, up to 10 (xaudio2.cpp:164-204)
    constexpr uint32_t kMinQueued = 3;
    constexpr uint32_t kMaxQueued = 10;
    uint32_t depth = kMinQueued;
    bool stopped = false;
    bool warned = false;
    uint64_t submitted = 0;
    SDL_ResumeAudioStreamDevice(out.stream);
    for (;;) {
        if (g_mode_changed.exchange(false)) {
            Plan plan = ChoosePlan(g_mode, DeviceChannels());
            if (plan.channels == out.plan.channels && plan.surround == out.plan.surround) {
                BuildMatrix(plan, out.matrix);
                out.plan = plan;
                g_surround = plan.surround;
                g_mono = plan.mono;
                LOGI("audio: %s output", OutputName(plan));
            } else {
                // a channel count change needs a new stream: the device starts it paused and empty
                Output fresh;
                if (Open(fresh, plan)) {
                    SDL_DestroyAudioStream(out.stream);
                    out = std::move(fresh);
                    depth = kMinQueued;
                    submitted = 0;
                    if (!stopped) SDL_ResumeAudioStreamDevice(out.stream);
                } else {
                    LOGE("audio: output mode change failed, keeping the device");
                }
            }
        }
        if (g_paused != stopped) {
            stopped = !stopped;
            if (stopped) SDL_PauseAudioStreamDevice(out.stream);
            else SDL_ResumeAudioStreamDevice(out.stream);
        }
        if (stopped) {
            SDL_Delay(10);
            continue;
        }
        uint32_t stride = out.plan.channels * uint32_t(sizeof(float));
        int queued = SDL_GetAudioStreamQueued(out.stream);
        if (queued < 0) {
            if (!warned) LOGE("audio: output queue unavailable: %s", SDL_GetError());
            warned = true;
            SDL_Delay(10);
            continue;
        }
        // a starved queue crackles: keep one block more each time, up to kMaxQueued
        if (submitted && !queued && depth < kMaxQueued) {
            depth++;
            g_diag_starved++;  // DIAG(audio): the per second line reports the starves
        }
        // DIAG(audio): queued depth and starves per second; the build otherwise reports an underrun only once
        if (const char* diag = std::getenv("XBE_DIAG")) {
            (void)diag;
            uint64_t now = SDL_GetTicks();
            if (now - g_diag_at >= 1000) {
                LOGR("diag: audio queued %d B (%.1f ms), depth %u, starved %u/s, device %u ch %u Hz",
                     queued, double(queued) / double(stride * kRate) * 1000.0, depth, g_diag_starved,
                     out.plan.channels, kRate);
                g_diag_at = now;
                g_diag_starved = 0;
            }
        }
        if (uint32_t(queued) / stride >= depth * kFrames) {
            // sleep for the audio still ahead of the target depth instead of a fixed 2 ms: the queue drains in real
            // time, so this is the exact wait, and one block keeps a mode change or a pause noticed promptly
            uint32_t excess = uint32_t(queued) / stride - depth * kFrames;
            SDL_Delay(int(std::min<uint32_t>(excess * 1000 / kRate + 1, 10)));
            continue;
        }
        render(out.mix.data(), kFrames);
        const uint32_t outputs = out.plan.channels;
        for (uint32_t f = 0; f < kFrames; f++) {
            const float* mix = &out.mix[size_t(f) * kChannels];
            float* block = &out.block[size_t(f) * outputs];
            for (uint32_t o = 0; o < outputs; o++) {
                const float* row = &out.matrix[size_t(o) * kChannels];
                float sum = 0.0f;
                for (uint32_t s = 0; s < kChannels; s++) sum += mix[s] * row[s];
                block[o] = sum;
            }
        }
        if (!SDL_PutAudioStreamData(out.stream, out.block.data(), int(out.block.size() * sizeof(float)))) {
            if (!warned) LOGE("audio: output write failed: %s", SDL_GetError());
            warned = true;
            SDL_Delay(10);
            continue;
        }
        submitted++;  // only a submitted block advances the mix
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
    std::thread([render = std::move(render), opened = std::move(opened)]() mutable {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            LOGE("audio: SDL audio unavailable, no sound: %s", SDL_GetError());
            opened.set_value();
            return;
        }
        Output out;
        bool ok = Open(out, ChoosePlan(g_mode, DeviceChannels()));
        opened.set_value();
        if (ok) Pump(out, render);
    }).detach();
    known.wait();
}

}  // namespace audio
