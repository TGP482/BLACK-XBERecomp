// pause on focus loss: the game thread waits while the window is in the background or minimized
#include <chrono>
#include <thread>

#include "app/app.h"
#include "audio/output.h"
#include "common/options.h"
#include "common/platform.h"
#include "core/log.h"

namespace game {

namespace {

constexpr int kPollMs = 20;

}  // namespace

void WaitWhileUnfocused() {
    if (!Opt().pause_on_focus_loss || !platform::WindowUnfocused()) return;
    LOGI("focus lost: paused");
    g_focus_paused = true;
    audio::SetPaused(true);
    while (Opt().pause_on_focus_loss && platform::WindowUnfocused())
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    audio::SetPaused(false);
    g_focus_pauses++;
    g_focus_paused = false;
    LOGI("focus back: resumed");
}

}  // namespace game
