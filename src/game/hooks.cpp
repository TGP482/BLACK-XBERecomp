// Hooks for default.xbe
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "app/app.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/input.h"
#include "game/layout.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;
using namespace layout;

namespace d3d {
void ViewScale(float xy);
}

namespace {

constexpr uint32_t kDevice = SYM(D3D_Device);
constexpr uint32_t kInitDefaultState = 0x271710;

void WriteFloat(uint32_t va, float f) { std::memcpy(HostPtr(va), &f, 4); }

// what the library's vertical blank interrupt does that the game sees: count, event, callback
void VerticalBlank(X86Context& ctx) {
    uint32_t info = mem::AllocSystem(12);
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        // the film decoder paces on the count: it stands still through a focus pause
        while (game::g_focus_paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            next = std::chrono::steady_clock::now();
        }
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
        uint32_t count = R32(kDevice + XDK_CDevice_VBlankCount) + 1;
        W32(kDevice + XDK_CDevice_VBlankCount, count);
        ObjectRef event = FromPointer(kDevice + XDK_CDevice_VBlankEvent);
        SetEvent(*event);
        if (uint32_t callback = R32(kDevice + XDK_CDevice_VBlankCallback)) {
            W32(info, count);
            W32(info + 4, R32(kDevice + XDK_CDevice_SwapCount));
            W32(info + 8, 0);
            CallGuest(ctx, callback, {info});
        }
        // the game clears the event itself before each wait, which the host object does not see
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ResetEvent(*event);
    }
}

}  // namespace

// the viewport as D3DDevice_SetViewport stores it, for the game's own readers
void StoreViewport(X86Context&, uint8_t*) {
    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
    const gpu::Viewport& v = d3d::State().viewport;
    uint32_t p = kDevice + XDK_CDevice_Viewport;
    W32(p, v.x);
    W32(p + 4, v.y);
    W32(p + 8, v.w);
    W32(p + 12, v.h);
    WriteFloat(p + 16, v.min_z);
    WriteFloat(p + 20, v.max_z);
}

// D3DDevice_GetViewportOffsetAndScale
X86_EXTERN_FUNC(__imp__sub_0026E9C0);
X86_FUNC(sub_0026E9C0) {
    StoreViewport(ctx, base);
    __imp__sub_0026E9C0(ctx, base);
}

// device fields the library's init sets and the game or its inlined library code reads, then the default states
void AfterCreateDevice(X86Context& ctx, uint8_t* base) {
    if (ctx.eax) return;
    W32(kDevice + XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
    W32(kDevice + XDK_CDevice_AutoDepth, R32(kDevice + XDK_CDevice_Depth));
    WriteFloat(kDevice + XDK_CDevice_BackBufferScale, 1.0f);
    WriteFloat(kDevice + XDK_CDevice_BackBufferScale + 4, 1.0f);
    W32(kDevice + XDK_CDevice_DisplayFlags, 0x400000 | (app::Widescreen() ? 0x10000 : 0));  // 60 Hz, widescreen
    W32(kDevice + XDK_CDevice_VBlankEvent, 0x00040000);  // notification event
    W32(kDevice + XDK_CDevice_VBlankEvent + 4, 1);
    StoreViewport(ctx, base);
    uint32_t ecx = ctx.ecx;
    uint32_t edx = ctx.edx;
    CallGuest(ctx, kInitDefaultState, {});
    ctx.eax = 0;
    ctx.ecx = ecx;
    ctx.edx = edx;
    static bool started = false;
    if (!started) StartHostThread("vertical blank", VerticalBlank);
    started = true;
}

constexpr uint32_t kFrameCount = 0x2F4368;

// seconds the last frame took, the frame time the game was made for at most
static float FrameTime() {
    using clock = std::chrono::steady_clock;
    static clock::time_point last;
    static uint32_t frame;
    static float dt = G<float>(kGameFrameTime);
    if (uint32_t f = R32(kFrameCount); f != frame) {
        auto now = clock::now();
        dt = std::min(std::chrono::duration<float>(now - last).count(), G<float>(kGameFrameTime));
        frame = f;
        last = now;
    }
    return dt;
}

// The game's timers step one fixed frame time a frame: they step the real one, so any frame rate plays at speed
X86_EXTERN_FUNC(__imp__sub_000BE3F0);
X86_FUNC(sub_000BE3F0) {
    uint32_t t = ctx.ecx;
    auto steps = [t] {
        double parts = double(R32(t + kTimerPartSteps)) / std::max(R32(t + kTimerDivisor), 1u);
        return R32(t + kTimerWholeSteps) + parts;
    };
    double before = steps();
    float time = G<float>(t + kTimerTime);
    __imp__sub_000BE3F0(ctx, base);
    float dt = FrameTime();
    G<float>(t + kTimerDt) = dt / std::max(R32(t + kTimerDivisor), 1u);
    G<float>(t + kTimerTime) = time + float((steps() - before) * dt);
}

// frame throttle: sleeps that gave the console's one CPU to the loader
X86_FUNC(sub_001F9130) { Return(ctx, ctx.eax, 1); }

// The Flash menus and HUD tick in milliseconds, 33 a call: the real ones, the part of one carried over
void MenuTick(X86Context& ctx, uint8_t*) {
    using clock = std::chrono::steady_clock;
    static clock::time_point last;
    static float carry;
    auto now = clock::now();
    carry += std::min(std::chrono::duration<float>(now - last).count(), G<float>(kGameFrameTime)) * 1000.0f;
    last = now;
    ctx.eax = uint32_t(carry);
    carry -= float(ctx.eax);
}

// what a step made once a frame at 30 becomes over the real frame: factor to the power of this
static float FrameShare() { return FrameTime() / G<float>(kGameFrameTime); }

// weapon and its data
constexpr uint32_t kWeaponData = 0xEC;
constexpr uint32_t kWeaponOwner = 0xF0;
constexpr uint32_t kDataAimZoom = 0xB4;
// player
constexpr uint32_t kPlayerForward = 0xF0;
constexpr uint32_t kPlayerEye = 0x100;
constexpr uint32_t kPlayerWeapon = 0x29C;
constexpr uint32_t kPlayerZoom = 0x2A4;
constexpr uint32_t kPlayerFireRequest = 0x32C;

// The zoom of the aim grows or shrinks by a fixed share a frame
void ZoomStep(X86Context& ctx, uint8_t*) {
    float old = G<float>(R32(ctx.ecx + kWeaponOwner) + kPlayerZoom);
    float now = ctx.xmm[0].f[0];
    if (now == old || old <= 0) return;
    bool out = !G<uint8_t>(ctx.ecx + 0x105) && old > 1;  // back towards 1
    ctx.xmm[0].f[0] = out ? 1 + (old - 1) * std::pow((now - 1) / (old - 1), FrameShare())
                          : old * std::pow(now / old, FrameShare());
}

constexpr float kHalf = 3.14159265f / 360.0f;

// tangent of a setting's half angle over that of the game's 70 degrees
static float Widen(uint32_t degrees) {
    return std::tan(std::clamp(degrees, 45u, 140u) * kHalf) / std::tan(70 * kHalf);
}

static float g_gun = 1.0f;  // the gun's picture over the view's

// The view of play, 70 over the zoom: the setting from the hip, the game's own once the aim is in.
// ecx the weapon, xmm0 its zoom. Scripted cameras write theirs after this
void FieldOfView(X86Context& ctx, uint8_t*) {
    uint32_t data = R32(ctx.ecx + kWeaponData);
    float aim = data ? G<float>(data + kDataAimZoom) : 1;
    float zoom = ctx.xmm[0].f[0];
    float hip = aim > 1 ? std::clamp((aim - zoom) / (aim - 1), 0.0f, 1.0f) : 1.0f;
    float view = 1 + (Widen(game::Opt().fov) - 1) * hip;
    g_gun = view / (1 + (Widen(game::Opt().viewmodel_fov) - 1) * hip);
    float& fov = ctx.xmm[1].f[0];
    if (view != 1) fov = std::atan(std::tan(fov * kHalf) * view) / kHalf;
}

// The list of the first person models, gun, arms and muzzle mesh: drawn at the viewmodel setting
X86_EXTERN_FUNC(__imp__sub_00176F60);
X86_FUNC(sub_00176F60) {
    bool gun = g_gun != 1 && ctx.esi == R32(kDisplay) + kDisplayDrawLists + kGunDrawList * kDrawListSize;
    if (gun) d3d::ViewScale(g_gun);
    __imp__sub_00176F60(ctx, base);
    if (gun) d3d::ViewScale(1);
}

static uint32_t Player() { return R32(kWorld) + kWorldPlayer; }

// What sits on the gun follows its picture: across the player's view a point or an axis grows by the same share,
// its depth stays
static void OnGun(uint32_t at, bool point) {
    float* v = &G<float>(at);
    const float* forward = &G<float>(Player() + kPlayerForward);
    const float* eye = &G<float>(Player() + kPlayerEye);
    float d[3];
    float depth = 0;
    for (int i = 0; i < 3; i++) {
        d[i] = v[i] - (point ? eye[i] : 0);
        depth += d[i] * forward[i];
    }
    for (int i = 0; i < 3; i++) v[i] += (g_gun - 1) * (d[i] - forward[i] * depth);
}

constexpr uint32_t kMatrixPosition = 48;  // the last of a matrix's four rows

// A locator's matrix moved onto the gun's picture, as a copy: the game's own stays where bullets start
static uint32_t Follow(uint32_t matrix, uint32_t owner, bool axes) {
    if (g_gun == 1 || matrix == 0x5655C0 || owner != Player()) return matrix;
    static uint32_t copy = mem::AllocSystem(64, 16);
    std::memcpy(HostPtr(copy), HostPtr(matrix), 64);
    for (uint32_t row = axes ? 0 : kMatrixPosition; row < 64; row += 16) OnGun(copy + row, row == kMatrixPosition);
    return copy;
}

constexpr uint32_t kEffectOwner = 0x44;

// the muzzle flash sprite, drawn with the matrix
void FlashSprite(X86Context& ctx, uint8_t*) { ctx.eax = Follow(ctx.eax, Player(), true); }
// effects that follow a locator of their owner (edi)
void GunEffect(X86Context& ctx, uint8_t*) { ctx.eax = Follow(ctx.eax, ctx.edi, false); }
// effects that start at one
void GunEffectStart(X86Context& ctx, uint8_t*) { ctx.edx = Follow(ctx.edx, R32(ctx.ebx + kEffectOwner), false); }

// bullet and its shot
constexpr uint32_t kBulletShot = 0x70;
constexpr uint32_t kShotWeapon = 4;

// esi a bullet that missed, eax where its tracer starts
void TracerStart(X86Context& ctx, uint8_t*) {
    uint32_t shot = R32(ctx.esi + kBulletShot);
    uint32_t weapon = shot ? R32(shot + kShotWeapon) : 0;
    if (g_gun == 1 || !weapon || R32(weapon + kWeaponOwner) != Player()) return;
    static uint32_t copy = mem::AllocSystem(16, 16);
    std::memcpy(HostPtr(copy), HostPtr(ctx.eax), 16);
    ctx.eax = copy;
    OnGun(copy, true);
}

constexpr uint32_t kFromTracerA = 0x1DBAAB;  // return addresses of the two tracers
constexpr uint32_t kFromTracerB = 0x1DC4D4;

// The player's muzzle matrix: moved for the two tracers that start from it
X86_EXTERN_FUNC(__imp__sub_001D2860);
X86_FUNC(sub_001D2860) {
    uint32_t from = R32(ctx.esp);
    uint32_t out = R32(ctx.esp + 4);
    __imp__sub_001D2860(ctx, base);
    if (g_gun != 1 && (from == kFromTracerA || from == kFromTracerB)) OnGun(out + kMatrixPosition, true);
}

// recoil dies away by a factor a frame
void RecoilDecay(X86Context& ctx, uint8_t*) { ctx.xmm[0].f[0] = std::pow(ctx.xmm[0].f[0], FrameShare()); }

constexpr uint32_t kDataSwayShares = 0x104;

// The gun trails the stick by two shares a frame, read from the weapon's data: scaled for the four blends
static uint32_t g_sway;
static float g_sway_share[2];
void SwayBegin(X86Context& ctx, uint8_t*) {
    g_sway = ctx.eax + kDataSwayShares;
    for (int i = 0; i < 2; i++) {
        float& k = G<float>(g_sway + i * 4);
        g_sway_share[i] = k;
        k = 1 - std::pow(1 - k, FrameShare());
    }
}
void SwayEnd(X86Context&, uint8_t*) {
    if (!g_sway) return;
    for (int i = 0; i < 2; i++) G<float>(g_sway + i * 4) = g_sway_share[i];
    g_sway = 0;
}

static float Dt() { return G<float>(R32(kWorld) + kTimerDt); }

// A wait set and tested once a frame lasts whole frames of the game's frame time: as many at any frame rate.
// rest is added, what the last wait ran over by
static float WholeFrames(float wait, float rest) {
    float step = G<float>(kGameFrameTime) / std::max(R32(R32(kWorld) + kTimerDivisor), 1u);
    if (!(Dt() < step)) return wait;
    int frames = 0;
    for (float left = wait; left > 0 && frames < 4096; left -= step) frames++;
    return std::max(frames, 1) * step + rest;
}

static bool g_idle_early;

// weapon
constexpr uint32_t kWeaponState = 0xD8;
constexpr uint32_t kWeaponWait = 0xDC;

// the wait between a weapon's shots. The frame of a first shot is part of it: the fire clip runs that frame too
// and holds its next shot where the wait ends
X86_EXTERN_FUNC(__imp__sub_001EEFD0);
X86_FUNC(sub_001EEFD0) {
    uint32_t weapon = R32(ctx.esp + 4);
    float& wait = G<float>(weapon + kWeaponWait);
    float rest = wait;
    float dt = Dt();
    __imp__sub_001EEFD0(ctx, base);
    uint32_t me = R32(weapon + kWeaponOwner);
    if (me == Player()) {
        if (R32(weapon + kWeaponState) != 1 || wait != rest) g_idle_early = false;
        else if (g_idle_early) W32(me + kPlayerFireRequest, 0);  // the fire request is spent, as on the console
    }
    if (wait == rest) return;
    wait = WholeFrames(wait, rest <= 0 && rest > -dt ? rest : -dt);
}

// animation controller
constexpr uint32_t kAnimOwner = 0;
constexpr uint32_t kAnimClip = 0x54;
constexpr uint32_t kAnimName = 0x5C;
constexpr uint32_t kAnimFrom = 0x90;
// animation clip
constexpr uint32_t kAnimClipHash = 0x18;
constexpr uint32_t kAnimClipFades = 0x88;
constexpr uint32_t kFadeSize = 0x28;
constexpr uint32_t kAnimClipFadeSlot = 0x1C8;  // the current fade
constexpr uint32_t kFireLoopHash = 0x6D9D2AC0;

static bool IsFireClip(const std::string& name) {
    static const char* fire[] = {"S_100", "S_110", "S_114", "S_130", "S_170", "S_176"};
    bool fired = false;
    for (auto f : fire) fired |= name.size() >= 5 && name.compare(name.size() - 5, 5, f) == 0;
    return fired;
}

// A fire clip that ends before the wait does hands over to the idle early: the request would start it again.
// An idle fading in over a looping fire clip holds it still instead of running it into its next shot
X86_EXTERN_FUNC(__imp__sub_0017B130);
X86_FUNC(sub_0017B130) {
    uint32_t ac = R32(ctx.esp + 4);
    uint32_t from = R32(ac + kAnimFrom);
    uint32_t clip = R32(ac + kAnimClip);
    if (R32(ac + kAnimOwner) != Player() || !clip) return __imp__sub_0017B130(ctx, base);
    int16_t slot = G<int16_t>(clip + kAnimClipFadeSlot);
    uint32_t fade = clip + kAnimClipFades + slot * kFadeSize;
    __imp__sub_0017B130(ctx, base);
    if (G<int16_t>(clip + kAnimClipFadeSlot) == slot || FrameShare() >= 1) return;
    uint32_t weapon = R32(R32(ac + kAnimOwner) + kPlayerWeapon);
    if (weapon && R32(weapon + kWeaponState) == 1) {
        if (!IsFireClip(ReadCString(ac + kAnimName, 13))) g_idle_early = true;
    }
    // from +0x10 3 and its hash: the looping fire clip
    bool fire = from && R32(from + 0x10) == 3 && R32(from + kAnimClipHash) == kFireLoopHash;
    if (fire && G<uint16_t>(fade + 0x24)) W32(fade + 0x18, 2);
}

// how long a shot holds an enemy's gun sound: past the wait
X86_EXTERN_FUNC(__imp__sub_0013FC20);
X86_FUNC(sub_0013FC20) {
    float& hold = G<float>(ctx.esp + 8);
    hold = WholeFrames(hold, Dt());
    __imp__sub_0013FC20(ctx, base);
}

// physics [kPhysics]: +0 the solver
constexpr uint32_t kPhysics = 0x2D45F4;
constexpr uint32_t kSolverWorld = 0x18;
// physics world
constexpr uint32_t kPhysicsBodyEnd = 0x270;
constexpr uint32_t kPhysicsBodies = 0x29C;
constexpr uint32_t kPhysicsGravity = 0x360;
constexpr uint32_t kPhysicsGravityStep = 0x370;  // gravity a step squared
constexpr uint32_t kPhysicsStep = 0x380;
constexpr uint32_t kPhysicsRate = 0x384;
constexpr uint32_t kPhysicsRestLimit = 0x390;
// body
constexpr uint32_t kBodyNext = 0x2C;
constexpr uint32_t kBodyImpulse = 0x90;

// bodies stepped by the old step move by the new one
static void ScaleBodies(uint32_t world, float by) {
    static const uint32_t kMove[] = {0x20, 0x24, 0x28, 0x30, 0x34, 0x38, 0xA0, 0xA4, 0xA8};
    uint32_t body = R32(world + kPhysicsBodies);
    for (int left = 4096; body && body != world + kPhysicsBodyEnd && left; left--, body = R32(body + kBodyNext)) {
        for (uint32_t at : kMove) G<float>(body + at) *= by;
        // gravity a step squared, an impulse on top of it a step
        for (int i = 0; i < 12; i += 4) {
            float g = G<float>(world + kPhysicsGravityStep + i);
            float& impulse = G<float>(body + kBodyImpulse + i);
            impulse = (impulse - g) * by + g * by * by;
        }
        G<float>(body + 0x9C) *= by * by;
    }
}

// The physics world steps once a frame by the frame time of the level's start and bodies hold their move a step:
// both follow the real frame time, before the characters write theirs
static bool g_rest_tick;
void PhysicsTime(X86Context&, uint8_t*) {
    uint32_t physics = R32(kPhysics);
    uint32_t solver = physics ? R32(physics) : 0;
    uint32_t world = solver ? R32(solver + kSolverWorld) : 0;
    float dt = FrameTime();
    float share = FrameShare();
    if (!world || !(dt > 0)) return;
    float& step = G<float>(world + kPhysicsStep);
    if (step > 0 && step != dt) ScaleBodies(world, dt / step);
    step = dt;
    G<float>(world + kPhysicsRate) = 1 / dt;
    for (int i = 0; i < 16; i += 4) {
        G<float>(world + kPhysicsGravityStep + i) = G<float>(world + kPhysicsGravity + i) * dt * dt;
    }
    // a body rests once its move a step stays under a limit for a count of steps: the limit shrinks with the
    // step, the count runs at the game's frame rate
    static float limit;
    static float written;
    static float ticks;
    float& now = G<float>(world + kPhysicsRestLimit);
    if (now != written) limit = now;
    now = written = limit * share * share;
    ticks += share;
    g_rest_tick = ticks >= 1;
    if (g_rest_tick) ticks -= 1;
}
bool BodyRestTick(X86Context&, uint8_t*) { return g_rest_tick; }

// a body's damping is a factor a step, its top speed a move a step
void BodyDamping(X86Context& ctx, uint8_t*) {
    for (int i = 0; i < 2; i++)
        if (float& keep = ctx.xmm[i].f[0]; keep > 0) keep = std::pow(keep, FrameShare());
}
void BodyTopSpeed(X86Context& ctx, uint8_t*) { ctx.xmm[1].f[0] *= FrameShare(); }

// a joint lets its bodies turn apart by at most a set angle a step
void JointTurnCap(X86Context& ctx, uint8_t*) { ctx.xmm[0].f[0] *= FrameShare(); }
void JointTurnCapNext(X86Context& ctx, uint8_t*) { ctx.xmm[6].f[0] *= FrameShare(); }

// contacts are searched this far ahead at least: twice the fall of a step at the game's frame rate
void ContactReach(X86Context& ctx, uint8_t*) { ctx.xmm[1].f[0] = 1 / G<float>(kGameFrameTime); }

// a ragdoll brakes for a wall within 8 frames of its speed: frames of the game's frame rate
void RagdollLookAhead(X86Context& ctx, uint8_t*) {
    if (float share = FrameShare(); share > 0) ctx.xmm[0].f[0] /= share;
}

// A joint's error, its anchors apart or a turn past its limit or lock, is put right within one step and that push
// stays on as speed: put right within a frame of the game's frame rate instead.
// xmm1 the gap of the anchors after the step, xmm2 the part of it the bodies' own motion makes
void JointAnchorGap(X86Context& ctx, uint8_t*) {
    float share = FrameShare();
    for (int i = 0; i < 4; i++) {
        float motion = ctx.xmm[2].f[i];
        ctx.xmm[1].f[i] = motion + (ctx.xmm[1].f[i] - motion) * share;
    }
}

// the turn a step may make on each of three axes, from [esp+0x60] to [esp+0x80]: a lock, or a limit already
// passed, asks for a turn back
constexpr uint32_t kJointTurnLow = 0x60;
constexpr uint32_t kJointTurnHigh = 0x80;
void JointTurnBack(X86Context& ctx, uint8_t*) {
    float share = FrameShare();
    for (uint32_t axis = 0; axis < 12; axis += 4) {
        float& low = G<float>(ctx.esp + kJointTurnLow + axis);
        float& high = G<float>(ctx.esp + kJointTurnHigh + axis);
        bool locked = low == high;
        if (locked || high < 0) high *= share;
        if (locked || low > 0) low *= share;
    }
}

constexpr uint32_t kShardAngle = 0xF4;

// Glass shards (0x1690D0, 0x110 bytes each) move, turn and count down once a frame of the game's frame rate
static bool g_shard_tick;
X86_EXTERN_FUNC(__imp__sub_001690D0);
X86_FUNC(sub_001690D0) {
    static float ticks;
    ticks += FrameShare();
    g_shard_tick = ticks >= 1;
    if (g_shard_tick) ticks -= 1;
    __imp__sub_001690D0(ctx, base);
}
bool ShardTick(X86Context&, uint8_t*) { return g_shard_tick; }

// xmm1 the gain of a shard's turn a frame
void ShardSpinGain(X86Context& ctx, uint8_t*) { ctx.xmm[1].f[0] *= FrameShare(); }

// xmm1 the new turn a frame: the angle takes a share of it
void ShardTurn(X86Context& ctx, uint8_t*) {
    ctx.xmm[0].f[0] = G<float>(ctx.ebx + kShardAngle) + ctx.xmm[1].f[0] * FrameShare();
}

// xmm0 the gain of a shard's move a frame, then the new move
void ShardStep(X86Context& ctx, uint8_t*) {
    float share = FrameShare();
    for (int i = 0; i < 4; i++) ctx.xmm[0].f[i] *= share;
}

// LOD and draw distances reach this many times farther: each compare sees a distance that much shorter or a
// limit that much longer. Characters, props, pickups, ground cover, particles and bullet holes
static float Reach() { return float(std::clamp(game::Opt().draw_distance, 1u, 8u)); }
void DistanceShorter(X86Context& ctx, uint8_t*) { ctx.xmm[0].f[0] /= Reach(); }
void DistanceShorterSquared(X86Context& ctx, uint8_t*) { ctx.xmm[0].f[0] /= Reach() * Reach(); }
void LimitLonger(X86Context& ctx, uint8_t*) { ctx.xmm[0].f[0] *= Reach(); }
void CharacterDetail(X86Context& ctx, uint8_t*) { ctx.xmm[1].f[0] /= Reach(); }
void PropLimit(X86Context& ctx, uint8_t*) { ctx.xmm[2].f[0] *= Reach() * Reach(); }
constexpr uint32_t kParticleFarCut = 0xC4;
constexpr uint32_t kCameraFarCut = 0x88;

// edi a particle type, xmm1 the view's scale, xmm2 its far cut under the camera's (edx): the fade before
// it starts as much farther as the cut got
void ParticleFade(X86Context& ctx, uint8_t*) {
    float cut = std::min(G<float>(ctx.edi + kParticleFarCut) * ctx.xmm[1].f[0], G<float>(ctx.edx + kCameraFarCut));
    if (Reach() > 1 && cut > 0) ctx.xmm[3].f[0] *= ctx.xmm[2].f[0] / cut;
}

// ground cover and its patches
constexpr uint32_t kCoverPatches = 0x28;
constexpr uint32_t kPatchCount = 63;
constexpr uint32_t kPatchSize = 16;
constexpr uint32_t kPatchDistance = 4;

// ebx the ground cover, 63 patches a frame: full, a nearer patch replaces the farthest
void GroundCoverFull(X86Context& ctx, uint8_t*) {
    if (ctx.eax < kPatchCount || game::Opt().draw_distance <= 1) return;
    uint32_t list = ctx.ebx + kCoverPatches;
    uint32_t last = list;
    for (uint32_t at = list + kPatchSize; at < list + kPatchCount * kPatchSize; at += kPatchSize)
        if (G<float>(at + kPatchDistance) > G<float>(last + kPatchDistance)) last = at;
    if (!(ctx.xmm[2].f[0] < G<float>(last + kPatchDistance))) return;
    W32(last, R32(ctx.ebp + 8));
    G<float>(last + kPatchDistance) = ctx.xmm[2].f[0];
    G<float>(last + 8) = ctx.xmm[1].f[0];
    G<uint8_t>(last + 0xC) = uint8_t(ctx.ecx);
    G<uint8_t>(last + 0xD) = ctx.edx == 2;
}

// draw lists 0, 2 and 3 hold every mesh in reach: a full list drops the rest
void DrawListRoom(X86Context&, uint8_t*) {
    static const uint32_t caps[] = {4096, 0, 2048, 512};
    static uint32_t room[4];
    uint32_t list = R32(kDisplay) + kDisplayDrawLists;
    for (int i = 0; i < 4; i++, list += kDrawListSize) {
        if (!caps[i]) continue;
        if (!room[i]) room[i] = mem::AllocSystem(caps[i] * 28);
        W32(list + kDrawListRoom, room[i]);
        if (R32(list + 4)) W32(list + 4, room[i] + caps[i] * 20);
        W32(list + kDrawListCap, caps[i]);
    }
}

// reload animations start the screen blur
bool ReloadBlur(X86Context&, uint8_t*) { return game::Opt().reload_blur; }

// the blur holds for the animation's frames times the last frame's time: frames of the game's frame rate instead
constexpr uint32_t kAnimationFrames = 0x1C;
void ReloadBlurHold(X86Context& ctx, uint8_t*) {
    float frames = G<float>(ctx.ecx + kAnimationFrames);
    ctx.xmm[0].f[0] = frames * G<float>(kGameFrameTime);
}

// A or Start on any pad, or the left mouse button, on the press
static bool SkipPressed() {
    static bool held = true;
    bool down = input::LeftButton();
    for (int port = 0; port < 4; port++) {
        input::Pad pad = input::Poll(port);
        down |= pad.analog[0] || (pad.buttons & 0x10);
    }
    bool pressed = down && !held;
    held = down;
    return pressed;
}

// film player: +0 the movie
constexpr uint32_t kFilmName = 0x84;
constexpr uint32_t kFilmFlags = 0x198;
constexpr uint32_t kFilmState = 0x1A8;
constexpr uint32_t kFilmStop = 0x1B9;
constexpr uint32_t kFilmPlaying = 0x1C;  // the last state
// movie
constexpr uint32_t kMovieFrameLength = 0x54;
constexpr uint32_t kMovieClockStep = 0x5C;

// The film player, one for every film: a film ends on a press unless the film table marks it as menu background,
// the logos (LO_) and the film after pressing start (02_) as they begin when intros are skipped
void SkipVideo(X86Context&, uint8_t*) {
    bool pressed = SkipPressed();
    uint32_t game_object = R32(kGame);
    uint32_t player = game_object + kFrontEnd + kFilmPlayer;
    uint32_t film = R32(game_object + kCurrentFilm);
    uint32_t state = R32(player + kFilmState);
    bool background = film < kFilms && G<uint8_t>(kFilmTable + film * kFilmEntry + kFilmBehindMenu);
    if (pressed) LOGI("film %u: press in state %X, flags %X", film, state, R32(player + kFilmFlags));
    // a film keeps the frame time of its start: its frame length, and its clock's step
    if (uint32_t movie = R32(player); movie && state >= 4 && state <= kFilmPlaying) {
        G<float>(movie + kMovieFrameLength) = G<float>(kGameFrameTime);
        G<float>(movie + kMovieClockStep) = G<float>(game_object + kGameClockStep);
    }
    if (background || state < 2 || state > kFilmPlaying) return;
    std::string name = ReadCString(player + kFilmName, 8);
    bool intro = name.rfind("LO_", 0) == 0 || name.rfind("02_", 0) == 0;
    if (pressed || (game::Opt().skip_intros && intro)) G<uint8_t>(player + kFilmStop) = 1;
}
