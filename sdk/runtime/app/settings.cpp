// the settings file beside the exe
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.custom_textures = t["custom_textures"].value_or(s.custom_textures);
    s.dump_textures = t["dump_textures"].value_or(s.dump_textures);
    s.texture_dump_format = std::min(t["texture_dump_format"].value_or(s.texture_dump_format), 1u);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    s.audio_output = std::min(t["audio_output"].value_or(s.audio_output), audio::kOutputMono);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
    audio::SetOutputMode(s.audio_output);
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"custom_textures", s.custom_textures},
                  {"dump_textures", s.dump_textures},
                  {"texture_dump_format", int64_t(s.texture_dump_format)},
                  {"join_ip", s.join_ip},
                  {"audio_output", int64_t(s.audio_output)}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

// written beside the real file and renamed over it, so a crash mid save keeps the old settings
void Save() {
    std::filesystem::path tmp = File();
    tmp += ".tmp";
    std::ofstream f(tmp);
    f << Table() << "\n";
    f.close();
#ifdef _WIN32
    if (f) MoveFileExW(tmp.c_str(), File().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    // the same rename over the old file; Windows' write-through is its own
    std::error_code ec;
    if (f) std::filesystem::rename(tmp, File(), ec);
#endif
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

#ifdef _WIN32
Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}
#endif  // elsewhere main_sdl.cpp: the SDL window's display

Mode Output() {
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

// Output for every frame: the desktop mode lookup reads the registry, so it runs again only when the resolution
// setting or the window's monitor changes
Mode CachedOutput() {
    struct Key {
        uint32_t width;
        uint32_t height;
#ifdef _WIN32
        HMONITOR monitor;
#else
        uintptr_t monitor;
#endif
        bool operator==(const Key&) const = default;
    };
    static std::mutex lock;
    static Key key{};
    static Mode mode{};
    const Settings& s = Get();
#ifdef _WIN32
    Key now{s.width, s.height, MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY)};
#else
    Key now{s.width, s.height, 0};  // the SDL window reports its display's mode itself
#endif
    std::lock_guard<std::mutex> guard(lock);
    if (!(now == key) || !mode.height) {
        key = now;
        mode = Output();
    }
    return mode;
}

// asked per frame and per HUD quad: CachedOutput costs a monitor lookup, the registry only on a change
bool Widescreen() {
    Mode m = CachedOutput();
    return m.width * 2 >= m.height * 3;  // 3:2 or wider
}

float RenderScale(uint32_t guest_height) {
    float h = CachedOutput().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app
