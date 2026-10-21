#pragma once
#include <atomic>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/menu.h"

// the settings panel's rows and the panel's drawing share this state
namespace app {
namespace panel {

inline std::atomic<bool> g_open{false};
inline std::atomic<int> g_capture{-1};
inline bool g_display_held = false;
inline Settings g_display;

// the panel's pages
inline constexpr int kSettingsPage = 0;
inline constexpr int kKeyboardPage = 1;
inline constexpr int kControllerPage = 2;

// the rows half
bool DisplayRow(int id);
bool Greyed(int id);
std::string RowText(int id, int step, Settings& s = Get());
void HoldDisplay(int id, int step);
void Apply();
void RestoreDefaults(int page);
std::vector<std::string> KeyBoxes(size_t action);
std::string KeyText(std::string keys);
int StepIndex(int index, int step, int count);

}  // namespace panel
}  // namespace app
