#pragma once
// Win32 virtual key codes, the games' key identifiers, and SDL3 scancodes, both by key position (a US keyboard's
// keys). The SDL host sends WM_KEYDOWN with these codes; a game's key state reads SDL by them.
#include <cstdint>

namespace app {

// the virtual key at a scancode's position, 0 for a key no virtual key names; both shifts are VK_SHIFT, as
// WM_KEYDOWN reports them, and the same for Ctrl and Alt
int VkFromScancode(int scancode);
// the scancodes of a virtual key: up to two (left and right modifiers, Enter on both blocks), 0 where unused
void ScancodesFromVk(int vk, int out[2]);
// SDL's mouse button mask of VK_LBUTTON to VK_XBUTTON2, 0 for other keys
uint32_t MouseMaskFromVk(int vk);

}  // namespace app
