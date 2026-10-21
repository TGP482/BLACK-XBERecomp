// the monitors and their modes, through SDL
#include <algorithm>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "common/monitor.h"
#include "common/options.h"

namespace game {

namespace {

// the display a device name stands for, 0 when it is gone
SDL_DisplayID DisplayOf(const std::string& device) {
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    if (!ids) return 0;
    SDL_DisplayID found = 0;
    for (int i = 0; i < count; i++) {
        const char* name = SDL_GetDisplayName(ids[i]);
        if (device.empty() && i == 0) found = ids[i];
        else if (name && device == name) found = ids[i];
    }
    SDL_free(ids);
    return found;
}

}  // namespace

std::vector<Display> Displays() {
    std::vector<Display> out;
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    if (!ids) return out;
    SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    for (int i = 0; i < count; i++) {
        SDL_Rect bounds{};
        if (!SDL_GetDisplayBounds(ids[i], &bounds)) continue;
        const char* name = SDL_GetDisplayName(ids[i]);
        Display d{name ? name : "", i + 1, app::Mode{uint32_t(bounds.w), uint32_t(bounds.h)}, ids[i] == primary};
        out.push_back(d);
    }
    SDL_free(ids);
    std::sort(out.begin(), out.end(), [](const Display& a, const Display& b) { return a.number < b.number; });
    return out;
}

std::vector<app::Mode> Modes(const std::string& device) {
    std::vector<app::Mode> modes;
    SDL_DisplayID id = DisplayOf(device);
    if (!id) id = SDL_GetPrimaryDisplay();
    int count = 0;
    SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(id, &count);
    if (!list) return modes;
    for (int i = 0; i < count; i++) {
        app::Mode m{uint32_t(list[i]->w), uint32_t(list[i]->h)};
        if (m.width < 640) continue;
        auto same = [&](const app::Mode& o) { return o.width == m.width && o.height == m.height; };
        if (std::none_of(modes.begin(), modes.end(), same)) modes.push_back(m);
    }
    SDL_free(list);
    std::sort(modes.begin(), modes.end(), [](const app::Mode& a, const app::Mode& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    });
    return modes;
}

// the window's monitor's refresh rate in Hz, 0 when unreadable
uint32_t RefreshRate() {
    SDL_DisplayID id = DisplayOf(Opt().monitor);
    if (!id) id = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(id);
    return mode && mode->refresh_rate > 1 ? uint32_t(mode->refresh_rate) : 0;
}

}  // namespace game
