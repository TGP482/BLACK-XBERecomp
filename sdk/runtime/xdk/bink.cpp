// BinkGetSummary divides bytes read * 1000 by the read time in ms + 1. Host reads take under a millisecond,
// the time stays 0 and the quotient of a large movie overflows the 32 bit div. Raise the time just enough.
#include <algorithm>
#include <mutex>
#include <vector>

#include "xdk/hle.h"

using namespace kernel;

X86_EXTERN_FUNC(GUEST_ORIG(BinkGetSummary));
X86_FUNC(GUEST(BinkGetSummary)) {
    uint32_t bink = R32(ctx.esp + 4);
    if (bink) {
        // 0x134 bytes read, 0x13C read time in ms
        uint64_t scaled = uint64_t(R32(bink + 0x134)) * 1000;
        uint32_t time = R32(bink + 0x13C);
        if (scaled >> 32 > time) W32(bink + 0x13C, uint32_t(scaled >> 32));
    }
    GUEST_ORIG(BinkGetSummary)(ctx, base);
}

// Movies step one frame per game frame, timed by the console's 30 Hz present. Above 30 fps a frame is decoded
// and advanced only once BinkWait, which follows the movie's own rate and audio, says it is due.
namespace {

constexpr hle::Va kBinkWait = SYM(BinkWait);
std::mutex g_held_lock;
// movies whose frame was not decoded this game frame: a film has one, and a vector holds it without the set's
// per frame node allocation
std::vector<uint32_t> g_held;

void Hold(uint32_t bink) {
    if (std::find(g_held.begin(), g_held.end(), bink) == g_held.end()) g_held.push_back(bink);
}

// the movie was held this game frame: take it out
bool Release(uint32_t bink) {
    auto it = std::find(g_held.begin(), g_held.end(), bink);
    if (it == g_held.end()) return false;
    g_held.erase(it);
    return true;
}

}  // namespace

X86_EXTERN_FUNC(GUEST_ORIG(BinkDoFrame));
X86_FUNC(GUEST(BinkDoFrame)) {
    uint32_t bink = R32(ctx.esp + 4);
    if (bink && CallGuest(ctx, kBinkWait, {bink})) {
        std::lock_guard<std::mutex> lock(g_held_lock);
        Hold(bink);
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    GUEST_ORIG(BinkDoFrame)(ctx, base);
}

X86_EXTERN_FUNC(GUEST_ORIG(BinkNextFrame));
X86_FUNC(GUEST(BinkNextFrame)) {
    uint32_t bink = R32(ctx.esp + 4);
    {
        std::lock_guard<std::mutex> lock(g_held_lock);
        if (Release(bink)) {
            ctx.esp += 8;
            return;
        }
    }
    GUEST_ORIG(BinkNextFrame)(ctx, base);
}

namespace kernel {

// kernel::Restart: as before the first boot
void ResetBink() {
    std::lock_guard<std::mutex> lock(g_held_lock);
    g_held.clear();
}

}  // namespace kernel
