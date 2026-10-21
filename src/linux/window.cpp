// the game's frame on the screen: the window, the renderer and the texture the CPU renderer fills
#include "linux/window.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"

namespace window {

namespace {

SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;
uint32_t g_texture_w = 0, g_texture_h = 0;

std::mutex g_lock;  // the renderer thread writes a frame while the window thread shows it
std::vector<uint8_t> g_buffers[2];
int g_write = 0;   // the buffer the renderer writes into
int g_ready = -1;  // the buffer with the newest frame
bool g_shown = false;  // the newest frame is on the screen
uint32_t g_frame_w = 0, g_frame_h = 0;
bool g_focused = true;
bool g_running = true;
std::atomic<bool> g_display_apply{false};
uint64_t shown_ = 0;

// SDL takes the title as bytes, the game's title is wide
std::string Utf8(const wchar_t* text) {
    std::string out;
    for (const wchar_t* c = text; c && *c; c++) {
        uint32_t cp = uint32_t(*c);
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

void MakeTexture(uint32_t w, uint32_t h) {
    if (g_texture) SDL_DestroyTexture(g_texture);
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_STREAMING, int(w), int(h));
    if (!g_texture) {
        LOGE("window: no texture: %s", SDL_GetError());
        return;
    }
    SDL_SetTextureScaleMode(g_texture, SDL_SCALEMODE_LINEAR);
    // the console's framebuffer carries no alpha, the frame is opaque
    SDL_SetTextureBlendMode(g_texture, SDL_BLENDMODE_NONE);
    g_texture_w = w;
    g_texture_h = h;
}

// the settings' display mode and size on the window
void ApplySettings() {
    if (!g_window) return;
    const app::Settings& s = app::Get();
    bool fullscreen = s.display_mode == app::DisplayMode::Fullscreen;
    bool borderless = s.display_mode == app::DisplayMode::Borderless;
    if (fullscreen) {
        SDL_SetWindowFullscreen(g_window, true);
        return;
    }
    SDL_SetWindowFullscreen(g_window, false);
    SDL_SetWindowBordered(g_window, !borderless);
    app::Mode mode = borderless ? app::Desktop() : app::Output();
    SDL_SetWindowSize(g_window, int(mode.width), int(mode.height));
}

}  // namespace

bool Open(const wchar_t* title, uint32_t width, uint32_t height, bool fullscreen, bool borderless) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOGE("window: SDL failed to start: %s", SDL_GetError());
        return false;
    }
    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN;
    if (borderless) flags |= SDL_WINDOW_BORDERLESS;
    g_window = SDL_CreateWindow(Utf8(title).c_str(), int(width), int(height), flags);
    if (!g_window) {
        LOGE("window: no window: %s", SDL_GetError());
        return false;
    }
    g_renderer = SDL_CreateRenderer(g_window, nullptr);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, "software");
    if (!g_renderer) {
        LOGE("window: no renderer: %s", SDL_GetError());
        return false;
    }
    MakeTexture(width, height);
    SDL_ShowWindow(g_window);
    if (fullscreen) SDL_SetWindowFullscreen(g_window, true);
    LOGI("window: %ux%u %s, %s driver, %s renderer", width, height,
         fullscreen ? "fullscreen" : borderless ? "borderless" : "windowed",
         SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "no driver",
         g_renderer ? SDL_GetRendererName(g_renderer) : "no renderer");
    return true;
}

void Close() {
    if (g_texture) SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    g_texture = nullptr;
    g_renderer = nullptr;
    g_window = nullptr;
    SDL_Quit();
}

void* Handle() { return g_window; }

bool Pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                g_running = false;
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                g_focused = true;
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                g_focused = false;
                break;
            default:
                break;
        }
    }
    if (g_display_apply.exchange(false)) ApplySettings();
    return g_running;
}

void Submit(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t pitch) {
    std::lock_guard lock(g_lock);
    std::vector<uint8_t>& buffer = g_buffers[g_write];
    if (buffer.size() != size_t(width) * height * 4) buffer.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; y++)
        std::memcpy(buffer.data() + size_t(y) * width * 4, pixels + size_t(y) * pitch, size_t(width) * 4);
    g_frame_w = width;
    g_frame_h = height;
    g_ready = g_write;
    g_write ^= 1;
    g_shown = false;
}

void Show() {
    std::lock_guard lock(g_lock);
    if (g_ready < 0 || g_shown || !g_renderer) return;
    if (!g_texture || g_texture_w != g_frame_w || g_texture_h != g_frame_h) MakeTexture(g_frame_w, g_frame_h);
    if (!g_texture) return;
    if (!SDL_UpdateTexture(g_texture, nullptr, g_buffers[g_ready].data(), int(g_frame_w * 4)))
        LOGE("window: texture %ux%u update: %s", g_frame_w, g_frame_h, SDL_GetError());
    if (!SDL_RenderTexture(g_renderer, g_texture, nullptr, nullptr))
        LOGE("window: render: %s", SDL_GetError());
    SDL_RenderPresent(g_renderer);
    g_shown = true;
    shown_++;
}

uint64_t Shown() { return shown_; }

bool SaveScreenshot(const char* path) {
    std::lock_guard lock(g_lock);
    if (g_ready < 0) return false;
    // the guest's frame as it is: the console's framebuffer carries no alpha, so it is filled in
    SDL_Surface* surface = SDL_CreateSurface(int(g_frame_w), int(g_frame_h), SDL_PIXELFORMAT_BGRA32);
    if (!surface) return false;
    for (uint32_t y = 0; y < g_frame_h; y++) {
        uint8_t* row = static_cast<uint8_t*>(surface->pixels) + size_t(y) * surface->pitch;
        std::memcpy(row, g_buffers[g_ready].data() + size_t(y) * g_frame_w * 4, size_t(g_frame_w) * 4);
        for (uint32_t x = 0; x < g_frame_w; x++) row[x * 4 + 3] = 255;
    }
    bool ok = SDL_SaveBMP(surface, path);
    SDL_DestroySurface(surface);
    return ok;
}

bool Focused() { return g_focused; }

void RequestDisplayApply() { g_display_apply = true; }

}  // namespace window
