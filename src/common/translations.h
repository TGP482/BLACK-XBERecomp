#pragma once
#include <cstdint>
#include <string>
#include <string_view>
// Text the recomp adds to the panel and the game's options menu, in the language the game booted in (translations.cpp)

namespace app {

// the text as UTF-8 with each phrase that has a translation (the longest from each word start) in that language, in
// capitals for caps; in English the text as it is. Phrases are the English text in any case.
std::string Ui(std::string_view text, bool caps = true);
void SetRussianFontAvailable(bool available);
// Original game menu catalog, keyed by the hexadecimal string IDs used by sub_001F3A80.
std::string GameMenuText(uint32_t id);

}  // namespace app
