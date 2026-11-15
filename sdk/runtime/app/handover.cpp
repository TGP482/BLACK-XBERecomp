// A title may relaunch itself between the front end and a level (a quick reboot on the Xbox, a new process here).
// The old instance keeps its window up in kernel::Relaunch until this one has presented its first frames, so the
// kept picture stays on screen through the switch. Here the window is created hidden and opens at that point.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace app {

namespace {
HANDLE g_ready = nullptr;
std::atomic<bool> g_handing{false};
}  // namespace

bool StartHandover() {
    g_ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, kernel::HandoverEvent(GetCurrentProcessId()).c_str());
    g_handing = g_ready != nullptr;
    if (g_handing) LOGI("handover: the previous instance waits for the first frames");
    return g_handing;
}

bool HandingOver() { return g_handing; }

void FinishHandover() {
    if (!g_handing.exchange(false)) return;
    OpenGameWindow(false);
    SetEvent(g_ready);
    CloseHandle(g_ready);
}

}  // namespace app
