#include <SDL3/SDL_main.h>
// The Linux host's program: the log, the settings and the game found, the SDL3 window (window_sdl.cpp) opened, the
// title booted, then the window's events until it closes. main.cpp is the Windows one.
#include <SDL3/SDL.h>

#include <algorithm>
#include <csignal>
#include <string>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <windows.h>  // the Linux compat header: virtual key numbers

#include "app/app.h"
#include "app/window_sdl.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace {

void DumpThreads() {
    gpu::g_capture = true;
    for (auto& t : kernel::Threads())
        if (t->ctx) {
            LOGI("thread %X", t->id);
            kernel::LogGuestStack(*t->ctx, 32);
        }
}

// a guest thread that faults or aborts: its guest stack to the log before the process goes
void OnCrash(int sig) {
    if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
        LOGE("signal %d on guest thread %X", sig, t->id);
        kernel::LogGuestStack(*t->ctx, 256);
    } else {
        LOGE("signal %d", sig);
    }
    logx::Flush();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

}  // namespace

int main(int argc, char** argv) {
    app::SetArguments(argc, argv);
    // SIGSEGV too: a guest that walks off its address space is the common bring-up crash, and without
    // this it dies with no guest stack at all. OnCrash re-raises with the default handler, so the core
    // dump is unchanged.
    for (int sig : {SIGABRT, SIGFPE, SIGILL, SIGBUS, SIGSEGV}) std::signal(sig, OnCrash);
    std::vector<std::string> args(argv, argv + argc);
    bool relaunched = std::find(args.begin(), args.end(), "--relaunched") != args.end();
    std::string log = app::kGame.log;
    if (app::Instance() > 1) log += "-" + std::to_string(app::Instance());
    logx::Init((app::ExeDir() / "logs").string(), log, relaunched);
    app::Load();
    if (app::kGame.init) app::kGame.init();

    // video first: input::Init sees the window owns SDL's events and leaves them to this thread
    SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_PREFER_LIBDECOR, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        LOGE("SDL video failed to start: %s", SDL_GetError());
        return 1;
    }
    if (relaunched && !logx::kVerbose) LOGR("Relaunched: %s", IMAGE_XBE);
    else app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;

    std::string title = app::Utf8(app::kGame.title);
    if (app::Instance()) title += " [" + std::to_string(app::Instance()) + "]";
    app::SetUnhandledKey([](int vk) {
        if (vk == VK_F3) DumpThreads();
    });
    if (!app::OpenWindow(title)) return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
#ifdef __ANDROID__
    // Rasterize a larger font instead of stretching the default 13px atlas.
    ImFontConfig font;
    font.SizePixels = 26.0f;
    io.Fonts->AddFontDefault(&font);
    ImGui::GetStyle().ScaleAllSizes(2.0f);
#endif
    // the game sets the window's cursor (WM_SETCURSOR): the panel leaves it alone
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplSDL3_InitForVulkan(static_cast<SDL_Window*>(app::Window()));

    input::Init();
    if (app::kGame.window) app::kGame.window(app::Window());
    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / hdd).string())) {
        app::ShowError(L"The game failed to start, see the log in the logs folder.");
        return 1;
    }
    while (app::PumpEvents(100)) {
    }
    logx::Flush();
    kernel::Shutdown(0);
}
