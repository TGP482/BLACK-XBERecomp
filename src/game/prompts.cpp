// Button legends on keyboard and mouse: a menu prompt's icon goes and its text leads with the key, "(ENTER) confirm".
// Texts naming the pad's buttons name the keys, a hint in play the key bound to its button. A change of device sets
// the texts on screen again. Texts naming the console name the PC, whatever the device.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "common/mouse_keyboard.h"
#include "common/translations.h"
#include "game/flash.h"
#include "xdk/hle.h"

using namespace kernel;

namespace game {
uint32_t OptionsText(uint32_t id);  // options_menu.cpp
uint32_t SaveName(uint32_t id);  // profile_messages.cpp
bool Translated();  // language.cpp
}

namespace {

constexpr uint32_t kResetTexts = 0xECA90;  // every text on screen is made again from its string
// string ids
constexpr uint32_t kConfirm = 0xE0CA7C5F;
constexpr uint32_t kBack = 0x64199B75;
constexpr uint32_t kBackspace = 0x32D594BD;
constexpr uint32_t kPressStart[] = {0x2FCCE936, 0x905E4EE0, 0x661A3A37};
// a hint's "PRESS (A) :" strings, in the order the keyboard numbers the pad: A B X Y black white, the triggers,
// dpad up down left right, start, back, the thumbs
constexpr uint32_t kButtons[] = {0x53773143, 0xCA7E60F9, 0x371C9983, 0x401BA915, 0x1C8B6988, 0xD7C11E04,
                                 0xDDC13733, 0xBCBBFDF2, 0xBB8906E1, 0xCA47AFA2, 0x53DFD8F1, 0xD4A32971,
                                 0x0A88862F, 0xDC57E320, 0x8D194B52, 0xB900C989};
constexpr uint32_t kHintSize = 32;  // the game expands a hint's button into this many characters
constexpr float kIconWidth = 28.0f;  // a prompt's text starts this far right of its icon's left edge
// "hold (X) to pick up" takes its button from four glyph strings, of Y A B X
constexpr uint32_t kPickupGlyphs = 0x2AB72C;
constexpr int kPickupButtons[] = {3, 0, 1, 2};
constexpr uint32_t kKeySize = 16;

void Replace(std::string& text, std::string_view from, std::string_view to) {
    for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size())
        text.replace(at, from.size(), to);
}

// the console in each language's wording, a phrase before its words: each is only in its own language
constexpr std::string_view kConsoleWords[][2] = {
    {"XBOX CONSOLE", "PC"},
    {"XBOX HARD DISK", "HARD DISK"},
    {"la console Xbox", "le PC"},
    {"disque dur Xbox", "disque dur"},
    {"La Xbox", "Le PC"},
    {"DIE XBOX-KONSOLE", "DEN PC"},
    {"DIE XBOX-FESTPLATTE", "DIE FESTPLATTE"},
    {"DIE XBOX HAT", "DER PC HAT"},
    {"disco rigido di Xbox", "disco rigido"},
    {"spegnere Xbox", "spegnere il PC"},
    {"Xbox non", "Il PC non"},
    {"LA CONSOLA XBOX", "EL PC"},
    {"DISCO DE MEMORIA DE XBOX", "DISCO DE MEMORIA"},
    {"XBOX NO TIENE", "EL PC NO TIENE"},
    {"XBOX", "PC"},
    {"Xbox", "PC"},
};

// a string as this PC words it, on the keyboard with its keys; empty when it stays
std::string Wording(uint32_t id, const std::string& pad, bool keyboard) {
    std::string text = pad;
    for (const auto& [from, to] : kConsoleWords) Replace(text, from, to);
    if (!keyboard) return text == pad ? std::string() : text;
    if (id == kConfirm) text = "(ENTER) " + text;
    else if (id == kBack || id == kBackspace) text = "(ESC) " + text;
    else if (std::find(std::begin(kPressStart), std::end(kPressStart), id) != std::end(kPressStart)) {
        Replace(text, "start", "enter");
        Replace(text, "START", "ENTER");
    } else {
        // the font's A and B button glyphs
        Replace(text, "\xC2\xB2", "ENTER");
        Replace(text, "\xC2\xB9", "ESC");
    }
    return text == pad ? std::string() : text;
}

std::mutex g_lock;
// string id, and in bit 0 the keyboard, to its wording in guest memory, 0 for none
std::unordered_map<uint64_t, uint32_t> g_wordings;

// a prompt is a clip of a "Button" icon and a "Text": on the keyboard the icon goes and the text takes its place
void Prompts(uint32_t clip, bool keyboard, int depth) {
    int left = 4096;
    for (uint32_t child = flash::FirstChild(clip); child && left; child = R32(child + flash::kClipNext), left--) {
        uint32_t icon = 0;
        uint32_t text = 0;
        for (uint32_t part = flash::FirstChild(child); part && left; part = R32(part + flash::kClipNext), left--) {
            if (!flash::IsClip(part)) continue;
            if (_stricmp(flash::Name(part), "Button") == 0) icon = part;
            if (_stricmp(flash::Name(part), "Text") == 0) text = part;
        }
        if (!icon || !text) {
            if (depth < 8) Prompts(child, keyboard, depth + 1);
            continue;
        }
        bool shown = R32(icon + flash::kClipFlags) & flash::kClipShown;
        if (shown != keyboard) continue;
        W32(icon + flash::kClipFlags, R32(icon + flash::kClipFlags) ^ flash::kClipShown);
        G<float>(text + flash::kClipX) += keyboard ? -kIconWidth : kIconWidth;
        W32(text + flash::kClipFlags, R32(text + flash::kClipFlags) | flash::kClipPlaced);
    }
}

}  // namespace

// thiscall(id): the string of that id, UTF-8
X86_EXTERN_FUNC(__imp__sub_001F3A80);
X86_FUNC(sub_001F3A80) {
    uint32_t id = R32(ctx.esp + 4);
    __imp__sub_001F3A80(ctx, base);
    if (uint32_t text = game::OptionsText(id)) {
        ctx.eax = text;
        return;
    }
    if (uint32_t name = game::SaveName(id)) {
        ctx.eax = name;
        return;
    }
    if (!ctx.eax) return;
    bool keyboard = input::KeyboardActive();
    std::lock_guard lock(g_lock);
    auto [it, fresh] = g_wordings.try_emplace(uint64_t(id) << 1 | keyboard, 0);
    const uint32_t* button = std::find(std::begin(kButtons), std::end(kButtons), id);
    if (keyboard && button != std::end(kButtons)) {
        // follows the bindings: worded at every ask
        std::string key = input::ButtonKey(int(button - kButtons));
        if (key.empty() || (!it->second && !(it->second = mem::AllocSystem(kHintSize)))) return;
        std::string press = game::Translated() ? app::Ui("PRESS") : "PRESS";
        std::snprintf(static_cast<char*>(HostPtr(it->second)), kHintSize, "%s %s :", press.c_str(), key.c_str());
    } else if (fresh) {
        std::string text = Wording(id, ReadCString(ctx.eax, 1024), keyboard);
        if (!text.empty() && (it->second = mem::AllocSystem(uint32_t(text.size() + 1))))
            std::memcpy(HostPtr(it->second), text.c_str(), text.size() + 1);
    }
    if (it->second) ctx.eax = it->second;
}

// the front end's tick, before the Flash player runs
void MenuPrompts(X86Context& ctx, uint8_t*) {
    static int was = -1;
    bool keyboard = input::KeyboardActive();
    if (int(keyboard) != was) {
        if (was >= 0 && R32(flash::kPlayerReady) == 1 && R32(flash::kPlayer)) {
            X86Context saved = ctx;
            CallGuest(ctx, kResetTexts, {});
            ctx = saved;
        }
        was = keyboard;
    }
    Prompts(flash::Level(flash::kMenuLevel), keyboard, 0);
    static uint32_t glyphs[4];
    static uint32_t keys;
    if (!keys && (keys = mem::AllocSystem(4 * kKeySize)))
        for (uint32_t i = 0; i < 4; i++) glyphs[i] = R32(kPickupGlyphs + i * 4);
    for (uint32_t i = 0; keys && i < 4; i++) {
        std::string key = keyboard ? input::ButtonKey(kPickupButtons[i]) : std::string();
        std::snprintf(static_cast<char*>(HostPtr(keys + i * kKeySize)), kKeySize, "%s", key.c_str());
        W32(kPickupGlyphs + i * 4, key.empty() ? glyphs[i] : keys + i * kKeySize);
    }
}
