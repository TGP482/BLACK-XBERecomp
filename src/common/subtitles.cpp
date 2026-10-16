#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/options.h"
#include "common/subtitles.h"
#include "core/log.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

namespace {
struct Cue { int64_t start, end; std::string text; };
std::mutex g_lock;
std::string g_name;
std::vector<Cue> g_cues;
std::chrono::steady_clock::time_point g_started;
std::chrono::steady_clock::time_point g_last_load;

int64_t Timestamp(const std::string& s) {
    int h = 0, m = 0, sec = 0, ms = 0;
    if (std::sscanf(s.c_str(), "%d:%d:%d,%d", &h, &m, &sec, &ms) != 4) return -1;
    return ((int64_t(h) * 60 + m) * 60 + sec) * 1000 + ms;
}

std::vector<Cue> ReadSrt(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<Cue> cues;
    std::string line;
    auto read_line = [&]() {
        if (!std::getline(file, line)) return false;
        // SRT packages may use CRLF. getline removes '\n' but leaves '\r',
        // which would make blank cue separators look like subtitle text.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };
    while (read_line()) {
        auto arrow = line.find(" --> ");
        if (arrow == std::string::npos) continue;
        Cue cue{Timestamp(line.substr(0, arrow)), Timestamp(line.substr(arrow + 5)), {}};
        if (cue.start < 0 || cue.end <= cue.start) continue;
        while (read_line() && !line.empty()) {
            if (!cue.text.empty()) cue.text += '\n';
            cue.text += line;
        }
        if (!cue.text.empty()) cues.push_back(std::move(cue));
    }
    return cues;
}

std::string Locale() {
    const auto& language = game::BootLanguage();
    if (language == "esp") return "es";
    if (language == "fra") return "fr";
    if (language == "deu") return "de";
    if (language == "ita") return "it";
    return language == "int" ? "en-US" : language;
}
}

std::string app::CurrentSubtitle() {
    if (!game::Opt().subtitles_enabled) return {};
    uint32_t game = R32(layout::kGame);
    if (!game) return {};
    uint32_t film = R32(game + kCurrentFilm);
    uint32_t player = game + kFrontEnd + kFilmPlayer;
    constexpr uint32_t kFilmName = 0x84, kFilmState = 0x1A8, kFilmPlaying = 0x1C;
    uint32_t state = R32(player + kFilmState);
    if (film >= kFilms || state < 4 || state > kFilmPlaying) {
        std::lock_guard guard(g_lock);
        g_name.clear();
        return {};
    }
    std::string name = ReadCString(player + kFilmName, 8);
    if (name.size() < 4) return {};
    std::string stem = name.substr(0, 4);
    if (stem.back() == '.') stem.pop_back();
    const std::string identity = Locale() + "/" + std::to_string(film) + "/" + stem;
    std::lock_guard guard(g_lock);
    if (identity != g_name) {
        g_name = identity;
        g_cues.clear();
        g_started = std::chrono::steady_clock::now();
        g_last_load = {};
    }
    const auto now = std::chrono::steady_clock::now();
    if (g_cues.empty() && (g_last_load.time_since_epoch().count() == 0 || now - g_last_load >= std::chrono::seconds(1))) {
        g_last_load = now;
        auto path = app::ExeDir() / "language" / Locale() / "Subtitles" / (stem + ".srt");
        g_cues = ReadSrt(path);
        if (g_cues.empty() && Locale() != "en-US") {
            path = app::ExeDir() / "language" / "en-US" / "Subtitles" / (stem + ".srt");
            g_cues = ReadSrt(path);
        }
        if (!g_cues.empty()) LOGR("subtitles: loaded %zu cues for %s (%s)", g_cues.size(), stem.c_str(), Locale().c_str());
    }
    int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_started).count();
    auto cue = std::find_if(g_cues.begin(), g_cues.end(), [elapsed](const Cue& c) { return elapsed >= c.start && elapsed <= c.end; });
    return cue == g_cues.end() ? std::string() : cue->text;
}
