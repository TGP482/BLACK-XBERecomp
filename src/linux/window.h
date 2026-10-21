#pragma once
#include <cstdint>

// the game's frame on the screen: the window, the renderer and the texture the CPU renderer fills
namespace window {

// opens the window: the size, the title and whether it covers the screen
bool Open(const wchar_t* title, uint32_t width, uint32_t height, bool fullscreen, bool borderless);
void Close();
// the SDL_Window, for app::Window()
void* Handle();
// the events: false when the player closed the window
bool Pump();
// a frame from the renderer: BGRA pixels, w*h*4 bytes, the rows at pitch bytes
void Submit(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t pitch);
// draws the last submitted frame
void Show();
// the frames shown so far
uint64_t Shown();
// the window's picture as a BMP, for the milestone's check
bool SaveScreenshot(const char* path);
// the window has the focus
bool Focused();
// the display settings changed: the window thread applies them
void RequestDisplayApply();

}  // namespace window
