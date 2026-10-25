// The game's language. This build sets English (US) at boot whatever the console's language is; the LANGUAGE setting
// takes its place when the game files have that language's strings, else the game stays in English. A Europe disc
// has UK English strings in place of the US ones, which its own build picks for English.
#include <string>
#include <vector>

#include "common/options.h"
#include "core/log.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// the language's index into the suffix table at 0x288318 (UK US Ja Ge Fr Sp It ...)
constexpr uint32_t kLanguage = 0x2D0D78;

struct Language {
    const char* code;    // game::kLanguages
    uint32_t index;
    const char* suffix;  // Main%s.bin
};
constexpr Language kLanguages[] = {{"fra", 4, "Fr"}, {"deu", 3, "Ge"}, {"ita", 6, "It"}, {"esp", 5, "Sp"}};
constexpr Language kUs = {"int", 1, "US"};
constexpr Language kUk = {"int", 0, "UK"};

std::string StringsFile(const Language& l) { return std::string("D:\\Language\\Strings\\Main") + l.suffix + ".bin"; }

bool OnDisc(const Language& l) {
    std::vector<uint8_t> file;
    return ReadGameFile(StringsFile(l), file);
}

// the index to boot in
uint32_t Chosen() {
    for (const Language& l : kLanguages) {
        if (game::BootLanguage() != l.code) continue;
        if (OnDisc(l)) {
            LOGI("language: %s", l.suffix);
            return l.index;
        }
        LOGW("language: no %s, the game stays in English", StringsFile(l).c_str());
    }
    bool uk_only = OnDisc(kUk) && !OnDisc(kUs);
    if (!uk_only) return kUs.index;
    LOGI("language: %s", kUk.suffix);
    return kUk.index;
}

uint32_t Index() {
    static const uint32_t index = Chosen();
    return index;
}

}  // namespace

// after the boot's own language store
void GameLanguage(X86Context&, uint8_t*) {
    W32(kLanguage, Index());
}

// Queues a speech line (eax the sound manager, edx the line). In the first level the US build only lets English lines
// play; the Europe disc has that level's speech in every language
X86_EXTERN_FUNC(__imp__sub_001407A0);
X86_FUNC(sub_001407A0) {
    uint32_t language = R32(kLanguage);
    W32(kLanguage, kUs.index);
    __imp__sub_001407A0(ctx, base);
    W32(kLanguage, language);
}

namespace game {

// the disc has a language besides English: the US release has none, so no LANGUAGE row and no translations
bool HasLanguages() {
    static const bool any = [] {
        for (const Language& l : ::kLanguages)
            if (OnDisc(l)) return true;
        return false;
    }();
    return any;
}

// prompts.cpp: the game shows the LANGUAGE setting's language, not English in its place
bool Translated() {
    uint32_t index = Index();
    return index != kUs.index && index != kUk.index;
}

}  // namespace game
