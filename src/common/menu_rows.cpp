// the settings panel's rows: the values, the steps and the defaults. The panel's drawing is in menu_panel.cpp
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "audio/output.h"
#include "common/menu.h"
#include "common/menu_state.h"
#include "common/monitor.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/translations.h"

namespace app {

namespace panel {

namespace {

constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"GAME DEFAULT", "2x", "4x", "8x", "16x"};
constexpr uint32_t kDistances[] = {1, 2, 4, 8, 12, 16};
const char* const kDistanceNames[] = {"GAME DEFAULT", "2x", "4x", "8x", "12x", "16x"};
constexpr uint32_t kMsaaValues[] = {0, 2, 4, 8};
const char* const kMsaaNames[] = {"OFF", "2x", "4x", "8x"};
constexpr uint32_t kAoValues[] = {0, 1, 2};
const char* const kAoNames[] = {"OFF", "SSAO", "GTAO"};
constexpr uint32_t kFmvScales[] = {0, 1};
const char* const kFmvScaleNames[] = {"FILL", "4:3"};
constexpr uint32_t kAudioOutputs[] = {audio::kOutputAuto, audio::kOutputMono, audio::kOutputStereo,
                                      audio::kOutputSurround};
const char* const kAudioOutputNames[] = {"AUTOMATIC", "MONO", "2.0 STEREO", "5.1 SURROUND"};
// "auto" then game::kLanguages
const char* const kLanguageNames[] = {"AUTOMATIC", "ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH"};
constexpr uint32_t kFpsCaps[] = {30, 60, 75, 90, 120, 144, 165, 240, 360, kFpsCapRefresh, 0};

int NearestIndex(const uint32_t* values, int count, uint32_t value) {
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < count; i++) {
        uint32_t d = values[i] > value ? values[i] - value : value - values[i];
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

// index nearest value, stepped first
template <size_t N>
int StepTable(const uint32_t (&values)[N], uint32_t& value, int step) {
    int i = NearestIndex(values, int(N), value);
    if (step) {
        i = StepIndex(i, step, int(N));
        value = values[i];
    }
    return i;
}

// value clamped to lo to hi, stepped by inc through the values StepIndex wraps over
int StepRange(int value, int lo, int hi, int inc, int step) {
    int count = (hi - lo) / inc + 1;
    return lo + inc * StepIndex((std::clamp(value, lo, hi) - lo) / inc, step, count);
}

enum RowId { kDisplayMode, kResolution, kRenderResolution, kMaxFps, kVsync, kFpsCounter, kAniso, kReloadBlur,
             kSkipIntros, kSensitivity, kInvertMouse, kWalkSpeed, kFov, kViewmodelFov, kHoldAim, kDrawDistance, kMsaa,
             kAmbientOcclusion, kAudioOutput, kHdrOutput, kMonitor, kLanguage, kSmaa,
             kPauseOnFocusLoss, kFmvScale,
             kControllerHoldAim, kControllerSensitivity, kRumbleIntensity, kMenuAnimations,
             kGyroAiming, kGyroSensitivity, kAimAssist };

using rows::Kind;
using rows::Row;

constexpr Row kSettingsRows[] = {
    {Kind::Heading, -1, "DISPLAY"},
    {Kind::Value, kMonitor, "MONITOR"},
    {Kind::Value, kDisplayMode, "DISPLAY MODE"},
    {Kind::Value, kResolution, "RESOLUTION"},
    {Kind::Value, kRenderResolution, "RENDER RESOLUTION"},
    {Kind::Value, kFov, "FIELD OF VIEW"},
    {Kind::Value, kViewmodelFov, "VIEWMODEL FIELD OF VIEW"},
    {Kind::Value, kVsync, "VERTICAL SYNC"},
    {Kind::Value, kHdrOutput, "HDR OUTPUT"},
    {Kind::Value, kMaxFps, "MAX FRAME RATE"},
    {Kind::Value, kFpsCounter, "FPS COUNTER"},
    {Kind::Value, kFmvScale, "FMV SCALE"},
    {Kind::Heading, -1, "GRAPHICS"},
    {Kind::Value, kMsaa, "MSAA"},
    {Kind::Value, kSmaa, "SMAA"},
    {Kind::Value, kAniso, "ANISOTROPIC FILTERING"},
    {Kind::Value, kDrawDistance, "DRAW DISTANCE"},
    {Kind::Value, kAmbientOcclusion, "AMBIENT OCCLUSION"},
    {Kind::Value, kReloadBlur, "RELOAD BLUR"},
    {Kind::Heading, -1, "GENERAL"},
    {Kind::Value, kSkipIntros, "SKIP INTROS"},
    {Kind::Value, kMenuAnimations, "MENU ANIMATIONS"},
    {Kind::Value, kPauseOnFocusLoss, "PAUSE ON FOCUS LOSS"},
    {Kind::Value, kLanguage, "LANGUAGE"},
    {Kind::Value, kAudioOutput, "AUDIO OUTPUT"},
};

const std::vector<Row>& InputRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {{Kind::Heading, -1, "MOUSE"},
                                {Kind::Value, kSensitivity, "MOUSE SENSITIVITY"},
                                {Kind::Value, kInvertMouse, "INVERT LOOK"},
                                {Kind::Value, kHoldAim, "HOLD TO AIM"},
                                {Kind::Heading, -1, "KEYBOARD"}};
        size_t count = 0;
        const input::Action* actions = input::Actions(&count);
        for (size_t i = 0; i < count; i++) out.push_back({Kind::Bind, int(i), actions[i].label});
        out.push_back({Kind::Value, kWalkSpeed, "WALK SPEED"});
        return out;
    }();
    return list;
}

constexpr Row kControllerRows[] = {
    {Kind::Heading, -1, "CONTROLLER"},
    {Kind::Value, kControllerHoldAim, "HOLD TO AIM"},
    {Kind::Value, kAimAssist, "AIM ASSIST"},
    {Kind::Value, kControllerSensitivity, "SENSITIVITY"},
    {Kind::Value, kRumbleIntensity, "RUMBLE INTENSITY"},
    {Kind::Value, kGyroAiming, "GYRO AIMING"},
    {Kind::Value, kGyroSensitivity, "GYRO SENSITIVITY"},
};

// the setting an ON / OFF row flips, nullptr for any other row
bool* ToggleFlag(Settings& s, int id) {
    game::Options& o = game::Opt();
    switch (id) {
        case kVsync: return &s.vsync;
        case kFpsCounter: return &o.fps_counter;
        case kHdrOutput: return &o.hdr_output;
        case kReloadBlur: return &o.reload_blur;
        case kSmaa: return &o.smaa;
        case kSkipIntros: return &o.skip_intros;
        case kMenuAnimations: return &o.menu_animations;
        case kPauseOnFocusLoss: return &o.pause_on_focus_loss;
        case kInvertMouse: return &o.invert_mouse;
        case kHoldAim: return &o.hold_aim;
        case kControllerHoldAim: return &o.controller_hold_aim;
        case kGyroAiming: return &o.gyro_aiming;
        case kAimAssist: return &o.aim_assist;
        default: return nullptr;
    }
}

std::string g_display_monitor;  // the held monitor, beside the held display settings

// the monitor setting to read or write beside s: the held one beside the held display settings
std::string& MonitorSetting(Settings& s) {
    if (&s == &Get()) return game::Opt().monitor;
    return g_display_monitor;
}

// the setting's monitor in the list, the primary's index when it is not connected
int MonitorIndex(const std::vector<game::Display>& list, const std::string& monitor) {
    int primary = 0;
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].device == monitor) return int(i);
        if (list[i].primary) primary = int(i);
    }
    return primary;
}

// the mode shown, stepped first when step is not zero
Mode StepResolution(Settings& s, int step) {
    static std::map<std::string, std::vector<Mode>> cache;
    std::vector<game::Display> monitors = game::Displays();
    std::string device;  // the setting's monitor, the primary when it is not connected
    Mode desktop = Desktop();
    if (!monitors.empty()) {
        int index = MonitorIndex(monitors, MonitorSetting(s));
        device = monitors[size_t(index)].device;
        desktop = monitors[size_t(index)].desktop;
    }
    auto [it, fresh] = cache.try_emplace(device);
    if (fresh) it->second = game::Modes(device);
    const std::vector<Mode>& modes = it->second;
    Mode m = s.width && s.height ? Mode{s.width, s.height} : desktop;
    int i = 0;
    for (size_t k = 0; k < modes.size(); k++) {
        if (modes[k].width == m.width && modes[k].height == m.height) i = int(k);
    }
    if (step && !modes.empty()) {
        m = modes[size_t(StepIndex(i, step, int(modes.size())))];
        // the desktop's own mode is kept as 0, which follows the desktop
        bool native = m.width == desktop.width && m.height == desktop.height;
        s.width = native ? 0 : m.width;
        s.height = native ? 0 : m.height;
    }
    return m;
}

// both field of view rows: 45 to 140 in steps of 1, 70 the game's own
constexpr int kFovMin = 45;
constexpr int kFovMax = 140;
constexpr uint32_t kGameFov = 70;

void FovText(uint32_t fov, char (&text)[64]) {
    std::snprintf(text, sizeof(text), fov == kGameFov ? "%u (DEFAULT)" : "%u", fov);
}

// the monitor shown, stepped first when step is not zero
void StepMonitor(int step, Settings& s, char (&text)[64]) {
    std::vector<game::Display> list = game::Displays();
    if (list.empty()) {
        std::snprintf(text, sizeof(text), "PRIMARY");
        return;
    }
    int current = MonitorIndex(list, MonitorSetting(s));
    int i = StepIndex(current, step, int(list.size()));
    const game::Display& d = list[size_t(i)];
    // another monitor starts on its own desktop mode
    if (step && i != current) {
        MonitorSetting(s) = d.device;
        s.width = 0;
        s.height = 0;
    }
    const char* format = d.primary ? "%d: %ux%u (PRIMARY)" : "%d: %ux%u";
    std::snprintf(text, sizeof(text), format, d.number, d.desktop.width, d.desktop.height);
}

// the display rows that are no ON / OFF, stepped first; false for another row
bool StepDisplayRow(int id, int step, Settings& s, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kMonitor:
            StepMonitor(step, s, text);
            return true;
        case kDisplayMode: {
            static const char* const kModes[] = {"FULLSCREEN", "BORDERLESS", "WINDOWED"};
            int count = int(std::size(kModes));
            if (step) s.display_mode = DisplayMode(StepIndex(int(s.display_mode), step, count));
            std::snprintf(text, sizeof(text), "%s", kModes[std::min(int(s.display_mode), count - 1)]);
            return true;
        }
        case kResolution: {
            Mode m = StepResolution(s, step);
            std::snprintf(text, sizeof(text), "%ux%u", m.width, m.height);
            return true;
        }
        case kRenderResolution: {
            // 20% to 200% in steps of 5
            if (step) s.render_percent = uint32_t(StepRange(int(s.render_percent), 20, 200, 5, step));
            Mode out = Output();
            std::snprintf(text, sizeof(text), "%u%% (%ux%u)", s.render_percent, out.width * s.render_percent / 100,
                          out.height * s.render_percent / 100);
            return true;
        }
        case kFmvScale:
            std::snprintf(text, sizeof(text), "%s", kFmvScaleNames[StepTable(kFmvScales, o.fmv_scale, step)]);
            return true;
        case kFov:
            if (step) o.fov = uint32_t(StepRange(int(o.fov), kFovMin, kFovMax, 1, step));
            FovText(o.fov, text);
            return true;
        case kViewmodelFov:
            if (step) o.viewmodel_fov = uint32_t(StepRange(int(o.viewmodel_fov), kFovMin, kFovMax, 1, step));
            FovText(o.viewmodel_fov, text);
            return true;
        case kMaxFps: {
            uint32_t cap = kFpsCaps[StepTable(kFpsCaps, s.fps_cap, step)];
            if (cap == kFpsCapRefresh) std::snprintf(text, sizeof(text), "REFRESH RATE");
            else if (cap) std::snprintf(text, sizeof(text), "%u", cap);
            else std::snprintf(text, sizeof(text), "UNLIMITED");
            return true;
        }
        default:
            return false;
    }
}

// the graphics rows that are no ON / OFF, stepped first; false for another row
bool StepGraphicsRow(int id, int step, Settings& s, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kMsaa:
            std::snprintf(text, sizeof(text), "%s", kMsaaNames[StepTable(kMsaaValues, o.msaa, step)]);
            return true;
        case kAniso:
            std::snprintf(text, sizeof(text), "%s", kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]);
            return true;
        case kAmbientOcclusion:
            std::snprintf(text, sizeof(text), "%s", kAoNames[StepTable(kAoValues, o.ambient_occlusion, step)]);
            return true;
        case kDrawDistance:
            std::snprintf(text, sizeof(text), "%s", kDistanceNames[StepTable(kDistances, o.draw_distance, step)]);
            return true;
        default:
            return false;
    }
}

// both sensitivity rows: 0.1 to 5.0 in steps of 0.1
void StepSensitivity(float& value, int step, char (&text)[64]) {
    if (step) {
        int tenths = StepRange(int(std::lround(value * 10.0f)), 1, 50, 1, step);
        value = float(tenths) / 10.0f;
    }
    std::snprintf(text, sizeof(text), "%.1f", value);
}

// the setting of kLanguageNames[i]
const char* LanguageCode(int i) { return i ? game::kLanguages[i - 1] : "auto"; }

// the general, mouse and keyboard and controller rows that are no ON / OFF, stepped first
void StepInputRow(int id, int step, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kAudioOutput:
            std::snprintf(text, sizeof(text), "%s", kAudioOutputNames[StepTable(kAudioOutputs, o.audio_output, step)]);
            break;
        case kSensitivity:
            StepSensitivity(o.mouse_sensitivity, step, text);
            break;
        case kControllerSensitivity:
            StepSensitivity(o.controller_sensitivity, step, text);
            break;
        case kGyroSensitivity:
            StepSensitivity(o.gyro_sensitivity, step, text);
            break;
        case kRumbleIntensity:
            // 10% to 200% in steps of 10
            if (step) o.rumble_intensity = uint32_t(StepRange(int(o.rumble_intensity), 10, 200, 10, step));
            std::snprintf(text, sizeof(text), "%u%%", o.rumble_intensity);
            break;
        case kWalkSpeed:
            // 10% to 100% in steps of 5
            if (step) o.walk_speed = uint32_t(StepRange(int(o.walk_speed), 10, 100, 5, step));
            std::snprintf(text, sizeof(text), "%u%%", o.walk_speed);
            break;
        case kLanguage: {
            int count = int(std::size(kLanguageNames));
            int i = 0;
            while (i < count - 1 && o.language != LanguageCode(i)) i++;
            if (step) {
                i = StepIndex(i, step, count);
                o.language = LanguageCode(i);
            }
            bool restart = game::ResolveLanguage(o.language) != game::BootLanguage();
            const char* format = restart ? "%s (RESTART)" : "%s";
            std::snprintf(text, sizeof(text), format, kLanguageNames[i]);
            break;
        }
    }
}

const char* OnOff(bool on) { return on ? "ON" : "OFF"; }

void Flip(bool& on, int step) {
    if (step) on = !on;
}

// applies and saves
void Changed(bool display) {
    if (display) ApplyDisplay();
    audio::SetOutputMode(game::Opt().audio_output);
    Save();
}

// the MOUSE AND KEYBOARD page's values, its binds aside
void CopyKeyboardValues(game::Options& to, const game::Options& from) {
    to.mouse_sensitivity = from.mouse_sensitivity;
    to.invert_mouse = from.invert_mouse;
    to.hold_aim = from.hold_aim;
    to.walk_speed = from.walk_speed;
}

void CopyControllerValues(game::Options& to, const game::Options& from) {
    to.controller_hold_aim = from.controller_hold_aim;
    to.aim_assist = from.aim_assist;
    to.controller_sensitivity = from.controller_sensitivity;
    to.rumble_intensity = from.rumble_intensity;
    to.gyro_aiming = from.gyro_aiming;
    to.gyro_sensitivity = from.gyro_sensitivity;
}

}  // namespace

int StepIndex(int index, int step, int count) {
    if (count <= 0) return 0;
    index = (index + step) % count;
    return index < 0 ? index + count : index;
}

bool DisplayRow(int id) { return id == kMonitor || id == kDisplayMode || id == kResolution; }

// shown dimmed and left as it is: HDR output without an HDR display
bool Greyed(int id) { return id == kHdrOutput && !game::g_hdr_display; }

std::string RowText(int id, int step, Settings& s) {
    if (Greyed(id)) return "NOT SUPPORTED";
    char text[64] = "";
    if (bool* on = ToggleFlag(s, id)) {
        Flip(*on, step);
        std::snprintf(text, sizeof(text), "%s", OnOff(*on));
    } else if (!StepDisplayRow(id, step, s, text) && !StepGraphicsRow(id, step, s, text)) {
        StepInputRow(id, step, text);
    }
    if (step && &s == &Get()) Changed(DisplayRow(id));
    return text;
}

std::string KeyText(std::string keys) {
    if (keys.empty()) return "UNBOUND";
    for (char& c : keys) c = c == ',' ? '/' : char(std::toupper(uint8_t(c)));
    return keys;
}

void RestoreDefaults(int page) {
    game::Options& o = game::Opt();
    if (page == kControllerPage) {
        CopyControllerValues(o, game::Options{});
        Save();
        return;
    }
    if (page == kKeyboardPage) {
        CopyKeyboardValues(o, game::Options{});
        input::ResetBindings();
        return;
    }
    game::Options keep = o;
    Get() = Settings{};
    o = game::Options{};
    CopyKeyboardValues(o, keep);
    CopyControllerValues(o, keep);
    o.keys = keep.keys;
    Changed(true);
}

void HoldDisplay(int id, int step) {
    const Settings& s = Get();
    if (!g_display_held) {
        g_display_monitor = game::Opt().monitor;
        g_display.display_mode = s.display_mode;
        g_display.width = s.width;
        g_display.height = s.height;
    }
    RowText(id, step, g_display);
    g_display_held = g_display_monitor != game::Opt().monitor || g_display.display_mode != s.display_mode ||
                     g_display.width != s.width || g_display.height != s.height;
}

void Apply() {
    Settings& s = Get();
    game::Opt().monitor = g_display_monitor;
    s.display_mode = g_display.display_mode;
    s.width = g_display.width;
    s.height = g_display.height;
    g_display_held = false;
    Changed(true);
}

// the action's key and its alternate as their boxes show them, the alternate blank when there is none
std::vector<std::string> KeyBoxes(size_t action) {
    std::string keys = input::Binding(action);
    size_t comma = keys.find(',');
    std::string second;
    if (comma != std::string::npos) second = keys.substr(comma + 1);
    second = second.substr(0, second.find(','));
    if (!second.empty()) second = KeyText(second);
    return {KeyText(keys.substr(0, comma)), second};
}

}  // namespace panel

// as app::Pace, at this cap
void PaceAt(uint32_t cap) {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

void PaceFrame() {
    if (Get().fps_cap != kFpsCapRefresh) {
        Pace();
        return;
    }
    // a few under, so a variable refresh display stays in its range and vsync never waits a whole refresh
    constexpr uint32_t kRefreshMargin = 3;
    uint32_t rate = game::RefreshRate();
    PaceAt(rate > kRefreshMargin ? rate - kRefreshMargin : 0);
}

const rows::Row* rows::Page(int page, size_t* count) {
    if (page == panel::kKeyboardPage) {
        *count = panel::InputRows().size();
        return panel::InputRows().data();
    }
    if (page == panel::kControllerPage) {
        *count = std::size(panel::kControllerRows);
        return panel::kControllerRows;
    }
    static const std::vector<rows::Row> rows = [] {
        std::vector<rows::Row> out(std::begin(panel::kSettingsRows), std::end(panel::kSettingsRows));
        if (!game::HasLanguages()) std::erase_if(out, [](const rows::Row& row) { return row.id == panel::kLanguage; });
        return out;
    }();
    *count = rows.size();
    return rows.data();
}

std::string rows::Value(const Row& row, int step) {
    return row.kind == rows::Kind::Bind ? panel::KeyText(input::Binding(size_t(row.id)))
                                         : panel::RowText(row.id, step);
}

void rows::Defaults(int page) { panel::RestoreDefaults(page); }

bool rows::Enabled(const Row& row) { return row.kind != rows::Kind::Value || !panel::Greyed(row.id); }

bool MenuOpen() { return panel::g_open.load(std::memory_order_relaxed); }

bool Capturing() { return panel::g_capture >= 0; }

void ToggleMenu() {
    bool open = !MenuOpen();
    panel::g_open.store(open, std::memory_order_relaxed);
    panel::g_capture = -1;
    panel::g_display_held = false;
    if (open) input::ReleaseCursor();
}

}  // namespace app
