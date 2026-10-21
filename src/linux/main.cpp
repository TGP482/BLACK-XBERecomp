// the Linux entry point: the settings, the game window's host, the boot and the run
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "linux/window.h"

int main(int argc, char** argv) {
    // <exe> [--instance N]; the game files are found beside the exe or in a folder above it, as on Windows
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--instance") && i + 1 < argc) setenv("INSTANCE", argv[++i], 1);
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    app::Load();
    app::LogSystem();
    app::LogSettings();
    if (app::kGame.init) app::kGame.init();

    // the game's window: the output resolution, fullscreen or borderless as the settings say
    const app::Settings& s = app::Get();
    app::Mode mode = s.display_mode == app::DisplayMode::Borderless ? app::Desktop() : app::Output();
    if (!window::Open(app::kGame.title, mode.width, mode.height, s.display_mode == app::DisplayMode::Fullscreen,
                      s.display_mode == app::DisplayMode::Borderless))
        return 1;
    if (app::kGame.window) app::kGame.window(app::Window());

    app::GameFiles game;
    if (!app::FindGame(game)) {  // FindGame told the player what is missing
        window::Close();
        return 1;
    }
    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / hdd).string())) {
        window::Close();
        return 1;
    }
    // the guest renders on its own threads: the window thread shows the frames and pumps the events
    // SHOT=<seconds>: the window's picture as a BMP that many seconds in, for the milestone's check
    const char* shot = std::getenv("SHOT");
    int shot_at = shot ? std::atoi(shot) : 0;
    auto started = std::chrono::steady_clock::now();
    for (uint64_t frame = 0;; frame++) {
        if (!window::Pump()) break;  // the player closed the window
        window::Show();
        if (shot_at && window::Shown() &&
            std::chrono::steady_clock::now() - started > std::chrono::seconds(shot_at)) {
            window::SaveScreenshot("shot.bmp");
            LOGI("window: wrote shot.bmp");
            shot_at = 0;
        }
        if (frame % 500 == 0) logx::Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    window::Close();
    return 0;
}
