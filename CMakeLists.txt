# Black (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(black LANGUAGES C CXX RC)

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
# The settings file is opened by the SDK. Preserve the native filesystem path type:
# path::string() can lose UTF-8 on Windows folders such as "Teste Tradução", causing
# toml++ to fail opening black.toml and silently keep every default setting.
set(SDK_SETTINGS_SOURCE "${SDK}/runtime/app/settings.cpp")
file(READ "${SDK_SETTINGS_SOURCE}" SDK_SETTINGS_TEXT)
set(SDK_SETTINGS_OLD "t = toml::parse_file(File().string());")
set(SDK_SETTINGS_NEW "t = toml::parse_file(File().c_str());")
string(FIND "${SDK_SETTINGS_TEXT}" "${SDK_SETTINGS_OLD}" SDK_SETTINGS_OLD_AT)
if(NOT SDK_SETTINGS_OLD_AT EQUAL -1)
    string(REPLACE "${SDK_SETTINGS_OLD}" "${SDK_SETTINGS_NEW}" SDK_SETTINGS_TEXT "${SDK_SETTINGS_TEXT}")
    file(WRITE "${SDK_SETTINGS_SOURCE}" "${SDK_SETTINGS_TEXT}")
else()
    string(FIND "${SDK_SETTINGS_TEXT}" "${SDK_SETTINGS_NEW}" SDK_SETTINGS_NEW_AT)
    if(SDK_SETTINGS_NEW_AT EQUAL -1)
        message(FATAL_ERROR "Could not apply the Unicode settings-path fix to ${SDK_SETTINGS_SOURCE}")
    endif()
endif()
# Use the verified, per-language font cache for the game's own D:\Language\Fonts reads when one is available.
# The original extracted game assets and disc image remain unchanged; absent cache files fall back to the game.
set(SDK_KERNEL_FILES_SOURCE "${SDK}/runtime/kernel/files.cpp")
file(READ "${SDK_KERNEL_FILES_SOURCE}" SDK_KERNEL_FILES_TEXT)
set(SDK_KERNEL_FILES_ORIGINAL "${SDK_KERNEL_FILES_TEXT}")
set(SDK_KERNEL_FILES_INCLUDE_OLD [=[#include "kernel/kernel.h"]=])
set(SDK_KERNEL_FILES_INCLUDE_NEW [=[#include "kernel/kernel.h"
#include "app/app.h"
#include "common/options.h"]=])
string(FIND "${SDK_KERNEL_FILES_TEXT}" "${SDK_KERNEL_FILES_INCLUDE_OLD}" SDK_KERNEL_FILES_INCLUDE_OLD_AT)
if(NOT SDK_KERNEL_FILES_INCLUDE_OLD_AT EQUAL -1)
    string(REPLACE "${SDK_KERNEL_FILES_INCLUDE_OLD}" "${SDK_KERNEL_FILES_INCLUDE_NEW}"
           SDK_KERNEL_FILES_TEXT "${SDK_KERNEL_FILES_TEXT}")
else()
    string(FIND "${SDK_KERNEL_FILES_TEXT}" "${SDK_KERNEL_FILES_INCLUDE_NEW}" SDK_KERNEL_FILES_INCLUDE_NEW_AT)
    if(SDK_KERNEL_FILES_INCLUDE_NEW_AT EQUAL -1)
        message(FATAL_ERROR "Could not add localized font override dependencies to ${SDK_KERNEL_FILES_SOURCE}")
    endif()
endif()
set(SDK_FONT_RESOLVE_OLD [=[Target Resolve(const std::string& xbox_path, uint32_t root_handle) {]=])
set(SDK_FONT_RESOLVE_NEW [=[std::string LocaleFontOverride(const std::vector<std::string>& parts) {
    if (parts.size() != 3 || Upper(parts[0]) != "LANGUAGE" || Upper(parts[1]) != "FONTS") return {};
    const std::string& code = game::BootLanguage();
    std::string locale;
    if (code == "fra") locale = "fr";
    else if (code == "deu") locale = "de";
    else if (code == "ita") locale = "it";
    else if (code == "esp") locale = "es";
    else if (code != "int") locale = code;
    if (locale.empty()) return {};
    const std::string filename = Upper(parts[2]);
    if (filename != "BIG.BIN" && filename != "SMALL.BIN") return {};
    fs::path path = app::ExeDir() / "language" / locale / "Localization" / "Fonts" / locale /
                    (filename == "BIG.BIN" ? "Big.bin" : "Small.bin");
    std::error_code ec;
    const bool available = fs::is_regular_file(path, ec);
    LOGI("language font lookup: %s -> %s", path.string().c_str(), available ? "override" : "disc fallback");
    return available ? path.string() : std::string{};
}

Target Resolve(const std::string& xbox_path, uint32_t root_handle) {]=])
string(FIND "${SDK_KERNEL_FILES_TEXT}" "std::string LocaleFontOverride(" SDK_FONT_RESOLVE_MARKER_AT)
if(SDK_FONT_RESOLVE_MARKER_AT EQUAL -1)
    string(FIND "${SDK_KERNEL_FILES_TEXT}" "${SDK_FONT_RESOLVE_OLD}" SDK_FONT_RESOLVE_OLD_AT)
    if(SDK_FONT_RESOLVE_OLD_AT EQUAL -1)
        message(FATAL_ERROR "Could not add localized font override lookup to ${SDK_KERNEL_FILES_SOURCE}")
    endif()
    string(REPLACE "${SDK_FONT_RESOLVE_OLD}" "${SDK_FONT_RESOLVE_NEW}"
           SDK_KERNEL_FILES_TEXT "${SDK_KERNEL_FILES_TEXT}")
endif()
set(SDK_FONT_DISC_OLD [=[    if (best->first == kCdRom)
        if (const Disc* disc = MountedDisc()) return {{}, true, disc->Find(parts)};]=])
set(SDK_FONT_DISC_NEW [=[    if (best->first == kCdRom) {
        const std::string font_override = LocaleFontOverride(parts);
        if (!font_override.empty()) {
            LOG_FIRST(4, "language font override: %s", font_override.c_str());
            return {font_override};
        }
        if (const Disc* disc = MountedDisc()) return {{}, true, disc->Find(parts)};
    }]=])
string(FIND "${SDK_KERNEL_FILES_TEXT}" "${SDK_FONT_DISC_OLD}" SDK_FONT_DISC_OLD_AT)
if(NOT SDK_FONT_DISC_OLD_AT EQUAL -1)
    string(REPLACE "${SDK_FONT_DISC_OLD}" "${SDK_FONT_DISC_NEW}" SDK_KERNEL_FILES_TEXT "${SDK_KERNEL_FILES_TEXT}")
else()
    string(FIND "${SDK_KERNEL_FILES_TEXT}" "${SDK_FONT_DISC_NEW}" SDK_FONT_DISC_NEW_AT)
    if(SDK_FONT_DISC_NEW_AT EQUAL -1)
        message(FATAL_ERROR "Could not add localized font override dispatch to ${SDK_KERNEL_FILES_SOURCE}")
    endif()
endif()
if(NOT SDK_KERNEL_FILES_TEXT STREQUAL SDK_KERNEL_FILES_ORIGINAL)
    file(WRITE "${SDK_KERNEL_FILES_SOURCE}" "${SDK_KERNEL_FILES_TEXT}")
endif()
# src/sound, src/d3d, src/audio and src/common/game_files.cpp replace the SDK ones
list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX "/(xdk/dsound|xdk/d3d8|gpu/d3d12|gpu/render_thread|audio/xaudio2|app/game_files)\\.cpp$")

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

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
if(tomlplusplus_SOURCE_DIR)
    target_include_directories(Black PRIVATE "${tomlplusplus_SOURCE_DIR}/vendor")
endif()
if(WIN32)
    target_link_libraries(Black PRIVATE winhttp bcrypt shell32)
endif()

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
set_target_properties(Black PROPERTIES OUTPUT_NAME BLACK)
