#include "app/sdl_keys.h"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <windows.h>  // the virtual key codes: the Linux compat header

namespace app {

namespace {

struct Pair {
    int scancode;
    int vk;
};

// every key position a virtual key names; the first pair of a virtual key is its main key
constexpr Pair kPairs[] = {
    {SDL_SCANCODE_RETURN, VK_RETURN}, {SDL_SCANCODE_KP_ENTER, VK_RETURN}, {SDL_SCANCODE_ESCAPE, VK_ESCAPE},
    {SDL_SCANCODE_BACKSPACE, VK_BACK}, {SDL_SCANCODE_TAB, VK_TAB}, {SDL_SCANCODE_SPACE, VK_SPACE},
    {SDL_SCANCODE_MINUS, VK_OEM_MINUS}, {SDL_SCANCODE_EQUALS, VK_OEM_PLUS}, {SDL_SCANCODE_LEFTBRACKET, VK_OEM_4},
    {SDL_SCANCODE_RIGHTBRACKET, VK_OEM_6}, {SDL_SCANCODE_BACKSLASH, VK_OEM_5}, {SDL_SCANCODE_SEMICOLON, VK_OEM_1},
    {SDL_SCANCODE_APOSTROPHE, VK_OEM_7}, {SDL_SCANCODE_GRAVE, VK_OEM_3}, {SDL_SCANCODE_COMMA, VK_OEM_COMMA},
    {SDL_SCANCODE_PERIOD, VK_OEM_PERIOD}, {SDL_SCANCODE_SLASH, VK_OEM_2}, {SDL_SCANCODE_NONUSBACKSLASH, VK_OEM_102},
    {SDL_SCANCODE_CAPSLOCK, VK_CAPITAL}, {SDL_SCANCODE_PAUSE, VK_PAUSE}, {SDL_SCANCODE_INSERT, VK_INSERT},
    {SDL_SCANCODE_HOME, VK_HOME}, {SDL_SCANCODE_PAGEUP, VK_PRIOR}, {SDL_SCANCODE_DELETE, VK_DELETE},
    {SDL_SCANCODE_END, VK_END}, {SDL_SCANCODE_PAGEDOWN, VK_NEXT}, {SDL_SCANCODE_RIGHT, VK_RIGHT},
    {SDL_SCANCODE_LEFT, VK_LEFT}, {SDL_SCANCODE_DOWN, VK_DOWN}, {SDL_SCANCODE_UP, VK_UP},
    {SDL_SCANCODE_KP_DIVIDE, VK_DIVIDE}, {SDL_SCANCODE_KP_MULTIPLY, VK_MULTIPLY}, {SDL_SCANCODE_KP_MINUS, VK_SUBTRACT},
    {SDL_SCANCODE_KP_PLUS, VK_ADD}, {SDL_SCANCODE_KP_PERIOD, VK_DECIMAL}, {SDL_SCANCODE_KP_0, VK_NUMPAD0},
    {SDL_SCANCODE_LSHIFT, VK_SHIFT}, {SDL_SCANCODE_RSHIFT, VK_SHIFT}, {SDL_SCANCODE_LCTRL, VK_CONTROL},
    {SDL_SCANCODE_RCTRL, VK_CONTROL}, {SDL_SCANCODE_LALT, VK_MENU}, {SDL_SCANCODE_RALT, VK_MENU},
};

}  // namespace

int VkFromScancode(int sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return VK_F1 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return VK_NUMPAD1 + (sc - SDL_SCANCODE_KP_1);
    for (const Pair& p : kPairs)
        if (p.scancode == sc) return p.vk;
    return 0;
}

void ScancodesFromVk(int vk, int out[2]) {
    out[0] = out[1] = 0;
    if (vk >= 'A' && vk <= 'Z') {
        out[0] = SDL_SCANCODE_A + (vk - 'A');
        return;
    }
    if (vk >= '1' && vk <= '9') {
        out[0] = SDL_SCANCODE_1 + (vk - '1');
        return;
    }
    if (vk == '0') {
        out[0] = SDL_SCANCODE_0;
        return;
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        out[0] = SDL_SCANCODE_F1 + (vk - VK_F1);
        return;
    }
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) {
        out[0] = SDL_SCANCODE_KP_1 + (vk - VK_NUMPAD1);
        return;
    }
    int n = 0;
    for (const Pair& p : kPairs)
        if (p.vk == vk && n < 2) out[n++] = p.scancode;
}

uint32_t MouseMaskFromVk(int vk) {
    switch (vk) {
        case VK_LBUTTON: return SDL_BUTTON_LMASK;
        case VK_RBUTTON: return SDL_BUTTON_RMASK;
        case VK_MBUTTON: return SDL_BUTTON_MMASK;
        case VK_XBUTTON1: return SDL_BUTTON_X1MASK;
        case VK_XBUTTON2: return SDL_BUTTON_X2MASK;
        default: return 0;
    }
}

}  // namespace app
