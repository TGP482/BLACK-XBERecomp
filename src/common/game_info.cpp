#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <toml++/toml.hpp>
#include <imgui.h>

#include "app/app.h"
#include "audio/output.h"
#include "common/menu.h"
#include "common/language_download.h"
#include "common/monitor.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/log.h"
#include "common/translations.h"

namespace game {

namespace {

bool g_language_detection_done = false;

std::vector<std::string> SystemUiLocales() {
    ULONG languages = 0, chars = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &languages, nullptr, &chars) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || chars < 2) return {};
    std::vector<wchar_t> names(chars);
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &languages, names.data(), &chars)) return {};
    std::vector<std::string> result;
    for (const wchar_t* p = names.data(); *p;) {
        std::string locale;
        bool ascii = true;
        while (*p) {
            if (*p > 0x7F) ascii = false;
            else locale += char(*p);
            ++p;
        }
        if (ascii && !locale.empty()) result.push_back(std::move(locale));
        ++p;  // next null-delimited language (the final item is followed by the second null)
    }
    return result;
}

std::string SupportedLanguage(const std::string& locale) {
    std::string lower = locale;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const size_t dash = lower.find('-');
    const std::string base = lower.substr(0, dash);
    if (base == "en") return "int";
    if (base == "fr") return "fra";
    if (base == "de") return "deu";
    if (base == "it") return "ita";
    if (base == "es") return "esp";
    if (base == "id") return "id";
    if (base == "ru") return "ru";
    if (base == "pt" && (lower == "pt-br" || lower == "pt")) return "pt-BR";
    return {};
}

std::string CanonicalLanguage(const std::string& value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* code : kLanguages) {
        std::string canonical = code;
        std::transform(canonical.begin(), canonical.end(), canonical.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (lower == canonical) return code;
    }
    return SupportedLanguage(value);
}

void DetectSystemLanguage(bool first_language_setup) {
    Options& o = Opt();
    o.unsupported_system_locale.clear();
    if (first_language_setup) {
        const std::vector<std::string> locales = SystemUiLocales();
        std::string locale;
        std::string detected;
        for (const std::string& candidate : locales) {
            detected = SupportedLanguage(candidate);
            if (!detected.empty()) {
                locale = candidate;
                break;
            }
        }
        if (locale.empty() && !locales.empty()) locale = locales.front();
        if (!locale.empty()) LOGR("language detection: Windows UI locale %s maps to %s", locale.c_str(),
                                   detected.empty() ? "English fallback" : detected.c_str());
        if (!detected.empty()) {
            o.language = detected;
            o.unsupported_language_prompted = true;
        } else if (!locale.empty()) {
            o.language = "int";
            o.unsupported_language_prompted = false;
            o.unsupported_system_locale = locale;
        }
    } else if (!o.unsupported_language_prompted) {
        const std::vector<std::string> locales = SystemUiLocales();
        const auto supported = std::find_if(locales.begin(), locales.end(), [](const std::string& locale) {
            return !SupportedLanguage(locale).empty();
        });
        if (supported == locales.end() && !locales.empty()) o.unsupported_system_locale = locales.front();
        else o.unsupported_language_prompted = true;
    }
    app::SetModalInputCaptured(!o.unsupported_system_locale.empty());
    g_language_detection_done = true;
}

constexpr uint32_t kHostApplyDisplay = WM_APP + 1;  // main.cpp's kApplyDisplay
constexpr intptr_t kKeyWasDown = 1 << 30;  // WM_KEYDOWN lParam: a held key's repeat

// the panel shows the system arrow, the game's menus their own cursor, play none
HCURSOR WindowCursor() {
    if (app::MenuOpen() || app::ModalInputCaptured()) return LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
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
            if (!ModalInputCaptured() && !(lp & kKeyWasDown) && input::PanelKey(int(wp))) {
                ToggleMenu();
                *result = 0;
                return true;
            }
            if (!ModalInputCaptured() && !(lp & kKeyWasDown) && !MenuOpen() && input::HudKey(int(wp))) {
                g_hide_hud = !g_hide_hud;
                *result = 0;
                return true;
            }
            if (msg == WM_KEYDOWN && wp == VK_ESCAPE && MenuOpen() && !Capturing() && !ModalInputCaptured()) ToggleMenu();
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

// port 0 has the keyboard; the game sees no input while the panel is open
void Pad(int port, input::Pad& pad) {
    if (port == 0) pad.connected = true;
    if (app::MenuOpen() || app::ModalInputCaptured()) {
        pad = input::Pad{.connected = pad.connected};
        if (port == 0) input::Keyboard(pad);  // mark held keys for release before gameplay resumes
        return;
    }
    input::SeenPad(pad);
    if (port == 0) input::Keyboard(pad);
}

// no vibration while the keyboard and mouse are in use
void Rumble(int port, uint16_t& left, uint16_t& right) {
    if (port != 0 || !input::KeyboardActive()) return;
    left = 0;
    right = 0;
}

// what the options list cannot hold
void Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    Options& o = Opt();
    o.monitor = t["monitor"].value_or(o.monitor);
    o.mouse_sensitivity = float(t["mouse_sensitivity"].value_or(double(o.mouse_sensitivity)));
    const auto saved_language = t["language"].value<std::string>();
    const bool first_language_setup = !saved_language.has_value();
    std::string language = saved_language.value_or(o.language);
    const std::string canonical_language = CanonicalLanguage(language);
    const bool known = !canonical_language.empty();
    if (known) o.language = canonical_language;
    o.subtitles_enabled = t["subtitles_enabled"].value_or(o.subtitles_enabled);
    o.subtitle_size = uint32_t(std::clamp<int64_t>(t["subtitle_size"].value_or(int64_t(o.subtitle_size)), 80, 160));
    o.subtitle_bold = t["subtitle_bold"].value_or(o.subtitle_bold);
    o.subtitle_color = uint32_t(std::clamp<int64_t>(t["subtitle_color"].value_or(int64_t(o.subtitle_color)), 0, 1));
    o.subtitle_shadow = uint32_t(std::clamp<int64_t>(t["subtitle_shadow"].value_or(int64_t(o.subtitle_shadow)), 0, 2));
    o.subtitle_background = t["subtitle_background"].value_or(o.subtitle_background);
    o.unsupported_language_prompted = t["unsupported_language_prompted"].value_or(true);
    DetectSystemLanguage(first_language_setup);
    LOGR("language: config requested %s; selected %s%s", language.c_str(), o.language.c_str(),
         known ? "" : " (unsupported, keeping default)");
    if (const toml::table* keys = t["keys"].as_table())
        for (auto&& [id, value] : *keys)
            if (auto v = value.value<std::string>()) o.keys[std::string(id.str())] = *v;
}

// after the settings load, before boot
void Init() {
    if (!g_language_detection_done) DetectSystemLanguage(true);  // settings.cpp skips the game callback on first run
    const std::string& language = BootLanguage();  // latched before the panel can change the setting
    LOGR("language: boot selection is %s", language.c_str());
    std::error_code ec;
    std::filesystem::create_directories(app::ExeDir() / "language", ec);
    if (ec) LOGW("language: could not create language folder: %s", ec.message().c_str());
    app::Save();  // persist first-run locale detection and create a settings file if needed
    app::RequestLanguageDownload(Opt().language);  // ensure the saved language is cached beside BLACK.exe
    input::LoadBindings();
    audio::SetOutputMode(Opt().audio_output);
}

// Add Cyrillic glyphs before the first frame when Russian was selected.
void Loaded() {
    if (BootLanguage() != "ru") return;
    ImGuiIO& io = ImGui::GetIO();
    constexpr const char* kFonts[] = {"C:\\Windows\\Fonts\\arial.ttf", "C:\\Windows\\Fonts\\segoeui.ttf",
                                      "C:\\Windows\\Fonts\\tahoma.ttf"};
    for (const char* path : kFonts) {
        ImFont* font = io.Fonts->AddFontFromFileTTF(path, 16.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
        if (!font) continue;
        io.FontDefault = font;
        app::SetRussianFontAvailable(true);
        return;
    }
    app::SetRussianFontAvailable(false);
}

void Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    const Options& o = Opt();
    t.insert("monitor", o.monitor);
    t.insert("mouse_sensitivity", double(o.mouse_sensitivity));
    t.insert("language", o.language);
    t.insert("subtitles_enabled", o.subtitles_enabled);
    t.insert("subtitle_size", int64_t(o.subtitle_size));
    t.insert("subtitle_bold", o.subtitle_bold);
    t.insert("subtitle_color", int64_t(o.subtitle_color));
    t.insert("subtitle_shadow", int64_t(o.subtitle_shadow));
    t.insert("subtitle_background", o.subtitle_background);
    t.insert("unsupported_language_prompted", o.unsupported_language_prompted);
    toml::table keys;
    for (const auto& [id, value] : o.keys) keys.insert(id, value);
    t.insert("keys", keys);
}

}  // namespace

Options& Opt() {
    static Options o;
    return o;
}

const std::string& BootLanguage() {
    static const std::string language = Opt().language;
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
                {"draw_distance", nullptr, &game::Opt().draw_distance},
                {"ambient_occlusion", nullptr, &game::Opt().ambient_occlusion},
                {"viewmodel_fov", nullptr, &game::Opt().viewmodel_fov},
                {"audio_output", nullptr, &game::Opt().audio_output}},
    .load = game::Load,
    .save = game::Save,
    .init = game::Init,
    .loaded = game::Loaded,
    .window = input::StartMouse,
    .message = game::Message,
    .ui = app::DrawUi,
    .pad = game::Pad,
    .rumble = game::Rumble,
};
