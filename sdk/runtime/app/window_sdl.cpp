// The Linux host's window (window_sdl.h): an SDL3 window and its events in place of the Win32 window and message
// loop. Events reach GameInfo::message as the Win32 messages a game already handles: keys as WM_KEYDOWN with the
// virtual key at that position, typed text as WM_CHAR, relative mouse motion and wheel as WM_INPUT carrying an
// app::RawMouse, focus as WM_ACTIVATE, and WM_SETCURSOR whenever the cursor may change. The window, its cursor and the
// Vulkan surface are touched on the window thread only; other threads post to it.
#include "app/window_sdl.h"

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>  // _NSGetExecutablePath: the port's /proc/self/exe
#endif
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <windows.h>  // the Linux compat header: window message and virtual key numbers

#include "app/app.h"
#include "app/sdl_keys.h"
#include "core/input.h"
#include "core/keyboard_pad.h"
#include "core/log.h"

namespace app {

namespace {

SDL_Window* g_window = nullptr;
std::vector<std::string> g_args;
std::atomic<uint32_t> g_pixel_w{1}, g_pixel_h{1};
std::atomic<bool> g_focused{true};
std::atomic<uint32_t> g_refresh{60};  // the display's rate, read on the window thread: Pace runs on a guest thread
Uint32 g_event_base = 0;  // SDL's user events: apply the display settings, refresh the cursor
enum UserEvent : Uint32 { kApply, kRefreshCursor };
constexpr uint32_t kHostApplyDisplay = WM_APP + 1;  // main.cpp's kApplyDisplay, which games look for
std::chrono::steady_clock::time_point g_ui_time = std::chrono::steady_clock::now();
std::function<void(int)> g_unhandled_key;

void Post(UserEvent kind) {
    SDL_Event e{};
    e.type = g_event_base + kind;
    SDL_PushEvent(&e);
}

// GameInfo::message, true when the game took it
bool Send(uint32_t msg, uintptr_t wp, intptr_t lp) {
    intptr_t result = 0;
    return kGame.message && kGame.message(g_window, msg, wp, lp, &result);
}


void UpdatePixels() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    g_pixel_w = uint32_t(std::max(w, 1));
    g_pixel_h = uint32_t(std::max(h, 1));
}

// DIAG(all): XBE_DIAG opens the temporary lines below; the lookup happens once
bool DiagOn() {
    static const bool on = [] { return std::getenv("XBE_DIAG") != nullptr; }();
    return on;
}

// DIAG(window plumbing): what size the renderer actually gets on Retina, and what a resize changes. Log only
void DiagWindow(const char* what, Uint32 type) {
    if (!DiagOn() || !g_window) return;
    int lw = 0, lh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(g_window, &lw, &lh);
    SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window));
    LOGR("diag: window %s (event %u): logical %dx%d, drawable %dx%d, density %.2f, scale %.2f, refresh %.1f Hz", what,
         unsigned(type), lw, lh, pw, ph, SDL_GetWindowPixelDensity(g_window), SDL_GetWindowDisplayScale(g_window),
         mode ? mode->refresh_rate : 0.0f);
}

// window thread only
void ApplyNow() {
    const Settings& s = Get();
    SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
    Mode out = Output();
    if (s.display_mode == DisplayMode::Windowed) {
        SDL_SetWindowFullscreen(g_window, false);
        SDL_SetWindowBordered(g_window, true);
        float density = std::max(SDL_GetWindowPixelDensity(g_window), 1.0f);
        SDL_SetWindowSize(g_window, int(out.width / density), int(out.height / density));
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    } else {
        // borderless: the desktop's own mode; fullscreen: the closest mode to the setting, which a Wayland
        // compositor scales rather than switches
        SDL_DisplayMode mode{};
        bool exclusive = s.display_mode == DisplayMode::Fullscreen && s.width && s.height &&
                         SDL_GetClosestFullscreenDisplayMode(display, int(s.width), int(s.height), 0, true, &mode);
        SDL_SetWindowFullscreenMode(g_window, exclusive ? &mode : nullptr);
        SDL_SetWindowFullscreen(g_window, true);
    }
    SDL_SyncWindow(g_window);
    UpdatePixels();
}

// the scancode's virtual key, as the window would have been sent it
void OnKey(const SDL_KeyboardEvent& k) {
    if (k.scancode == SDL_SCANCODE_F10) {
        if (k.down && !k.repeat) ToggleGraphicsMenu();
        return;
    }
    if (GraphicsMenuOpen() && !kGame.ui) {
        if (k.down && !k.repeat && k.scancode == SDL_SCANCODE_ESCAPE) ToggleGraphicsMenu();
        return;
    }
    if (!k.repeat) input::keyboard::OnKey(k.scancode, k.down);
    int vk = VkFromScancode(k.scancode);
    if (!vk) return;
    if (!k.down) {
        Send(k.mod & SDL_KMOD_ALT ? WM_SYSKEYUP : WM_KEYUP, uintptr_t(vk), 0);
        return;
    }
    // Windows sends Alt combinations and F10 as system keys; bit 30: the key was already down
    bool sys = (k.mod & SDL_KMOD_ALT) || vk == VK_F10 || vk == VK_MENU;
    intptr_t lp = k.repeat ? intptr_t(1) << 30 : 0;
    if (!Send(sys ? WM_SYSKEYDOWN : WM_KEYDOWN, uintptr_t(vk), lp) && !k.repeat && g_unhandled_key) g_unhandled_key(vk);
}

// UTF-8 text as the UTF-16 units WM_CHAR carries
void OnText(const char* text) {
    const auto* s = reinterpret_cast<const unsigned char*>(text);
    while (*s) {
        uint32_t c = *s++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3F >> more;
        while (more-- && (*s & 0xC0) == 0x80) c = c << 6 | (*s++ & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            Send(WM_CHAR, 0xD800 + (c >> 10), 0);
            Send(WM_CHAR, 0xDC00 + (c & 0x3FF), 0);
        } else {
            Send(WM_CHAR, c, 0);
        }
    }
}

// relative motion in whole counts, the fractions carried to the next event
void OnMotion(const SDL_MouseMotionEvent& m) {
    static float fx = 0, fy = 0;
    fx += m.xrel;
    fy += m.yrel;
    RawMouse raw{int32_t(fx), int32_t(fy), 0};
    fx -= float(raw.dx);
    fy -= float(raw.dy);
    if (raw.dx || raw.dy) Send(WM_INPUT, 0, reinterpret_cast<intptr_t>(&raw));
    Send(WM_SETCURSOR, 0, HTCLIENT);
}

void OnWheel(const SDL_MouseWheelEvent& w) {
    int notches = w.integer_y;
    if (w.direction == SDL_MOUSEWHEEL_FLIPPED) notches = -notches;
    // one report per notch, as raw input sends them
    for (int i = 0; i < std::abs(notches); i++) {
        RawMouse raw{0, 0, notches > 0 ? 1 : -1};
        Send(WM_INPUT, 0, reinterpret_cast<intptr_t>(&raw));
    }
}

// false once the window closes
bool Dispatch(const SDL_Event& e) {
    {
        std::lock_guard lock(UiLock());
        if (ImGui::GetCurrentContext()) ImGui_ImplSDL3_ProcessEvent(&e);
    }
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return false;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: OnKey(e.key); break;
        case SDL_EVENT_TEXT_INPUT: OnText(e.text.text); break;
        case SDL_EVENT_MOUSE_MOTION: OnMotion(e.motion); break;
        case SDL_EVENT_MOUSE_WHEEL: OnWheel(e.wheel); break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT) Send(WM_LBUTTONUP, 0, 0);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            g_focused = true;
            Send(WM_ACTIVATE, WA_ACTIVE, 0);
            Send(WM_SETCURSOR, 0, HTCLIENT);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            g_focused = false;
            input::keyboard::ReleaseAll();  // the key ups go to whichever window has the focus
            Send(WM_ACTIVATE, WA_INACTIVE, 0);
            break;
        case SDL_EVENT_WINDOW_RESIZED: DiagWindow("SDL_EVENT_WINDOW_RESIZED", e.type); break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            DiagWindow("SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED", e.type);
            UpdatePixels();
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            DiagWindow("SDL_EVENT_WINDOW_DISPLAY_CHANGED", e.type);
            UpdatePixels();
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
                ToggleGraphicsMenu();
                LOGI("input: R3 settings menu %s", GraphicsMenuOpen() ? "opened" : "closed");
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED: input::OnDeviceEvent(e.type, e.gdevice.which); break;
        default:
            if (e.type == g_event_base + kApply) {
                ApplyNow();
                Send(kHostApplyDisplay, 0, 0);
            } else if (e.type == g_event_base + kRefreshCursor) {
                Send(WM_SETCURSOR, 0, HTCLIENT);
            }
            break;
    }
    return true;
}


}  // namespace

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = [] {
#if defined(__ANDROID__)
        // the app's own storage: on Android SDL_GetBasePath is "./", the working directory, which is "/" and
        // not writable. The internal storage path is where the pushed data and the logs and settings go.
        if (const char* p = SDL_GetAndroidInternalStoragePath(); p && *p) return std::filesystem::path(p);
        return std::filesystem::path("/data/data/com.nfshp2.recomp/files");
#elif defined(__APPLE__)
        // _NSGetExecutablePath returns what was launched, which may be relative or a symlink to the executable, so
        // realpath turns it into the absolute path of the file itself: settings and logs go beside that, as on Windows
        char self[PATH_MAX] = {};
        uint32_t size = sizeof(self);
        if (_NSGetExecutablePath(self, &size) != 0) return std::filesystem::path();
        char real[PATH_MAX] = {};
        if (!::realpath(self, real)) return std::filesystem::path();
        return std::filesystem::path(real).parent_path();
#else
        char self[4096] = {};
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        return std::filesystem::path(std::string(self, n > 0 ? size_t(n) : 0)).parent_path();
#endif
    }();
    return dir;
}

int Instance() {
    static const int n = [] {
        for (size_t i = 0; i + 1 < g_args.size(); i++)
            if (g_args[i] == "--instance") return std::clamp(std::atoi(g_args[i + 1].c_str()), 0, 8);
        return 0;
    }();
    return n;
}

void* Window() { return g_window; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() { Post(kApply); }
void OpenGameWindow(bool apply) {
    if (apply) Post(kApply);
}
void Quit() {
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
}

bool Unfocused() {
    if (!g_window) return false;
    SDL_WindowFlags f = SDL_GetWindowFlags(g_window);
    if (f & SDL_WINDOW_HIDDEN) return false;
    return (f & SDL_WINDOW_MINIMIZED) || !g_focused;
}

std::string Utf8(const wchar_t* w) {
    std::string out;
    for (; *w; w++) {
        uint32_t c = uint32_t(*w);
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | c >> 6);
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | c >> 12);
            out += char(0x80 | (c >> 6 & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | c >> 18);
            out += char(0x80 | (c >> 12 & 0x3F));
            out += char(0x80 | (c >> 6 & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

void ShowError(const wchar_t* text) {
    std::string title = Utf8(kGame.short_title), body = Utf8(text);
    LOGE("%s", body.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), body.c_str(), g_window);
}

bool AskYesNo(const wchar_t* text) {
    std::string title = Utf8(kGame.short_title), body = Utf8(text);
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "No"}, {0, 1, "Yes"}};
    SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, g_window, title.c_str(), body.c_str(), 2, buttons, nullptr};
    int pressed = 0;
    return SDL_ShowMessageBox(&box, &pressed) && pressed == 1;
}

Mode Desktop() {
    SDL_DisplayID d = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
    if (!d) d = SDL_GetPrimaryDisplay();
    if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(d)) {
        float density = m->pixel_density > 0 ? m->pixel_density : 1.0f;
        return {uint32_t(std::lround(m->w * density)), uint32_t(std::lround(m->h * density))};
    }
    return {1920, 1080};
}

std::vector<const char*> VulkanInstanceExtensions() {
    Uint32 n = 0;
    const char* const* list = SDL_Vulkan_GetInstanceExtensions(&n);
    return list ? std::vector<const char*>(list, list + n) : std::vector<const char*>{};
}

bool CreateVulkanSurface(void* instance, uint64_t* surface) {
    struct Job {
        void* instance;
        uint64_t* surface;
        bool ok = false;
    } job{instance, surface};
    auto make = [](void* p) {
        auto* j = static_cast<Job*>(p);
        VkSurfaceKHR s = VK_NULL_HANDLE;
        j->ok = SDL_Vulkan_CreateSurface(g_window, static_cast<VkInstance>(j->instance), nullptr, &s);
        if (!j->ok) LOGE("vulkan: SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        *j->surface = reinterpret_cast<uint64_t>(s);
    };
    if (!SDL_RunOnMainThread(make, &job, true)) return false;
    return job.ok;
}

void WindowPixels(uint32_t& w, uint32_t& h) {
    w = g_pixel_w;
    h = g_pixel_h;
}

// ImGui's platform frame from the render thread: sizes and time only; input arrives through events on the window
// thread, and the panel's gamepad navigation reads the pads here as imgui_impl_sdl3 does
void UiNewFrame() {
    ImGuiIO& io = ImGui::GetIO();
    int lw = 0, lh = 0;
    SDL_GetWindowSize(g_window, &lw, &lh);
    lw = std::max(lw, 1);
    lh = std::max(lh, 1);
    io.DisplaySize = ImVec2(float(lw), float(lh));
    io.DisplayFramebufferScale = ImVec2(float(g_pixel_w) / lw, float(g_pixel_h) / lh);
    auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::max(std::chrono::duration<float>(now - g_ui_time).count(), 1e-4f);
    g_ui_time = now;
    if (!(io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad)) return;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    SDL_Gamepad* pad = ids && count ? SDL_GetGamepadFromID(ids[0]) : nullptr;
    if (ids && count && !pad) pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
    if (!pad) {
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        return;
    }
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    struct Map {
        ImGuiKey key;
        SDL_GamepadButton button;
    };
    static const Map kMap[] = {
        {ImGuiKey_GamepadStart, SDL_GAMEPAD_BUTTON_START},       {ImGuiKey_GamepadBack, SDL_GAMEPAD_BUTTON_BACK},
        {ImGuiKey_GamepadFaceDown, SDL_GAMEPAD_BUTTON_SOUTH},    {ImGuiKey_GamepadFaceRight, SDL_GAMEPAD_BUTTON_EAST},
        {ImGuiKey_GamepadFaceLeft, SDL_GAMEPAD_BUTTON_WEST},     {ImGuiKey_GamepadFaceUp, SDL_GAMEPAD_BUTTON_NORTH},
        {ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},    {ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {ImGuiKey_GamepadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},  {ImGuiKey_GamepadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    };
    for (const Map& m : kMap) io.AddKeyEvent(m.key, SDL_GetGamepadButton(pad, m.button));
    auto stick = [&](ImGuiKey key, SDL_GamepadAxis axis, float sign) {
        float v = float(SDL_GetGamepadAxis(pad, axis)) / 32767.0f * sign;
        io.AddKeyAnalogEvent(key, v > 0.3f, std::clamp((v - 0.3f) / 0.7f, 0.0f, 1.0f));
    };
    stick(ImGuiKey_GamepadLStickLeft, SDL_GAMEPAD_AXIS_LEFTX, -1);
    stick(ImGuiKey_GamepadLStickRight, SDL_GAMEPAD_AXIS_LEFTX, 1);
    stick(ImGuiKey_GamepadLStickUp, SDL_GAMEPAD_AXIS_LEFTY, -1);
    stick(ImGuiKey_GamepadLStickDown, SDL_GAMEPAD_AXIS_LEFTY, 1);
}

// no boot splash or handover window on Linux: the game window opens at once
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

// kFpsCapRefresh is the rate of the display the window is on, as main.cpp asks Windows for it
uint32_t RefreshCap() { return g_refresh.load(); }

void Pace() {
    using clock = std::chrono::steady_clock;
    // DIAG(frame pacing): is the 60 fps cap actually hitting, and where are the stalls? at most 5 lines a second;
    // temporary
    if (DiagOn()) {
        static clock::time_point prev = clock::now();
        static clock::time_point window = prev;
        static int lines = 0;
        clock::time_point t = clock::now();
        double ms = std::chrono::duration<double, std::milli>(t - prev).count();
        prev = t;
        if (t - window >= std::chrono::seconds(1)) {
            window = t;
            lines = 0;
        }
        uint32_t cap = Get().fps_cap;
        if (ms > 25.0 && lines < 5) {
            lines++;
            LOGR("diag: pace gap %.2f ms, fps_cap %u, cap in use %u fps, display %u Hz", ms, cap,
                 cap == kFpsCapRefresh ? RefreshCap() : cap, g_refresh.load());
        }
    }
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    if (cap == kFpsCapRefresh) cap = RefreshCap();
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    // sleeping the rest of the frame beats a yield spin: the wake-up costs about a millisecond, which the next frame absorbs
    if (next > clock::now()) std::this_thread::sleep_until(next);
}

void SetArguments(int argc, char** argv) { g_args.assign(argv, argv + argc); }

void SetUnhandledKey(std::function<void(int)> f) { g_unhandled_key = std::move(f); }

bool OpenWindow(const std::string& title) {
    g_event_base = SDL_RegisterEvents(2);
    g_window = SDL_CreateWindow(title.c_str(), 1280, 720,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!g_window) {
        LOGE("no window: %s", SDL_GetError());
        return false;
    }
    // the refresh rate is cached here: SDL_GetCurrentDisplayMode may only be asked on the window thread
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window)))
        if (mode->refresh_rate > 0) g_refresh.store(uint32_t(std::lround(mode->refresh_rate)));
    Send(WM_CREATE, 0, 0);
    ApplyNow();
    DiagWindow("created", 0);
    SDL_ShowWindow(g_window);
#if !defined(__ANDROID__)
    SDL_StartTextInput(g_window);
#endif
    return true;
}

bool PumpEvents(int timeout_ms) {
    SDL_Event e;
    if (!SDL_WaitEventTimeout(&e, timeout_ms)) return true;
    if (!Dispatch(e)) return false;
    while (SDL_PollEvent(&e))
        if (!Dispatch(e)) return false;
    return true;
}

}  // namespace app

