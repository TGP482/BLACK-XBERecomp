#pragma once
#include <cstddef>
#include <string>

#include "core/input.h"

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

}  // namespace input
