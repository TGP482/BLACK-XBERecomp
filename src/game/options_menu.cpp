// Recomp settings in the game's own options menus: RECOMP SETTINGS, MOUSE AND KEYBOARD and CONTROLLER buttons under
// the stock ones load the page again with the F10 panel's rows in their place. A row is two copies of a stock button
// clip, its name and its value; the name's copy stands in for the script's button object, so the script moves the
// focus and the cursor works. On the pause menu's options our buttons take the prompts' rows: the select prompt goes,
// copies of the confirm and back prompts sit side by side below.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/translations.h"
#include "core/log.h"
#include "game/flash.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;
using app::rows::Row;
using flash::Button;
using flash::Find;
using flash::FindField;
using flash::Focus;
using flash::Link;
using flash::Member;
using flash::Members;
using flash::Method;
using flash::Set;
using flash::String;
using flash::Text;

namespace {

constexpr uint32_t kHashTable = 0x2AC908;
const char* const kCaptions[] = {"RECOMP SETTINGS", "MOUSE AND KEYBOARD", "CONTROLLER"};
constexpr int kEntries = int(std::size(kCaptions));
constexpr int kSlots = 8;
constexpr int kDepth = 100;
constexpr int kEscape = 0x1B;
constexpr int kSettingsMode = 1;  // a page's mode: 0 our buttons, else the caption shown plus one
constexpr int kPrompts = 2;

// the selection bar, Main.ButtonSelector: its parts by depth; once shown it stops on frame 10, its data's frame 9
constexpr int kBarRest = 9;
constexpr int kBarBox = 1;
constexpr int kBarLine = 5;
constexpr int kBarTick = 7;
constexpr int kBarScan = 11;
constexpr float kBarLeft = -5.0f;  // the box's left end and the left tick
constexpr float kTickGap = 10.0f;  // past the end of the widest value
constexpr float kNameGap = 12.0f;  // between the widest name and the values

// the rows' font, as the name entry's caret measures it
constexpr uint32_t kFindFont = 0x13CD0;   // stdcall (game object, name): a font
constexpr uint32_t kFontName = 0x28E63C;  // "Eurostile LT Std"
constexpr float kTextHeight = 15.0f;      // the stock buttons' text fields
constexpr float kTextLeft = 2.0f;         // the text field in its button's label clip

// the stock bar on frame 10: its box's x scale, the x of its right tick, right line and Scan
struct Bar {
    float box;
    float tick;
    float line;
    float scan;
};

// an options page of the game
struct Host {
    const char* page;
    const char* holder;        // the clip in Main its buttons are in, null for Main
    const char* button;        // a stock button, copied for ours
    const char* under;         // the clip our buttons go under, the first this far down
    float drop;
    const char* first;         // the first and last button objects of its ring
    const char* last;
    // the rows, from the stock button
    float pitch;
    float name_x;
    float value_x;
    float scale;
    const char* hint;          // the string of a prompt our buttons take the place of
    const char* stock[14];     // what the rows take the place of
    Bar bar;
    // prompts our buttons push down, copied in a row below them: x of each and y, from the stock clip
    const char* prompts[kPrompts];
    float prompt_x[kPrompts];
    float prompt_y;
};
constexpr Host kHosts[] = {
    {"OptionsMenuXbox", "TextAnim", "_level4.Main.TextAnim.ControllerSetup_Text", "Playerprofile_Text", 25.0f,
     "controllerButton", "Playerprofile", 25.0f, -37.0f, 185.0f, 0.9f, nullptr,
     {"ControllerSetup_Text", "SoundSetup_Text", "PictureSetup_Text", "Playerprofile_Text"},
     {1.0f, 245.0f, 445.0f, -5.0f}, {}, {}, 0.0f},
    {"PauseMenuOptionXbox", nullptr, "_level4.Main.ToggleCrouch", "ToggleCrouch", 222.0f,
     "toggleCrouchButton", "musicVolumeButton", 27.0f, 0.0f, 245.0f, 1.0f, "$FE_SELECT",
     {"ToggleCrouch", "ToggleCrouchOn_Text", "ToggleCrouchOff_Text", "InvertLook", "InvertLookOn_Text",
      "InvertLookOff_Text", "Vibration", "VibrationOn_Text", "VibrationOff_Text", "SFXVolume", "Sound_Volume",
      "MusicVolume", "Music_Volume"},
     {1.4f, 343.55f, 443.55f, 168.35f},
     // stage y 458 under the third button's bar (to 441), the screen ends at 480
     {"_level4.Main.ConfirmPrompt", "_level4.Main.BackPrompt"}, {-16.0f, 164.0f}, 306.0f},
};

// a text the page shows from here: its string id, the text, the data of the field showing it
struct Field {
    uint32_t id;
    uint32_t text;
    uint32_t data;
};
enum { kTitle, kEntry, kName = kEntry + kEntries, kValue = kName + kSlots, kFields = kValue + kSlots };
constexpr uint32_t kFieldSize = 64;
Field g_fields[kFields];

struct Page {
    const Host* host = nullptr;
    int mode = -1;  // 0 the stock buttons and ours, 1 and 2 the rows of each caption
    bool failed = false;
    uint32_t first = 0;  // our first clip
    uint32_t lead = 0;   // the stock clip ours keep their places from
    uint32_t entry[kEntries] = {};
    uint32_t prompt[kPrompts] = {};
    uint32_t name[kSlots] = {};
    uint32_t value[kSlots] = {};
    size_t slots = 0;
    size_t top = 0;
    float value_x = 0;  // the values' column, past the widest name
    std::vector<const Row*> rows;  // null restores the defaults
    int capture = 0;  // a key binding: 1 the keys come up, 2 the next key binds or Escape leaves, 3 they come up again
    size_t bind = 0;  // its row
};
Page g_page;     // game thread
int g_next = 0;  // the mode of the page being loaded
int g_last = 0;  // the mode before it, whose button takes the focus back
const Row* g_group = nullptr;  // the settings' category shown, null for the list of them

// the id of a string by its name
uint32_t Hash(const char* text) {
    uint32_t h = ~0u;
    for (; *text; text++) h = uint32_t(int32_t(h) >> 8) ^ R32(kHashTable + ((h ^ uint8_t(*text)) & 0xFF) * 4);
    return h;
}

// that field shows field n from now on
bool Show(X86Context& ctx, int n, uint32_t clip, const char* shows = nullptr, const char* text = nullptr) {
    Field& f = g_fields[n];
    char name[16];
    std::snprintf(name, sizeof(name), "$RC_%d", n);
    if (!f.text) {
        f.id = Hash(name + 1);
        f.text = mem::AllocSystem(kFieldSize);
        G<char>(f.text) = 0;
    }
    uint32_t field = FindField(clip, shows);
    if (!field) return false;
    f.data = R32(field + flash::kClipData);
    if (text) std::snprintf(static_cast<char*>(HostPtr(f.text)), kFieldSize, "%s", text);
    return Set(ctx, field, "variable", String(ctx, name));
}

void Write(Field& f, const std::string& text) {
    char* shown = static_cast<char*>(HostPtr(f.text));
    if (!f.data || std::strncmp(text.c_str(), shown, kFieldSize - 1) == 0) return;
    std::snprintf(shown, kFieldSize, "%s", text.c_str());
    // made again at the next draw
    W32(f.data + flash::kFieldFlags, (R32(f.data + flash::kFieldFlags) & ~1u) | 0x204);
}

// a copy of a stock clip beside it, stopped
uint32_t Copy(X86Context& ctx, uint32_t holder, const char* from, const char* name, int depth) {
    char number[12];
    std::snprintf(number, sizeof(number), "%d", depth);
    ctx.eax = Text(from);
    CallGuest(ctx, flash::kCall, {Text("duplicateMovieClip"), 0, 2, Text(name), Text(number)});
    uint32_t clip = Member(ctx, holder, name);
    if (flash::Type(clip) != flash::kSprite) return 0;
    uint32_t data = R32(clip + flash::kClipData);
    W32(data + flash::kDataFlags, R32(data + flash::kDataFlags) & ~flash::kDataPlaying);
    return clip;
}

void Hide(uint32_t clip) {
    if (flash::IsClip(clip)) W32(clip + flash::kClipFlags, R32(clip + flash::kClipFlags) & ~flash::kClipShown);
}

// ours keep their places from a stock clip, which the page slides and fades in: x, y, then its colours
void Follow(const Page& p) {
    const Host& h = *p.host;
    auto follow = [&p](uint32_t clip, float x, float y) {
        if (!clip) return;
        G<float>(clip + flash::kClipX) = G<float>(p.lead + flash::kClipX) + x;
        G<float>(clip + flash::kClipY) = G<float>(p.lead + flash::kClipY) + y;
        std::memcpy(HostPtr(clip + flash::kClipColours), HostPtr(p.lead + flash::kClipColours), flash::kColoursSize);
    };
    for (int i = 0; i < kEntries; i++) follow(p.entry[i], 0.0f, h.drop + h.pitch * float(i));
    for (int i = 0; i < kPrompts; i++) follow(p.prompt[i], h.prompt_x[i], h.prompt_y);
    for (int i = 0; i < kSlots; i++) {
        follow(p.name[i], h.name_x, h.pitch * float(i));
        follow(p.value[i], p.value_x, h.pitch * float(i));
    }
}

void Sound(X86Context& ctx) {
    ctx.eax = Text("_level4.MyMenu");
    CallGuest(ctx, flash::kCall, {Text("PlayChangeSound"), 0, 0});
}

// the prompts our buttons push down: each hidden, a copy in its place below
bool MovePrompts(X86Context& ctx, uint32_t holder) {
    Page& p = g_page;
    for (int i = 0; i < kPrompts; i++) {
        const char* from = p.host->prompts[i];
        if (!from) continue;
        char name[16];
        std::snprintf(name, sizeof(name), "RC_P%d", i);
        if (!(p.prompt[i] = Copy(ctx, holder, from, name, kDepth + kEntries + i))) return false;
        Hide(Member(ctx, holder, std::strrchr(from, '.') + 1));
    }
    return true;
}

// our buttons under the stock ones, in their ring
bool Entries(X86Context& ctx, uint32_t root, uint32_t holder) {
    Page& p = g_page;
    const Host& h = *p.host;
    uint32_t top = Member(ctx, root, h.first);
    uint32_t above = Member(ctx, root, h.last);
    p.lead = Member(ctx, holder, h.under);
    if (!top || !above || !flash::IsClip(p.lead)) return false;
    for (int i = 0; i < kEntries; i++) {
        char name[16];
        std::snprintf(name, sizeof(name), "RC_E%d", i);
        if (!(p.entry[i] = Copy(ctx, holder, h.button, name, kDepth + i))) return false;
        if (!Show(ctx, kEntry + i, p.entry[i], nullptr, app::Ui(kCaptions[i]).c_str())) return false;
    }
    if (!MovePrompts(ctx, holder)) return false;
    p.first = p.entry[0];
    Follow(p);
    CallGuest(ctx, flash::kRunActions, {R32(flash::kPlayer)});
    for (int i = 0; i < kEntries; i++) {
        if (!Button(ctx, p.entry[i], top)) return false;
        if (!Set(ctx, p.entry[i], "actionSelectPage", String(ctx, h.page))) return false;
        Link(ctx, above, p.entry[i]);
        above = p.entry[i];
    }
    Link(ctx, above, top);
    if (!h.hint) return true;
    for (uint32_t clip = FindField(holder, h.hint); clip; clip = R32(clip + flash::kClipParent))
        if (R32(clip + flash::kClipParent) == holder) Hide(clip);
    return true;
}

// every page shows a list of its categories first
bool Grouped(int mode) { return mode != 0; }

// a grouped page's categories, each a row
bool Listing(const Page& p) { return Grouped(p.mode) && !g_group; }

// the rows of the page's mode
void Fill(Page& p) {
    p.rows.clear();
    size_t count = 0;
    const Row* list = app::rows::Page(p.mode - 1, &count);
    bool listing = Listing(p);
    bool inside = !g_group;
    for (size_t i = 0; i < count; i++) {
        bool heading = list[i].kind == app::rows::Kind::Heading;
        if (g_group && heading) inside = &list[i] == g_group;
        if (heading == listing && inside) p.rows.push_back(&list[i]);
    }
    bool defaults = !Grouped(p.mode) || listing;
    if (p.mode > kSettingsMode && defaults) p.rows.push_back(nullptr);
    p.slots = std::min(p.slots, p.rows.size());
    p.top = 0;
}

float ValueColumn(X86Context& ctx, const Page& p);

// the panel's rows in place of the stock buttons
bool Rows(X86Context& ctx, uint32_t root, uint32_t holder, uint32_t menu) {
    Page& p = g_page;
    const Host& h = *p.host;
    uint32_t like = Member(ctx, root, h.first);
    uint32_t nothing = Method(ctx, like, "Select");
    p.lead = Member(ctx, holder, std::strrchr(h.button, '.') + 1);
    if (!nothing || !flash::IsClip(p.lead)) return false;
    p.slots = kSlots;
    Fill(p);
    for (size_t i = 0; i < p.slots; i++) {
        char name[16];
        std::snprintf(name, sizeof(name), "RC_N%d", int(i));
        p.name[i] = Copy(ctx, holder, h.button, name, kDepth + int(i));
        name[3] = 'V';
        p.value[i] = Copy(ctx, holder, h.button, name, kDepth + kSlots + int(i));
        if (!p.name[i] || !p.value[i]) return false;
        for (uint32_t clip : {p.name[i], p.value[i]}) {
            G<float>(clip + flash::kClipScaleX) = h.scale;
            G<float>(clip + flash::kClipScaleY) = h.scale;
        }
        if (!Show(ctx, kName + int(i), p.name[i]) || !Show(ctx, kValue + int(i), p.value[i])) return false;
    }
    p.first = p.name[0];
    p.value_x = ValueColumn(ctx, p);
    Follow(p);
    CallGuest(ctx, flash::kRunActions, {R32(flash::kPlayer)});
    for (size_t i = 0; i < p.slots; i++) {
        // a confirm on a button with no action is a back
        if (!Button(ctx, p.name[i], like)) return false;
        // a category loads the page again, with its rows
        bool set = Listing(p) ? Set(ctx, p.name[i], "actionSelectPage", String(ctx, h.page))
                              : Set(ctx, p.name[i], "actionSelectFunction", nothing);
        if (!set) return false;
        if (i) Link(ctx, p.name[i - 1], p.name[i]);
    }
    for (const char* stock : h.stock)
        if (stock) Hide(Member(ctx, holder, stock));
    Show(ctx, kTitle, holder, "$FE_OPTIONS");
    // back is the options page, not its own back
    Set(ctx, menu, "mfuncFunctionBack", 0);
    Set(ctx, menu, "mstrPageBack", String(ctx, h.page));
    Focus(ctx, p.name[0]);
    return true;
}

std::string NameText(const Row* row) { return app::Ui(row ? row->label : "RESTORE DEFAULTS"); }

// what row i of the page shows as its value
std::string ValueText(const Page& p, size_t i) {
    const Row* row = p.rows[i];
    if ((p.capture == 1 || p.capture == 2) && p.bind == i) return app::Ui("PRESS A KEY");
    if (!row || row->kind == app::rows::Kind::Heading) return " ";
    std::string value = app::rows::Value(*row);
    // key names as they are
    if (row->kind == app::rows::Kind::Bind && value != "UNBOUND") return value;
    return app::Ui(value);
}

uint32_t Glyph(uint32_t font, uint16_t code) {
    uint32_t glyph = R32(font + kFontGlyphs + (code & 0x7F) * 4);
    uint32_t last = R32(font + kFontLastGlyph);
    if (!glyph) return last;
    for (int left = 256; G<uint16_t>(glyph + kGlyphCode) != code && glyph != last && left; left--) glyph += kGlyphSize;
    return glyph;
}

// UTF-8 text's characters, as the font codes its glyphs
std::vector<uint16_t> Codes(const std::string& text) {
    std::vector<uint16_t> codes;
    for (size_t i = 0; i < text.size(); i++) {
        uint8_t c = uint8_t(text[i]);
        if (c >= 0xE0 && i + 2 < text.size()) {
            codes.push_back(uint16_t((c & 0x0F) << 12 | (text[i + 1] & 0x3F) << 6 | (text[i + 2] & 0x3F)));
            i += 2;
        } else if (c >= 0xC0 && i + 1 < text.size()) {
            codes.push_back(uint16_t((c & 0x1F) << 6 | (text[i + 1] & 0x3F)));
            i++;
        } else {
            codes.push_back(c);
        }
    }
    return codes;
}

// a text's width at a height of 1, as 0xC3AF0
float Measure(uint32_t font, const std::string& text) {
    std::vector<uint16_t> codes = Codes(text);
    if (!font || codes.empty()) return 0.0f;
    uint32_t glyph = Glyph(font, codes[0]);
    float x = -G<float>(glyph + kGlyphLeft);
    for (size_t i = 1; i < codes.size(); i++) {
        x += G<float>(glyph + kGlyphAdvance);
        glyph = Glyph(font, codes[i]);
    }
    float spacing = R32(glyph + kGlyphSpaced) ? G<float>(font + kFontSpacing) : 1.0f;
    float end = x + G<float>(glyph + kGlyphLeft) + G<float>(glyph + kGlyphWidth) * spacing;
    return G<float>(font + kFontScale) * end;
}

// the stock column, further right when a name would run into it
float ValueColumn(X86Context& ctx, const Page& p) {
    const Host& h = *p.host;
    uint32_t font = CallGuest(ctx, kFindFont, {R32(kGame), kFontName});
    float widest = 0.0f;
    for (const Row* row : p.rows) widest = std::max(widest, Measure(font, NameText(row)));
    float name_end = h.name_x + (kTextLeft + widest * kTextHeight) * h.scale;
    return std::max(h.value_x, name_end + kNameGap);
}

// how far right of the focused name the widest value of the page ends, the label clip scaled as it lights
float Reach(X86Context& ctx, const Page& p, uint32_t focus) {
    const Host& h = *p.host;
    uint32_t font = CallGuest(ctx, kFindFont, {R32(kGame), kFontName});
    uint32_t label = flash::FirstChild(focus);
    float scale = h.scale * (label ? G<float>(label + flash::kClipScaleX) : 1.0f);
    // capture text left out so the bar keeps its width while a key is bound
    Page shown = p;
    shown.capture = 0;
    float widest = 0.0f;
    for (size_t i = 0; i < p.rows.size(); i++) widest = std::max(widest, Measure(font, ValueText(shown, i)));
    return p.value_x - h.name_x + (kTextLeft + widest * kTextHeight) * scale + kTickGap;
}

// the bar on the focused row reaches past the values, its ends and line as stock
void StretchBar(X86Context& ctx, const Page& p, uint32_t root, uint32_t focus) {
    const Bar& stock = p.host->bar;
    uint32_t bar = Member(ctx, Member(ctx, root, "Main"), "ButtonSelector");
    if (flash::Type(bar) != flash::kSprite) return;
    uint32_t data = R32(bar + flash::kClipData);
    if (!data || data == flash::kNoData || R32(data + flash::kDataFrame) != kBarRest) return;
    float right = std::max(stock.tick, Reach(ctx, p, focus));
    float stretch = (right - kBarLeft) / (stock.tick - kBarLeft);
    int left = 256;
    for (uint32_t part = flash::FirstChild(bar); part && left; part = R32(part + flash::kClipNext), left--) {
        int depth = flash::Depth(part);
        if (depth == kBarBox) G<float>(part + flash::kClipScaleX) = stock.box * stretch;
        if (depth == kBarLine) G<float>(part + flash::kClipX) = stock.line + right - stock.tick;
        if (depth == kBarTick) G<float>(part + flash::kClipX) = right;
        if (depth == kBarScan) {
            G<float>(part + flash::kClipScaleX) = stretch;
            G<float>(part + flash::kClipX) = kBarLeft + (stock.scan - kBarLeft) * stretch;
        }
    }
}

// Delete on a binding row
void Clear(X86Context& ctx, const Row* row) {
    if (!row || row->kind != app::rows::Kind::Bind) return;
    std::string was = app::rows::Value(*row);
    input::Unbind(size_t(row->id));
    if (app::rows::Value(*row) != was) Sound(ctx);
}

const Host* FindHost(const char* name) {
    for (const Host& h : kHosts)
        if (std::strcmp(name, h.page) == 0) return &h;
    return nullptr;
}

// the page's clips made again: the stock buttons and ours, or the rows of the mode asked for
void Build(X86Context& ctx, const Host& host, uint32_t root, uint32_t holder, uint32_t menu) {
    Page& p = g_page;
    p = {};
    for (Field& f : g_fields) f.data = 0;
    p.host = &host;
    p.mode = g_next;
    g_next = 0;
    // back from a category is its list
    const Row* back = nullptr;
    if (!p.mode && Grouped(g_last) && g_group) {
        back = g_group;
        g_group = nullptr;
        p.mode = g_last;
    }
    p.failed = !(p.mode ? Rows(ctx, root, holder, menu) : Entries(ctx, root, holder));
    const char* shown = p.mode ? kCaptions[p.mode - 1] : "buttons";
    LOGI("options menu: %s, %s%s", host.page, shown, p.failed ? ", failed" : "");
    if (!p.failed && !p.mode && g_last) Focus(ctx, p.entry[g_last - 1]);
    size_t at = size_t(std::find(p.rows.begin(), p.rows.end(), back) - p.rows.begin());
    if (!p.failed && back && at < p.slots) Focus(ctx, p.name[at]);
    g_last = p.mode;
}

// the key binding's next step, see Page::capture
void UpdateCapture(uint32_t ui, uint32_t id) {
    Page& p = g_page;
    int vk = input::PressedKey();
    if (p.capture == 2 && vk) {
        if (vk != kEscape) input::Bind(size_t(p.rows[p.bind]->id), vk);
        p.capture = 3;
    } else if (p.capture != 2 && !vk) {
        p.capture = p.capture == 1 ? 2 : 0;
    } else if (id == kInputBack) {
        p.capture = 0;  // a controller's back
    }
    W32(ui + kMenuInput, 0);
}

// past either end the rows scroll, then go round; false off the ends
bool UpdateScroll(X86Context& ctx, size_t slot, uint32_t id) {
    Page& p = g_page;
    size_t hidden = p.rows.size() - p.slots;
    if (id == kInputUp && slot == 0) {
        if (p.top) {
            p.top--;
        } else {
            p.top = hidden;
            Focus(ctx, p.name[p.slots - 1]);
        }
        return true;
    }
    if (id == kInputDown && slot == p.slots - 1) {
        if (p.top < hidden) {
            p.top++;
        } else {
            p.top = 0;
            Focus(ctx, p.name[0]);
        }
        return true;
    }
    return false;
}

// left, right or confirm on a row: a value steps, a binding listens, the last restores
void ApplyRowInput(size_t slot, uint32_t id, bool locked) {
    Page& p = g_page;
    const Row* row = p.rows[p.top + slot];
    bool value = row && row->kind == app::rows::Kind::Value;
    bool changes = id >= kInputLeft && id <= kInputConfirm;
    if (value && changes) {
        if (!locked) app::rows::Value(*row, id == kInputLeft ? -1 : 1);
    } else if (id == kInputConfirm && row && row->kind == app::rows::Kind::Heading) {
        g_group = row;
        g_next = p.mode;
    } else if (id == kInputConfirm && row) {
        p.capture = 1;
        p.bind = p.top + slot;
    } else if (id == kInputConfirm) {
        app::rows::Defaults(p.mode - 1);
    }
}

void UpdateRow(X86Context& ctx, uint32_t ui, size_t slot, uint32_t id, bool clear) {
    const Row* row = g_page.rows[g_page.top + slot];
    if (clear) Clear(ctx, row);
    bool value = row && row->kind == app::rows::Kind::Value;
    bool locked = row && !app::rows::Enabled(*row);  // greyed in the panel
    bool edge = UpdateScroll(ctx, slot, id);
    if (edge) {
        W32(ui + kMenuInput, 0);
    } else {
        ApplyRowInput(slot, id, locked);
    }
    bool stepped = value && !locked && (id == kInputLeft || id == kInputRight);
    if (edge || stepped) Sound(ctx);
}

// the title, the rows' names and values
void UpdateTexts(const Page& p) {
    // a value lights up with its name: a clip's matrix, then its colours
    constexpr uint32_t kMatrixAndColours = 0x38;
    Write(g_fields[kTitle], app::Ui(g_group ? g_group->label : kCaptions[p.mode - 1]));
    for (size_t i = 0; i < p.slots; i++) {
        const Row* row = p.rows[p.top + i];
        Write(g_fields[kName + i], NameText(row));
        Write(g_fields[kValue + i], ValueText(p, p.top + i));
        uint32_t from = flash::FirstChild(p.name[i]);
        uint32_t to = flash::FirstChild(p.value[i]);
        if (!from || !to) continue;
        std::memcpy(HostPtr(to + flash::kClipMatrix), HostPtr(from + flash::kClipMatrix), kMatrixAndColours);
    }
}

void Tick(X86Context& ctx, uint32_t ui) {
    // read every tick, so a Delete that answers a capture is not a fresh press once it ends
    bool clear = input::TakeDeletePress();
    Page& p = g_page;
    const char* name = static_cast<const char*>(HostPtr(ui + kPageName));
    const Host* host = FindHost(name);
    bool running = host && R32(flash::kPlayerReady) == 1 && R32(flash::kPlayer) && R32(ui + kPageReady) == kPageRunning;
    if (!running) {
        if (*name && !host) {
            g_next = g_last = 0;
            g_group = nullptr;
        }
        p = {};
        return;
    }
    uint32_t going = R32(ui + kPageLeaving);
    bool leaving = going || R32(ui + kPageAsked) == 1;
    uint32_t root = flash::Level(flash::kMenuLevel);
    uint32_t holder = Member(ctx, root, "Main");
    uint32_t menu = Member(ctx, root, "MyMenu");
    if (host->holder) holder = Member(ctx, holder, host->holder);
    if (!holder || !menu) return;
    if (p.failed) {
        if (going == 1) p = {};
        return;
    }
    if (p.mode < 0 || Member(ctx, holder, p.mode ? "RC_N0" : "RC_E0") != p.first) {
        if (!leaving) Build(ctx, *host, root, holder, menu);
        return;
    }
    Follow(p);
    if (going == 1) p = {};
    if (leaving) return;
    uint32_t id = InputTaken(ui) ? R32(ui + kMenuInput) : 0;
    uint32_t focus = Member(ctx, menu, "mFocusButton");
    if (!p.mode) {
        const uint32_t* at = std::find(std::begin(p.entry), std::end(p.entry), focus);
        if (id == kInputConfirm && at != std::end(p.entry)) {
            g_next = int(at - p.entry) + 1;
            g_group = nullptr;
        }
        return;
    }
    size_t slot = size_t(std::find(p.name, p.name + p.slots, focus) - p.name);
    if (p.capture) {
        UpdateCapture(ui, id);
    } else if (slot < p.slots) {
        UpdateRow(ctx, ui, slot, id, clear);
    }
    UpdateTexts(p);
    if (slot < p.slots) StretchBar(ctx, p, root, focus);
}

}  // namespace

// ebp the front end, after the cursor's input and before the Flash player runs the page's script
void OptionsMenu(X86Context& ctx, uint8_t*) {
    X86Context saved = ctx;
    Tick(ctx, ctx.ebp);
    ctx = saved;
}

namespace game {

// mouse.cpp: a key binding takes the next click
bool OptionsCapturing() { return g_page.capture != 0; }

// prompts.cpp's string lookup: a text of the page by its string id, 0 for none
uint32_t OptionsText(uint32_t id) {
    for (const Field& f : g_fields)
        if (f.text && f.id == id) return f.text;
    return 0;
}

}  // namespace game
