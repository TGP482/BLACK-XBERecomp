// Native Linux application host using SDL3 and Vulkan
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <limits.h>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "common/mouse_keyboard.h"
#include "common/menu.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace app {

namespace {

SDL_Window* g_window = nullptr;
bool g_fullscreen = false;

void DumpThreads() {
    gpu::g_capture = true;
    for (auto& t : kernel::Threads()) {
        if (t->ctx) {
            LOGI("thread %X", t->id);
            kernel::LogGuestStack(*t->ctx, 32);
        }
    }
}

}  // namespace

void ApplyNow() {
    if (!g_window) return;
    const Settings& s = Get();
    Mode out = Output();

    if (s.display_mode == DisplayMode::Fullscreen) {
        SDL_SetWindowBordered(g_window, false);
        SDL_SetWindowFullscreen(g_window, true);
        g_fullscreen = true;
    } else if (s.display_mode == DisplayMode::Borderless) {
        SDL_SetWindowFullscreen(g_window, false);
        SDL_SetWindowBordered(g_window, false);
        SDL_SetWindowSize(g_window, int(out.width), int(out.height));
        SDL_SetWindowPosition(g_window, 0, 0);
        g_fullscreen = false;
    } else {
        SDL_SetWindowFullscreen(g_window, false);
        SDL_SetWindowBordered(g_window, true);
        SDL_SetWindowSize(g_window, int(out.width), int(out.height));
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        g_fullscreen = false;
    }
}

std::filesystem::path ExeDir() {
    char p[PATH_MAX] = {};
    ssize_t len = readlink("/proc/self/exe", p, sizeof(p) - 1);
    if (len > 0) {
        p[len] = '\0';
        return std::filesystem::path(p).parent_path();
    }
    return std::filesystem::current_path();
}

int Instance() {
    // Check command line arguments for --instance
    return 0;
}

void* Window() { return g_window; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() { ApplyNow(); }
void OpenGameWindow(bool apply) {
    if (apply) ApplyNow();
    if (g_window) SDL_ShowWindow(g_window);
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) SDL_Delay(1);
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

static void CrashHandler(int sig) {
    LOGE("Caught signal %d", sig);
    if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
        LOGE("Guest thread %X crashed", t->id);
        kernel::LogGuestStack(*t->ctx, 256);
    }
    logx::Flush();
    _exit(1);
}

int main(int argc, char** argv) {
    std::signal(SIGSEGV, CrashHandler);
    std::signal(SIGABRT, CrashHandler);
    std::signal(SIGFPE, CrashHandler);

    bool relaunched = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--relaunched") == 0) relaunched = true;
    }

    std::string log = app::kGame.log;
    logx::Init((app::ExeDir() / "logs").string(), log, relaunched);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (relaunched && !logx::kVerbose) LOGR("Relaunched: %s", IMAGE_XBE);
    else app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) {
        LOGE("Game files not found");
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        LOGE("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    app::Mode out = app::Output();
    uint32_t w = out.width ? out.width : 1280;
    uint32_t h = out.height ? out.height : 720;

    SDL_Window* window = SDL_CreateWindow(
        "Black - XBE Recomp",
        int(w), int(h),
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        LOGE("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }
    app::g_window = window;
    app::OpenGameWindow(true);
    SDL_RaiseWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL3_InitForVulkan(window);

    input::Init();
    if (app::kGame.window) app::kGame.window(window);

    std::string hdd = "hdd";
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / hdd).string())) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", "The game failed to start, see log in logs folder.", window);
        return 1;
    }

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            {
                std::lock_guard lock(app::UiLock());
                if (ImGui::GetCurrentContext()) {
                    ImGui_ImplSDL3_ProcessEvent(&event);
                }
            }
            if (event.type == SDL_EVENT_QUIT) {
                LOGI("main_linux: SDL_EVENT_QUIT received, exiting main loop");
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_F3) {
                    app::DumpThreads();
                } else if (event.key.key == SDLK_F10) {
                    app::ToggleMenu();
                } else if (event.key.key == SDLK_ESCAPE && app::MenuOpen() && !app::Capturing()) {
                    app::ToggleMenu();
                }
            }
            input::OnSdlEvent(&event);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    logx::Flush();
    kernel::Shutdown(0);
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
