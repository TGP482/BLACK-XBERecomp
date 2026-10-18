#include "common/monitor.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <SDL3/SDL.h>
#endif

#include "common/aspect.h"
#include "common/options.h"

namespace game {

#ifdef _WIN32
namespace {

// the setting, copied under the panel's lock
std::string Monitor() {
    std::lock_guard lock(app::UiLock());
    return Opt().monitor;
}

// the primary for empty or one not connected
HMONITOR MonitorHandle(const std::string& device) {
    struct Find {
        std::wstring device;
        HMONITOR found;
    } find{std::wstring(device.begin(), device.end()), nullptr};
    auto match = [](HMONITOR mon, HDC, LPRECT, LPARAM p) -> BOOL {
        auto* f = reinterpret_cast<Find*>(p);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi) && f->device == mi.szDevice) f->found = mon;
        return !f->found;
    };
    if (!device.empty()) EnumDisplayMonitors(nullptr, nullptr, match, LPARAM(&find));
    return find.found ? find.found : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

// the host restores a fullscreen mode only on the window's monitor, so the one left behind is restored here
void EnsureDesktopMode(HMONITOR mon) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    DEVMODEW desktop{};
    desktop.dmSize = sizeof(desktop);
    if (!GetMonitorInfoW(mon, &mi)) return;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &current)) return;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &desktop)) return;
    if (current.dmPelsWidth == desktop.dmPelsWidth && current.dmPelsHeight == desktop.dmPelsHeight) return;
    ChangeDisplaySettingsExW(mi.szDevice, nullptr, nullptr, 0, nullptr);
}

}  // namespace

std::vector<Display> Displays() {
    std::vector<Display> out;
    auto add = [](HMONITOR mon, HDC, LPRECT, LPARAM list) -> BOOL {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!GetMonitorInfoW(mon, &mi) || !EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm)) return TRUE;
        // no saved mode yet, as on a monitor just connected
        if (!dm.dmPelsWidth && !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return TRUE;
        Display d{std::string(), 0, {dm.dmPelsWidth, dm.dmPelsHeight}, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0};
        for (const wchar_t* c = mi.szDevice; *c; c++) d.device += char(*c);
        size_t digits = d.device.find_last_not_of("0123456789") + 1;
        d.number = digits < d.device.size() ? std::atoi(d.device.c_str() + digits) : 0;
        reinterpret_cast<std::vector<Display>*>(list)->push_back(d);
        return TRUE;
    };
    EnumDisplayMonitors(nullptr, nullptr, add, LPARAM(&out));
    std::sort(out.begin(), out.end(), [](const Display& a, const Display& b) { return a.number < b.number; });
    return out;
}

void UpdateWindowMonitor(void* window) {
    HWND hwnd = static_cast<HWND>(window);
    HMONITOR target = MonitorHandle(Monitor());
    HMONITOR current = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    if (target == current) return;
    // first: restoring the left monitor's mode can shift the target's rect
    EnsureDesktopMode(current);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(target, &mi)) return;
    const RECT& m = mi.rcMonitor;
    SetWindowPos(hwnd, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_NOZORDER | SWP_NOACTIVATE);
    g_monitor_moves++;
}

void UpdateSplashMonitor() {
    // the splash is made on this thread, before the game window
    auto match = [](HWND hwnd, LPARAM p) -> BOOL {
        wchar_t name[64] = L"";
        GetClassNameW(hwnd, name, int(std::size(name)));
        std::wstring splash = std::wstring(app::kGame.name) + L"Splash";  // splash.cpp's window class
        if (splash != name) return TRUE;
        *reinterpret_cast<HWND*>(p) = hwnd;
        return FALSE;
    };
    HWND splash = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), match, LPARAM(&splash));
    if (!splash) return;
    RECT r{};
    MONITORINFO mi{sizeof(mi)};
    if (!GetWindowRect(splash, &r) || !GetMonitorInfoW(MonitorHandle(Monitor()), &mi)) return;
    const RECT& m = mi.rcMonitor;
    int x = m.left + (m.right - m.left - (r.right - r.left)) / 2;
    int y = m.top + (m.bottom - m.top - (r.bottom - r.top)) / 2;
    SetWindowPos(splash, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

#else

namespace {

std::string Monitor() {
    std::lock_guard lock(app::UiLock());
    return Opt().monitor;
}

}  // namespace

std::vector<Display> Displays() {
    std::vector<Display> out;
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays) return out;
    SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    for (int i = 0; i < count; i++) {
        SDL_DisplayID id = displays[i];
        const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(id);
        const char* name = SDL_GetDisplayName(id);
        Display d{
            name ? std::string(name) : ("Display " + std::to_string(i + 1)),
            i + 1,
            {uint32_t(mode ? mode->w : 1920), uint32_t(mode ? mode->h : 1080)},
            (id == primary)
        };
        out.push_back(d);
    }
    SDL_free(displays);
    return out;
}

void UpdateWindowMonitor(void* window) {
    SDL_Window* win = static_cast<SDL_Window*>(window);
    if (!win) return;
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays) return;
    std::string target_name = Monitor();
    SDL_DisplayID target_id = 0;
    for (int i = 0; i < count; i++) {
        const char* name = SDL_GetDisplayName(displays[i]);
        if (name && target_name == name) {
            target_id = displays[i];
            break;
        }
    }
    if (!target_id) target_id = SDL_GetPrimaryDisplay();
    SDL_Rect rect{};
    if (SDL_GetDisplayBounds(target_id, &rect)) {
        SDL_SetWindowPosition(win, rect.x, rect.y);
        g_monitor_moves++;
    }
    SDL_free(displays);
}

void UpdateSplashMonitor() {
}

#endif

}  // namespace game
