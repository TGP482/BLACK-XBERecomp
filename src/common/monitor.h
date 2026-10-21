#pragma once
#include <string>
#include <vector>

#include "app/app.h"

// Options::monitor: the host sizes the game window on the monitor it is on, so it is moved to the chosen one first
namespace game {

struct Display {
    std::string device;  // \\.\DISPLAY1
    int number;          // the device's number, as Windows numbers it
    app::Mode desktop;
    bool primary;
};
// the connected monitors, in their device numbers' order
std::vector<Display> Displays();
// the monitor's modes smallest first, so Left steps down; the primary's for an empty device
std::vector<app::Mode> Modes(const std::string& device);
// the game window's monitor's refresh rate in Hz, 0 when unreadable
uint32_t RefreshRate();
// window thread: the game window onto the setting's monitor, before the host's display apply
void UpdateWindowMonitor(void* hwnd);
// the host's splash, already up when the game window is created, centred on the setting's monitor
void UpdateSplashMonitor();

}  // namespace game
