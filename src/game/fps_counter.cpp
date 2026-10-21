// Frame rate (top left) and the main menu watermark, drawn by the game in its small font over the finished frame.
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <optional>

#include "app/app.h"
#include "common/aspect.h"
#include "common/casing.h"
#include "common/options.h"
#include "game/flash.h"
#include "game/layout.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;
using namespace layout;
using flash::kClipNext;
using flash::kClipX;
using flash::kClipY;

namespace {

// the game's 2D layer: Text(font, wide text, x, y, size, rgba floats), viewport pixels from the top left
constexpr uint32_t kBegin = 0xD99A0;
constexpr uint32_t kText = 0xD9600;
constexpr uint32_t kFlush = 0xD4C50;
constexpr uint32_t kEnd = 0xD3740;
constexpr uint32_t kApplyStates = 0x29EF0;
constexpr uint32_t kQuads = 0x2F3710;
constexpr uint32_t kTexture = 0x56EF30;
constexpr uint32_t kFont = 0x2F433C;  // the small font

// on a frame 480 high
constexpr float kSize = 15;
constexpr float kMargin = 8;
constexpr float kGrey = 186 / 255.0f;  // the HUD's text

// as SCDA: at rest it ends 80 in from the right, on the row of the menu's button legend
constexpr char kWatermark[] = "XBE Recomp V2";
constexpr float kWatermarkRight = 80;
constexpr float kWatermarkTop = 427;
// the page's Main slides its legend in from the right and back out; x is the confirm prompt's at rest (frame 29)
struct TitlePage {
    const char* name;
    float legend_rest_x;
};
// longest name first, they are compared by prefix
constexpr TitlePage kTitlePages[] = {{"BlackTitleMenu2", -44.0f}, {"BlackTitleMenu", -53.5f}};

struct View {
    float unit;    // viewport pixels per line of a 480 frame
    float narrow;  // a wide frame is shown stretched: glyphs are drawn narrower by as much
};

// the glyph 0xD9600 draws for a code
uint32_t Glyph(uint32_t font, uint16_t code) {
    uint32_t glyph = R32(font + kFontGlyphs + (code & 0x7F) * 4);
    uint32_t last = R32(font + kFontLastGlyph);
    while (G<uint16_t>(glyph + kGlyphCode) != code && glyph != last) glyph += kGlyphSize;
    return glyph;
}

// in lines of a 480 frame, at kSize
float TextWidth(uint32_t font, const char* text) {
    float advance = 0;
    for (const char* c = text; *c; c++) advance += G<float>(Glyph(font, uint8_t(*c)) + kGlyphAdvance);
    return advance * G<float>(font + kFontScale) * kSize;
}

// x and y in lines of a 480 frame from the output's top left
void DrawText(X86Context& ctx, uint32_t font, const View& view, const char* text, float x, float y, bool shadow) {
    static const uint32_t block = mem::AllocSystem(0x60);
    uint32_t colour = block;
    uint32_t chars = block + 0x10;
    int n = std::min(int(std::strlen(text)), 31);
    for (int i = 0; i <= n; i++) G<uint16_t>(chars + i * 2) = i < n ? uint8_t(text[i]) : 0;
    for (int s = shadow; s >= 0; s--) {
        float rgb = s ? 0.0f : kGrey;  // the shadow black
        for (int i = 0; i < 4; i++) G<float>(colour + i * 4) = i == 3 ? 1.0f : rgb;
        float px = (x + s) * view.unit * view.narrow;
        float py = (y + s) * view.unit;
        CallGuest(ctx, kText, {font, chars, std::bit_cast<uint32_t>(px), std::bit_cast<uint32_t>(py),
                               std::bit_cast<uint32_t>(kSize * view.unit), colour});
    }
}

const TitlePage* CurrentTitlePage() {
    uint32_t ui = R32(kUi);
    if (!ui) return nullptr;
    const char* name = static_cast<const char*>(HostPtr(ui + kPageName));
    for (const TitlePage& page : kTitlePages)
        if (nocase::StartsWith(name, page.name)) return &page;
    return nullptr;
}

uint32_t Child(uint32_t clip, const char* name) {
    int left = 4096;
    for (uint32_t child = flash::FirstChild(clip); child && left; child = R32(child + kClipNext), left--)
        if (flash::IsClip(child) && nocase::Equals(flash::Name(child), name)) return child;
    return 0;
}

// a prompt is a clip of a "Button" icon and a "Text"; the lowest one confirms
uint32_t ConfirmPrompt(uint32_t main) {
    uint32_t found = 0;
    int left = 4096;
    for (uint32_t child = flash::FirstChild(main); child && left; child = R32(child + kClipNext), left--) {
        if (!flash::IsClip(child) || !Child(child, "Button") || !Child(child, "Text")) continue;
        if (!found || G<float>(child + kClipY) > G<float>(found + kClipY)) found = child;
    }
    return found;
}

// how far the legend is from its rest, in lines of a 480 frame like a stage pixel; none while the page has no legend
std::optional<float> LegendSlide(const TitlePage& page) {
    uint32_t prompt = ConfirmPrompt(Child(flash::Level(flash::kMenuLevel), "Main"));
    if (!prompt) return std::nullopt;
    return G<float>(prompt + kClipX) - page.legend_rest_x;
}

}  // namespace

// D3DVIEWPORT8
constexpr uint32_t kViewportWidth = 8;
constexpr uint32_t kViewportHeight = 12;

// the back buffer is the target here, the frame is shown next
void DrawOverlay(X86Context& ctx, uint8_t*) {
    uint32_t font = R32(kFont);
    uint32_t viewport = SYM(D3D_Device) + XDK_CDevice_Viewport;
    float w = float(R32(viewport + kViewportWidth));
    float h = float(R32(viewport + kViewportHeight));
    bool fps = game::Opt().fps_counter;
    const TitlePage* title = CurrentTitlePage();
    std::optional<float> slide = title ? LegendSlide(*title) : std::nullopt;
    bool watermark = slide.has_value();
    if (!(fps || watermark) || !font || !(h > 0)) return;
    View view{h / 480, w / h / game::Aspect()};
    float& width = G<float>(font + kFontScale);
    float game_width = width;
    float output_width = game::Aspect() * 480;
    float watermark_x = output_width - kWatermarkRight - TextWidth(font, kWatermark) + slide.value_or(0.0f);
    CallGuest(ctx, kBegin, {});
    width = game_width * view.narrow;
    if (fps) {
        char text[32];
        std::snprintf(text, sizeof text, "FPS: %.1f", double(game::g_fps.load()));
        DrawText(ctx, font, view, text, kMargin, kMargin, true);
    }
    if (watermark) DrawText(ctx, font, view, kWatermark, watermark_x, kWatermarkTop, false);
    width = game_width;
    if (R32(kQuads)) CallGuest(ctx, kFlush, {});
    CallGuest(ctx, kEnd, {});
    W32(kTexture, 0);
    CallGuest(ctx, kApplyStates, {});
}
