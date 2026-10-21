// the Windows side of the focus pause: is the game window in the background?
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/app.h"
#include "common/platform.h"

namespace platform {

// a hidden window, behind the splash or for a handover, never pauses: it opens from the game thread
bool WindowUnfocused() {
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!hwnd || !IsWindowVisible(hwnd)) return false;
    return GetForegroundWindow() != hwnd || IsIconic(hwnd);
}

}  // namespace platform
