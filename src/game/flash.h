#pragma once
// The game's Flash player: the script's values and the clips on screen
#include <string.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>

#include "common/casing.h"
#include "xdk/hle.h"

namespace flash {

constexpr uint32_t kPlayer = 0x2EC988;
constexpr uint32_t kPlayerReady = 0x2ED408;  // 1 once the player runs
constexpr uint32_t kPlayerMovies = 0x18;

// value: +0 vtable
constexpr uint32_t kMembersMethod = 0x0C;  // in the vtable
constexpr uint32_t kValueFlags = 4;
constexpr uint32_t kValueDefined = 0x10;
constexpr uint32_t kTypeShift = 25;
constexpr uint32_t kValueNumber = 8;  // an integer's or a float's

// value types
constexpr uint32_t kFloat = 6;
constexpr uint32_t kInteger = 7;
constexpr uint32_t kFirstClip = 0xC;
constexpr uint32_t kSprite = 0xD;
constexpr uint32_t kTextField = 0xF;
constexpr uint32_t kMovie = 0x12;

// clip: matrix a b c d x y, depth in 17 bits
constexpr uint32_t kClipName = 0x08;
constexpr uint32_t kClipMatrix = 0x0C;
constexpr uint32_t kClipScaleX = kClipMatrix;  // the matrix's a
constexpr uint32_t kClipScaleY = 0x18;         // its d
constexpr uint32_t kClipX = 0x1C;
constexpr uint32_t kClipY = 0x20;
constexpr uint32_t kClipColours = 0x24;
constexpr uint32_t kColoursSize = 0x20;
constexpr uint32_t kClipParent = 0x44;
constexpr uint32_t kClipData = 0x48;
constexpr uint32_t kClipNext = 0x50;
constexpr uint32_t kClipDepth = 0x54;
constexpr uint32_t kClipFlags = 0x58;
constexpr uint32_t kClipShown = 0x100000;
constexpr uint32_t kClipPlaced = 0x10000;  // placed by the script

// a sprite's data
constexpr uint32_t kDataCharacter = 0x0C;  // read on every frame the clip plays (0x10E3B0)
constexpr uint32_t kDataFrame = 0x18;
constexpr uint32_t kDataFlags = 0x1C;
constexpr uint32_t kDataPlaying = 0x2000000;
constexpr uint32_t kDataChildren = 0x24;
constexpr uint32_t kNoData = 0xBAADF00D;

// a text field's data
constexpr uint32_t kFieldString = 0x1C;
constexpr uint32_t kFieldFlags = 0x6C;

// a value's members: {name, value} each
constexpr uint32_t kMemberSlots = 0;
constexpr uint32_t kMemberTable = 4;
constexpr uint32_t kMemberClass = 8;
constexpr uint32_t kMemberSize = 8;
constexpr uint32_t kNameText = 8;          // in a name
constexpr uint32_t kDeletedName = 0x2AC7EC;  // the name of a deleted member

inline uint32_t Type(uint32_t value) {
    if (!value || !(kernel::R32(value + kValueFlags) & kValueDefined)) return 0;
    return kernel::R32(value + kValueFlags) >> kTypeShift;
}

inline bool IsClip(uint32_t value) { return Type(value) >= kFirstClip && Type(value) <= kMovie; }

inline const char* NameText(uint32_t name) { return static_cast<const char*>(kernel::HostPtr(name + kNameText)); }

inline const char* Name(uint32_t clip) { return NameText(kernel::R32(clip + kClipName)); }

inline int Depth(uint32_t clip) { return int32_t(kernel::R32(clip + kClipDepth) << 15) >> 15; }

inline uint32_t Members(X86Context& ctx, uint32_t value) {
    if (!Type(value)) return 0;
    ctx.ecx = value;
    return kernel::CallGuest(ctx, kernel::R32(kernel::R32(value) + kMembersMethod), {});
}

inline uint32_t Find(uint32_t members, const char* name) {
    uint32_t slots = members ? kernel::R32(members + kMemberSlots) : 0;
    uint32_t table = members ? kernel::R32(members + kMemberTable) : 0;
    for (uint32_t i = 0; table && slots <= 0x1000 && i < slots; i++) {
        uint32_t key = kernel::R32(table + i * kMemberSize);
        uint32_t value = kernel::R32(table + i * kMemberSize + 4);
        if (key && key != kDeletedName && nocase::Equals(NameText(key), name)) return Type(value) ? value : 0;
    }
    return 0;
}

inline uint32_t Member(X86Context& ctx, uint32_t value, const char* name) { return Find(Members(ctx, value), name); }

constexpr int kMenuLevel = 4;  // the level menu pages load at

// the movie at a level
inline uint32_t Level(int level) {
    uint32_t player = kernel::R32(kPlayer);
    uint32_t movies = player ? kernel::R32(player + kPlayerMovies) : 0;
    for (uint32_t clip = movies ? kernel::R32(movies) : 0; clip; clip = kernel::R32(clip + kClipNext))
        if (Depth(clip) == level) return Type(clip) == kMovie ? clip : 0;
    return 0;
}

// a sprite's first child, the rest follow at kClipNext
inline uint32_t FirstChild(uint32_t clip) {
    if (Type(clip) != kSprite && Type(clip) != kMovie) return 0;
    uint32_t data = kernel::R32(clip + kClipData);
    uint32_t list = data && data != kNoData ? kernel::R32(data + kDataChildren) : 0;
    uint32_t head = list ? kernel::R32(list) : 0;
    return head ? kernel::R32(head + kClipNext) : 0;
}

// the script: calls into it and its members set from here
constexpr uint32_t kInterpreter = 0x562C08;
constexpr uint32_t kSetMember = 0x100DB0;   // eax value or 0 to delete, edx name, ecx 0; (interpreter, object, 1, 0, 1)
constexpr uint32_t kNewString = 0xF5600;    // cdecl (text): a string value
constexpr uint32_t kCall = 0x101420;        // eax object path; cdecl (function, result, count, texts...)
constexpr uint32_t kCallValues = 0x101260;  // the same, its arguments values
constexpr uint32_t kRunActions = 0x10F180;  // (player): the frame actions waiting, a new clip's stop() among them

// a name the player may keep: +0 its handle, +4 uses, length, room and hash, +12 the text
inline uint32_t KeptName(const char* text) {
    static std::map<std::string, uint32_t> names;
    uint32_t& at = names[text];
    if (!at) {
        uint32_t length = uint32_t(std::strlen(text));
        at = mem::AllocSystem(length + 13);
        kernel::W32(at, at + 4);
        kernel::G<uint16_t>(at + 4) = 0x4000;
        kernel::G<uint16_t>(at + 6) = uint16_t(length);
        kernel::G<uint16_t>(at + 8) = uint16_t(length);
        kernel::G<uint16_t>(at + 10) = 0;
        std::memcpy(kernel::HostPtr(at + 12), text, length + 1);
    }
    return at;
}

inline uint32_t Text(const char* text) { return KeptName(text) + 12; }

inline uint32_t String(X86Context& ctx, const char* text) { return kernel::CallGuest(ctx, kNewString, {Text(text)}); }

inline bool Set(X86Context& ctx, uint32_t object, const char* name, uint32_t value) {
    ctx.eax = value;
    ctx.edx = KeptName(name);
    ctx.ecx = 0;
    return kernel::CallGuest(ctx, kSetMember, {kInterpreter, object, 1, 0, 1}) & 0xFF;
}

// a text field in a clip, the one showing that string or the first
inline uint32_t FindField(uint32_t clip, const char* shows, int depth = 0) {
    int left = 256;
    for (uint32_t child = FirstChild(clip); child && left; child = kernel::R32(child + kClipNext), left--) {
        uint32_t data = Type(child) == kTextField ? kernel::R32(child + kClipData) : 0;
        const char* name = data ? NameText(kernel::R32(data + kFieldString)) : nullptr;
        if (data && (!shows || std::strcmp(name, shows) == 0)) return child;
        if (uint32_t found = depth < 4 ? FindField(child, shows, depth + 1) : 0) return found;
    }
    return 0;
}

// a method of a script object's class or of one that extends
inline uint32_t Method(X86Context& ctx, uint32_t object, const char* name) {
    for (int up = 0; up < 4; up++) {
        uint32_t members = Members(ctx, object);
        if (!members) break;
        object = kernel::R32(members + kMemberClass);
        if (uint32_t found = Find(Members(ctx, object), name)) return found;
    }
    return 0;
}

// a clip the page's script takes for one of its button objects, such as like
inline bool Button(X86Context& ctx, uint32_t clip, uint32_t like) {
    uint32_t frame = Member(ctx, like, "HighlightFrame");
    uint32_t on = Method(ctx, like, "HighLight");
    uint32_t off = Method(ctx, like, "UnHighLight");
    return frame && on && off && Set(ctx, clip, "movieClip", clip) && Set(ctx, clip, "HighlightFrame", frame) &&
           Set(ctx, clip, "HighLight", on) && Set(ctx, clip, "UnHighLight", off);
}

inline void Link(X86Context& ctx, uint32_t above, uint32_t below) {
    Set(ctx, above, "buttonDown", below);
    Set(ctx, below, "buttonUp", above);
}

inline void Focus(X86Context& ctx, uint32_t button) {
    ctx.eax = Text("_level4.MyMenu");
    kernel::CallGuest(ctx, kCallValues, {Text("SetFocusButton"), 0, 1, button});
}

}  // namespace flash
