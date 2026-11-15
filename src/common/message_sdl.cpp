// app::ShowError and app::ShowConfirm through SDL's message box, the port's system dialog. wchar_t holds the code point
// itself outside Windows, so the texts are narrowed to the UTF-8 SDL wants.
#include <SDL3/SDL_messagebox.h>

#include <cstdint>
#include <string>

#include "common/message.h"
#include "core/log.h"

namespace app {

namespace {

// the text as UTF-8: UTF-32 in, so each wchar_t is one code point
std::string Narrow(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t c : text) {
        const uint32_t cp = uint32_t(c);
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

}  // namespace

void ShowError(const std::wstring& title, const std::wstring& text) {
    const std::string t = Narrow(title), m = Narrow(text);
    // no video target: the log still carries it
    if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, t.c_str(), m.c_str(), nullptr))
        LOGE("%s: %s (%s)", t.c_str(), m.c_str(), SDL_GetError());
}

bool ShowConfirm(const std::wstring& title, const std::wstring& text) {
    // MB_DEFBUTTON2's plain No is what the return key and escape press here, and closing the box answers No
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"},
        {0, 1, "Yes"},
    };
    const std::string t = Narrow(title), m = Narrow(text);
    const SDL_MessageBoxData box = {SDL_MESSAGEBOX_WARNING, nullptr, t.c_str(), m.c_str(), 2, buttons, nullptr};
    int id = 0;
    if (!SDL_ShowMessageBox(&box, &id)) return false;  // no dialog to ask in: the answer that does not start it
    return id == 1;
}

}  // namespace app
