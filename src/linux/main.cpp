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

int main(int argc, char** argv) {
    // <exe> [--instance N]; the game files are found beside the exe or in a folder above it, as on Windows
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--instance") && i + 1 < argc) setenv("INSTANCE", argv[++i], 1);
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    app::Load();
    app::LogSystem();
    app::LogSettings();
    if (app::kGame.init) app::kGame.init();
    if (app::kGame.window) app::kGame.window(app::Window());

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;  // FindGame told the player what is missing
    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / hdd).string())) return 1;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        logx::Flush();
    }
}
