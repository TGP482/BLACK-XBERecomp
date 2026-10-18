# Black (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
if(WIN32)
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
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)
# src/sound, src/d3d, src/audio and src/common/game_files.cpp replace the SDK ones
list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX "/(xdk/dsound|xdk/d3d8|gpu/d3d12|gpu/render_thread|audio/xaudio2|app/game_files)\\.cpp$")
if(NOT WIN32)
    list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX "/app/main\\.cpp$")
endif()

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")
if(WIN32)
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/audio/sdl3\\.cpp$")
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/common/main_linux\\.cpp$")
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/d3d/vulkan\\.cpp$")
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/common/europe_patch\\.cpp$")
else()
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/audio/xaudio2\\.cpp$")
    list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/d3d/d3d12\\.cpp$")
endif()

# partitions as config/game.toml sets them
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

add_xbe_image(Black default.xbe game.toml ${GAME_PARTITIONS} game)

# build_info.h for the version resource, rewritten only when the commit changes
find_package(Git QUIET)
set(BUILD_INFO ${CMAKE_CURRENT_BINARY_DIR}/build_info.h)
add_custom_target(build_info
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${BUILD_INFO} -DGIT=${GIT_EXECUTABLE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_info.cmake
    BYPRODUCTS ${BUILD_INFO}
    VERBATIM)
if(WIN32)
    # version.rc also embeds src/common/europe.patch
    set_source_files_properties(src/common/version.rc PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR};${CMAKE_CURRENT_SOURCE_DIR}/src/common"
        OBJECT_DEPENDS "${BUILD_INFO};${CMAKE_CURRENT_SOURCE_DIR}/src/common/europe.patch")
    target_sources(Black PRIVATE src/common/version.rc)
else()
    set_source_files_properties(src/common/europe_patch.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/common/europe.patch")
endif()
add_dependencies(Black build_info)
set_target_properties(Black PROPERTIES OUTPUT_NAME BLACK)
