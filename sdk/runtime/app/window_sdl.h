#pragma once
// The Linux host's SDL3 window (window_sdl.cpp), which main_sdl.cpp opens and runs; app.h has the rest of its API
#include <functional>
#include <string>

namespace app {

void SetArguments(int argc, char** argv);  // the command line, for Instance()
// window thread, SDL's video started: the window made hidden, the display settings applied, then shown
bool OpenWindow(const std::string& title);
// window thread: waits up to timeout_ms for events and handles them; false once the window is closed
bool PumpEvents(int timeout_ms);
// a key down no game took: the host's own keys (F3, the thread dump)
void SetUnhandledKey(std::function<void(int vk)> f);
std::string Utf8(const wchar_t* text);

}  // namespace app
