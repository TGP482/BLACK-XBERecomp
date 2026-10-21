// the Linux stand-ins for the host the console-side hooks link against: the keyboard and mouse, the cursor and the
// display. They do nothing until the SDL3 host lands in milestone 3
#include <cstddef>
#include <cstdint>
#include <string>

#include "app/app.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"

namespace input {

namespace {

// the actions the game binds, the same list the Windows host holds
constexpr Action kActions[] = {
    {"move_forward", "MOVE FORWARD", "W"},
    {"move_back", "MOVE BACK", "S"},
    {"strafe_left", "STRAFE LEFT", "A"},
    {"strafe_right", "STRAFE RIGHT", "D"},
    {"walk", "WALK", "Alt"},
    {"crouch", "CROUCH", "C"},
    {"fire", "FIRE", "LMB"},
    {"aim", "AIM", "RMB"},
    {"melee", "MELEE", "V"},
    {"reload", "RELOAD", "R"},
    {"interact", "INTERACT", "F"},
    {"grenade", "THROW GRENADE", "G"},
    {"weapon_up", "CHANGE WEAPON UP", "E,Wheel Up"},
    {"weapon_down", "CHANGE WEAPON DOWN", "Q,Wheel Down"},
    {"suppressor", "TOGGLE SUPPRESSOR", "T"},
    {"firing_mode", "FIRING MODE / SCOPE ZOOM", "X"},
    {"health_pack", "HEALTH PACK", "H"},
    {"next_track", "NEXT MUSIC TRACK", "N"},
    {"back", "OBJECTIVES", "Tab"},
    {"pause", "PAUSE MENU", "Escape"},
    {"hide_hud", "TOGGLE HUD", ""},
    {"settings_panel", "SETTINGS PANEL", "F10"},
    {"look_up", "LOOK UP", "Up"},
    {"look_down", "LOOK DOWN", "Down"},
    {"look_left", "LOOK LEFT", "Left"},
    {"look_right", "LOOK RIGHT", "Right"},
};
constexpr size_t kCount = sizeof(kActions) / sizeof(kActions[0]);

}  // namespace

void StartMouse(void*) {}
void OnRawInput(void*) {}
void ReleaseCursor() {}
void Keyboard(Pad&) {}
bool TakeMouse(float&, float&) { return false; }

const Action* Actions(size_t* count) {
    *count = kCount;
    return kActions;
}

std::string Binding(size_t) { return {}; }
void Bind(size_t, int, int) {}
void Unbind(size_t) {}
bool TakeDeletePress() { return false; }
void ResetBindings() {}
void LoadBindings() {}
int PressedKey() { return 0; }
bool PanelKey(int) { return false; }
bool HudKey(int) { return false; }
std::string ButtonKey(int) { return {}; }
void SeenPad(const Pad&) {}
void SetMenu(bool) {}
bool KeyboardActive() { return false; }
bool CursorShown() { return false; }
bool Cursor(float&, float&, bool&) { return false; }
int TakeWheel() { return 0; }
bool LeftButton() { return false; }
void TapConfirm() {}
void SetTextEntry(bool) {}
void OnChar(unsigned) {}
std::u16string TakeChars() { return {}; }

}  // namespace input

namespace app {

void DrawUi(double) {}  // the F10 panel arrives in milestone 5
void* CursorImage() { return nullptr; }

}  // namespace app
