// pause on focus loss: the game thread waits while the window is in the background or minimized
#include <chrono>
#include <thread>

#include "app/app.h"
#include "audio/output.h"
#include "common/options.h"
#include "core/log.h"

namespace game {

namespace {

constexpr auto kPoll = std::chrono::milliseconds(20);

// a hidden window, behind the splash or for a handover, never pauses: it opens from the game thread
bool Unfocused() { return app::Unfocused(); }

}  // namespace

void WaitWhileUnfocused() {
    if (!Opt().pause_on_focus_loss || !Unfocused()) return;
    LOGI("focus lost: paused");
    g_focus_paused = true;
    audio::SetPaused(true);
    while (Opt().pause_on_focus_loss && Unfocused()) std::this_thread::sleep_for(kPoll);
    audio::SetPaused(false);
    g_focus_pauses++;
    g_focus_paused = false;
    LOGI("focus back: resumed");
}

}  // namespace game
