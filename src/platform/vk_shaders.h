#pragma once
// HLSL to SPIR-V at runtime, the stand in for D3DCompile: glslangValidator in a child process, keyed by the source
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gpu::vk {

// the stages the backend compiles for: the title's passes, and points as geometry
enum class Stage : uint8_t { Vertex = 0, Pixel = 1, Geometry = 2, Compute = 3 };

// the descriptor set the backend binds: b0 and b1 at bindings 0 and 1, t0 to t3 at 2 to 5, s0 to s3 at 6 to 9
constexpr uint32_t kBindingConstants = 0;  // b0, b1
constexpr uint32_t kBindingTextures = 2;   // t0 to t3
constexpr uint32_t kBindingSamplers = 6;   // s0 to s3
constexpr uint32_t kBindingCount = 10;

// src's entry point as SPIR-V words; empty with the validator's message in error when it refuses
std::vector<uint32_t> CompileHlsl(const std::string& src, const char* entry, Stage stage, std::string& error);
// false, with the reason, when no glslangValidator can be found
bool CompilerAvailable(std::string& error);
// the same, as a module on dev. VK_NULL_HANDLE when it doesn't compile
VkShaderModule CreateShaderModule(VkDevice dev, const std::string& src, const char* entry, Stage stage,
                                  std::string& error);
// vs, ps, gs, cs: the cache name and the command line's stage
const char* StageName(Stage stage);

// how this run went: modules built by glslang, and ones the disk cache answered
struct CacheStats {
    uint32_t compiled = 0;
    uint32_t cached = 0;
};
CacheStats Stats();

}  // namespace gpu::vk
