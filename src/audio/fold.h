#pragma once
// The 5.1 mix folded for a device without 5.1 speakers: the matrix both outputs (XAudio2's voice matrix, the SDL
// stream's own fold) use
#include <cstdint>
#include <vector>

namespace audio {

constexpr float kCentreFold = 0.707f;

// L = FL + kCentreFold C + BL and R = FR + kCentreFold C + BR without the sub, on the first two outputs, row major:
// m[output * kChannels + source]. Mono, or a single output, hears the average of the two.
std::vector<float> FoldToStereo(uint32_t outputs, bool mono);

// frames of kChannels in, frames of outputs out, through m
void ApplyMatrix(const std::vector<float>& m, uint32_t outputs, const float* in, float* out, uint32_t frames);

}  // namespace audio
