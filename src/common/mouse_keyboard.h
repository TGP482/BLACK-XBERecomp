#pragma once
#include <cstddef>
#include <string>

#include "core/input.h"

#ifndef _WIN32
enum {
    VK_LBUTTON = 0x01,
    VK_RBUTTON = 0x02,
    VK_MBUTTON = 0x04,
    VK_XBUTTON1 = 0x05,
    VK_XBUTTON2 = 0x06,
    VK_BACK = 0x08,
    VK_TAB = 0x09,
    VK_RETURN = 0x0D,
    VK_SHIFT = 0x10,
    VK_CONTROL = 0x11,
    VK_MENU = 0x12,
    VK_CAPITAL = 0x14,
    VK_ESCAPE = 0x1B,
    VK_SPACE = 0x20,
    VK_PRIOR = 0x21,
    VK_NEXT = 0x22,
    VK_END = 0x23,
    VK_HOME = 0x24,
    VK_LEFT = 0x25,
    VK_UP = 0x26,
    VK_RIGHT = 0x27,
    VK_DOWN = 0x28,
    VK_INSERT = 0x2D,
    VK_DELETE = 0x2E,
    VK_F1 = 0x70,
    VK_F2 = 0x71,
    VK_F4 = 0x73,
    VK_F5 = 0x74,
    VK_F6 = 0x75,
    VK_F7 = 0x76,
    VK_F8 = 0x77,
    VK_F9 = 0x78,
    VK_F10 = 0x79,
    VK_F11 = 0x7A,
    VK_F12 = 0x7B,
    VK_NUMPAD0 = 0x60,
    VK_NUMPAD1 = 0x61,
    VK_NUMPAD2 = 0x62,
    VK_NUMPAD3 = 0x63,
    VK_NUMPAD4 = 0x64,
    VK_NUMPAD5 = 0x65,
    VK_NUMPAD6 = 0x66,
    VK_NUMPAD7 = 0x67,
    VK_NUMPAD8 = 0x68,
    VK_NUMPAD9 = 0x69,
    VK_MULTIPLY = 0x6A,
    VK_ADD = 0x6B,
    VK_SUBTRACT = 0x6D,
    VK_DECIMAL = 0x6E,
    VK_DIVIDE = 0x6F,
    VK_OEM_1 = 0xBA,
    VK_OEM_PLUS = 0xBB,
    VK_OEM_COMMA = 0xBC,
    VK_OEM_MINUS = 0xBD,
    VK_OEM_PERIOD = 0xBE,
    VK_OEM_2 = 0xBF,
    VK_OEM_3 = 0xC0,
    VK_OEM_4 = 0xDB,
    VK_OEM_5 = 0xDC,
    VK_OEM_6 = 0xDD,
    VK_OEM_7 = 0xDE,
};
#endif

// the keyboard and mouse as the controller on port 0
namespace input {

void StartMouse(void* hwnd);     // window thread, the game window is up
void OnRawInput(void* handle);   // window thread, WM_INPUT
void ReleaseCursor();            // the game lost the input: the clip goes, held keys wait for release
void Keyboard(Pad& pad);         // held keys onto port 0's pad
// raw mouse counts since the last call, scaled by the settings; false when there are none
bool TakeMouse(float& dx, float& dy);

// actions the keyboard and mouse can be bound to, key names comma separated
// letter, digit and symbol keys bind by position as the US key there; Binding and ButtonKey show the player's labels
struct Action {
    const char* id;
    const char* label;
    const char* defaults;
};
const Action* Actions(size_t* count);
std::string Binding(size_t action);
// slot 0 the key, 1 its alternate (the F10 panel's second box); vk 0 clears the slot. A key the other slot has moves.
void Bind(size_t action, int vk, int slot = 0);
// every key off the action; neither this nor Bind with vk 0 leaves the settings panel without a key
void Unbind(size_t action);
// Delete went down in the game window since the last call, the menus' key for clearing a binding
bool TakeDeletePress();
void ResetBindings();
void LoadBindings();     // after the settings load
int PressedKey();        // a bindable key or mouse button held now, 0 for none
bool PanelKey(int vk);   // the settings panel is bound to this key and no key capture wants it
bool HudKey(int vk);     // the toggle HUD action is bound to this key and no key capture wants it
// the first key bound to a pad button, upper case, empty for none: 0 to 7 A B X Y black white and the triggers,
// 8 to 15 dpad up down left right, start, back, the thumbs
std::string ButtonKey(int button);

// the game's own menus
void SeenPad(const Pad& pad);  // a controller's report before the keys join it: its input hides the cursor
void SetMenu(bool menu);       // game thread, each tick: a menu page is up
bool KeyboardActive();         // keyboard or mouse used last, not a controller
bool CursorShown();            // in a menu with the keyboard or mouse used last
// the cursor in the shown frame, 0 to 1 across and down, and the left button; false while it is not shown
bool Cursor(float& x, float& y, bool& down);
int TakeWheel();               // wheel notches in the menus since the last call, up positive
bool LeftButton();             // the left mouse button held in the game window
void TapConfirm();             // a short press of A on the pad itself
// a name being typed: the keys type (OnChar) and press nothing but the arrows and pause
void SetTextEntry(bool on);   // game thread, each tick
void OnChar(unsigned c);      // window thread, WM_CHAR
std::u16string TakeChars();   // typed since the last call, Backspace as '\b', Enter as '\r'
void OnSdlEvent(const void* event);

}  // namespace input
