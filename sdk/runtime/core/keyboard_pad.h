#pragma once
// The keyboard as the first controller: keys held, by SDL scancode (USB usage, the same on every platform), mapped
// onto the console controller's report. The host's window sends its key events here; input::Poll merges the result
// into port 0, which counts as connected for it.
#include <array>
#include <cstdint>
#include <string>

#include <toml++/toml.hpp>

#include "core/input.h"

namespace input::keyboard {

// what a key can press, in the settings file's [keyboard] names
enum Control : uint8_t {
    kDpadUp, kDpadDown, kDpadLeft, kDpadRight, kStart, kBack, kLeftThumb, kRightThumb,  // Pad::buttons bits 0-7
    kA, kB, kX, kY, kBlack, kWhite, kLeftTrigger, kRightTrigger,                       // Pad::analog 0-7
    kLeftUp, kLeftDown, kLeftLeft, kLeftRight, kRightUp, kRightDown, kRightLeft, kRightRight,
    kControls
};
const char* Name(Control c);

constexpr int kScancodes = 512;  // SDL_SCANCODE_COUNT
constexpr int kKeysPerControl = 2;
// up to two scancodes per control, 0 unused
struct Bindings {
    std::array<std::array<uint16_t, kKeysPerControl>, kControls> key{};
};
Bindings Defaults();
const Bindings& Current();
void Set(const Bindings& b);

// the report the held keys make: buttons full on, sticks full over, opposite directions cancelling
Pad FromKeys(const bool (&down)[kScancodes], const Bindings& b);
// a controller's report with the keyboard's added: buttons and analog the stronger, a stick the keyboard pushes wins
void Merge(Pad& into, const Pad& keys);

// key names as SDL has them ("Z", "Space", "Return", "Up"); a binding is one name or names joined by ", "
std::string KeysText(const std::array<uint16_t, kKeysPerControl>& keys);
// false for a name SDL does not know, keys left as they were
bool ParseKeys(const std::string& text, std::array<uint16_t, kKeysPerControl>& keys);

// the settings file's [keyboard] table: each control's keys; a binding SDL cannot read keeps the current one
void Load(const toml::table& t);
toml::table Table();

// held keys: from the window's key events, all let go when it loses focus
void OnKey(int scancode, bool down);
void ReleaseAll();
// how often a key was pressed since the start, for a game's own hotkeys outside the controller: a change is a press,
// however short, and reading it takes nothing from the controller
uint32_t Presses(int scancode);
Pad Held();  // FromKeys of the held keys with Current(); a shorter press reads as held 50 ms, and by one call at least

}  // namespace input::keyboard
