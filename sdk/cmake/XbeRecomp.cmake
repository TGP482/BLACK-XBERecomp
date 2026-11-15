# Original Xbox static recompilation: xbe_recomp, the host runtime and add_xbe_image()
include_guard(GLOBAL)

get_filename_component(XBE_SDK_DIR ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
# build scripts find the SDK's xdk folder and THIRD_PARTY.txt through this
file(TO_NATIVE_PATH "${XBE_SDK_DIR}" XBE_SDK_DIR_NATIVE)
file(WRITE ${CMAKE_BINARY_DIR}/sdk_dir.txt "${XBE_SDK_DIR_NATIVE}\n")

include(FetchContent)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis GIT_REPOSITORY https://github.com/zyantific/zydis.git GIT_TAG v4.1.1 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
# macOS takes the last 1.91 release: 1.91.0's SDL3 backend predates SDL 3.2's API; the core API is the same
if(WIN32)
    FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.0 GIT_SHALLOW TRUE)
else()
    FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9b GIT_SHALLOW TRUE)
endif()
# SDL3 for controllers, static so it links into the exe
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
# guest threads read the pads, which SDL's debug-only main thread checks reject
set(SDL_ASSERTIONS release CACHE STRING "" FORCE)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.16 GIT_SHALLOW TRUE)
if(WIN32)
    FetchContent_MakeAvailable(zydis tomlplusplus imgui SDL3)
else()
    # macOS: an installed SDL3 when there is one, else the fetched source; the fetched one builds the window and pads
    FetchContent_MakeAvailable(zydis tomlplusplus imgui)
    find_package(SDL3 3.2 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        FetchContent_MakeAvailable(SDL3)
    endif()
    # the Vulkan renderer's: the Vulkan Memory Allocator places its images and buffers (header only)
    FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(vma)
endif()

# What the game needs to build its code on the player's PC (runtime/app/game_code.cpp), beside the executables: the
# recompiler, clang and lld from the toolchain building this, the game's configs, the XDK layouts they include and the
# headers the generated code includes. Windows only: elsewhere the image is recompiled by CMake, not at first launch
set(XBE_COMPILER_DIR ${CMAKE_BINARY_DIR}/compiler)
get_filename_component(XBE_LLVM_BIN ${CMAKE_CXX_COMPILER} DIRECTORY)
file(GLOB XBE_GAME_CONFIGS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/config/*.toml)
set(XBE_COMPILER_HEADERS ${XBE_SDK_DIR}/runtime/cpu/x86_ops.h ${XBE_SDK_DIR}/runtime/cpu/x86_context.h)
file(GLOB XBE_FREESTANDING_HEADERS CONFIGURE_DEPENDS ${XBE_SDK_DIR}/runtime/cpu/freestanding/*)
file(GLOB XBE_LAYOUTS CONFIGURE_DEPENDS ${XBE_SDK_DIR}/xdk/*/layout.toml)
set(XBE_COPY_LAYOUTS)
foreach(layout ${XBE_LAYOUTS})
    get_filename_component(build_dir ${layout} DIRECTORY)
    get_filename_component(build ${build_dir} NAME)
    list(APPEND XBE_COPY_LAYOUTS COMMAND ${CMAKE_COMMAND} -E make_directory ${XBE_COMPILER_DIR}/xdk/${build}
         COMMAND ${CMAKE_COMMAND} -E copy_if_different ${layout} ${XBE_COMPILER_DIR}/xdk/${build})
endforeach()
if(WIN32)
    add_custom_target(compiler_files ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${XBE_COMPILER_DIR}/config ${XBE_COMPILER_DIR}/include/cpu
                ${XBE_COMPILER_DIR}/include/freestanding
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${XBE_LLVM_BIN}/clang.exe ${XBE_LLVM_BIN}/lld-link.exe
                ${XBE_SDK_DIR}/tools/LLVM-LICENSE.txt ${XBE_COMPILER_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${XBE_GAME_CONFIGS} ${XBE_COMPILER_DIR}/config
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${XBE_COMPILER_HEADERS} ${XBE_COMPILER_DIR}/include/cpu
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${XBE_FREESTANDING_HEADERS} ${XBE_COMPILER_DIR}/include/freestanding
        ${XBE_COPY_LAYOUTS}
        VERBATIM)
endif()

# x86 to C++ recompiler
file(GLOB XBE_RECOMP_SOURCES CONFIGURE_DEPENDS ${XBE_SDK_DIR}/tools/recomp/*.cpp)
add_executable(xbe_recomp ${XBE_RECOMP_SOURCES})
set_target_properties(xbe_recomp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${XBE_COMPILER_DIR})
target_include_directories(xbe_recomp PRIVATE ${XBE_SDK_DIR}/runtime ${XBE_SDK_DIR}/tools/recomp)
target_compile_definitions(xbe_recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(xbe_recomp PRIVATE Zydis tomlplusplus::tomlplusplus)

# XDK signature scanner that writes a new title's config
add_executable(xbe_symbols ${XBE_SDK_DIR}/tools/symbols/main.cpp)
target_include_directories(xbe_symbols PRIVATE ${XBE_SDK_DIR}/runtime)
target_compile_definitions(xbe_symbols PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(xbe_symbols PRIVATE Zydis tomlplusplus::tomlplusplus)

set(XBE_IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
if(WIN32)
    list(APPEND XBE_IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
else()
    list(APPEND XBE_IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
endif()

file(GLOB XBE_RUNTIME_SOURCES CONFIGURE_DEPENDS ${XBE_SDK_DIR}/runtime/*/*.cpp)
# each system's host: Win32 with D3D12 and XAudio2, or the SDL3 window (the game repo brings a Vulkan renderer)
if(WIN32)
    list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX "(_linux|_sdl)\\.cpp$|/app/sdl_keys\\.cpp$|/gpu/(vk_|vulkan)[a-z_]*\\.cpp$")
else()
    list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX
        "/app/(main|splash|handover|system_info|game_code)\\.cpp$|/gpu/(d3d12|vulkan[a-z_]*|vk_shaders)\\.cpp$|/audio/xaudio2\\.cpp$")
endif()
# SDL's headers go to the input code alone (it is linked LINK_ONLY)
set_source_files_properties(${XBE_SDK_DIR}/runtime/core/input.cpp PROPERTIES INCLUDE_DIRECTORIES "${sdl3_SOURCE_DIR}/include")

# One executable per recompiled image: the SDK runtime, XBE_HOST_SOURCES shared by every image of the game, and
# src/<game_dir> hooks only this image has. The image's code goes to <target>.dll beside it, which the executable delay
# loads: built here from config/<config> (partitions as that config sets them, library = "<target>.dll"), while a
# release leaves it out and the executable builds it from the player's xbe (runtime/app/game_code.cpp, which compiles
# with the same flags). Expects assets/<xbe> and the config's out = "generated/<game_dir>". At run time the game may be
# a disc image instead.
function(add_xbe_image target xbe config partitions game_dir)
    set(GEN ${CMAKE_CURRENT_SOURCE_DIR}/generated/${game_dir})
    set(GEN_SOURCES ${GEN}/x86_func_mapping.cpp)
    math(EXPR last "${partitions} - 1")
    foreach(i RANGE ${last})
        list(APPEND GEN_SOURCES ${GEN}/x86_recomp.${i}.cpp)
    endforeach()
    file(GLOB configs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/config/*.toml ${XBE_SDK_DIR}/xdk/*/*.toml)
    if(WIN32)
        add_custom_command(
            OUTPUT ${GEN}/codegen.stamp
            BYPRODUCTS ${GEN_SOURCES} ${GEN}/x86_recomp_shared.h ${GEN}/image_symbols.h ${GEN}/hook_exports.h
                       ${GEN}/exports.def ${GEN}/imports.def ${GEN}/code.lib
            COMMAND xbe_recomp config/${config} --xdk ${XBE_SDK_DIR}/xdk
            COMMAND ${XBE_LLVM_BIN}/lld-link.exe /lib /machine:x64 /def:${GEN}/imports.def /out:${GEN}/code.lib
            COMMAND ${CMAKE_COMMAND} -E touch ${GEN}/codegen.stamp
            DEPENDS xbe_recomp ${configs} ${CMAKE_CURRENT_SOURCE_DIR}/assets/${xbe}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Recompiling ${xbe}"
            VERBATIM)
    else()
        # no import library: the recompiled code is compiled into the executable
        add_custom_command(
            OUTPUT ${GEN}/codegen.stamp
            BYPRODUCTS ${GEN_SOURCES} ${GEN}/x86_recomp_shared.h ${GEN}/image_symbols.h
            COMMAND xbe_recomp config/${config} --xdk ${XBE_SDK_DIR}/xdk
            COMMAND ${CMAKE_COMMAND} -E touch ${GEN}/codegen.stamp
            DEPENDS xbe_recomp ${configs} ${CMAKE_CURRENT_SOURCE_DIR}/assets/${xbe}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Recompiling ${xbe}"
            VERBATIM)
        set_source_files_properties(${GEN_SOURCES} PROPERTIES GENERATED TRUE
            COMPILE_OPTIONS "-O2;-fno-strict-aliasing;-Wno-unused-label;-Wno-parentheses-equality;-Wno-unused-variable")
    endif()
    add_custom_target(recompile_${target} DEPENDS ${GEN}/codegen.stamp)
    file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/${game_dir}/*.cpp)
    # the xbe's SHA-1: the executable checks the game it finds is the release it was made from
    set(xbe_file ${CMAKE_CURRENT_SOURCE_DIR}/assets/${xbe})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${xbe_file})
    file(SHA1 ${xbe_file} xbe_sha1)
    set(XBE_HASH ${CMAKE_CURRENT_BINARY_DIR}/${target}_xbe.cpp)
    file(CONFIGURE OUTPUT ${XBE_HASH} CONTENT "extern const char kImageSha1[] = \"${xbe_sha1}\";\n")
    if(WIN32)
        add_executable(${target} WIN32 ${XBE_RUNTIME_SOURCES} ${XBE_HOST_SOURCES} ${GAME_SOURCES} ${XBE_IMGUI_SOURCES} ${XBE_HASH})
    else()
        add_executable(${target} ${XBE_RUNTIME_SOURCES} ${XBE_HOST_SOURCES} ${GAME_SOURCES} ${GEN_SOURCES}
            ${XBE_IMGUI_SOURCES} ${XBE_HASH})
        set_source_files_properties(${GEN_SOURCES} PROPERTIES GENERATED TRUE)
    endif()
    add_dependencies(${target} recompile_${target})
    if(WIN32)
        add_dependencies(${target} compiler_files)
        # the import library of what the executable exports to its DLL goes to the compiler folder
        set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON ARCHIVE_OUTPUT_DIRECTORY ${XBE_COMPILER_DIR})
    endif()
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${XBE_SDK_DIR}/runtime ${GEN})
    target_include_directories(${target} SYSTEM PRIVATE ${imgui_SOURCE_DIR})
    target_compile_options(${target} PRIVATE -fno-strict-aliasing)
    if(WIN32)
        target_compile_definitions(${target} PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(${target} PRIVATE tomlplusplus::tomlplusplus $<LINK_ONLY:SDL3::SDL3-static> d3d12 dxgi dxguid d3dcompiler xaudio2 ole32 windowscodecs dbghelp winmm dwmapi ws2_32)
        target_link_libraries(${target} PRIVATE ${GEN}/code.lib delayimp)
        target_link_options(${target} PRIVATE -fuse-ld=lld -g -Wl,/delayload:${target}.dll)
    else()
        # the Win32 names portable sources use, as values; SDL3 and Vulkan for the window and the renderer
        target_include_directories(${target} PRIVATE ${XBE_SDK_DIR}/runtime/platform/compat)
        # the MSVC runtime's names portable sources use
        target_compile_definitions(${target} PRIVATE _stricmp=strcasecmp _strnicmp=strncasecmp)
        target_compile_options(${target} PRIVATE -fdeclspec)
        if(XBE_DXC_DIR)
            target_include_directories(${target} SYSTEM PRIVATE ${XBE_DXC_DIR}/include)
        endif()
        target_link_libraries(${target} PRIVATE tomlplusplus::tomlplusplus SDL3::SDL3 Vulkan::Vulkan
            GPUOpen::VulkanMemoryAllocator pthread)
        if(APPLE)
            # Mach-O's ld64, which Apple's clang has; ld64.lld is not shipped with it
            target_link_options(${target} PRIVATE -g)
        else()
            target_link_options(${target} PRIVATE -fuse-ld=lld -g)
        endif()
    endif()

    if(WIN32)
        # without a C runtime: the executable passes on what the code calls
        add_library(${target}_code SHARED ${GEN_SOURCES})
        set_target_properties(${target}_code PROPERTIES OUTPUT_NAME ${target} CXX_EXTENSIONS OFF)
        set_source_files_properties(${GEN_SOURCES} PROPERTIES GENERATED TRUE)
        add_dependencies(${target}_code recompile_${target})
        target_include_directories(${target}_code PRIVATE ${XBE_SDK_DIR}/runtime ${GEN})
        target_include_directories(${target}_code SYSTEM PRIVATE ${XBE_SDK_DIR}/runtime/cpu/freestanding)
        target_compile_definitions(${target}_code PRIVATE X86_GAME_CODE)
        target_compile_options(${target}_code PRIVATE -fms-compatibility-version=19.40 -O2 -fno-strict-aliasing -nostdinc
            -nostdinc++ -w)
        target_link_libraries(${target}_code PRIVATE ${target})
        target_link_options(${target}_code PRIVATE -fuse-ld=lld -Wl,/noentry,/nodefaultlib,/opt:noicf
            -Wl,/def:${GEN}/exports.def)
        # a stamp from an earlier build on the executable's first start would outrank this DLL
        add_custom_command(TARGET ${target}_code POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f ${XBE_COMPILER_DIR}/work/${target}/built.txt)
    endif()
endfunction()
