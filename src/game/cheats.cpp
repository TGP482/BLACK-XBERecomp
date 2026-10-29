// The cheats page's god mode, one-hit kill and infinite grenades. The rest live where the game
// already has a hook: infinite ammo, rapid fire and no recoil with the weapon's, game speed with the frame time
#include <algorithm>

#include "common/options.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

namespace {

// character: the player is the one in the world container, its kind 2
constexpr uint32_t kCharKind = 0xC4;
constexpr uint32_t kCharGrenades = 0x286;  // uint16
constexpr uint32_t kCharHealth = 0x2F0;    // float, the difficulty's full at most
constexpr uint32_t kCharDying = 0x384;     // not 0 once it is killed
// byte the scripts set: the player's damage calls (its class's 0x1ED990, 0x1ED700, 0x1ED2B0, 0x1ED5D0, and
// 0x1ED1E0) take nothing while it is
constexpr uint32_t kCharInvulnerable = 0x8F2;
constexpr uint32_t kPlayerKind = 2;

constexpr uint32_t kDifficulty = 0x2014C;  // in the game object, 0 easy to 3 black ops
// full health, as 0x13F10 scales 1.0: easy and normal, then hard and black ops
constexpr float kFullHealth[] = {1200.0f, 750.0f};

uint32_t Player() { return R32(kWorld) ? R32(kWorld) + kWorldPlayer : 0; }

float FullHealth() { return kFullHealth[R32(R32(kGame) + kDifficulty) >= 2]; }

// One-hit kill: a hit of the player's on another character starts from a sliver of health, any damage kills.
// A hit that took nothing gives the health back
constexpr float kSliver = 0.001f;

struct Doomed {
    uint32_t who = 0;
    float health = 0;
};

Doomed Doom(uint32_t victim, uint32_t attacker) {
    uint32_t me = Player();
    if (!game::Opt().one_hit_kill || !me || attacker != me || victim == me || R32(victim + kCharDying)) return {};
    float& health = G<float>(victim + kCharHealth);
    if (!(health > kSliver)) return {};
    Doomed kept{victim, health};
    health = kSliver;
    return kept;
}

void Spare(const Doomed& kept) {
    if (!kept.who || R32(kept.who + kCharDying)) return;
    float& health = G<float>(kept.who + kCharHealth);
    if (health > 0) health = kept.health - (kSliver - health);
}

bool g_invulnerable;  // the flag is ours, cleared when god mode goes off

}  // namespace

// The player's update, a frame (ecx the player, its class's own): god mode fills the health and holds the flag
// its damage calls test
X86_EXTERN_FUNC(__imp__sub_001E5FA0);
X86_FUNC(sub_001E5FA0) {
    uint32_t me = ctx.ecx;
    if (me && me == Player() && R32(me + kCharKind) == kPlayerKind) {
        if (game::Opt().god_mode && !R32(me + kCharDying)) {
            float& health = G<float>(me + kCharHealth);
            health = std::max(health, FullHealth());
            G<uint8_t>(me + kCharInvulnerable) = 1;
            g_invulnerable = true;
        } else if (g_invulnerable) {
            G<uint8_t>(me + kCharInvulnerable) = 0;
            g_invulnerable = false;
        }
    }
    __imp__sub_001E5FA0(ctx, base);
}

// A soldier takes a bullet (ecx the victim, the sixth of seven arguments its attacker, ret 0x1C): the
// characters' class, the player's has its own
X86_EXTERN_FUNC(__imp__sub_001E74B0);
X86_FUNC(sub_001E74B0) {
    Doomed doomed = Doom(ctx.ecx, R32(ctx.esp + 0x18));
    __imp__sub_001E74B0(ctx, base);
    Spare(doomed);
}

// A character's animation event (64-bit hash, then a pointer to the character): the throw takes a grenade
// (0x1E8934), the cheat puts the player's back
X86_EXTERN_FUNC(__imp__sub_001E8830);
X86_FUNC(sub_001E8830) {
    uint32_t who = R32(ctx.esp + 0xC);
    uint32_t me = who ? R32(who) : 0;
    bool keep = game::Opt().infinite_grenades && me && me == Player();
    uint16_t grenades = keep ? G<uint16_t>(me + kCharGrenades) : 0;
    __imp__sub_001E8830(ctx, base);
    if (keep && G<uint16_t>(me + kCharGrenades) < grenades) G<uint16_t>(me + kCharGrenades) = grenades;
}
