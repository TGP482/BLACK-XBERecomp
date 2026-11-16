// Options::monitor through SDL3 (Linux; monitor.cpp on Windows): monitors by name, their modes and refresh rate
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

#include "common/aspect.h"
#include "common/monitor.h"
#include "common/options.h"

namespace game {

namespace {

// the setting, copied under the panel's lock
std::string Monitor() {
    std::lock_guard lock(app::UiLock());
    return Opt().monitor;
}

// a display's name in the settings: its own name, numbered when two share one
std::vector<std::pair<SDL_DisplayID, std::string>> Named() {
    std::vector<std::pair<SDL_DisplayID, std::string>> out;
    int n = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&n);
    for (int i = 0; i < n; i++) {
        const char* name = SDL_GetDisplayName(ids[i]);
        std::string s = name && *name ? name : "Display";
        int same = int(std::count_if(out.begin(), out.end(), [&](const auto& o) { return o.second.rfind(s, 0) == 0; }));
        if (same) s += " #" + std::to_string(same + 1);
        out.emplace_back(ids[i], s);
    }
    SDL_free(ids);
    return out;
}

// the primary for empty or one not connected
SDL_DisplayID DisplayOf(const std::string& device) {
    for (const auto& [id, name] : Named())
        if (name == device) return id;
    return SDL_GetPrimaryDisplay();
}

app::Mode Pixels(const SDL_DisplayMode& m) {
    float density = m.pixel_density > 0 ? m.pixel_density : 1.0f;
    return {uint32_t(std::lround(m.w * density)), uint32_t(std::lround(m.h * density))};
}

}  // namespace

std::vector<Display> Displays() {
    std::vector<Display> out;
    SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    int number = 1;
    for (const auto& [id, name] : Named()) {
        const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(id);
        if (!m) continue;
        out.push_back(Display{name, number++, Pixels(*m), id == primary});
    }
    return out;
}

std::vector<app::Mode> Modes(const std::string& monitor) {
    std::vector<app::Mode> modes;
    int n = 0;
    SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(DisplayOf(monitor), &n);
    for (int i = 0; list && i < n; i++) {
        app::Mode m = Pixels(*list[i]);
        if (m.width < 640) continue;
        auto same = [&](const app::Mode& o) { return o.width == m.width && o.height == m.height; };
        if (std::none_of(modes.begin(), modes.end(), same)) modes.push_back(m);
    }
    SDL_free(list);
    // a compositor that lists no modes (some Wayland ones): the desktop's own
    if (modes.empty())
        if (const SDL_DisplayMode* d = SDL_GetDesktopDisplayMode(DisplayOf(monitor))) modes.push_back(Pixels(*d));
    std::sort(modes.begin(), modes.end(), [](const app::Mode& a, const app::Mode& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    });
    return modes;
}

uint32_t RefreshRate() {
    using clock = std::chrono::steady_clock;
    static uint32_t rate = 0;
    static clock::time_point next{};
    auto now = clock::now();
    if (now < next) return rate;
    next = now + std::chrono::seconds(1);
    auto* window = static_cast<SDL_Window*>(app::Window());
    const SDL_DisplayMode* m = window ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)) : nullptr;
    rate = m && m->refresh_rate > 1 ? uint32_t(std::lround(m->refresh_rate)) : 0;
    return rate;
}

// window thread: onto the setting's display, where the host's display apply then sizes it
void UpdateWindowMonitor(void* window) {
    auto* w = static_cast<SDL_Window*>(window);
    SDL_DisplayID target = DisplayOf(Monitor());
    if (!w || target == SDL_GetDisplayForWindow(w)) return;
    SDL_Rect r{};
    if (!SDL_GetDisplayBounds(target, &r)) return;
    SDL_SetWindowPosition(w, r.x, r.y);
    g_monitor_moves++;
}

// no splash window on Linux
void UpdateSplashMonitor() {}

}  // namespace game
