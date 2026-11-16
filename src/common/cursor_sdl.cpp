// The game menus' cursor through SDL3 (Linux; cursor.cpp on Windows): cursor.png or cursor.bmp beside the exe,
// untinted, its top-left pixel on the pointer. Its visible part is sized, not the image: the longer side is 17 units of
// the 480 line page. Without a file the system arrow stays.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "app/app.h"
#include "common/aspect.h"
#include "common/menu.h"
#include "core/log.h"

namespace app {

namespace {

constexpr float kPageHeight = 480.0f;
constexpr float kVisibleSize = 17.0f;
constexpr uint32_t kVisibleAlpha = 32;  // a fainter pixel is edge fringe, not part of the shape

SDL_Surface* g_image = nullptr;  // ARGB8888, straight alpha
int g_extent = 0;                // the longer side of the visible part, image pixels
bool g_read = false;
SDL_Cursor* g_cursor = nullptr;
SDL_Cursor* g_previous = nullptr;
int g_height = 0;  // the frame height g_cursor is sized for

// the longer side of the pixels at or over kVisibleAlpha, the whole image when none is
int VisibleExtent(SDL_Surface* s) {
    int x0 = s->w, y0 = s->h, x1 = 0, y1 = 0;
    for (int y = 0; y < s->h; y++) {
        const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(s->pixels) + size_t(y) * s->pitch);
        for (int x = 0; x < s->w; x++) {
            if (row[x] >> 24 < kVisibleAlpha) continue;
            x0 = std::min(x0, x);
            x1 = std::max(x1, x + 1);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y + 1);
        }
    }
    return x1 > x0 ? std::max(x1 - x0, y1 - y0) : std::max(s->w, s->h);
}

void ReadCursorFile() {
    g_read = true;
    // SDL decodes png and bmp only: WIC's dds reading has no counterpart here
    std::error_code ec;
    std::filesystem::path dds = ExeDir() / "cursor.dds";
    if (std::filesystem::exists(dds, ec)) LOGW("cursor: cursor.dds is unsupported here, use cursor.png");
    for (const char* name : {"cursor.png", "cursor.bmp"}) {
        std::string path = (ExeDir() / name).string();
        SDL_Surface* loaded = std::string(name).ends_with(".png") ? SDL_LoadPNG(path.c_str()) : SDL_LoadBMP(path.c_str());
        if (!loaded) continue;
        g_image = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ARGB8888);
        SDL_DestroySurface(loaded);
        if (!g_image) continue;
        g_extent = VisibleExtent(g_image);
        LOGI("cursor: %s, %dx%d, visible extent %d", path.c_str(), g_image->w, g_image->h, g_extent);
        return;
    }
}

SDL_Cursor* Build(int frame_height) {
    float scale = frame_height * kVisibleSize / kPageHeight / float(g_extent);
    int cw = std::max(1, int(std::lround(g_image->w * scale)));
    int ch = std::max(1, int(std::lround(g_image->h * scale)));
    SDL_Surface* sized = SDL_ScaleSurface(g_image, cw, ch, SDL_SCALEMODE_LINEAR);
    if (!sized) return nullptr;
    SDL_Cursor* c = SDL_CreateColorCursor(sized, 0, 0);
    SDL_DestroySurface(sized);
    return c;
}

}  // namespace

void* ArrowCursor() {
    static SDL_Cursor* arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    return arrow;
}

void ShowCursorImage(void* cursor) {
    if (!cursor) {
        SDL_HideCursor();
        return;
    }
    SDL_SetCursor(static_cast<SDL_Cursor*>(cursor));
    SDL_ShowCursor();
}

void* CursorImage() {
    if (!g_read) ReadCursorFile();
    auto* window = static_cast<SDL_Window*>(Window());
    int w = 0, h = 0;
    if (!g_image || !window || !SDL_GetWindowSize(window, &w, &h)) return ArrowCursor();
    // the frame is shown in the middle of the window
    int frame_height = int(std::min(float(h), w / game::Aspect()));
    if (frame_height <= 0) return ArrowCursor();
    if (frame_height != g_height || !g_cursor) {
        SDL_Cursor* cursor = Build(frame_height);
        if (!cursor) return ArrowCursor();
        // the one on screen goes a rebuild later, never while it is shown
        if (g_previous) SDL_DestroyCursor(g_previous);
        g_previous = g_cursor;
        g_cursor = cursor;
        g_height = frame_height;
    }
    return g_cursor;
}

}  // namespace app
