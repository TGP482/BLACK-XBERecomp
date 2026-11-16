// A message the player has to see before the game has a window: a message box that blocks until it is answered. The
// title and text come from the call sites as wide strings (the game's own labels), so both take them that way.
#pragma once

#include <string>

namespace app {

// the error box: the player reads it and acknowledges it
void ShowError(const std::wstring& title, const std::wstring& text);

// the Yes/No box with No as the default button: true only when the player chose Yes
bool ShowConfirm(const std::wstring& title, const std::wstring& text);

}  // namespace app
