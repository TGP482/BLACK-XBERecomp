#pragma once
// What the keyboard and mouse code asks of the system: key states, the keyboard layout, the cursor and raw mouse
// motion. platform_input_win32.cpp has the Win32 calls, platform_input_sdl.cpp SDL3's; mouse_keyboard.cpp is the same
// on both. Keys are Win32 virtual key codes (windows.h, or the Linux compat header) on both.
#include <cstdint>

namespace input::os {

void Start(void* window);      // the game window is up (window thread)
bool Foreground();             // the game window has the keyboard
bool KeyDown(int vk);          // the key at vk's US position, or a mouse button, held now
int LiveVk(int vk);            // the key the current layout has where the US layout has vk
int PositionVk(int live_vk);   // a key the window was sent, as the US key at its position
uint32_t KeyChar(int live_vk); // the character the layout's key types (a dead key's too), 0 for none
void Clip(bool on);            // the cursor held inside the window, or let go
void RefreshCursor();          // the window picks its cursor again (WM_SETCURSOR)
// the cursor in client pixels and the client area's size; false when unknown
bool CursorInWindow(float& x, float& y, float& w, float& h);
// WM_INPUT's lp: relative motion and wheel notches (up positive); false when it is no mouse report
bool ReadRawMouse(void* handle, int& dx, int& dy, int& wheel, bool& absolute);

}  // namespace input::os
