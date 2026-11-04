// The cheats page's ESP LINES: each living enemy gets a box round it and a line to it from the bottom of the screen,
// drawn over the finished frame with the view the scene was drawn with. AIMBOT: while the player aims in the view
// turns to the body of the living enemy nearest the crosshair
#include <chrono>
#include <cmath>
#include <cstring>

#include "common/options.h"
#include "game/layout.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;
using namespace layout;

namespace {

// character: kind 1 an enemy (the game's aim assist takes only those), its position the one its update makes a
// velocity of. The world is Y up: that update zeroes Y for the speed along the ground
constexpr uint32_t kCharKind = 0xC4;
constexpr uint32_t kCharPosition = 0xA0;
constexpr uint32_t kCharDying = 0x384;
constexpr uint32_t kEnemyKind = 1;

// the player's controller: yaw positive left, pitch positive down, in degrees (mouse.cpp)
constexpr uint32_t kControllerYaw = 0x18;
constexpr uint32_t kControllerPitch = 0x1C;
// the player's zoom of the aim, above 1 while it aims in (hooks.cpp's ZoomStep)
constexpr uint32_t kPlayerZoom = 0x2A4;

constexpr float kHeight = 1.8f;      // world units are metres: the near plane is 0.08
constexpr float kBoxWidth = 0.45f;   // of the box's height
constexpr float kNear = 0.08f;       // clip w nearer than this is behind the camera
constexpr float kThickness = 1.5f;   // lines of a 480 frame
constexpr float kChest = 1.2f;       // above the feet, where the aimbot aims: the body, a bigger hit than the head
constexpr float kCone = 30.0f;       // degrees from the crosshair an enemy is taken in
constexpr float kPull = 1.0f;        // of the turn to the target, a frame
constexpr float kDegrees = 180.0f / 3.14159265f;
constexpr float kColour[4] = {1.0f, 0.125f, 0.125f, 1.0f};  // rgba

// an enemy is drawn while its update ran this recently: none once the game pauses, no flicker between ticks
constexpr auto kSeen = std::chrono::milliseconds(250);
constexpr size_t kMaxEnemies = 64;

struct Seen {
    uint32_t who;
    std::chrono::steady_clock::time_point at;
};
Seen g_seen[kMaxEnemies];
size_t g_count;

// the scene's view and projection, row vectors: clip = position * view * projection
float g_view[16], g_projection[16];
bool g_camera;
uint32_t g_camera_frame;  // counts the scene's draws: the aimbot turns once for each view it reads

// the 2D layer's untextured triangles (eax triangles, ecx their points; float x, y of the points' offset, then a
// float rgba of each point, 16 aligned): in its batch, drawn with its shader and states at the next flush
constexpr uint32_t kTriangles = 0xD5A00;
struct Vertex {
    float x, y;
};
constexpr size_t kSegments = 5;  // the line and the box's four sides
constexpr size_t kMaxVertices = kMaxEnemies * kSegments * 6;

void Remember(uint32_t who) {
    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < g_count; i++)
        if (g_seen[i].who == who) {
            g_seen[i].at = now;
            return;
        }
    if (g_count < kMaxEnemies) {
        g_seen[g_count++] = {who, now};
        return;
    }
    // full with the ESP off, which keeps the list: the one seen longest ago goes
    Seen* oldest = g_seen;
    for (Seen& s : g_seen)
        if (s.at < oldest->at) oldest = &s;
    *oldest = {who, now};
}

// clip space; false behind the camera
bool Clip(const float p[3], float clip[4]) {
    float eye[4];
    for (int c = 0; c < 4; c++)
        eye[c] = p[0] * g_view[c] + p[1] * g_view[4 + c] + p[2] * g_view[8 + c] + g_view[12 + c];
    for (int c = 0; c < 4; c++)
        clip[c] = eye[0] * g_projection[c] + eye[1] * g_projection[4 + c] + eye[2] * g_projection[8 + c] +
                  eye[3] * g_projection[12 + c];
    return clip[3] > kNear;
}

// screen position in viewport pixels; false behind the camera
bool Project(const float p[3], float vx, float vy, float vw, float vh, float out[2]) {
    float clip[4];
    if (!Clip(p, clip)) return false;
    out[0] = vx + (clip[0] / clip[3] + 1) * 0.5f * vw;
    out[1] = vy + (1 - clip[1] / clip[3]) * 0.5f * vh;
    return true;
}

// a segment as a quad of two triangles, its width across it
Vertex* Segment(Vertex* v, float x0, float y0, float x1, float y1, float width) {
    float dx = x1 - x0, dy = y1 - y0;
    float length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 0)) return v;
    float nx = -dy / length * width * 0.5f, ny = dx / length * width * 0.5f;
    const float corner[4][2] = {{x0 + nx, y0 + ny}, {x0 - nx, y0 - ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}};
    for (int i : {0, 1, 2, 2, 1, 3}) *v++ = {corner[i][0], corner[i][1]};
    return v;
}

}  // namespace

namespace game {

// the device's view and projection as the scene draws them (D3DDevice_SetTransform, view 0 and projection 1)
void EspCamera() {
    uint32_t transforms = SYM(D3D_Device) + XDK_CDevice_Transforms;
    std::memcpy(g_view, HostPtr(transforms), sizeof(g_view));
    std::memcpy(g_projection, HostPtr(transforms + 64), sizeof(g_projection));
    g_camera = true;
    g_camera_frame++;
}

// esi the player's controller in its look update, before the game clamps the pitch and wraps the yaw; while the
// player aims in. The turn is read off the projection, the angles of the target from the middle of the view
void Aimbot(X86Context& ctx) {
    static uint32_t turned;
    uint32_t c = ctx.esi;
    uint32_t me = R32(kWorld) ? R32(kWorld) + kWorldPlayer : 0;
    if (!Opt().aimbot || !g_camera || !me || turned == g_camera_frame || !(G<float>(me + kPlayerZoom) > 1.01f)) return;
    turned = g_camera_frame;
    auto now = std::chrono::steady_clock::now();
    float best = kCone, yaw = 0, pitch = 0;
    for (size_t i = 0; i < g_count; i++) {
        uint32_t who = g_seen[i].who;
        if (now - g_seen[i].at > kSeen || R32(who + kCharKind) != kEnemyKind || R32(who + kCharDying)) continue;
        const float* at = &G<float>(who + kCharPosition);
        float chest[3] = {at[0], at[1] + kChest, at[2]}, clip[4];
        if (!Clip(chest, clip)) continue;
        float y = -std::atan(clip[0] / clip[3] / g_projection[0]) * kDegrees;
        float p = -std::atan(clip[1] / clip[3] / g_projection[5]) * kDegrees;
        float off = std::sqrt(y * y + p * p);
        if (off < best) best = off, yaw = y, pitch = p;
    }
    G<float>(c + kControllerYaw) += yaw * kPull;
    G<float>(c + kControllerPitch) += pitch * kPull;
}

// D3DVIEWPORT8: x, y, width, height. Called between the 2D layer's begin and end
void DrawEsp(X86Context& ctx, float unit) {
    if (!Opt().esp || !g_camera || !R32(kWorld)) return;
    uint32_t viewport = SYM(D3D_Device) + XDK_CDevice_Viewport;
    float vx = float(R32(viewport)), vy = float(R32(viewport + 4));
    float vw = float(R32(viewport + 8)), vh = float(R32(viewport + 12));
    static const uint32_t buffer = mem::AllocSystem(kMaxVertices * sizeof(Vertex), 16);
    static const uint32_t offset = mem::AllocSystem(8, 16);
    static const uint32_t colours = [] {
        uint32_t c = mem::AllocSystem(kMaxVertices * sizeof(kColour), 16);
        for (size_t i = 0; i < kMaxVertices; i++) std::memcpy(HostPtr(c + i * sizeof(kColour)), kColour, sizeof(kColour));
        return c;
    }();
    Vertex* first = static_cast<Vertex*>(HostPtr(buffer));
    Vertex* v = first;
    float width = kThickness * unit;
    auto now = std::chrono::steady_clock::now();
    size_t kept = 0;
    for (size_t i = 0; i < g_count; i++) {
        uint32_t who = g_seen[i].who;
        if (now - g_seen[i].at > kSeen || R32(who + kCharKind) != kEnemyKind || R32(who + kCharDying)) continue;
        g_seen[kept++] = g_seen[i];
        const float* at = &G<float>(who + kCharPosition);
        float feet[3] = {at[0], at[1], at[2]};
        float head[3] = {at[0], at[1] + kHeight, at[2]};
        float f[2], h[2];
        if (!Project(feet, vx, vy, vw, vh, f) || !Project(head, vx, vy, vw, vh, h)) continue;
        float half = (f[1] - h[1]) * kBoxWidth * 0.5f;
        float left = f[0] - half, right = f[0] + half;
        v = Segment(v, vx + vw * 0.5f, vy + vh, f[0], f[1], width);
        v = Segment(v, left, h[1], right, h[1], width);
        v = Segment(v, left, f[1], right, f[1], width);
        v = Segment(v, left, h[1], left, f[1], width);
        v = Segment(v, right, h[1], right, f[1], width);
    }
    g_count = kept;
    if (v == first) return;
    W32(offset, 0);
    W32(offset + 4, 0);
    uint32_t eax = ctx.eax, ecx = ctx.ecx;
    ctx.eax = uint32_t(v - first) / 3;
    ctx.ecx = buffer;
    CallGuest(ctx, kTriangles, {offset, colours});
    ctx.eax = eax;
    ctx.ecx = ecx;
}

}  // namespace game

// The characters' update, a frame (ecx the character, its class's own; the player's is 0x1E5FA0): the enemies
// that ran it are the ones drawn
X86_EXTERN_FUNC(__imp__sub_001DFD90);
X86_FUNC(sub_001DFD90) {
    uint32_t who = ctx.ecx;
    if ((game::Opt().esp || game::Opt().aimbot) && who && R32(who + kCharKind) == kEnemyKind && !R32(who + kCharDying)) Remember(who);
    __imp__sub_001DFD90(ctx, base);
}
