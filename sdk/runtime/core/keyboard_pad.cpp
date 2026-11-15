#include "core/keyboard_pad.h"

#include <SDL3/SDL.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>

#include "core/log.h"

namespace input::keyboard {

namespace {

constexpr const char* kNames[kControls] = {
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "start", "back", "left_thumb", "right_thumb",
    "a", "b", "x", "y", "black", "white", "left_trigger", "right_trigger",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right"};

std::mutex g_lock;
bool g_down[kScancodes] = {};
// a press shorter than kMinPress still reads as held that long, and by one poll at least: a game that reads its pads
// more than once a frame, or only every few frames, sees it
constexpr auto kMinPress = std::chrono::milliseconds(50);
bool g_tapped[kScancodes] = {};
std::chrono::steady_clock::time_point g_pressed_at[kScancodes];
uint32_t g_presses[kScancodes] = {};
Bindings g_bindings = Defaults();

int16_t Axis(bool neg, bool pos) { return int16_t(pos == neg ? 0 : pos ? 32767 : -32768); }

}  // namespace

const char* Name(Control c) { return c < kControls ? kNames[c] : ""; }

// arrows steer, a racing and menu layout that leaves the letters for the face buttons
Bindings Defaults() {
    Bindings b;
    auto set = [&](Control c, SDL_Scancode k0, SDL_Scancode k1 = SDL_SCANCODE_UNKNOWN) {
        b.key[c] = {uint16_t(k0), uint16_t(k1)};
    };
    set(kLeftUp, SDL_SCANCODE_UP);
    set(kLeftDown, SDL_SCANCODE_DOWN);
    set(kLeftLeft, SDL_SCANCODE_LEFT);
    set(kLeftRight, SDL_SCANCODE_RIGHT);
    set(kRightUp, SDL_SCANCODE_I);
    set(kRightDown, SDL_SCANCODE_K);
    set(kRightLeft, SDL_SCANCODE_J);
    set(kRightRight, SDL_SCANCODE_L);
    set(kDpadUp, SDL_SCANCODE_T);
    set(kDpadDown, SDL_SCANCODE_G);
    set(kDpadLeft, SDL_SCANCODE_F);
    set(kDpadRight, SDL_SCANCODE_H);
    set(kA, SDL_SCANCODE_Z, SDL_SCANCODE_SPACE);
    set(kB, SDL_SCANCODE_X);
    set(kX, SDL_SCANCODE_A);
    set(kY, SDL_SCANCODE_S);
    set(kBlack, SDL_SCANCODE_D);
    set(kWhite, SDL_SCANCODE_C);
    set(kLeftTrigger, SDL_SCANCODE_Q, SDL_SCANCODE_LSHIFT);
    set(kRightTrigger, SDL_SCANCODE_W, SDL_SCANCODE_RSHIFT);
    set(kStart, SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER);
    set(kBack, SDL_SCANCODE_BACKSPACE);
    set(kLeftThumb, SDL_SCANCODE_V);
    set(kRightThumb, SDL_SCANCODE_B);
    return b;
}

const Bindings& Current() { return g_bindings; }

void Set(const Bindings& b) {
    std::lock_guard lock(g_lock);
    g_bindings = b;
}

Pad FromKeys(const bool (&down)[kScancodes], const Bindings& b) {
    bool on[kControls] = {};
    for (int c = 0; c < kControls; c++)
        for (uint16_t k : b.key[c]) on[c] |= k && k < kScancodes && down[k];
    Pad p;
    for (int i = 0; i < 8; i++) p.buttons |= uint16_t(on[kDpadUp + i]) << i;
    for (int i = 0; i < 8; i++) p.analog[i] = on[kA + i] ? 0xFF : 0;
    p.lx = Axis(on[kLeftLeft], on[kLeftRight]);
    p.ly = Axis(on[kLeftDown], on[kLeftUp]);  // the console's sticks have y up
    p.rx = Axis(on[kRightLeft], on[kRightRight]);
    p.ry = Axis(on[kRightDown], on[kRightUp]);
    return p;
}

void Merge(Pad& into, const Pad& keys) {
    into.buttons |= keys.buttons;
    for (int i = 0; i < 8; i++) into.analog[i] = std::max(into.analog[i], keys.analog[i]);
    if (keys.lx || keys.ly) {
        into.lx = keys.lx;
        into.ly = keys.ly;
    }
    if (keys.rx || keys.ry) {
        into.rx = keys.rx;
        into.ry = keys.ry;
    }
}

std::string KeysText(const std::array<uint16_t, kKeysPerControl>& keys) {
    std::string s;
    for (uint16_t k : keys) {
        if (!k) continue;
        if (!s.empty()) s += ", ";
        s += SDL_GetScancodeName(SDL_Scancode(k));
    }
    return s;
}

bool ParseKeys(const std::string& text, std::array<uint16_t, kKeysPerControl>& keys) {
    std::array<uint16_t, kKeysPerControl> out{};
    size_t n = 0, at = 0;
    while (at <= text.size()) {
        size_t comma = text.find(',', at);
        if (comma == std::string::npos) comma = text.size();
        std::string name = text.substr(at, comma - at);
        name.erase(0, name.find_first_not_of(' '));
        name.erase(name.find_last_not_of(' ') + 1);
        at = comma + 1;
        if (name.empty()) continue;
        SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN || n == kKeysPerControl) return false;
        out[n++] = uint16_t(sc);
    }
    keys = out;
    return true;
}

void Load(const toml::table& t) {
    Bindings b = Current();
    for (int c = 0; c < kControls; c++) {
        auto text = t[Name(Control(c))].value<std::string>();
        if (text && !ParseKeys(*text, b.key[c]))
            LOGE("settings: [keyboard] %s = '%s': not key names SDL knows, the default stays", Name(Control(c)),
                 text->c_str());
    }
    Set(b);
}

toml::table Table() {
    toml::table t;
    for (int c = 0; c < kControls; c++) t.insert(Name(Control(c)), KeysText(Current().key[c]));
    return t;
}

void OnKey(int scancode, bool down) {
    if (scancode <= 0 || scancode >= kScancodes) return;
    std::lock_guard lock(g_lock);
    if (down && !g_down[scancode]) {
        g_presses[scancode]++;
        g_pressed_at[scancode] = std::chrono::steady_clock::now();
    }
    g_down[scancode] = down;
    if (down) g_tapped[scancode] = true;
}

void ReleaseAll() {
    std::lock_guard lock(g_lock);
    std::fill(std::begin(g_down), std::end(g_down), false);
    std::fill(std::begin(g_tapped), std::end(g_tapped), false);
    std::fill(std::begin(g_pressed_at), std::end(g_pressed_at), std::chrono::steady_clock::time_point{});
}

uint32_t Presses(int scancode) {
    if (scancode <= 0 || scancode >= kScancodes) return 0;
    std::lock_guard lock(g_lock);
    return g_presses[scancode];
}

Pad Held() {
    std::lock_guard lock(g_lock);
    auto now = std::chrono::steady_clock::now();
    bool seen[kScancodes];
    for (int k = 0; k < kScancodes; k++) {
        seen[k] = g_down[k] || g_tapped[k] || (g_presses[k] && now - g_pressed_at[k] < kMinPress);
        g_tapped[k] = false;
    }
    return FromKeys(seen, g_bindings);
}

}  // namespace input::keyboard
