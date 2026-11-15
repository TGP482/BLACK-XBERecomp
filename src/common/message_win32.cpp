// app::ShowError and app::ShowConfirm as the Win32 message boxes the call sites used directly, so the texts, the icons
// and the default button stay exactly as they were.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "common/message.h"

namespace app {

void ShowError(const std::wstring& title, const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), title.c_str(), MB_ICONERROR);
}

bool ShowConfirm(const std::wstring& title, const std::wstring& text) {
    return MessageBoxW(nullptr, text.c_str(), title.c_str(), MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

}  // namespace app
