// Shared F10 panel for titles without a custom GameInfo::ui panel.
#include "app/app.h"
#include "core/keyboard_pad.h"
#include "core/host_pause.h"
#include "core/log.h"
#include "core/input.h"
#include <imgui.h>
#include <atomic>
#include <algorithm>
#include <cstdio>

namespace app {
namespace {
std::atomic<bool> open{false};
Settings draft;
bool refresh = true;

template<size_t N>
void Choice(const char* label, uint32_t& value, const uint32_t (&values)[N], const char* const (&names)[N]) {
    const char* selected = "Custom";
    for (size_t i = 0; i < N; ++i) if (value == values[i]) selected = names[i];
    if (ImGui::BeginCombo(label, selected)) {
        for (size_t i = 0; i < N; ++i)
            if (ImGui::Selectable(names[i], value == values[i])) value = values[i];
        ImGui::EndCombo();
    }
}
}
bool GraphicsMenuOpen() { return open.load(); }
void SetGraphicsMenuOpen(bool value) {
    std::lock_guard lock(UiLock());
    input::keyboard::ReleaseAll();
    host::Pause().Menu(value);
    open = value;
    if (value) for (int port = 0; port < 4; ++port) input::Rumble(port, 0, 0);
    LOGI("settings: %s; guest clocks and audio %s", value ? "opened" : "closed", value ? "paused" : "resumed");
}
void ToggleGraphicsMenu() {
    std::lock_guard lock(UiLock());
    if (kGame.toggle_menu) { kGame.toggle_menu(); return; }
    SetGraphicsMenuOpen(!open);
    refresh = true;
    input::keyboard::ReleaseAll();
}
void DrawGraphicsMenu(double fps) {
    if (!open || kGame.ui) return;
    if (refresh) { draft = Get(); refresh = false; }
    auto& io = ImGui::GetIO();
    const float scale = ImGui::GetFontSize() / 13.0f;
    ImGui::SetNextWindowSize(ImVec2(std::min(520.0f * scale, io.DisplaySize.x - 20),
                                  std::min(510.0f * scale, io.DisplaySize.y - 20)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Appearing, ImVec2(.5f,.5f));
    bool visible = true;
#ifdef __ANDROID__
    const char* title = "Settings  |  R3";
#else
    const char* title = "Graphics settings  |  F10";
#endif
    if (ImGui::Begin(title, &visible, ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("%.1f FPS", fps);
#ifdef __ANDROID__
        if (ImGui::IsWindowAppearing()) ImGui::SetWindowFocus();
        ImGui::TextWrapped("Game paused. R3 closes this panel and resumes. D-pad or left stick: navigate. A: select. B: back.");
        if (ImGui::CollapsingHeader("Xbox / Retroid controls")) {
            ImGui::TextWrapped("A / B / X / Y: Xbox face buttons. D-pad and both sticks: Xbox directional controls. L2 / R2: analog triggers. L1: White. R1: Black. Start: Start / pause. Select: Back. L3: left stick click. R3: this menu.");
        }
#else
        ImGui::TextWrapped("Game paused. R3, F10 or Escape closes this panel and resumes.");
#endif
        ImGui::Separator();
        uint32_t mode = uint32_t(draft.display_mode);
        const uint32_t modes[] = {0,1,2};
        const char* const mode_names[] = {"Fullscreen", "Borderless", "Windowed"};
        Choice("Display mode", mode, modes, mode_names);
        ImGui::SetItemDefaultFocus();
        draft.display_mode = DisplayMode(mode);
        const Mode sizes[] = {{0,0},{640,480},{1280,720},{1280,800},{1920,1080},{1920,1200},{2560,1440},{3840,2160}};
        char current[64];
        if (!draft.width || !draft.height) std::snprintf(current,sizeof(current),"Desktop");
        else std::snprintf(current,sizeof(current),"%u x %u",draft.width,draft.height);
        if (ImGui::BeginCombo("Output resolution", current)) {
            for (auto s : sizes) {
                char label[64];
                if (!s.width) std::snprintf(label,sizeof(label),"Desktop");
                else std::snprintf(label,sizeof(label),"%u x %u",s.width,s.height);
                if (ImGui::Selectable(label, s.width == draft.width && s.height == draft.height)) {
                    draft.width=s.width; draft.height=s.height;
                }
            }
            ImGui::EndCombo();
        }
        int scale = int(draft.render_percent);
        if (ImGui::SliderInt("Render scale", &scale, 25, 300, "%d%%")) draft.render_percent=uint32_t(scale);
        ImGui::TextWrapped("100%% renders at the selected output height. Higher values supersample; lower values reduce GPU load.");
        const uint32_t filters[] = {0,2,4,8,16};
        const char* const filter_names[] = {"Game default", "2x", "4x", "8x", "16x"};
        Choice("Texture filtering",draft.aniso,filters,filter_names);
        const uint32_t caps[] = {30,60,90,120,144,165,240,0};
        const char* const cap_names[] = {"30", "60", "90", "120", "144", "165", "240", "Unlimited"};
        Choice("Frame limit",draft.fps_cap,caps,cap_names);
        ImGui::Checkbox("VSync", &draft.vsync);
        ImGui::TextWrapped("Some games depend on their original frame rate. Use 60 FPS unless the title supports other rates.");
        ImGui::Separator();
        if (ImGui::Button("Apply and save")) {
            auto& s = Get();
            s.display_mode=draft.display_mode; s.width=draft.width; s.height=draft.height;
            s.render_percent=draft.render_percent; s.aniso=draft.aniso;
            s.fps_cap=draft.fps_cap; s.vsync=draft.vsync;
            ApplyDisplay(); Save();
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert edits")) draft=Get();
        ImGui::SameLine();
        if (ImGui::Button("Close")) visible=false;
    }
    ImGui::End();
    if (!visible) ToggleGraphicsMenu();
}
} // namespace app
