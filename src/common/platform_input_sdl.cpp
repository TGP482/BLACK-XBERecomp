// the keyboard and mouse code's system calls through SDL3 (Linux): key states by position, the layout's labels,
// relative mouse mode in place of ClipCursor, the motion the host sends with WM_INPUT
#include <SDL3/SDL.h>

#include <atomic>
#include <windows.h>  // the Linux compat header: virtual key and message numbers

#include "app/app.h"
#include "app/sdl_keys.h"
#include "common/platform_input.h"

namespace input::os {

namespace {

SDL_Window* g_window = nullptr;
std::atomic<int> g_clipped{-1};  // the relative mode last asked, -1 before any

// the window's state changes on the window thread; a call from there runs at once
void OnWindowThread(SDL_MainThreadCallback fn, void* user) { SDL_RunOnMainThread(fn, user, false); }

}  // namespace

void Start(void* window) { g_window = static_cast<SDL_Window*>(window); }

bool Foreground() { return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS); }

bool KeyDown(int vk) {
    if (uint32_t mask = app::MouseMaskFromVk(vk)) return (SDL_GetMouseState(nullptr, nullptr) & mask) != 0;
    int n = 0;
    const bool* keys = SDL_GetKeyboardState(&n);
    int sc[2];
    app::ScancodesFromVk(vk, sc);
    for (int s : sc)
        if (s > 0 && s < n && keys[s]) return true;
    return false;
}

// letters and digits as the layout has them at the bind's position; other keys keep their US code
int LiveVk(int vk) {
    int sc[2];
    app::ScancodesFromVk(vk, sc);
    if (!sc[0] || app::MouseMaskFromVk(vk)) return vk;
    SDL_Keycode k = SDL_GetKeyFromScancode(SDL_Scancode(sc[0]), SDL_KMOD_NONE, false);
    if (k >= 'a' && k <= 'z') return int('A' + (k - 'a'));
    if (k >= '0' && k <= '9') return int(k);
    return vk;
}

// the host sends keys by position already
int PositionVk(int live) { return live; }

uint32_t KeyChar(int live) {
    int sc[2];
    app::ScancodesFromVk(live, sc);
    if (!sc[0]) return 0;
    SDL_Keycode k = SDL_GetKeyFromScancode(SDL_Scancode(sc[0]), SDL_KMOD_NONE, false);
    if (k >= 'a' && k <= 'z') return uint32_t('A' + (k - 'a'));
    return k < 0x80 ? uint32_t(k) : 0;
}

// play holds the pointer in relative mode: hidden, confined, motion unaccelerated; menus let it go
void Clip(bool on) {
    if (!g_window || g_clipped.exchange(on) == int(on)) return;
    OnWindowThread([](void* p) { SDL_SetWindowRelativeMouseMode(g_window, p != nullptr); }, on ? g_window : nullptr);
}

void RefreshCursor() {
    OnWindowThread(
        [](void*) {
            intptr_t result = 0;
            if (app::kGame.message) app::kGame.message(g_window, WM_SETCURSOR, 0, HTCLIENT, &result);
        },
        nullptr);
}

bool CursorInWindow(float& x, float& y, float& w, float& h) {
    if (!g_window) return false;
    int lw = 0, lh = 0;
    SDL_GetWindowSize(g_window, &lw, &lh);
    SDL_GetMouseState(&x, &y);
    w = float(lw);
    h = float(lh);
    return lw > 0 && lh > 0;
}

bool ReadRawMouse(void* handle, int& dx, int& dy, int& wheel, bool& absolute) {
    const auto* raw = static_cast<const app::RawMouse*>(handle);
    if (!raw) return false;
    dx = raw->dx;
    dy = raw->dy;
    wheel = raw->wheel;
    absolute = false;
    return true;
}

}  // namespace input::os
