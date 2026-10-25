// Per voice DLS2 low pass, the filter IDirectSoundBuffer_SetFilter sets in the voice's FCA and FCB registers
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sound {

// DSFILTERDESC mode, low two bits
constexpr uint32_t kFilterBypass = 0;
constexpr uint32_t kFilterDls2 = 1;
constexpr uint32_t kFilterPeq = 2;
constexpr uint32_t kFilterI3dl2 = 3;  // 3D voices; mono 2D voices read it as DLS2 plus PEQ

struct FilterChannel {
    float frequency = 1;
    float damping = 1;
    float low = 0;
    float band = 0;

    // only the low word of each coefficient reaches the register
    void Configure(uint32_t cutoff, uint32_t resonance) {
        float octaves = float(int16_t(cutoff & 0xFFFF)) / 4096.0f;
        frequency = std::clamp(std::exp2(octaves), 1.0f / 256, 1.0f);
        damping = std::clamp(float(resonance & 0xFFFF) / 32768.0f, 0.079407f, 1.0f);
    }

    // state variable low pass at the mixer rate
    float Run(float input) {
        float high = input - low - damping * band;
        band += frequency * high;
        low += frequency * band;
        if (!std::isfinite(low) || !std::isfinite(band)) {
            low = 0;
            band = 0;
            return 0;
        }
        if (std::fabs(low) < 1e-20f) low = 0;  // no denormals on silent voices
        if (std::fabs(band) < 1e-20f) band = 0;
        return std::clamp(low, -1.0f, 1.0f);
    }
};

struct VoiceFilter {
    FilterChannel channel[2];  // the second coefficient pair is a stereo voice's right channel
    bool enabled = false;

    // false for PEQ, which keeps the last accepted filter
    bool Set(uint32_t mode, const uint32_t coefficients[4], bool is3d, uint32_t channels) {
        bool mono_peq = mode == kFilterI3dl2 && !is3d && channels == 1;
        if (mode == kFilterPeq || mono_peq) return false;
        enabled = mode == kFilterDls2;
        channel[0].Configure(coefficients[0], coefficients[1]);
        channel[1].Configure(coefficients[2], coefficients[3]);
        return true;
    }

    void Reset() {
        for (FilterChannel& c : channel) {
            c.low = 0;
            c.band = 0;
        }
    }

    void Run(float samples[2], uint32_t channels) {
        if (!enabled) return;
        samples[0] = channel[0].Run(samples[0]);
        if (channels == 1) {
            samples[1] = samples[0];
            return;
        }
        samples[1] = channel[1].Run(samples[1]);
    }
};

}  // namespace sound
