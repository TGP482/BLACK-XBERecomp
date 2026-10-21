#include "common/monitor.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "common/aspect.h"
#include "common/options.h"

namespace game {

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

std::vector<app::Mode> Modes(const std::string& device) {
    std::vector<app::Mode> modes;
    std::wstring name(device.begin(), device.end());
    const wchar_t* monitor = name.empty() ? nullptr : name.c_str();
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(monitor, i, &dm); i++) {
        if (dm.dmBitsPerPel < 32 || dm.dmPelsWidth < 640) continue;
        app::Mode m{dm.dmPelsWidth, dm.dmPelsHeight};
        auto same = [&](const app::Mode& o) { return o.width == m.width && o.height == m.height; };
        if (std::none_of(modes.begin(), modes.end(), same)) modes.push_back(m);
    }
    std::sort(modes.begin(), modes.end(), [](const app::Mode& a, const app::Mode& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    });
    return modes;
}

// the game window's monitor's refresh rate, read again each second (a mode change, a move to another monitor); 0
// when unreadable
uint32_t RefreshRate() {
    static uint32_t rate = 0;
    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (now < next) return rate;
    next = now + 1000;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTOPRIMARY);
    bool read = GetMonitorInfoW(monitor, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode);
    rate = read && mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
    return rate;
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

}  // namespace game
