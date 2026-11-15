# Black (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(black LANGUAGES C CXX)
# the Windows executable's version resource
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)
# src/sound, src/d3d, src/audio, src/app/game_code.cpp and src/common/game_files.cpp replace the SDK ones
list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX "/(xdk/dsound|xdk/d3d8|gpu/d3d12|gpu/render_thread|audio/xaudio2|app/game_code|app/game_files)\\.cpp$")

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")
# Windows: D3D12, XAudio2 and Win32; macOS: Vulkan (src/vk), SDL3 audio and the SDL3 window
if(WIN32)
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/vk/|/src/platform/|_sdl\\.cpp$|/src/audio/fold\\.cpp$")
else()
    # src/platform holds the macOS stand-ins for the SDK's Win32-only parts (the shader compiler, the audio sink)
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX
        "/src/d3d/d3d12\\.cpp$|/src/app/|/src/audio/(xaudio2|sdl_audio)\\.cpp$|/src/common/(cursor|monitor)\\.cpp$|_win32\\.cpp$")
    include(cmake/macos.cmake)
endif()
# gyro aiming and the compiler window read the controllers through SDL
set_source_files_properties(src/common/gyro.cpp src/app/game_code.cpp
                            PROPERTIES INCLUDE_DIRECTORIES "${sdl3_SOURCE_DIR}/include")

# partitions as config/game.toml sets them
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# the game's own code needs its xbe: without assets/default.xbe only the host and the tools build
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe)
    add_xbe_image(Black default.xbe game.toml ${GAME_PARTITIONS} game)
    set_target_properties(Black PROPERTIES OUTPUT_NAME BLACK)
    if(APPLE)
        target_compile_options(Black PRIVATE -Wno-nullability-completeness)  # vma's annotations, not ours
    endif()
    if(WIN32)
        # build_info.h for the version resource, rewritten only when the commit changes
        find_package(Git QUIET)
        set(BUILD_INFO ${CMAKE_CURRENT_BINARY_DIR}/build_info.h)
        add_custom_target(build_info
            COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${BUILD_INFO} -DGIT=${GIT_EXECUTABLE}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_info.cmake
            BYPRODUCTS ${BUILD_INFO}
            VERBATIM)
        # version.rc also embeds src/common/europe.patch
        set_source_files_properties(src/common/version.rc PROPERTIES
            INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR};${CMAKE_CURRENT_SOURCE_DIR}/src/common"
            OBJECT_DEPENDS "${BUILD_INFO};${CMAKE_CURRENT_SOURCE_DIR}/src/common/europe.patch")
        target_sources(Black PRIVATE src/common/version.rc)
        add_dependencies(Black build_info)
    else()
        # europe.patch as an array in place of the Windows resource
        black_platform_image(Black)
        # CoreFoundation only, for the system language (src/common/game_info.cpp)
        if(APPLE)
            target_link_libraries(Black PRIVATE "-framework CoreFoundation")
        endif()
    endif()
else()
    message(STATUS "assets/default.xbe not found: building the host and the tools only")
endif()
