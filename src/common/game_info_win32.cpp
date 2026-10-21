// the Windows side of the host: the game window's messages, the cursor, the missing game message and the
// display language. The shared host code reaches these through common/platform.h
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/app.h"
#include "common/menu.h"
#include "common/monitor.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/platform.h"

namespace platform {

namespace {

constexpr uint32_t kHostApplyDisplay = WM_APP + 1;  // main.cpp's kApplyDisplay
constexpr intptr_t kKeyWasDown = 1 << 30;  // WM_KEYDOWN lParam: a held key's repeat

// the panel shows the system arrow, the game's menus their own cursor, play none
HCURSOR WindowCursor() {
    if (app::MenuOpen()) return LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    if (input::CursorShown()) return static_cast<HCURSOR>(app::CursorImage());
    return nullptr;
}

}  // namespace

// the panel's keys and the mouse; F3 and the display mode stay with the host
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    using namespace app;
    switch (msg) {
        case WM_CREATE:
            game::UpdateSplashMonitor();
            game::UpdateWindowMonitor(hwnd);
            break;
        case kHostApplyDisplay:
            game::UpdateWindowMonitor(hwnd);
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (!(lp & kKeyWasDown) && input::PanelKey(int(wp))) {
                ToggleMenu();
                *result = 0;
                return true;
            }
            if (!(lp & kKeyWasDown) && !MenuOpen() && input::HudKey(int(wp))) {
                game::g_hide_hud = !game::g_hide_hud;
                *result = 0;
                return true;
            }
            if (msg == WM_KEYDOWN && wp == VK_ESCAPE && MenuOpen() && !Capturing()) ToggleMenu();
            break;
        case WM_SYSCOMMAND:
            // a bound Alt must not open the window menu
            if ((wp & 0xFFF0) == SC_KEYMENU) {
                *result = 0;
                return true;
            }
            break;
        case WM_CHAR:
            input::OnChar(unsigned(wp));
            break;
        case WM_INPUT:
            input::OnRawInput(reinterpret_cast<void*>(lp));
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) input::ReleaseCursor();
            break;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(WindowCursor());
                *result = TRUE;
                return true;
            }
            break;
    }
    return false;
}

void* Cursor() { return WindowCursor(); }

void MissingGame(const wchar_t* message) {
    MessageBoxW(nullptr, message, L"Black - XBE Recomp", MB_OK | MB_ICONWARNING);
}

bool Confirm(const wchar_t* message) {
    return MessageBoxW(nullptr, message, L"Black - XBE Recomp", MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

void RequestClose() { PostMessageW(static_cast<HWND>(app::Window()), WM_CLOSE, 0, 0); }

const char* SystemLanguage() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_FRENCH: return "fra";
        case LANG_GERMAN: return "deu";
        case LANG_ITALIAN: return "ita";
        case LANG_SPANISH: return "esp";
        default: return "int";
    }
}

}  // namespace platform
