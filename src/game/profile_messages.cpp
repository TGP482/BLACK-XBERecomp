// Profile messages such as LOADING or SAVING. The game holds each one for 3.2 seconds (0x1F3140) and only then
// looks at the work behind it; the hold is now 1 second, or longer while that work is still running.
// The profile save's name is English in every language.
#include <cstring>

#include "xdk/hle.h"

using namespace kernel;

namespace {

// profile manager [0x2D19BC]
constexpr uint32_t kWorkState = 0x34;  // 0x1F done, 0x20 failed, lower while running (0x1F6F60)
constexpr uint32_t kHoldLeft = 0x504;  // float seconds, counted down each frame by 0x204C90

constexpr float kHoldSeconds = 1.0f;

constexpr uint32_t kWorkDone = 0x1F;
constexpr uint32_t kWorkFailed = 0x20;

bool g_work_started = false;

bool WorkFinished(uint32_t manager) {
    uint32_t state = R32(manager + kWorkState);
    return state == kWorkDone || state == kWorkFailed;
}

// FE_MCSAVE_PROFILE, the name the save is made and found by (0x1F78EC)
constexpr uint32_t kSaveName = 0x7D0DDAB7;

}  // namespace

namespace game {

// prompts.cpp: the save's name, or a profile made in one language is not found in another
uint32_t SaveName(uint32_t id) {
    if (id != kSaveName) return 0;
    static const uint32_t name = [] {
        uint32_t at = mem::AllocSystem(sizeof("Profile"));
        if (at) std::memcpy(HostPtr(at), "Profile", sizeof("Profile"));
        return at;
    }();
    return name;
}

}  // namespace game

// shows a message (esi manager, edx text) and starts its hold
X86_EXTERN_FUNC(__imp__sub_001F3140);
X86_FUNC(sub_001F3140) {
    g_work_started = false;
    uint32_t manager = ctx.esi;
    __imp__sub_001F3140(ctx, base);
    if (G<float>(manager + kHoldLeft) > 0) G<float>(manager + kHoldLeft) = kHoldSeconds;
}

// starts work (edi manager, ebx work)
X86_EXTERN_FUNC(__imp__sub_001FB560);
X86_FUNC(sub_001FB560) {
    g_work_started = true;
    __imp__sub_001FB560(ctx, base);
}

// true once the hold is over (eax manager)
X86_EXTERN_FUNC(__imp__sub_001FE1D0);
X86_FUNC(sub_001FE1D0) {
    bool running = g_work_started && !WorkFinished(ctx.eax);
    bool hold_over = G<float>(ctx.eax + kHoldLeft) <= 0;
    if (running && hold_over) {
        Return(ctx, 0, 0);  // still held
        return;
    }
    __imp__sub_001FE1D0(ctx, base);
}
