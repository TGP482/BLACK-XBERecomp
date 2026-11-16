#include "audio/fold.h"

#include "audio/output.h"

namespace audio {

std::vector<float> FoldToStereo(uint32_t outputs, bool mono) {
    static const float kLeft[kChannels] = {1, 0, kCentreFold, 0, 1, 0};
    static const float kRight[kChannels] = {0, 1, kCentreFold, 0, 0, 1};
    std::vector<float> m(outputs * kChannels, 0.0f);
    bool average = mono || outputs == 1;
    for (uint32_t s = 0; s < kChannels; s++) {
        float left = kLeft[s];
        float right = kRight[s];
        if (average) {
            left = (kLeft[s] + kRight[s]) * 0.5f;
            right = left;
        }
        m[s] = left;
        if (outputs > 1) m[kChannels + s] = right;
    }
    return m;
}

void ApplyMatrix(const std::vector<float>& m, uint32_t outputs, const float* in, float* out, uint32_t frames) {
    for (uint32_t f = 0; f < frames; f++) {
        const float* src = in + size_t(f) * kChannels;
        float* dst = out + size_t(f) * outputs;
        for (uint32_t o = 0; o < outputs; o++) {
            float v = 0;
            for (uint32_t s = 0; s < kChannels; s++) v += m[o * kChannels + s] * src[s];
            dst[o] = v;
        }
    }
}

}  // namespace audio
