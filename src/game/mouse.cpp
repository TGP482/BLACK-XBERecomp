// The mouse on the game side.
// Play: raw counts turn the view in the player's look update.
// Menus: Flash pages whose script keeps the focused button, each button's neighbours and takes one input a tick.
// The cursor steps the focus to the button under it, a click confirms, the wheel steps up and down. In a row of
// options a click picks the value or arrow under it, the left button held on a slider drags it.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/gyro.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/log.h"
#include "game/flash.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;
using flash::Find;
using flash::IsClip;
using flash::Member;
using flash::Members;
using flash::Type;

// degrees a count at sensitivity 1
constexpr float kDegrees = 0.05f;
constexpr float kHalfRadians = 3.14159265f / 360.0f;  // degrees to radians of half the angle

// the player's controller: yaw positive left, pitch positive down, in degrees
constexpr uint32_t kControllerYaw = 0x18;
constexpr uint32_t kControllerPitch = 0x1C;
constexpr uint32_t kControllerAimWanted = 0x45;
constexpr uint32_t kControllerAimHeld = 0x104;
constexpr uint32_t kControllerInvertY = 0x101;  // the game's invert look, for the stick
// degrees a second at full stick, and the power the stick is raised to
constexpr uint32_t kControllerYawSpeed = 0xB8;
constexpr uint32_t kControllerPitchSpeed = 0xBC;
constexpr uint32_t kControllerCurve = 0xC8;
// the right stick this frame, turn left and turn up; the gun's sway reads them as player+0x5C8 and +0x5CC
constexpr uint32_t kControllerStickYaw = 0xD8;
constexpr uint32_t kControllerStickPitch = 0xDC;

// camera: the view's degrees, as FieldOfView leaves them
constexpr uint32_t kCamera = 0x2D12B8;
constexpr uint32_t kCameraFov = 0x750;

// the view's tangent over that of the view from the hip: a count turns the same share of the view zoomed or not
float ViewZoom() {
    uint32_t camera = R32(kCamera);
    float fov = camera ? G<float>(camera + kCameraFov) : 0.0f;
    if (!(fov > 0 && fov < 180)) return 1.0f;
    float hip = float(std::clamp(game::Opt().fov, 45u, 140u));  // the range FieldOfView takes
    return std::tan(fov * kHalfRadians) / std::tan(hip * kHalfRadians);
}

// the stick deflection that turns as far through the game's curve, full the turn of a full stick
float Deflection(float degrees, float full, float curve) {
    if (!(full > 0)) return 0.0f;
    float x = std::min(std::fabs(degrees) / full, 1.0f);
    if (curve > 0) x = std::pow(x, 1.0f / curve);
    return std::copysign(x, degrees);
}

// xmm2 the stick's zoom scale, under 1 while the view is zoomed in: aiming or a scope
bool Zoomed(const X86Context& ctx) { return ctx.xmm[2].f[0] < 1.0f; }

// The gun sways with the right stick: while it rests, the stick that turns the view as fast as the mouse did
void UpdateSway(X86Context& ctx, float yaw, float pitch) {
    uint32_t c = ctx.esi;
    if (G<float>(c + kControllerStickYaw) != 0 || G<float>(c + kControllerStickPitch) != 0) return;
    float frame = ctx.xmm[2].f[0] * G<float>(ctx.edi + kTimerDt);  // edi the world, xmm2 the stick's zoom scale
    float curve = G<float>(c + kControllerCurve);
    G<float>(c + kControllerStickYaw) = Deflection(yaw, frame * G<float>(c + kControllerYawSpeed), curve);
    G<float>(c + kControllerStickPitch) = Deflection(-pitch, frame * G<float>(c + kControllerPitchSpeed), curve);
}

// esi the player's controller. The game adds the stick, clamps the pitch and wraps the yaw after this.
void MouseLook(X86Context& ctx, uint8_t*) {
    float yaw = 0.0f;
    float pitch = 0.0f;
    float dx;
    float dy;
    if (input::TakeMouse(dx, dy)) {
        const game::Options& o = game::Opt();
        bool zoomed = Zoomed(ctx);
        yaw -= dx * kDegrees * (zoomed ? o.aim_horizontal_sensitivity : 1.0f);
        pitch += dy * kDegrees * (zoomed ? o.aim_vertical_sensitivity : 1.0f);
    }
    gyro::Turn turn = gyro::Take(game::Opt().gyro);
    if (!app::MenuOpen()) {
        bool invert = G<uint8_t>(ctx.esi + kControllerInvertY);
        yaw += turn.yaw;
        pitch += invert ? -turn.pitch : turn.pitch;
        if (turn.recenter) G<float>(ctx.esi + kControllerPitch) = 0.0f;  // level
    }
    if (yaw == 0.0f && pitch == 0.0f) return;
    float zoom = ViewZoom();
    yaw = std::remainder(yaw * zoom, 360.0f);
    pitch *= zoom;
    G<float>(ctx.esi + kControllerYaw) += yaw;
    G<float>(ctx.esi + kControllerPitch) += pitch;
    UpdateSway(ctx, yaw, pitch);
}

// false with the keyboard or mouse used last or the option off: no pull of the view to a target, no shots bent to one
bool AimAssist(X86Context&, uint8_t*) { return game::Opt().aim_assist && !input::KeyboardActive(); }

// the look update: xmm3 the stick's yaw speed, xmm0 its pitch speed, before the zoom scale and frame time
void StickLook(X86Context& ctx, uint8_t*) {
    if (input::KeyboardActive()) return;
    const game::Options& o = game::Opt();
    bool zoomed = Zoomed(ctx);
    ctx.xmm[3].f[0] *= o.controller_sensitivity * (zoomed ? o.controller_aim_horizontal_sensitivity : 1.0f);
    ctx.xmm[0].f[0] *= o.controller_vertical_sensitivity * (zoomed ? o.controller_aim_vertical_sensitivity : 1.0f);
}

// the game drops the aim of a weapon whose zoom is 1 each frame (0x1BB143)
static bool CanAim(uint32_t weapon) {
    uint32_t data = R32(weapon + kWeaponData);
    return data && G<float>(data + kDataAimZoom) != 1.0f;
}

constexpr uint32_t kHoldAimWeapon = 0x18;  // esp offset of the weapon at the hook

// ebx the player's controller past the game's toggle aim, scopes and all.
// A toggle on a weapon that cannot aim would stay on and aim the next weapon.
void HoldAim(X86Context& ctx, uint8_t*) {
    bool hold = input::KeyboardActive() ? game::Opt().hold_aim : game::Opt().controller_hold_aim;
    if (!hold && CanAim(R32(ctx.esp + kHoldAimWeapon))) return;
    G<uint8_t>(ctx.ebx + kControllerAimWanted) = G<uint8_t>(ctx.ebx + kControllerAimHeld);
}

namespace game {
bool OptionsCapturing();  // options_menu.cpp
}

namespace {

constexpr uint32_t kBounds = 0xE9310;  // eax = float box[4] out, ecx = clip: its box in its parent
constexpr float kPad = 6.0f;  // around a value's or an arrow's clip, some are a few pixels across
constexpr size_t kMaxNodes = 64;
constexpr const char* kButtonLinks[] = {"buttonUp", "buttonDown", "buttonLeft", "buttonRight"};
constexpr const char* kValueLinks[] = {"upPoint", "downPoint", "leftPoint", "rightPoint"};
constexpr const char* kFilmPages[] = {"FMVPlayer", "BootSequence", "IntroCredits", "BlackIntroSequence"};

float Number(uint32_t value) {
    if (Type(value) == flash::kInteger) return float(int32_t(R32(value + flash::kValueNumber)));
    if (Type(value) == flash::kFloat) return G<float>(value + flash::kValueNumber);
    return 0.0f;
}

// the page while it takes input
uint32_t Page(uint32_t ui) {
    bool input = InputTaken(ui) && !R32(ui + kPageLeaving);
    return input ? flash::Level(flash::kMenuLevel) : 0;
}

// a clip's box on the stage; false when hidden or empty
bool Box(X86Context& ctx, uint32_t clip, float box[4]) {
    static const uint32_t out = mem::AllocSystem(16);
    ctx.eax = out;
    ctx.ecx = clip;
    CallGuest(ctx, kBounds, {});
    float r[4];
    std::memcpy(r, HostPtr(out), sizeof(r));
    if (r[2] < r[0] || r[3] < r[1]) return false;
    float x[4] = {r[0], r[2], r[0], r[2]};
    float y[4] = {r[1], r[1], r[3], r[3]};
    bool shown = R32(clip + flash::kClipFlags) & flash::kClipShown;
    int depth = 0;
    for (uint32_t parent = R32(clip + flash::kClipParent); parent && depth < 32; depth++) {
        float m[6];
        std::memcpy(m, HostPtr(parent + flash::kClipMatrix), sizeof(m));
        for (int i = 0; i < 4; i++) {
            float px = x[i];
            x[i] = m[0] * px + m[2] * y[i] + m[4];
            y[i] = m[1] * px + m[3] * y[i] + m[5];
        }
        shown = shown && (R32(parent + flash::kClipFlags) & flash::kClipShown);
        parent = R32(parent + flash::kClipParent);
    }
    box[0] = *std::min_element(x, x + 4);
    box[1] = *std::min_element(y, y + 4);
    box[2] = *std::max_element(x, x + 4);
    box[3] = *std::max_element(y, y + 4);
    return shown;
}

bool Over(X86Context& ctx, uint32_t clip, float x, float y, float pad = 0.0f, float* across = nullptr) {
    float box[4];
    if (!IsClip(clip) || !Box(ctx, clip, box)) return false;
    if (across) *across = (x - box[0]) / std::max(box[2] - box[0], 1.0f);
    return x >= box[0] - pad && x <= box[2] + pad && y >= box[1] - pad && y <= box[3] + pad;
}

// a button or value that can be reached from the first and the first input on the way there
struct Node {
    uint32_t object;
    uint32_t members;
    uint32_t step;
};

bool Listed(const Node* list, size_t count, uint32_t object) {
    return std::any_of(list, list + count, [object](const Node& n) { return n.object == object; });
}

size_t Reach(X86Context& ctx, uint32_t start, const char* const* links, Node* list) {
    size_t count = 0;
    list[count++] = {start, Members(ctx, start), kNoInput};
    for (size_t i = 0; i < count; i++) {
        for (uint32_t d = 0; d < 4; d++) {
            uint32_t next = Find(list[i].members, links[d]);
            if (!next || count == kMaxNodes || Listed(list, count, next)) continue;
            list[count++] = {next, Members(ctx, next), i ? list[i].step : kInputUp + d};
        }
    }
    return count;
}

// the value of a row of values (on, off; the map's levels) under the cursor
uint32_t ValueAt(X86Context& ctx, uint32_t values, float x, float y) {
    uint32_t active = Member(ctx, values, "mActive");
    if (!active) return 0;
    Node nodes[kMaxNodes];
    size_t count = Reach(ctx, active, kValueLinks, nodes);
    for (size_t i = 0; i < count; i++)
        if (Over(ctx, Find(nodes[i].members, "movieClip"), x, y, kPad)) return nodes[i].object;
    return 0;
}

// the cursor on the stage this tick
struct Pointer {
    float x = 0;
    float y = 0;
    bool down = false;
    bool click = false;
    bool moved = false;
};

// what the cursor is on: the button to focus, then the value to pick in its row or the input to give it
struct Aim {
    uint32_t button = 0;
    uint32_t values = 0;
    uint32_t value = 0;
    uint32_t then = kNoInput;
};

Aim Point(X86Context& ctx, const Node* buttons, size_t count, uint32_t map, const Pointer& p) {
    float x = p.x;
    float y = p.y;
    bool click = p.click;
    bool clips = false;
    for (size_t i = 0; i < count; i++) {
        uint32_t button = buttons[i].object;
        uint32_t clip = Find(buttons[i].members, "movieClip");
        uint32_t options = Find(buttons[i].members, "mChoiceList");
        uint32_t members = Members(ctx, options);
        clips |= IsClip(clip);
        if (uint32_t value = ValueAt(ctx, options, x, y)) return {button, options, click ? value : 0};
        if (Over(ctx, Find(members, "mLeftAnimation"), x, y, kPad)) {
            return {button, 0, 0, click ? kInputLeft : kNoInput};
        }
        if (Over(ctx, Find(members, "mRightAnimation"), x, y, kPad)) {
            return {button, 0, 0, click ? kInputRight : kNoInput};
        }
        if (Over(ctx, Find(members, "mMovieClip"), x, y)) return {button};
        // a row of options has no action of its own, confirm would back out of the page
        if (Over(ctx, clip, x, y)) return {button, 0, 0, click && !options ? kInputConfirm : kNoInput};
    }
    if (uint32_t value = click ? ValueAt(ctx, map, x, y) : 0) return {buttons[0].object, map, value};
    // a page of one button with nothing to point at takes a click anywhere
    if (click && !clips) return {buttons[0].object, 0, 0, kInputConfirm};
    return {};
}

// the left button held on the focused row's slider: a step toward the cursor
uint32_t Slide(X86Context& ctx, uint32_t row, float x, float y) {
    uint32_t slider = Members(ctx, Find(row, "mChoiceList"));
    uint32_t value = Find(slider, "mValue");
    float across;
    float step = Number(Find(slider, "mChange"));
    if (!value || !(step > 0) || !Over(ctx, Find(slider, "mMovieClip"), x, y, 0.0f, &across)) return kNoInput;
    float off = across * 100.0f - Number(value);
    if (off >= step / 2) return kInputRight;
    if (off <= -step / 2) return kInputLeft;
    return kNoInput;
}

Aim g_aim;
uint32_t g_logged_page = 0;  // the menu the last log line was for

// the step from the aimed value's row's active value to the aimed value, none when it is not in reach
uint32_t ValueStep(X86Context& ctx) {
    Node values[kMaxNodes];
    uint32_t active = Member(ctx, g_aim.values, "mActive");
    size_t n = active ? Reach(ctx, active, kValueLinks, values) : 0;
    const Node* value = std::find_if(values, values + n, [](const Node& v) { return v.object == g_aim.value; });
    return value != values + n ? value->step : kNoInput;
}

// the input toward the aim: a step of the focus to its button, then its value or input; an aim given or lost ends
uint32_t ApplyAim(X86Context& ctx, const Node* buttons, size_t count, uint32_t focus) {
    const Node* at = std::find_if(buttons, buttons + count, [](const Node& n) { return n.object == g_aim.button; });
    if (!g_aim.button || at == buttons + count) {
        g_aim = {};
        return kNoInput;
    }
    if (g_aim.button != focus) return at->step;
    if (g_aim.value) {
        uint32_t input = ValueStep(ctx);
        if (!input) g_aim = {};
        return input;
    }
    uint32_t input = g_aim.then;
    g_aim = {};
    return input;
}

// a page's script objects: the page, its MyMenu and the focused button
struct MenuPage {
    const char* name;
    uint32_t root;
    uint32_t list;
    uint32_t focus;
};

// the input the cursor gives the page this tick
uint32_t CursorInput(X86Context& ctx, const MenuPage& page, const Pointer& p) {
    Node buttons[kMaxNodes];
    size_t count = Reach(ctx, page.focus, kButtonLinks, buttons);
    if (page.list != g_logged_page) LOGI("menu cursor: page %s, %zu buttons", page.name, count);
    g_logged_page = page.list;
    // a click's aim holds while the cursor stays on its button
    if (p.moved || p.click) {
        Aim over = Point(ctx, buttons, count, Member(ctx, page.root, "MyMap"), p);
        if (p.click || over.button != g_aim.button) g_aim = over;
    }
    uint32_t input = ApplyAim(ctx, buttons, count, page.focus);
    if (!input && p.down) input = Slide(ctx, buttons[0].members, p.x, p.y);
    return input;
}

bool IsMenuPage(const char* name) {
    auto film = [name](const char* page) { return _strnicmp(name, page, std::strlen(page)) == 0; };
    return *name && std::none_of(std::begin(kFilmPages), std::end(kFilmPages), film);
}

// the cursor from the output onto the 640 stage; false until the page has a scale
bool UpdateStagePosition(uint32_t ui, Pointer& p) {
    uint32_t display = R32(kDisplay);
    float sx = G<float>(ui + kStageScaleX);
    float sy = G<float>(ui + kStageScaleY);
    if (!display || !(sx > 0) || !(sy > 0)) return false;
    float inset = G<uint8_t>(display + kDisplayWidescreen) ? G<float>(kStageX) : 0.0f;
    p.x = p.x * float(int32_t(R32(display + kDisplayWidth))) / sx - inset;
    p.y = p.y * float(int32_t(R32(display + kDisplayHeight))) / sy;
    return true;
}

void Menu(X86Context& ctx, uint32_t ui) {
    static bool held;
    static float seen_x;
    static float seen_y;
    const char* name = static_cast<const char*>(HostPtr(ui + kPageName));
    bool menu = IsMenuPage(name);
    input::SetMenu(menu);
    int wheel = input::TakeWheel();
    Pointer p;
    bool pointer = menu && !game::OptionsCapturing() && input::Cursor(p.x, p.y, p.down);
    p.click = p.down && !held;
    held = p.down;
    // waiting for a player's Start the game takes the pad that pressed: a click presses A on it
    if (pointer && R32(ui + kWaitsForStart)) {
        if (p.click) input::TapConfirm();
        g_aim = {};
        return;
    }
    uint32_t root = pointer ? Page(ui) : 0;
    uint32_t list = Member(ctx, root, "MyMenu");
    uint32_t focus = Member(ctx, list, "mFocusButton");
    if (!focus || !UpdateStagePosition(ui, p)) {
        g_aim = {};
        return;
    }
    p.moved = p.x != seen_x || p.y != seen_y;
    seen_x = p.x;
    seen_y = p.y;
    // the pad or the keys act this tick
    if (R32(ui + kMenuInput)) {
        g_aim = {};
        return;
    }
    uint32_t input = kNoInput;
    if (p.moved || p.down || g_aim.button) input = CursorInput(ctx, {name, root, list, focus}, p);
    if (!input && wheel) input = wheel > 0 ? kInputUp : kInputDown;
    if (input) W32(ui + kMenuInput, input);
}

}  // namespace

// ebp the front end, after it took the pad's menu input and before the Flash player runs the page's script
void MenuCursor(X86Context& ctx, uint8_t*) {
    X86Context saved = ctx;
    Menu(ctx, ctx.ebp);
    ctx = saved;
}
