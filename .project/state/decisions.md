# Decision Log

| Timestamp | Topic | Selection | Rationale |
|-----------|-------|-----------|-----------|
| 2026-10-17T19:50:00-03:00 | Graphics Backend | Dedicated Vulkan (`gpu::Backend`) | vkd3d lacks runtime HLSL compiler and Win32 swapchain support on native Linux. Vulkan with shaderc provides native high-performance presentation. |
| 2026-10-17T20:00:00-03:00 | Window & Input | SDL3 | SDL3 handles Wayland/X11 windowing, Vulkan surfaces, audio, mouse capture, and keyboard cleanly. |
| 2026-10-17T20:30:00-03:00 | Shader Compilation | `shaderc` runtime compilation | Xbox dynamically generates HLSL strings; shaderc compiles HLSL directly to SPIR-V in memory with deterministic register bindings. |
| 2026-10-17T20:36:00-03:00 | Binary Resource Embedding | Inline ASM `.incbin` in `europe_patch.cpp` | Replaces Windows resource compiler `.rc` with cross-platform native gas assembly directive directly in C++ translation unit. |
| 2026-10-17T20:46:00-03:00 | Descriptor Management | Fixed `std::array` for image/sampler infos | Prevents pointer invalidation and dangling references during `vkUpdateDescriptorSets`. |