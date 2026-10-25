#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <string>

#include "common/gyro.h"

// the game's own settings, in the settings file through app::kGame.options
namespace game {

// the languages, saved by these codes: English, French, German, Italian, Spanish
inline constexpr const char* kLanguages[] = {"int", "fra", "deu", "ita", "esp"};

inline constexpr float kGameDeadZone = 0.2f;  // each stick axis: pad+0xCC, set at 0xC2857
inline constexpr bool kCustomTexturesDefault = false;  // texture replacement, on in the SDK's settings

struct Options {
    std::string monitor;  // its device name, \\.\DISPLAY2; empty the primary
    bool skip_intros = false;  // logos at boot and the film after pressing start
    bool fps_counter = false;
    uint32_t fmv_scale = 0;  // 4:3 films on a wider screen: 0 fill it, top and bottom cut; 1 kept 4:3
    uint32_t colour_blind = 0;  // 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia
    bool hdr_output = true;  // HDR10 on a display with Windows HDR on
    bool reload_blur = true;
    uint32_t ambient_occlusion = 0;  // screen space, from the scene's depth: 0 off, 1 SSAO, 2 GTAO
    uint32_t msaa = 0;  // samples a pixel of the scene, 0 off
    uint32_t draw_distance = 1;  // times the game's LOD and draw distances
    float mouse_sensitivity = 1.0f;  // horizontal
    float mouse_vertical_sensitivity = 1.0f;
    float aim_horizontal_sensitivity = 1.0f;  // times the mouse's while the view is zoomed in: aiming and scopes
    float aim_vertical_sensitivity = 1.0f;
    bool invert_mouse = false;  // the Y axis
    bool invert_mouse_x = false;
    bool hold_aim = true;  // scopes too, keyboard and mouse
    uint32_t fov = 70;  // horizontal degrees on a 4:3 frame, 70 is the game's
    uint32_t viewmodel_fov = 70;  // the gun and arms, the same measure
    uint32_t walk_speed = 50;  // percent of running, while the walk key is held
    uint32_t audio_output = 0;  // audio::kOutputAuto, kOutputStereo, kOutputSurround or kOutputMono
    std::string language = "auto";  // "auto" (the Windows display language) or one of kLanguages
    bool smaa = false;  // over the finished frame, HUD included; combines with msaa
    bool pause_on_focus_loss = true;  // the game and its sound stop while the window is in the background
    bool aim_assist = true;  // the game's pull to targets on a controller, never on the keyboard and mouse
    bool controller_hold_aim = false;  // scopes too; off is the game's own, held to aim and scopes toggled
    float controller_sensitivity = 1.0f;  // horizontal, times the game's right stick look speeds
    float controller_vertical_sensitivity = 1.0f;
    float controller_aim_horizontal_sensitivity = 1.0f;  // times the stick's while the view is zoomed in
    float controller_aim_vertical_sensitivity = 1.0f;
    bool invert_controller_x = false;  // the Y axis is the game's own invert look
    float controller_dead_zone = 0.0f;  // 0 the game's own kGameDeadZone
    uint32_t rumble_intensity = 100;  // percent of the game's vibration
    gyro::Settings gyro;  // the controller's gyro turns the view, beside the right stick
    bool menu_animations = true;  // off: menu pages and the pause menu change without sliding in and out
    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
};
Options& Opt();
// one of kLanguages for a language setting: "auto" is the Windows display language, English when the game lacks it
std::string ResolveLanguage(const std::string& setting);
// the language setting as this run started, resolved: the language the game boots in
const std::string& BootLanguage();
bool HasLanguages();  // language.cpp
inline std::atomic<float> g_fps{0};  // from the host, for the game's own counter
inline std::atomic<bool> g_hdr_display{false};  // the window's display has Windows HDR on, from the renderer
inline std::atomic<bool> g_hide_hud{false};  // the toggle HUD key: the gameplay HUD is not drawn
inline std::atomic<bool> g_focus_paused{false};  // the game thread waits for the window's focus
inline std::atomic<uint32_t> g_focus_pauses{0};  // focus pauses ended so far
void WaitWhileUnfocused();  // focus_pause.cpp: game thread, before the present

}  // namespace game
