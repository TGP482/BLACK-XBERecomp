// SDL3 audio output: the platform side of audio/output.h where there is no XAudio2
#pragma once

#include <cstdint>
#include <functional>

#include "audio/output.h"

namespace audio {

// the four entry points of audio/output.h, implemented on an SDL3 audio stream: the mixer still pushes interleaved
// 5.1 float frames, this sink folds and submits them
void StartOutput(std::function<void(float* out, uint32_t frames)> render);
bool IsSurround();
bool IsMono();
void SetOutputMode(uint32_t mode);
void SetPaused(bool paused);

}  // namespace audio
