#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// the F10 settings panel, drawn by DrawUi (GameInfo::ui)
namespace app {

bool MenuOpen();
bool ModalInputCaptured();
void SetModalInputCaptured(bool captured);
bool Capturing();  // a key binding waits for a key
void ToggleMenu();
void DrawUi(double fps);
void SetSubtitleSettingsPreview(bool visible);  // game options page asks DrawUi to show the styled sample
inline constexpr uint32_t kFpsCapRefresh = 1;  // Settings::fps_cap: just under the monitor's refresh rate
void PaceFrame();  // app::Pace, with kFpsCapRefresh; game thread, after the present
// the game menus' cursor (HCURSOR): cursor.png beside the exe sized to the frame, else the system arrow
void* CursorImage();

// the panel's rows, for the game's own options menu
namespace rows {

enum class Kind { Heading, Value, Bind, Page, Action };
struct Row {
    Kind kind;
    int id;  // a bind row's input action
    const char* label;
};
const Row* Page(int page, size_t* count);         // 0 the settings, 1 mouse and keyboard
const Row* Language();                            // the language selector, including on English-only discs
const Row* Languages(size_t* count);               // one selectable row per supported language
const Row* SubtitleSettings(size_t* count);        // subtitle controls followed by the fixed size summary
std::string Value(const Row& row, int step = 0);  // what the row shows, stepped first when step is not zero
void Activate(const Row& row);  // invoke a button-style row
void Defaults(int page);
// false for a row shown greyed that takes no change: HDR output without an HDR display
bool Enabled(const Row& row);

}  // namespace rows

}  // namespace app
