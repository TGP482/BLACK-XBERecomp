# macOS: the Vulkan renderer's shader compiler, europe.patch in place of the Windows resource, and the host's objects
# (Windows: D3D12, XAudio2, the Win32 window and the version resource)

# the Vulkan loader and MoltenVK, which draws the renderer's calls
find_package(Vulkan REQUIRED)

# the renderer compiles the SDK's HLSL with DXC, which ships no macOS build: point BLACK_DXC_DIR at an include/ with
# dxc/dxcapi.h and a lib/libdxcompiler.dylib to build it; without one the renderer's shaders cannot be built
set(BLACK_DXC_DIR "" CACHE PATH "DXC install (include/dxc/dxcapi.h, lib/libdxcompiler.dylib)")
if(BLACK_DXC_DIR AND EXISTS ${BLACK_DXC_DIR}/include/dxc/dxcapi.h)
    set(XBE_DXC_DIR ${BLACK_DXC_DIR})
else()
    message(STATUS "BLACK_DXC_DIR unset: src/vk's shader compiler has no dxcapi.h")
endif()

# europe.patch as kEuropePatch, written again when the patch changes
set(EUROPE_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/src/common/europe.patch)
set(EUROPE_CPP ${CMAKE_CURRENT_BINARY_DIR}/europe_patch.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${EUROPE_PATCH})
if(EXISTS ${EUROPE_PATCH})
    file(READ ${EUROPE_PATCH} europe_hex HEX)
    string(LENGTH "${europe_hex}" europe_len)
    math(EXPR europe_size "${europe_len} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," europe_bytes "${europe_hex}")
    file(CONFIGURE OUTPUT ${EUROPE_CPP} CONTENT "// generated from src/common/europe.patch\n#include <cstddef>\n#include <cstdint>\nnamespace app {\nextern const uint8_t kEuropePatch[] = {${europe_bytes}};\nextern const size_t kEuropePatchSize = ${europe_size};\n}  // namespace app\n")
endif()

# the macOS additions to an image's executable
function(black_platform_image target)
    if(EXISTS ${EUROPE_CPP})
        target_sources(${target} PRIVATE ${EUROPE_CPP})
    endif()
endfunction()

# the host on its own, without the game's recompiled code: what compiles without assets/default.xbe. The image above
# adds the same sources to its executable; this target exists so the host can be built and checked without the game.
add_library(black_host OBJECT ${XBE_RUNTIME_SOURCES} ${XBE_HOST_SOURCES} ${XBE_IMGUI_SOURCES})
target_include_directories(black_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${SDK}/runtime
    ${CMAKE_CURRENT_BINARY_DIR} ${SDK}/runtime/platform/compat)
target_include_directories(black_host SYSTEM PRIVATE ${imgui_SOURCE_DIR})
target_compile_options(black_host PRIVATE -fno-strict-aliasing)
# the SDK's guest-call macros spell their conventions __declspec; the screens' windows and the SDL name them as values
target_compile_options(black_host PRIVATE -fdeclspec)
target_compile_definitions(black_host PRIVATE _stricmp=strcasecmp _strnicmp=strncasecmp)
target_link_libraries(black_host PRIVATE tomlplusplus::tomlplusplus SDL3::SDL3 Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
