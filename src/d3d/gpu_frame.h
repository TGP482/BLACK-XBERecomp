#pragma once
#include <cstdint>

namespace gpu {
// Render-thread control carried by Clear, like occlusion: color enables the weapon's depth clamp.
constexpr uint32_t kClearViewmodelDepthClamp = 0x200;
// until the GPU has drawn present number n, counted from 0; any thread
void WaitGpuFrame(uint64_t n);
}  // namespace gpu
