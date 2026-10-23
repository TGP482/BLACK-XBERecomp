#include <algorithm>
#include <atomic>
#include <iterator>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "audio/output.h"
#include "common/menu.h"
#include "common/monitor.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/log.h"

namespace game {

namespace {

constexpr uint32_t kHostApplyDisplay = WM_APP + 1;  // main.cpp's kApplyDisplay
constexpr intptr_t kKeyWasDown = 1 << 30;  // WM_KEYDOWN lParam: a held key's repeat

// the panel shows the system arrow, the game's menus their own cursor, play none
HCURSOR WindowCursor() {
    if (app::MenuOpen()) return LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    if (input::CursorShown()) return static_cast<HCURSOR>(app::CursorImage());
    return nullptr;
}

// the panel's keys and the mouse; F3 and the display mode stay with the host
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    using namespace app;
    switch (msg) {
        case WM_CREATE:
            UpdateSplashMonitor();
            UpdateWindowMonitor(hwnd);
            break;
        case kHostApplyDisplay:
            UpdateWindowMonitor(hwnd);
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (!(lp & kKeyWasDown) && input::PanelKey(int(wp))) {
                ToggleMenu();
                *result = 0;
                return true;
            }
            if (!(lp & kKeyWasDown) && !MenuOpen() && input::HudKey(int(wp))) {
                g_hide_hud = !g_hide_hud;
                *result = 0;
                return true;
            }
            if (msg == WM_KEYDOWN && wp == VK_ESCAPE && MenuOpen() && !Capturing()) ToggleMenu();
            break;
        case WM_SYSCOMMAND:
            // a bound Alt must not open the window menu
            if ((wp & 0xFFF0) == SC_KEYMENU) {
                *result = 0;
                return true;
            }
            break;
        case WM_CHAR:
            input::OnChar(unsigned(wp));
            break;
        case WM_INPUT:
            input::OnRawInput(reinterpret_cast<void*>(lp));
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) input::ReleaseCursor();
            break;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(WindowCursor());
                *result = TRUE;
                return true;
            }
            break;
    }
    return false;
}

// A pad that has not reported yet can read its triggers half pressed (Proton, Steam Deck): fire held.
// A trigger counts once seen released.
void UpdateTriggerRest(int port, input::Pad& pad) {
    constexpr uint8_t kReleased = 0x20;
    constexpr int kLeftTrigger = 6;
    static std::atomic<uint8_t> rested[4];  // bit 0 left trigger, bit 1 right
    static std::atomic<bool> logged;
    if (port < 0 || port > 3) return;
    if (!pad.connected) {
        rested[port] = 0;
        return;
    }
    for (int t = 0; t < 2; t++) {
        uint8_t& value = pad.analog[kLeftTrigger + t];
        uint8_t bit = uint8_t(1u << t);
        if (value <= kReleased) rested[port] |= bit;
        if (rested[port] & bit) continue;
        if (!logged.exchange(true)) LOGR("input: controller %d trigger pressed before a release, ignored", port + 1);
        value = 0;
    }
}

// port 0 has the keyboard; the game sees no input while the panel is open
void Pad(int port, input::Pad& pad) {
    UpdateTriggerRest(port, pad);
    input::SeenPad(pad);
    if (port == 0) pad.connected = true;
    if (app::MenuOpen()) pad = input::Pad{.connected = pad.connected};
    else if (port == 0) input::Keyboard(pad);
}

// the vibration at the rumble intensity, none while the keyboard and mouse are in use
void Rumble(int port, uint16_t& left, uint16_t& right) {
    uint32_t percent = std::clamp(Opt().rumble_intensity, 10u, 200u);
    if (port == 0 && input::KeyboardActive()) percent = 0;
    left = uint16_t(std::min(left * percent / 100, 0xFFFFu));  // past 100% the motors saturate
    right = uint16_t(std::min(right * percent / 100, 0xFFFFu));
}

// the Windows display language when the game has it, else English
std::string SystemLanguage() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_FRENCH: return "fra";
        case LANG_GERMAN: return "deu";
        case LANG_ITALIAN: return "ita";
        case LANG_SPANISH: return "esp";
        default: return "int";
    }
}

// what the options list cannot hold
void Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    Options& o = Opt();
    o.monitor = t["monitor"].value_or(o.monitor);
    o.mouse_sensitivity = float(t["mouse_sensitivity"].value_or(double(o.mouse_sensitivity)));
    o.controller_sensitivity = float(t["controller_sensitivity"].value_or(double(o.controller_sensitivity)));
    o.gyro_sensitivity = float(t["gyro_sensitivity"].value_or(double(o.gyro_sensitivity)));
    std::string language = t["language"].value_or(o.language);
    bool known = std::find(std::begin(kLanguages), std::end(kLanguages), language) != std::end(kLanguages);
    if (known || language == "auto") o.language = language;
    if (const toml::table* keys = t["keys"].as_table())
        for (auto&& [id, value] : *keys)
            if (auto v = value.value<std::string>()) o.keys[std::string(id.str())] = *v;
}

// after the settings load, before boot
void Init() {
    BootLanguage();  // latched before the panel can change the setting
    input::LoadBindings();
    audio::SetOutputMode(Opt().audio_output);
}

void Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    const Options& o = Opt();
    t.insert("monitor", o.monitor);
    t.insert("mouse_sensitivity", double(o.mouse_sensitivity));
    t.insert("controller_sensitivity", double(o.controller_sensitivity));
    t.insert("gyro_sensitivity", double(o.gyro_sensitivity));
    t.insert("language", o.language);
    toml::table keys;
    for (const auto& [id, value] : o.keys) keys.insert(id, value);
    t.insert("keys", keys);
}

}  // namespace

Options& Opt() {
    static Options o;
    return o;
}

std::string ResolveLanguage(const std::string& setting) { return setting == "auto" ? SystemLanguage() : setting; }

const std::string& BootLanguage() {
    static const std::string language = ResolveLanguage(Opt().language);
    return language;
}

}  // namespace game

const app::GameInfo app::kGame = {
    .title = L"Black - XBE Recomp",
    .short_title = L"Black",
    .name = L"Black",
    .log = "black",
    .settings = "black.toml",
    .splash = L"BlackLogo",
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with GlobData.bin and the "
               L"chars, data, export, language, levels, sound and videos folders beside it.",
    .images = {{"default.xbe", "Black"}},
    .options = {{"skip_intros", &game::Opt().skip_intros, nullptr},
                {"fps_counter", &game::Opt().fps_counter, nullptr},
                {"reload_blur", &game::Opt().reload_blur, nullptr},
                {"fmv_scale", nullptr, &game::Opt().fmv_scale},
                {"hdr_output", &game::Opt().hdr_output, nullptr},
                {"invert_mouse", &game::Opt().invert_mouse, nullptr},
                {"hold_aim", &game::Opt().hold_aim, nullptr},
                {"walk_speed", nullptr, &game::Opt().walk_speed},
                {"fov", nullptr, &game::Opt().fov},
                {"msaa", nullptr, &game::Opt().msaa},
                {"smaa", &game::Opt().smaa, nullptr},
                {"pause_on_focus_loss", &game::Opt().pause_on_focus_loss, nullptr},
                {"texture_replacement", &game::Opt().texture_replacement, nullptr},
                {"texture_dump", &game::Opt().texture_dump, nullptr},
                {"draw_distance", nullptr, &game::Opt().draw_distance},
                {"ambient_occlusion", nullptr, &game::Opt().ambient_occlusion},
                {"viewmodel_fov", nullptr, &game::Opt().viewmodel_fov},
                {"audio_output", nullptr, &game::Opt().audio_output},
                {"controller_hold_aim", &game::Opt().controller_hold_aim, nullptr},
                {"aim_assist", &game::Opt().aim_assist, nullptr},
                {"rumble_intensity", nullptr, &game::Opt().rumble_intensity},
                {"gyro_aiming", &game::Opt().gyro_aiming, nullptr},
                {"menu_animations", &game::Opt().menu_animations, nullptr}},
    .load = game::Load,
    .save = game::Save,
    .init = game::Init,
    .loaded = nullptr,
    .window = input::StartMouse,
    .message = game::Message,
    .ui = app::DrawUi,
    .pad = game::Pad,
    .rumble = game::Rumble,
};
