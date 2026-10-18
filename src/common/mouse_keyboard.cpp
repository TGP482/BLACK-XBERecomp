// the keyboard and mouse as the controller on port 0
#include "common/mouse_keyboard.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iterator>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

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
#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#endif

#include "app/app.h"
#include "common/aspect.h"
#include "common/menu.h"
#include "common/options.h"

namespace input {

namespace {

#ifdef _WIN32
HWND g_hwnd = nullptr;
#else
SDL_Window* g_window = nullptr;
#endif
std::atomic<int> g_dx{0};
std::atomic<int> g_dy{0};
std::atomic<uint64_t> g_take_ms{0};     // TakeMouse, asked every tick in play
std::atomic<uint64_t> g_capture_ms{0};  // PressedKey
std::atomic<bool> g_settle{false};
std::atomic<uint64_t> g_confirm_until{0};
std::atomic<bool> g_menu{false};
std::atomic<bool> g_keyboard{true};
std::atomic<bool> g_text{false};
std::mutex g_chars_lock;
std::u16string g_chars;  // typed, not yet taken
constexpr size_t kMaxChars = 64;
std::atomic<int> g_wheel{0};
constexpr uint64_t kPlayMs = 250;
constexpr uint64_t kTapMs = 80;
constexpr uint64_t kCaptureMs = 250;  // PressedKey asked this recently: a key capture is up
// The wheel binds as two keys, codes Windows leaves unassigned. A notch holds what is bound to it for one tap.
constexpr int kWheelUp = 0x0E;
constexpr int kWheelDown = 0x0F;
std::atomic<uint64_t> g_wheel_ms[2] = {};  // the last notch up, down

uint64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()) + 1;
}

// what an action presses: 0-7 analog A B X Y black white LT RT, 8-15 the buttons bits, 16-19 the move keys,
// 20-23 the look keys
struct Target {
    Action action;
    int button;
};
constexpr int kHostKey = -2;
constexpr int kWalk = -3;
constexpr int kFirstButtonBit = 8;
constexpr int kStart = 12;
constexpr int kFirstMoveKey = 16;
constexpr int kFirstLookKey = 20;
constexpr int kStickMax = 32767;
// the game's default controller layout
constexpr Target kActions[] = {
    {{"move_forward", "MOVE FORWARD", "W"}, 16},
    {{"move_back", "MOVE BACK", "S"}, 17},
    {{"strafe_left", "STRAFE LEFT", "A"}, 18},
    {{"strafe_right", "STRAFE RIGHT", "D"}, 19},
    {{"walk", "WALK", "Alt"}, kWalk},
    {{"crouch", "CROUCH", "C"}, 14},
    {{"fire", "FIRE", "LMB"}, 7},
    {{"aim", "AIM", "RMB"}, 15},
    {{"melee", "MELEE", "V"}, 1},
    {{"reload", "RELOAD", "R"}, 0},
    {{"interact", "INTERACT", "F"}, 2},
    {{"grenade", "THROW GRENADE", "G"}, 6},
    {{"weapon_up", "CHANGE WEAPON UP", "E,Wheel Up"}, 11},
    {{"weapon_down", "CHANGE WEAPON DOWN", "Q,Wheel Down"}, 10},
    {{"suppressor", "TOGGLE SUPPRESSOR", "T"}, 3},
    {{"firing_mode", "FIRING MODE / SCOPE ZOOM", "X"}, 8},
    {{"health_pack", "HEALTH PACK", "H"}, 9},
    {{"next_track", "NEXT MUSIC TRACK", "N"}, 5},
    {{"back", "OBJECTIVES", "Tab"}, 13},
    {{"pause", "PAUSE MENU", "Escape"}, 12},
    {{"hide_hud", "TOGGLE HUD", ""}, kHostKey},
    {{"settings_panel", "SETTINGS PANEL", "F10"}, kHostKey},
    {{"look_up", "LOOK UP", "Up"}, 20},
    {{"look_down", "LOOK DOWN", "Down"}, 21},
    {{"look_left", "LOOK LEFT", "Left"}, 22},
    {{"look_right", "LOOK RIGHT", "Right"}, 23},
};
constexpr size_t kCount = std::size(kActions);
constexpr size_t HostAction(std::string_view id) {
    size_t i = 0;
    while (kActions[i].action.id != id) i++;
    return i;
}
constexpr size_t kHudAction = HostAction("hide_hud");
constexpr size_t kPanelAction = HostAction("settings_panel");
struct Name {
    int vk;
    const char* name;
};
constexpr Name kNames[] = {
    {VK_LBUTTON, "LMB"}, {VK_RBUTTON, "RMB"}, {VK_MBUTTON, "MMB"}, {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"},
    {kWheelUp, "Wheel Up"}, {kWheelDown, "Wheel Down"},
    {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_ESCAPE, "Escape"}, {VK_BACK, "Backspace"}, {VK_TAB, "Tab"},
    {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"}, {VK_CAPITAL, "CapsLock"},
    {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
    {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"},
    {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"},
    {'A', "A"}, {'B', "B"}, {'C', "C"}, {'D', "D"}, {'E', "E"}, {'F', "F"}, {'G', "G"}, {'H', "H"}, {'I', "I"},
    {'J', "J"}, {'K', "K"}, {'L', "L"}, {'M', "M"}, {'N', "N"}, {'O', "O"}, {'P', "P"}, {'Q', "Q"}, {'R', "R"},
    {'S', "S"}, {'T', "T"}, {'U', "U"}, {'V', "V"}, {'W', "W"}, {'X', "X"}, {'Y', "Y"}, {'Z', "Z"},
    {'0', "0"}, {'1', "1"}, {'2', "2"}, {'3', "3"}, {'4', "4"}, {'5', "5"}, {'6', "6"}, {'7', "7"}, {'8', "8"},
    {'9', "9"},
    {VK_F1, "F1"}, {VK_F2, "F2"}, {VK_F4, "F4"}, {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"},
    {VK_F8, "F8"}, {VK_F9, "F9"}, {VK_F10, "F10"}, {VK_F11, "F11"}, {VK_F12, "F12"},
    {VK_NUMPAD0, "Numpad0"}, {VK_NUMPAD1, "Numpad1"}, {VK_NUMPAD2, "Numpad2"}, {VK_NUMPAD3, "Numpad3"},
    {VK_NUMPAD4, "Numpad4"}, {VK_NUMPAD5, "Numpad5"}, {VK_NUMPAD6, "Numpad6"}, {VK_NUMPAD7, "Numpad7"},
    {VK_NUMPAD8, "Numpad8"}, {VK_NUMPAD9, "Numpad9"}, {VK_ADD, "NumpadPlus"}, {VK_SUBTRACT, "NumpadMinus"},
    {VK_MULTIPLY, "NumpadStar"}, {VK_DIVIDE, "NumpadSlash"}, {VK_DECIMAL, "NumpadPeriod"},
    {VK_OEM_3, "Backtick"}, {VK_OEM_MINUS, "Minus"}, {VK_OEM_PLUS, "Equals"}, {VK_OEM_4, "LBracket"},
    {VK_OEM_6, "RBracket"}, {VK_OEM_5, "Backslash"}, {VK_OEM_1, "Semicolon"}, {VK_OEM_7, "Quote"},
    {VK_OEM_COMMA, "Comma"}, {VK_OEM_PERIOD, "Period"}, {VK_OEM_2, "Slash"},
};

// not bindable: in the game's menus the arrows are the dpad, Enter accepts, Backspace backs out
constexpr int kFixed[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK};

#ifndef _WIN32
using UINT = uint32_t;
#endif

// US layout scan codes of the keys that move with the layout: a bind names the US key at that position
struct Position {
    int vk;
    UINT scan;
};
constexpr Position kPositions[] = {
    {'1', 0x02}, {'2', 0x03}, {'3', 0x04}, {'4', 0x05}, {'5', 0x06}, {'6', 0x07}, {'7', 0x08}, {'8', 0x09},
    {'9', 0x0A}, {'0', 0x0B}, {VK_OEM_MINUS, 0x0C}, {VK_OEM_PLUS, 0x0D},
    {'Q', 0x10}, {'W', 0x11}, {'E', 0x12}, {'R', 0x13}, {'T', 0x14}, {'Y', 0x15}, {'U', 0x16}, {'I', 0x17},
    {'O', 0x18}, {'P', 0x19}, {VK_OEM_4, 0x1A}, {VK_OEM_6, 0x1B},
    {'A', 0x1E}, {'S', 0x1F}, {'D', 0x20}, {'F', 0x21}, {'G', 0x22}, {'H', 0x23}, {'J', 0x24}, {'K', 0x25},
    {'L', 0x26}, {VK_OEM_1, 0x27}, {VK_OEM_7, 0x28}, {VK_OEM_3, 0x29}, {VK_OEM_5, 0x2B},
    {'Z', 0x2C}, {'X', 0x2D}, {'C', 0x2E}, {'V', 0x2F}, {'B', 0x30}, {'N', 0x31}, {'M', 0x32},
    {VK_OEM_COMMA, 0x33}, {VK_OEM_PERIOD, 0x34}, {VK_OEM_2, 0x35},
};

using Binds = std::array<std::vector<int>, kCount>;
std::mutex g_bind_lock;
Binds g_binds;

int Vk(const std::string& name) {
    for (const Name& n : kNames)
        if (_stricmp(n.name, name.c_str()) == 0) return n.vk;
    return 0;
}

const char* NameOf(int vk) {
    for (const Name& n : kNames)
        if (n.vk == vk) return n.name;
    return nullptr;
}

UINT ScanOf(int vk) {
    for (const Position& p : kPositions)
        if (p.vk == vk) return p.scan;
    return 0;
}

#ifdef _WIN32
// the keyboard layout the player types with, switched per window thread
HKL Layout() { return GetKeyboardLayout(GetWindowThreadProcessId(g_hwnd, nullptr)); }

// the key the current layout has at the bind's position
int LiveVk(int vk) {
    UINT scan = ScanOf(vk);
    if (!scan) return vk;
    UINT live = MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, Layout());
    return live ? int(live) : vk;
}

// a key the window was sent, as the position bound to it
int PositionVk(int live) {
    if (NameOf(live) && !ScanOf(live)) return live;
    UINT scan = MapVirtualKeyExW(UINT(live), MAPVK_VK_TO_VSC, Layout());
    for (const Position& p : kPositions)
        if (p.scan == scan) return p.vk;
    return live;
}

// the key as the player's keyboard labels it
std::string Label(int vk) {
    int live = LiveVk(vk);
    if (live == vk) return NameOf(vk);
    bool alnum = (live >= 'A' && live <= 'Z') || (live >= '0' && live <= '9');
    if (alnum) return std::string(1, char(live));
    // the top bit flags a dead key
    UINT c = MapVirtualKeyExW(UINT(live), MAPVK_VK_TO_CHAR, Layout()) & 0x7FFF;
    // a comma would split the key list
    if (c == ',') return NameOf(VK_OEM_COMMA);
    if (c > ' ' && c < 0x7F) return std::string(1, char(c));
    return NameOf(vk);
}
#else
int LiveVk(int vk) { return vk; }
int PositionVk(int live) { return live; }
std::string Label(int vk) {
    const char* name = NameOf(vk);
    return name ? name : std::string();
}
#endif

std::vector<int> Parse(const std::string& list) {
    std::vector<int> out;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        if (int vk = Vk(list.substr(start, comma - start))) out.push_back(vk);
        start = comma + 1;
    }
    return out;
}

// the saved names, or the player's labels for the menus
std::string Join(const std::vector<int>& vks, bool labels = false) {
    std::string s;
    for (int vk : vks) {
        if (!s.empty()) s += ',';
        s += labels ? Label(vk) : NameOf(vk);
    }
    return s;
}

#ifdef _WIN32
bool Foreground() { return g_hwnd && GetForegroundWindow() == g_hwnd; }
#else
bool Foreground() { return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS); }
#endif
bool Active() { return Foreground() && !app::MenuOpen(); }
bool Wheel(int vk) { return vk == kWheelUp || vk == kWheelDown; }

#ifndef _WIN32
static SDL_Scancode VkToScancode(int vk) {
    if (vk >= 'A' && vk <= 'Z') return SDL_Scancode(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return SDL_Scancode(SDL_SCANCODE_1 + (vk - '1'));
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_F1 && vk <= VK_F12) return SDL_Scancode(SDL_SCANCODE_F1 + (vk - VK_F1));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return SDL_Scancode(SDL_SCANCODE_KP_0 + (vk - VK_NUMPAD0));
    switch (vk) {
        case VK_SPACE: return SDL_SCANCODE_SPACE;
        case VK_RETURN: return SDL_SCANCODE_RETURN;
        case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
        case VK_BACK: return SDL_SCANCODE_BACKSPACE;
        case VK_TAB: return SDL_SCANCODE_TAB;
        case VK_SHIFT: return SDL_SCANCODE_LSHIFT;
        case VK_CONTROL: return SDL_SCANCODE_LCTRL;
        case VK_MENU: return SDL_SCANCODE_LALT;
        case VK_CAPITAL: return SDL_SCANCODE_CAPSLOCK;
        case VK_UP: return SDL_SCANCODE_UP;
        case VK_DOWN: return SDL_SCANCODE_DOWN;
        case VK_LEFT: return SDL_SCANCODE_LEFT;
        case VK_RIGHT: return SDL_SCANCODE_RIGHT;
        case VK_INSERT: return SDL_SCANCODE_INSERT;
        case VK_DELETE: return SDL_SCANCODE_DELETE;
        case VK_HOME: return SDL_SCANCODE_HOME;
        case VK_END: return SDL_SCANCODE_END;
        case VK_PRIOR: return SDL_SCANCODE_PAGEUP;
        case VK_NEXT: return SDL_SCANCODE_PAGEDOWN;
        case VK_ADD: return SDL_SCANCODE_KP_PLUS;
        case VK_SUBTRACT: return SDL_SCANCODE_KP_MINUS;
        case VK_MULTIPLY: return SDL_SCANCODE_KP_MULTIPLY;
        case VK_DIVIDE: return SDL_SCANCODE_KP_DIVIDE;
        case VK_DECIMAL: return SDL_SCANCODE_KP_PERIOD;
        case VK_OEM_1: return SDL_SCANCODE_SEMICOLON;
        case VK_OEM_PLUS: return SDL_SCANCODE_EQUALS;
        case VK_OEM_COMMA: return SDL_SCANCODE_COMMA;
        case VK_OEM_MINUS: return SDL_SCANCODE_MINUS;
        case VK_OEM_PERIOD: return SDL_SCANCODE_PERIOD;
        case VK_OEM_2: return SDL_SCANCODE_SLASH;
        case VK_OEM_3: return SDL_SCANCODE_GRAVE;
        case VK_OEM_4: return SDL_SCANCODE_LEFTBRACKET;
        case VK_OEM_5: return SDL_SCANCODE_BACKSLASH;
        case VK_OEM_6: return SDL_SCANCODE_RIGHTBRACKET;
        case VK_OEM_7: return SDL_SCANCODE_APOSTROPHE;
        default: return SDL_SCANCODE_UNKNOWN;
    }
}
#endif

bool Down(int vk) {
    if (Wheel(vk)) return NowMs() - g_wheel_ms[vk == kWheelDown] < kTapMs;
#ifdef _WIN32
    return GetAsyncKeyState(LiveVk(vk)) < 0;
#else
    if (vk == VK_LBUTTON) return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
    if (vk == VK_RBUTTON) return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_RMASK) != 0;
    if (vk == VK_MBUTTON) return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MMASK) != 0;
    if (vk == VK_XBUTTON1) return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_X1MASK) != 0;
    if (vk == VK_XBUTTON2) return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_X2MASK) != 0;
    SDL_Scancode sc = VkToScancode(vk);
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    const bool* state = SDL_GetKeyboardState(nullptr);
    if (!state) return false;
    if (vk == VK_SHIFT) return state[SDL_SCANCODE_LSHIFT] || state[SDL_SCANCODE_RSHIFT];
    if (vk == VK_CONTROL) return state[SDL_SCANCODE_LCTRL] || state[SDL_SCANCODE_RCTRL];
    if (vk == VK_MENU) return state[SDL_SCANCODE_LALT] || state[SDL_SCANCODE_RALT];
    return state[sc];
#endif
}

bool Mouse(int vk) {
    bool button = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
    return button || Wheel(vk);
}
bool Play() { return NowMs() - g_take_ms < kPlayMs; }
// Delete clears a binding on the options pages: bound to a move key it would move the selection off the row first
bool GameDown(int vk) { return Down(vk) && !(g_menu && vk == VK_DELETE); }
bool KeyCaptureUp() { return NowMs() - g_capture_ms <= kCaptureMs; }

void Clip() {
#ifdef _WIN32
    RECT r;
    RECT cur;
    GetClientRect(g_hwnd, &r);
    MapWindowPoints(g_hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    if (!GetClipCursor(&cur) || !EqualRect(&r, &cur)) ClipCursor(&r);
#else
    if (g_window) SDL_SetWindowRelativeMouseMode(g_window, true);
#endif
}

// Windows asks for the cursor only when the mouse moves
void RefreshCursor() {
#ifdef _WIN32
    if (g_hwnd) PostMessageW(g_hwnd, WM_SETCURSOR, WPARAM(g_hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
#endif
}

// the action is bound to this key and no key capture wants it
bool HostKeyBound(size_t action, int vk) {
    if (KeyCaptureUp()) return false;
    std::lock_guard lock(g_bind_lock);
    const auto& b = g_binds[action];
    return std::find(b.begin(), b.end(), PositionVk(vk)) != b.end();
}

void SetKeyboard(bool on) {
    if (g_keyboard.exchange(on) != on) RefreshCursor();
}

// keys held from before the game had the input wait for release; true once none is held
bool UpdateSettle(const Binds& binds) {
    if (!g_settle) return true;
    for (auto& b : binds)
        for (int vk : b)
            if (Down(vk)) return false;
    for (int vk : kFixed)
        if (Down(vk)) return false;
    g_settle = false;
    return true;
}

// the bound keys held this tick, beside the pad buttons they pressed
struct Held {
    bool used = false;
    bool walk = false;
    int stick[8] = {};  // move forward back left right, look up down left right
};

Held PressActions(const Binds& binds, bool play, Pad& p) {
    Held held;
    // in a menu the wheel scrolls, it presses its binds in play only
    auto pressed = [play](int vk) { return GameDown(vk) && (play || !Wheel(vk)); };
    static bool pause_held;
    static bool pause_backs;
    bool pause = false;
    for (size_t i = 0; i < kCount; i++) {
        int b = kActions[i].button;
        if (b == kHostKey || !std::any_of(binds[i].begin(), binds[i].end(), pressed)) continue;
        held.used = true;
        if (g_text && b != kStart) continue;
        if (b == kWalk) {
            held.walk = true;
        } else if (b >= kFirstLookKey && !play) {
            continue;
        } else if (b >= kFirstMoveKey) {
            held.stick[b - kFirstMoveKey] = 1;
        } else if (b < kFirstButtonBit) {
            p.analog[b] = 0xFF;
        } else if (b == kStart) {
            // pressed in a menu it backs out of it, and does nothing more once the menu has gone
            if (!pause_held) pause_backs = g_menu;
            pause = true;
            if (!pause_backs) p.buttons |= uint16_t(1u << (b - kFirstButtonBit));
            else if (g_menu) p.analog[1] = 0xFF;
        } else {
            p.buttons |= uint16_t(1u << (b - kFirstButtonBit));
        }
    }
    pause_held = pause;
    return held;
}

}  // namespace

void StartMouse(void* handle) {
#ifdef _WIN32
    g_hwnd = static_cast<HWND>(handle);
    RAWINPUTDEVICE mouse{1, 2, 0, g_hwnd};  // generic desktop page, mouse usage
    RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
#else
    g_window = static_cast<SDL_Window*>(handle);
#endif
}

void OnRawInput(void* handle) {
#ifdef _WIN32
    RAWINPUT ri;
    UINT size = sizeof(ri);
    HRAWINPUT input = static_cast<HRAWINPUT>(handle);
    if (GetRawInputData(input, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
    if (ri.header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE& m = ri.data.mouse;
    int notch = 0;
    if (m.usButtonFlags & RI_MOUSE_WHEEL) notch = SHORT(m.usButtonData) > 0 ? 1 : -1;
    // noted while the settings panel has the input too, so its key capture sees the wheel
    if (notch && Foreground()) g_wheel_ms[notch < 0] = NowMs();
    if (!Active()) return;
    SetKeyboard(true);
    // the cursor is free in the game's menus, where the wheel scrolls unless a key capture takes it
    if (!Play()) {
        ClipCursor(nullptr);
        if (!KeyCaptureUp()) g_wheel += notch;
        return;
    }
    Clip();
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        g_dx += m.lLastX;
        g_dy += m.lLastY;
    }
#endif
}

void ReleaseCursor() {
#ifdef _WIN32
    ClipCursor(nullptr);
#else
    if (g_window) SDL_SetWindowRelativeMouseMode(g_window, false);
#endif
    g_settle = true;
}

#ifndef _WIN32
void OnSdlEvent(const void* ev) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(ev);
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION: {
            if (!Active()) break;
            SetKeyboard(true);
            if (!Play()) {
                if (g_window) SDL_SetWindowRelativeMouseMode(g_window, false);
                break;
            }
            if (g_window) SDL_SetWindowRelativeMouseMode(g_window, true);
            g_dx += int(e.motion.xrel);
            g_dy += int(e.motion.yrel);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            int notch = e.wheel.y > 0 ? 1 : (e.wheel.y < 0 ? -1 : 0);
            if (notch && Foreground()) g_wheel_ms[notch < 0] = NowMs();
            if (Active()) {
                SetKeyboard(true);
                if (!Play()) {
                    if (!KeyCaptureUp()) g_wheel += notch;
                }
            }
            break;
        }
        case SDL_EVENT_TEXT_INPUT: {
            for (const char* p = e.text.text; *p; ++p) {
                OnChar((unsigned char)*p);
            }
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            ReleaseCursor();
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED: {
            g_settle = true;
            break;
        }
    }
}
#else
void OnSdlEvent(const void*) {}
#endif

bool TakeMouse(float& dx, float& dy) {
    uint64_t now = NowMs();
    bool stale = now - g_take_ms.exchange(now) >= kPlayMs;
    int x = g_dx.exchange(0);
    int y = g_dy.exchange(0);
    if ((!x && !y) || stale || !Active()) return false;
    const game::Options& o = game::Opt();
    dx = float(x) * o.mouse_sensitivity;
    dy = float(o.invert_mouse ? -y : y) * o.mouse_sensitivity;
    return true;
}

void Keyboard(Pad& p) {
    if (!Active()) {
        g_settle = true;
        return;
    }
    Binds binds;
    {
        std::lock_guard lock(g_bind_lock);
        binds = g_binds;
    }
    if (!UpdateSettle(binds)) return;
    bool play = Play();
    bool fixed_down = std::any_of(std::begin(kFixed), std::end(kFixed), Down);
    Held held = PressActions(binds, play, p);
    // Enter, Space and Backspace act in menus only; in play they do what they are bound to, in a name they type
    bool confirm = !play && !g_text && (Down(VK_RETURN) || Down(VK_SPACE));
    if (confirm || NowMs() < g_confirm_until) p.analog[0] = 0xFF;
    if ((!play && !g_text && Down(VK_BACK)) || (g_menu && Down(VK_RBUTTON))) p.analog[1] = 0xFF;
    // the arrows, kFixed's first four, are the dpad's bits
    if (!play) {
        for (int i = 0; i < 4; i++) p.buttons |= uint16_t(Down(kFixed[i]) << i);
    }
    const int* stick = held.stick;
    float speed = held.walk ? float(game::Opt().walk_speed) / 100.0f : 1.0f;
    if (int x = stick[3] - stick[2]) p.lx = int16_t(float(x) * speed * float(kStickMax));
    if (int y = stick[0] - stick[1]) p.ly = int16_t(float(y) * speed * float(kStickMax));
    if (int x = stick[7] - stick[6]) p.rx = int16_t(x * kStickMax);
    if (int y = stick[4] - stick[5]) p.ry = int16_t(y * kStickMax);
    if (fixed_down || held.used) SetKeyboard(true);
}

void SeenPad(const Pad& p) {
    constexpr int16_t kStickPushed = 16000;
    constexpr uint8_t kAnalogPushed = 0x20;
    auto pushed = [](int16_t v) { return v > kStickPushed || v < -kStickPushed; };
    auto pressed = [](uint8_t a) { return a > kAnalogPushed; };
    bool analog = std::any_of(std::begin(p.analog), std::end(p.analog), pressed);
    bool sticks = pushed(p.lx) || pushed(p.ly) || pushed(p.rx) || pushed(p.ry);
    if (p.buttons || analog || sticks) SetKeyboard(false);
}

void SetMenu(bool menu) {
    if (g_menu.exchange(menu) != menu) RefreshCursor();
}

bool KeyboardActive() { return g_keyboard; }
bool CursorShown() { return g_menu && g_keyboard; }

bool Cursor(float& x, float& y, bool& down) {
#ifdef _WIN32
    POINT p;
    RECT r;
    if (!Active() || !CursorShown()) return false;
    if (!GetCursorPos(&p) || !ScreenToClient(g_hwnd, &p) || !GetClientRect(g_hwnd, &r)) return false;
    if (r.right <= 0 || r.bottom <= 0) return false;
    // the frame is shown in the middle of the window
    float aspect = game::Aspect();
    float w = float(r.right);
    float h = float(r.bottom);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    x = (float(p.x) - (w - vw) / 2) / vw;
    y = (float(p.y) - (h - vh) / 2) / vh;
    down = Down(VK_LBUTTON);
    return true;
#else
    if (!Active() || !CursorShown() || !g_window) return false;
    float mx = 0, my = 0;
    SDL_MouseButtonFlags btn = SDL_GetMouseState(&mx, &my);
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(g_window, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return false;
    float aspect = game::Aspect();
    float w = float(win_w), h = float(win_h);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    x = (mx - (w - vw) / 2.0f) / vw;
    y = (my - (h - vh) / 2.0f) / vh;
    down = (btn & SDL_BUTTON_LMASK) != 0;
    return true;
#endif
}

int TakeWheel() { return g_wheel.exchange(0); }

bool LeftButton() { return Active() && !g_settle && Down(VK_LBUTTON); }

void TapConfirm() { g_confirm_until = NowMs() + kTapMs; }

void SetTextEntry(bool on) {
    if (g_text.exchange(on) == on) return;
    std::lock_guard lock(g_chars_lock);
    g_chars.clear();
}

void OnChar(unsigned c) {
    if (!g_text || !Active() || c > 0xFFFF) return;
    std::lock_guard lock(g_chars_lock);
    if (g_chars.size() < kMaxChars) g_chars += char16_t(c);
}

std::u16string TakeChars() {
    std::lock_guard lock(g_chars_lock);
    return std::exchange(g_chars, {});
}

const Action* Actions(size_t* count) {
    static const auto list = [] {
        std::array<Action, kCount> a{};
        for (size_t i = 0; i < kCount; i++) a[i] = kActions[i].action;
        return a;
    }();
    *count = kCount;
    return list.data();
}

std::string Binding(size_t action) {
    std::lock_guard lock(g_bind_lock);
    return action < kCount ? Join(g_binds[action], true) : std::string();
}

void Bind(size_t action, int vk, int slot) {
    if (action >= kCount || slot < 0 || slot > 1 || (vk && !NameOf(vk))) return;
    // the host keys act on the window's key messages, which mouse buttons do not send
    if (kActions[action].button == kHostKey && Mouse(vk)) return;
    {
        std::lock_guard lock(g_bind_lock);
        std::vector<int>& keys = g_binds[action];
        int slots[2] = {0, 0};
        for (size_t i = 0; i < keys.size() && i < 2; i++) slots[i] = keys[i];
        if (vk && slots[1 - slot] == vk) slots[1 - slot] = 0;
        slots[slot] = vk;
        // the settings panel with no key would leave a keyboard player no way back into it
        if (!slots[0] && !slots[1] && action == kPanelAction) return;
        keys.clear();
        for (int k : slots) {
            if (k) keys.push_back(k);
        }
        game::Opt().keys[kActions[action].action.id] = Join(keys);
    }
    app::Save();
}

void Unbind(size_t action) {
    if (action >= kCount || action == kPanelAction) return;
    {
        std::lock_guard lock(g_bind_lock);
        g_binds[action].clear();
        game::Opt().keys[kActions[action].action.id].clear();
    }
    app::Save();
}

bool TakeDeletePress() {
    static bool held = false;
    bool down = Down(VK_DELETE);
    bool fresh = down && !held;
    held = down;
    return fresh && Active();
}

void ResetBindings() {
    game::Opt().keys.clear();
    LoadBindings();
    app::Save();
}

void LoadBindings() {
    std::lock_guard lock(g_bind_lock);
    const auto& keys = game::Opt().keys;
    for (size_t i = 0; i < kCount; i++) {
        auto it = keys.find(kActions[i].action.id);
        g_binds[i] = Parse(it != keys.end() ? it->second : kActions[i].action.defaults);
    }
}

std::string ButtonKey(int button) {
    std::lock_guard lock(g_bind_lock);
    for (size_t i = 0; i < kCount; i++) {
        if (kActions[i].button != button || g_binds[i].empty()) continue;
        std::string name = g_binds[i][0] == VK_ESCAPE ? "Esc" : Label(g_binds[i][0]);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        return name;
    }
    return {};
}

int PressedKey() {
    g_capture_ms = NowMs();
    for (const Name& n : kNames)
        if (Down(n.vk)) return n.vk;
    return 0;
}

bool PanelKey(int vk) { return HostKeyBound(kPanelAction, vk); }
bool HudKey(int vk) { return HostKeyBound(kHudAction, vk); }

}  // namespace input
