# Black (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
if(WIN32)
    # the version resource is a Windows resource
    project(black LANGUAGES C CXX RC)
else()
    project(black LANGUAGES C CXX)
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
    # pinned: the upstream main branch builds the game code into a DLL with the Windows LLVM tools, which the
    # Linux build has no use for. This is the last revision whose runtime and harness build here.
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git
                         GIT_TAG 99078b119b5d5e0d1d6afb1390e29437c0c984c0)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
if(NOT WIN32)
    # the CPU path draws into an SDL texture: SDL's renderer is off in the SDK's build. The SDK sets the cache
    # variable SDL_RENDER, which SDL3's own loop reads uppercased, so a plain variable here shadows it
    set(SDL_RENDER ON)
endif()
include(${SDK}/cmake/XbeRecomp.cmake)
# src/sound, src/d3d, src/audio and src/common/game_files.cpp replace the SDK ones
if(WIN32)
    list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX "/(xdk/dsound|xdk/d3d8|gpu/d3d12|gpu/render_thread|audio/xaudio2|app/game_files)\\.cpp$")
else()
    # the SDK's application shell is Windows only: src/linux replaces it
    list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX "/(xdk/dsound|xdk/d3d8|gpu/d3d12|gpu/render_thread|audio/xaudio2|app/[^/]+)\\.cpp$")
endif()

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")
if(WIN32)
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/linux/")
else()
    # the Windows only host sources; src/linux replaces them
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/(audio/xaudio2|d3d/d3d12|common/(cursor|focus_pause_win32|game_info_win32|menu_panel|monitor|mouse_keyboard))\\.cpp$")
endif()
# gyro aiming reads the controllers' sensors through SDL
set_source_files_properties(src/common/gyro.cpp PROPERTIES INCLUDE_DIRECTORIES "${sdl3_SOURCE_DIR}/include")

# partitions as config/game.toml sets them
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

if(NOT WIN32)
    # the F10 panel draws with ImGui and needs a GPU context the CPU path does not have: no ImGui on Linux yet
    set(XBE_IMGUI_SOURCES "")
endif()

add_xbe_image(Black default.xbe game.toml ${GAME_PARTITIONS} game)

if(NOT WIN32)
    # the SDK's link line is the Windows one: drop the libraries Linux does not have
    get_target_property(BLACK_LIBS Black LINK_LIBRARIES)
    list(REMOVE_ITEM BLACK_LIBS d3d12 dxgi dxguid d3dcompiler xaudio2 ole32 windowscodecs dbghelp winmm dwmapi ws2_32)
    list(APPEND BLACK_LIBS pthread)
    set_property(TARGET Black PROPERTY LINK_LIBRARIES "${BLACK_LIBS}")
    # lld is not needed here: on ELF a strong definition overrides a weak one, which is how the host
    # replaces the recompiled functions
    get_target_property(BLACK_LINK_OPTIONS Black LINK_OPTIONS)
    list(REMOVE_ITEM BLACK_LINK_OPTIONS -fuse-ld=lld)
    set_property(TARGET Black PROPERTY LINK_OPTIONS "${BLACK_LINK_OPTIONS}")
    # the SDK's harness rasterizer draws the frames on the CPU
    target_sources(Black PRIVATE ${SDK}/harness/reference_renderer.cpp)
    target_include_directories(Black PRIVATE ${SDK}/harness)
    # the CPU rasterizer reads the converted vertices, not the guest's own: the harness builds the SDK the same way
    target_compile_definitions(Black PRIVATE XBE_RAW_VERTICES=0 XBE_CPU_RASTERIZER=1)
    # the SDK's compat headers stand in for the few Win32 names the portable sources name
    target_include_directories(Black PRIVATE ${SDK}/harness/compat)
    # SDL3's headers, for the Linux host sources
    target_include_directories(Black PRIVATE ${sdl3_SOURCE_DIR}/include)
    # the SDK's POSIX memory path calls ftruncate without its header
    target_compile_options(Black PRIVATE -include unistd.h)
endif()

# build_info.h for the version resource, rewritten only when the commit changes
if(WIN32)
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
endif()
set_target_properties(Black PROPERTIES OUTPUT_NAME BLACK)
