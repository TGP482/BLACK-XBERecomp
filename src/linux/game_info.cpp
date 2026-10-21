// the Linux side of the platform hooks: the messages go nowhere, the answers are the defaults
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>

#include "common/platform.h"
#include "linux/window.h"

namespace platform {

namespace {

// the display language's code from the locale, the game's own codes
const char* LanguageFromLocale() {
    const char* locale = std::getenv("LC_ALL");
    if (!locale || !*locale) locale = std::getenv("LC_MESSAGES");
    if (!locale || !*locale) locale = std::getenv("LANG");
    if (!locale || !*locale) return "int";
    switch (locale[0]) {
        case 'f': return "fra";
        case 'd': return "deu";
        case 'i': return "ita";
        case 'e': return "esp";
        default: return "int";
    }
}

}  // namespace

bool Message(void*, uint32_t, uintptr_t, intptr_t, intptr_t*) { return false; }

void* Cursor() { return nullptr; }

void MissingGame(const wchar_t* message) {
    // the terminal takes the wide message as bytes
    std::fprintf(stderr, "%ls\n", message);
}

bool Confirm(const wchar_t* message) {
    std::fprintf(stderr, "%ls\n", message);
    return false;
}

void RequestClose() {
    // the event pump sees the quit and ends the run loop
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
}

const char* SystemLanguage() { return LanguageFromLocale(); }

bool WindowUnfocused() { return window::Handle() && !window::Focused(); }

}  // namespace platform
