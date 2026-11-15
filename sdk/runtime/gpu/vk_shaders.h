#pragma once
// HLSL to SPIR-V through DXC (libdxcompiler.so beside the executable, else the system's), in place of D3DCompile.
// Registers keep the D3D12 root signature's slots as one descriptor set: b0 and b1 at bindings 0 and 1, t0 to t3 at
// 2 to 5, s0 to s3 at 6 to 9. SV_Position.w in a pixel shader is w as Direct3D gives it, not 1 / w.
#include <cstdint>
#include <string>
#include <vector>

namespace gpu::vk {

enum class Stage : uint8_t { Vertex, Pixel, Geometry };

constexpr uint32_t kBindingConstants = 0;  // b0, b1
constexpr uint32_t kBindingTextures = 2;   // t0 to t3
constexpr uint32_t kBindingSamplers = 6;   // s0 to s3
constexpr uint32_t kBindingCount = 10;

// SPIR-V words, empty on failure with the compiler's message in error
std::vector<uint32_t> CompileHlsl(const std::string& src, const char* entry, Stage stage, std::string& error);
// false, with the reason, when the compiler library can't be loaded
bool CompilerAvailable(std::string& error);

}  // namespace gpu::vk
