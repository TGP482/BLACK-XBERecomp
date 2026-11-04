// The cheats page's mission unlocks. The save keeps, per difficulty, which of the 8 missions are open; the game opens
// a mission on Black Ops once it was beaten on hard, the next mission once one is beaten. UNLOCK ALL MISSIONS opens
// every mission on the difficulties shown, UNLOCK BLACK OPS opens on Black Ops each mission open on another one and
// has the difficulty select offer it. A mission stays open while its cheat is on: loading one reads the table in many
// places, and a mission closed again under them hung the load. Turned off, the cheat closes what it opened; a save
// written while it was on keeps them open.
#include <vector>

#include "common/options.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// mission table: 8 missions of 0x20 bytes a difficulty, the difficulties 0x100 apart
constexpr uint32_t kMissionOpen = 0x2D0D9E;  // byte: 1 open
constexpr uint32_t kMissionSize = 0x20;
constexpr uint32_t kDifficultySize = 0x100;
constexpr uint32_t kMissions = 8;
constexpr uint32_t kBlackOps = 3;  // easy 0 to black ops 3

// as many difficulties as the difficulty select offers: 3 when no mission was picked on the level select, else as
// many as the mission is open on
constexpr uint32_t kDifficultySelection = 0x2EC560;

uint32_t Open(uint32_t difficulty, uint32_t mission) {
    return kMissionOpen + difficulty * kDifficultySize + mission * kMissionSize;
}

// the missions each cheat opened, closed again when it goes off
std::vector<uint32_t> g_levels;
std::vector<uint32_t> g_black_ops;

void Set(uint32_t at, std::vector<uint32_t>& opened) {
    if (G<uint8_t>(at)) return;
    G<uint8_t>(at) = 1;
    opened.push_back(at);
}

void Close(std::vector<uint32_t>& opened) {
    // a byte the game set to something else since is the game's
    for (uint32_t at : opened)
        if (G<uint8_t>(at) == 1) G<uint8_t>(at) = 0;
    opened.clear();
}

// the table as the cheats want it, before the game reads it
void Unlock() {
    const game::Options& o = game::Opt();
    if (!o.unlock_black_ops) Close(g_black_ops);
    if (!o.unlock_levels) Close(g_levels);
    for (uint32_t m = 0; m < kMissions; m++) {
        bool any = false;
        for (uint32_t d = 0; d < kBlackOps; d++) {
            if (o.unlock_levels) Set(Open(d, m), g_levels);
            any |= G<uint8_t>(Open(d, m)) == 1;
        }
        if (o.unlock_black_ops && any) Set(Open(kBlackOps, m), g_black_ops);
    }
}

// the count the cheat raised, given back when it goes off. The difficulty select's bindings set 3 once at boot and
// the level select a count when a mission is picked, so the cheat is kept there every tick of the front end too: on
// from the options, it shows without a restart
bool g_raised = false;
uint32_t g_count = 0;

void OfferBlackOps() {
    uint32_t count = R32(kDifficultySelection);
    if (game::Opt().unlock_black_ops) {
        if (count == kBlackOps + 1) return;
        g_count = count;
        g_raised = true;
        W32(kDifficultySelection, kBlackOps + 1);
    } else if (g_raised) {
        g_raised = false;
        if (count == kBlackOps + 1) W32(kDifficultySelection, g_count);
    }
}

}  // namespace

// the level select's flags and the difficulty select's ticks, by the difficulty in play; the front end's every tick
X86_EXTERN_FUNC(__imp__sub_00115030);
X86_FUNC(sub_00115030) {
    Unlock();
    __imp__sub_00115030(ctx, base);
    OfferBlackOps();
}

// the level select's choice of mission: its ticks and the count of difficulties open on it
X86_EXTERN_FUNC(__imp__sub_0011B890);
X86_FUNC(sub_0011B890) {
    Unlock();
    __imp__sub_0011B890(ctx, base);
    OfferBlackOps();
}

// the difficulty select's bindings, the count among them
X86_EXTERN_FUNC(__imp__sub_00124F30);
X86_FUNC(sub_00124F30) {
    Unlock();
    __imp__sub_00124F30(ctx, base);
    OfferBlackOps();
}

// the mission starts on the difficulty chosen, nothing when it is not open there
X86_EXTERN_FUNC(__imp__sub_00124CF0);
X86_FUNC(sub_00124CF0) {
    Unlock();
    __imp__sub_00124CF0(ctx, base);
}
