#pragma once
#include <cstdint>

// the few places the shared host code needs the platform under it
namespace platform {

// the game's window message: true when the host handled it
bool Message(void* window, uint32_t msg, uintptr_t wparam, intptr_t lparam, intptr_t* result);
// the window cursor to show, null for the system's own
void* Cursor();
// the game files are missing: tell the player
void MissingGame(const wchar_t* message);
// ask the player a yes or no question, true for yes
bool Confirm(const wchar_t* message);
// close the game window
void RequestClose();
// the display language's code, one of the game's
const char* SystemLanguage();
// the game window is in the background or minimized
bool WindowUnfocused();

}  // namespace platform
