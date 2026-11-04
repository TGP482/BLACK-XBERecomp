// The weapons page of the F10 panel: any of the game's player weapons in any level.
//
// Every level's weapon table holds all seventeen player weapons, but a level ships only the first person banks
// (Levels\Level_NN\fpguns\<weapon>.db and .wdd: the gun, arms, animations and sounds) and the world models
// (Guns.bin, the "<weapon>_LOD" models a dropped gun shows) of the guns it uses. So a weapon is given the game's way:
// created from the table, put in the slot not in hand, and switched to as the d-pad does; a bank the level lacks is
// read from another level's folder, and a world model it lacks from another level's Guns.bin.
#include "game/weapons.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/layout.h"
#include "kernel/disc.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;
namespace fs = std::filesystem;

namespace {

// the weapon manager: +0 the weapon table, +4 the weapon objects (0x110 each), +8 their in use flags
constexpr uint32_t kWeaponManager = 0x2D12D0;
constexpr uint32_t kTableCount = 1;  // byte
constexpr uint32_t kTableEntries = 4;
constexpr uint32_t kEntrySize = 0x20;  // name key (8 bytes), weapon data, ...
// resource manager: the TEXTURE and MODELS dictionaries looked up by name, 11 of each
constexpr uint32_t kResources = 0x2D45D8;
constexpr uint32_t kTextureDicts = 0xC64;
constexpr uint32_t kModelDicts = 0xC90;
constexpr int kDictSlots = 11;
constexpr int kDictSlotsKept = 3;  // left free for the level's own later loads
// guest functions
constexpr uint32_t kCreateWeapon = 0x1CEE10;  // edi the manager; (owner, type, 0) -> the weapon
constexpr uint32_t kFreeWeapon = 0x1C9B90;    // edi the manager, edx the weapon
constexpr uint32_t kFindModel = 0x1F83E0;     // (resources, key low, key high) -> the model or 0
constexpr uint32_t kFixGuns = 0xB5F20;        // eax a Guns.bin in memory: relocated and its dictionaries registered
// world
constexpr uint32_t kWorldState = 0x5AE0;  // the level loader's step
constexpr uint32_t kStatePlaying = 0x1C;
constexpr uint32_t kWorldSilver = 0x5CB0;  // byte: the level loaded Guns_s.bin
// player
constexpr uint32_t kPlayerKind = 0xC4;
constexpr uint32_t kKindPlayer = 2;
constexpr uint32_t kPlayerInventory = 0x278;
// inventory: the ammunition of each type (words) first
constexpr uint32_t kInvOwner = 0x1C;
constexpr uint32_t kInvSlots = 0x20;     // the weapons, one a slot
constexpr uint32_t kInvHeld = 0x24;      // the weapon in hand
constexpr uint32_t kInvSwitchTo = 0x28;  // the weapon a switch brings out
constexpr uint32_t kInvRequest = 0x38;   // 2 asks to switch to the other slot, as the d-pad does
constexpr uint32_t kInvSlotCount = 0x42;  // byte
constexpr uint32_t kInvSlotHeld = 0x43;   // byte
constexpr int kAmmoTypes = 10;
constexpr uint32_t kAmmoMax = 999;
// weapon and its data
constexpr uint32_t kWeaponType = 0;  // byte: index in the table
constexpr uint32_t kWeaponData = 0xEC;
constexpr uint32_t kWeaponMagazine = 0xF4;  // the main fire's magazine, +0xF8 the alternate fire's
constexpr uint32_t kMagazineData = 8;
constexpr uint32_t kDataClip = 0x60;
constexpr uint32_t kDataAmmoType = 0x64;

// the game's files under D:, the disc image when it runs from one, else the game's folder
const std::string kDiscRoot = "D:\\";

// names as the game keeps them: twelve characters of " -/0-9A-Z_" in base 40
std::string Decode(uint64_t key) {
    char s[13] = {};
    for (int i = 11; i >= 0; i--) {
        int c = int(key % 40);
        key /= 40;
        s[i] = c == 0 ? ' ' : c == 1 ? '-' : c == 2 ? '/' : c < 13 ? char('0' + c - 3) : c < 39 ? char('A' + c - 13) : '_';
    }
    std::string out = s;
    out.erase(out.find_last_not_of(' ') + 1);
    return out;
}

uint64_t Encode(const std::string& name) {
    uint64_t key = 0;
    for (size_t i = 0; i < 12; i++) {
        char ch = i < name.size() ? char(std::toupper(uint8_t(name[i]))) : ' ';
        int c = ch == '-' ? 1 : ch == '/' ? 2 : ch >= '0' && ch <= '9' ? 3 + ch - '0' : ch >= 'A' && ch <= 'Z' ? 13 + ch - 'A' : ch == '_' ? 39 : 0;
        key = key * 40 + uint64_t(c);
    }
    return key;
}

// the names the game's own text gives them, told apart by clip and ammunition; the silver ones end in 5
const std::map<std::string, std::string> kNames = {
    {"PST", "Pistol"},         {"PS5", "Pistol"},         {"GK1", "Glock"},        {"GK5", "Glock"},
    {"MGN", "Magnum"},         {"MG5", "Magnum"},         {"SMG", "Uzi"},          {"SM5", "Uzi"},
    {"SM3", "MAC-10"},         {"MC5", "MAC-10"},         {"MP1", "MP5"},          {"MP5", "MP5"},
    {"P90", "P90"},            {"P95", "P90"},            {"AK1", "AK-47"},        {"AK5", "AK-47"},
    {"ASR", "G36C"},           {"AS5", "G36C"},           {"M16", "M16"},          {"M45", "M16"},
    {"BNS", "M16A2 + M203"},   {"SHG", "SPAS 12"},        {"SH5", "SPAS 12"},      {"RM1", "Remington 870"},
    {"RM5", "Remington 870"},  {"SNR", "Walther 2000"},   {"SN5", "Walther 2000"}, {"HVY", "M249"},
    {"HV5", "M249"},           {"RPG", "RPG"},            {"GRL", "Grenade launcher"},
};

std::string Label(const std::string& key_name) {
    std::string code = key_name.size() > 4 && key_name.compare(0, 4, "BG1_") == 0 ? key_name.substr(4) : key_name;
    auto it = kNames.find(code);
    return it == kNames.end() ? code : it->second + " (" + code + ")";
}

// what the game thread found and the panel shows
std::mutex g_lock;
weapons::View g_view;
std::atomic<int> g_give{-1};
std::atomic<bool> g_fill{false};

void SetStatus(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_view.status = text;
    LOGI("weapons: %s", text.c_str());
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

// a file of the game, by its path under D:
bool GameFileExists(const std::string& path) {
    if (const Disc* disc = MountedDisc()) {
        const Disc::Entry* e = disc->Find(path);
        return e && !e->dir;
    }
    std::string host = ResolvePath(kDiscRoot + path, 0);
    std::error_code ec;
    return !host.empty() && fs::is_regular_file(host, ec);
}

// a file of the game, read whole: from the disc image when it runs from one, else from the game's folder
bool LoadGameFile(const std::string& path, std::vector<uint8_t>& out) { return ReadGameFile(kDiscRoot + path, out); }

int LevelNumber(std::string name) {
    name = Lower(name);
    bool ok = name.size() == 8 && name.compare(0, 6, "level_") == 0 && std::isdigit(uint8_t(name[6])) &&
              std::isdigit(uint8_t(name[7]));
    return ok ? (name[6] - '0') * 10 + (name[7] - '0') : -1;
}

// the level folders the game has, by number
const std::vector<int>& Levels() {
    static const std::vector<int> levels = [] {
        std::vector<int> out;
        if (const Disc* disc = MountedDisc()) {
            if (const Disc::Entry* dir = disc->Find("Levels"))
                for (const Disc::Entry& e : dir->children)
                    if (e.dir && LevelNumber(e.name) >= 0) out.push_back(LevelNumber(e.name));
        } else {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(ResolvePath(kDiscRoot + "Levels", 0), ec))
                if (e.is_directory(ec) && LevelNumber(e.path().filename().string()) >= 0)
                    out.push_back(LevelNumber(e.path().filename().string()));
        }
        std::sort(out.begin(), out.end());
        return out;
    }();
    return levels;
}

// ---- first person banks: Level_NN\fpguns\ a level lacks is read from another level's -----------------------------

// in place, the same length: the level number of a bank path to one that has the file
void RedirectBank(uint32_t path_va) {
    std::string path = ReadCString(path_va, 260);
    std::string low = Lower(path);
    size_t at = low.find("level_");
    if (at == std::string::npos || low.find("\\fpguns\\", at) != at + 8 || !std::isdigit(uint8_t(low[at + 6])) ||
        !std::isdigit(uint8_t(low[at + 7])))
        return;
    static std::mutex lock;
    static std::map<std::string, int> chosen;  // lowercase path -> the level to read it from, -1 as it is
    std::lock_guard<std::mutex> guard(lock);
    auto it = chosen.find(low);
    if (it == chosen.end()) {
        size_t root = path.find(":\\");
        size_t skip = root == std::string::npos ? 0 : root + 2;  // a D:\ in front
        std::string rel = path.substr(skip);
        int level = -1;
        if (!GameFileExists(rel)) {
            for (int n : Levels()) {
                char digits[3];
                std::snprintf(digits, sizeof(digits), "%02d", n);
                std::string other = rel;
                other.replace(at - skip + 6, 2, digits);
                if (GameFileExists(other)) {
                    level = n;
                    break;
                }
            }
            if (level < 0) LOGW("weapons: no level has %s", path.c_str());
            else LOGI("weapons: %s from Level_%02d", path.c_str(), level);
        }
        it = chosen.emplace(low, level).first;
    }
    if (it->second < 0) return;
    char* s = static_cast<char*>(HostPtr(path_va));
    s[at + 6] = char('0' + it->second / 10);
    s[at + 7] = char('0' + it->second % 10);
}

// ---- world models: another level's Guns.bin for the "_LOD" models this one lacks ----------------------------------

struct GunsFile {
    std::string path;  // Levels\Level_NN\Stg_0001\Guns.bin
    bool silver;       // Guns_s.bin
    std::vector<uint64_t> models;
    uint32_t buffer = 0;     // guest copy, made the first time it is needed
    uint32_t dicts[2] = {};  // its TEXTURE and MODELS dictionaries while registered, this level
};

std::vector<GunsFile>& GunsFiles() {
    static std::vector<GunsFile> files = [] {
        std::vector<GunsFile> out;
        for (int n : Levels()) {
            for (const char* name : {"Guns.bin", "Guns_s.bin"}) {
                char path[64];
                std::snprintf(path, sizeof(path), "Levels\\Level_%02d\\Stg_0001\\%s", n, name);
                std::vector<uint8_t> d;
                if (!GameFileExists(path) || !LoadGameFile(path, d)) continue;
                auto u32 = [&](size_t at) {
                    uint32_t v = 0;
                    if (at + 4 <= d.size()) std::memcpy(&v, &d[at], 4);
                    return v;
                };
                // +4 the MODELS dictionary: +8 its count, +0xC its entries, 16 bytes each from the name key
                uint32_t dict = u32(4);
                uint32_t count = u32(dict + 8), entries = dict + u32(dict + 0xC);
                GunsFile g{path, name[4] == '_', {}};
                for (uint32_t i = 0; i < count && i < 64; i++) {
                    uint64_t key = u32(entries + i * 16) | uint64_t(u32(entries + i * 16 + 4)) << 32;
                    if (key) g.models.push_back(key);
                }
                out.push_back(std::move(g));
            }
        }
        return out;
    }();
    return files;
}

int FreeSlots(uint32_t list) {
    int free = 0;
    for (int i = 0; i < kDictSlots; i++) free += R32(list + i * 4) == 0;
    return free;
}

void Unregister(uint32_t list, uint32_t dict) {
    for (int i = 0; i < kDictSlots; i++)
        if (dict && R32(list + i * 4) == dict) W32(list + i * 4, 0);
}

// a level is loading: what this one registered goes, the game may have cleared it already
void ForgetExtraGuns() {
    uint32_t res = R32(kResources);
    for (GunsFile& g : GunsFiles()) {
        if (res) {
            Unregister(res + kTextureDicts, g.dicts[0]);
            Unregister(res + kModelDicts, g.dicts[1]);
        }
        g.dicts[0] = g.dicts[1] = 0;
    }
}

// a world model, from another level's Guns.bin when this level lacks it; false when there is no room
bool EnsureModel(X86Context& ctx, uint64_t key, bool silver) {
    uint32_t res = R32(kResources);
    if (!res) return false;
    if (CallGuest(ctx, kFindModel, {res, uint32_t(key), uint32_t(key >> 32)})) return true;
    GunsFile* pick = nullptr;
    for (GunsFile& g : GunsFiles()) {
        if (g.dicts[1] || std::find(g.models.begin(), g.models.end(), key) == g.models.end()) continue;
        if (!pick || (g.silver == silver && pick->silver != silver)) pick = &g;
    }
    if (!pick) return false;
    if (FreeSlots(res + kTextureDicts) <= kDictSlotsKept || FreeSlots(res + kModelDicts) <= kDictSlotsKept) return false;
    std::vector<uint8_t> d;
    if (!LoadGameFile(pick->path, d) || d.size() < 16) return false;
    if (!pick->buffer) pick->buffer = mem::AllocContiguous(uint32_t(d.size()), 0, 0xFFFFFFFF, 0x1000);
    if (!pick->buffer) return false;
    std::memcpy(HostPtr(pick->buffer), d.data(), d.size());  // as on disc again: the game relocates it in place
    // known before the fix up: its models look their textures up while it runs (sub_001FBCE0 below)
    pick->dicts[0] = pick->buffer + R32(pick->buffer);
    pick->dicts[1] = pick->buffer + R32(pick->buffer + 4);
    uint32_t eax = ctx.eax;
    ctx.eax = pick->buffer;
    CallGuest(ctx, kFixGuns, {});
    ctx.eax = eax;
    LOGI("weapons: world models from %s", pick->path.c_str());
    return CallGuest(ctx, kFindModel, {res, uint32_t(key), uint32_t(key >> 32)}) != 0;
}

bool EnsureWorldModel(X86Context& ctx, const std::string& weapon_name, bool silver) {
    return EnsureModel(ctx, Encode(weapon_name + "_LOD"), silver);
}

// A dropped weapon becomes a pickup with its world model, a null one crashes: it is loaded now if it can be, else
// the drop makes none, as when the game's pickups are all in use. pm the pickup manager: +0x5844 the pickup table
// (count byte, +8 entries of 0x18: weapon key, model key, weapon type byte at +0x10), +0x5800 its models found
constexpr uint32_t kPickupTable = 0x5844;
constexpr uint32_t kPickupModels = 0x5800;
constexpr uint32_t kPickupSize = 0x18;
bool DropHasModel(X86Context& ctx, uint32_t pm, uint8_t type) {
    uint32_t table = pm ? R32(pm + kPickupTable) : 0;
    if (!table) return true;
    uint32_t entries = R32(table + 8);
    int count = G<uint8_t>(table);
    int found = -1;
    for (int i = 0; i < count && found < 0; i++)
        if (G<uint8_t>(entries + uint32_t(i) * kPickupSize + 0x10) == type) found = i;
    if (found < 0) return false;
    uint32_t models = R32(pm + kPickupModels);
    if (models && R32(models + uint32_t(found) * 4)) return true;
    uint32_t e = entries + uint32_t(found) * kPickupSize;
    uint64_t key = R32(e + 8) | uint64_t(R32(e + 12)) << 32;
    uint32_t world = R32(kWorld);
    return EnsureModel(ctx, key, world && G<uint8_t>(world + kWorldSilver) != 0);
}

// ---- the player and the level's weapon table ---------------------------------------------------------------------

struct Level {
    uint32_t world, player, inventory, manager, entries;
    int count;
};

bool ReadLevel(Level& l) {
    l.world = R32(kWorld);
    if (!l.world || R32(l.world + kWorldState) != kStatePlaying) return false;
    l.player = l.world + kWorldPlayer;
    l.inventory = l.player + kPlayerInventory;
    if (R32(l.player + kPlayerKind) != kKindPlayer || R32(l.inventory + kInvOwner) != l.player ||
        !R32(l.inventory + kInvSlots))
        return false;
    l.manager = R32(kWeaponManager);
    uint32_t table = l.manager ? R32(l.manager) : 0;
    if (!table || !R32(l.manager + 4)) return false;
    l.count = G<uint8_t>(table + kTableCount);
    l.entries = R32(table + kTableEntries);
    return l.entries && l.count > 0 && l.count <= 64;
}

uint64_t EntryKey(const Level& l, int type) {
    uint32_t e = l.entries + uint32_t(type) * kEntrySize;
    return R32(e) | uint64_t(R32(e + 4)) << 32;
}

uint32_t Slot(const Level& l, int i) { return R32(R32(l.inventory + kInvSlots) + uint32_t(i) * 4); }

void Refresh() {
    Level l;
    weapons::View v;
    v.in_level = ReadLevel(l);
    if (v.in_level) {
        int slots = std::min<int>(G<uint8_t>(l.inventory + kInvSlotCount), 2);
        for (int t = 0; t < l.count; t++) {
            std::string name = Decode(EntryKey(l, t));
            if (name.empty()) continue;
            bool held = false;
            for (int i = 0; i < slots; i++)
                if (uint32_t w = Slot(l, i)) held |= G<uint8_t>(w + kWeaponType) == t;
            v.choices.push_back({Label(name), t, held});
        }
        std::sort(v.choices.begin(), v.choices.end(), [](const auto& a, const auto& b) { return a.label < b.label; });
    }
    std::lock_guard<std::mutex> lock(g_lock);
    v.status = g_view.status;
    g_view = std::move(v);
}

// the type's ammunition to at least five clips
void FillType(const Level& l, uint32_t data, bool top_up) {
    if (!data) return;
    uint32_t type = R32(data + kDataAmmoType), clip = G<uint16_t>(data + kDataClip);
    if (type >= kAmmoTypes) return;
    uint16_t& ammo = G<uint16_t>(l.inventory + type * 2);
    uint32_t want = std::min(kAmmoMax, top_up ? kAmmoMax : std::max(clip * 5, 1u));
    if (ammo < want) ammo = uint16_t(want);
}

void FillWeapon(const Level& l, uint32_t weapon, bool top_up) {
    for (uint32_t m = 0; m < 2; m++)
        if (uint32_t mag = R32(weapon + kWeaponMagazine + m * 4)) FillType(l, R32(mag + kMagazineData), top_up);
}

void DoGive(X86Context& ctx, int type) {
    Level l;
    if (!ReadLevel(l)) return SetStatus("Only during a level");
    if (type < 0 || type >= l.count) return;
    std::string name = Decode(EntryKey(l, type));
    if (G<uint8_t>(l.inventory + kInvSlotCount) < 2) return SetStatus("The player has a single weapon slot here");
    if (R32(l.inventory + kInvRequest) || R32(l.inventory + kInvHeld) != R32(l.inventory + kInvSwitchTo))
        return SetStatus("Wait for the weapon switch to end");
    int held = G<int8_t>(l.inventory + kInvSlotHeld);
    int other = held == 0 ? 1 : 0;
    uint32_t slots = R32(l.inventory + kInvSlots);
    uint32_t old = R32(slots + uint32_t(other) * 4);
    if (old && old == R32(l.inventory + kInvHeld)) return SetStatus("Wait for the weapon switch to end");
    // the gun made first: a full pool keeps the old one
    uint32_t edi = ctx.edi;
    ctx.edi = l.manager;
    uint32_t weapon = CallGuest(ctx, kCreateWeapon, {l.player, uint32_t(type), 0});
    ctx.edi = edi;
    if (!weapon) return SetStatus("The game has no room for another weapon");
    if (old) {
        uint32_t edx = ctx.edx;
        ctx.edi = l.manager;
        ctx.edx = old;
        CallGuest(ctx, kFreeWeapon, {});
        ctx.edi = edi;
        ctx.edx = edx;
    }
    W32(slots + uint32_t(other) * 4, weapon);
    FillWeapon(l, weapon, false);
    W32(l.inventory + kInvRequest, 2);  // out it comes, as the d-pad brings it
    bool model = EnsureWorldModel(ctx, name, G<uint8_t>(l.world + kWorldSilver) != 0);
    SetStatus(Label(name) + (model ? " given" : " given (dropped, it will vanish)"));
    Refresh();
}

void DoFill() {
    Level l;
    if (!ReadLevel(l)) return SetStatus("Only during a level");
    for (int i = 0; i < 2; i++)
        if (uint32_t w = Slot(l, i)) FillWeapon(l, w, true);
    SetStatus("Ammunition full");
}

}  // namespace

namespace weapons {

View Current() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_view;
}

void Give(int type) { g_give = type; }

void FillAmmo() { g_fill = true; }

}  // namespace weapons

// game thread, each menu and HUD tick (hooks.cpp MenuTick)
void WeaponsTick(X86Context& ctx) {
    static int tick;
    if (int type = g_give.exchange(-1); type >= 0) DoGive(ctx, type);
    if (g_fill.exchange(false)) DoFill();
    if (++tick % 10 == 0) Refresh();
}

// the game's file opening: a first person bank the level lacks comes from another level's folder
X86_EXTERN_FUNC(__imp__sub_000C2C50);
X86_FUNC(sub_000C2C50) {
    RedirectBank(R32(ctx.esp + 4));
    __imp__sub_000C2C50(ctx, base);
}

// A texture by name, (resources, name) stdcall: the game takes only dictionaries in its own memory heaps, so the
// ones added from other levels' Guns.bin are searched after it (their world models showed white without)
constexpr uint32_t kFindInTextures = 0x14B00;  // esi a TEXTURE dictionary; (name) -> the texture or 0
X86_EXTERN_FUNC(__imp__sub_001FBCE0);
X86_FUNC(sub_001FBCE0) {
    uint32_t name = R32(ctx.esp + 8);
    __imp__sub_001FBCE0(ctx, base);
    if (ctx.eax || !name) return;
    for (const GunsFile& g : GunsFiles()) {
        if (!g.dicts[0]) continue;
        uint32_t esi = ctx.esi;
        ctx.esi = g.dicts[0];
        uint32_t texture = CallGuest(ctx, kFindInTextures, {name});
        ctx.esi = esi;
        if (texture) {
            ctx.eax = texture;
            return;
        }
    }
}

// a weapon dropped as a pickup: eax the pickup manager, (type, ...) stdcall, five arguments
X86_EXTERN_FUNC(__imp__sub_001E0EB0);
X86_FUNC(sub_001E0EB0) {
    uint8_t type = G<uint8_t>(ctx.esp + 4);
    if (!DropHasModel(ctx, ctx.eax, type)) {
        LOGI("weapons: no world model for weapon %u, the drop makes no pickup", type);
        Return(ctx, 0, 5);
        return;
    }
    __imp__sub_001E0EB0(ctx, base);
}

// a level's Guns.bin is in: the world models added for the last level go first
X86_EXTERN_FUNC(__imp__sub_001D3790);
X86_FUNC(sub_001D3790) {
    ForgetExtraGuns();
    __imp__sub_001D3790(ctx, base);
}
