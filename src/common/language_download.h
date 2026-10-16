#pragma once

#include <string>

namespace app {

// Queue the selected language for background download. Rapid selector changes are coalesced.
void RequestLanguageDownload(const std::string& language);
// Check the current GitHub manifest and update every locale's game assets beside the exe.
void RequestTranslationUpdate();
std::string TranslationUpdateStatus();
// Short status text for the language selector; empty when this language has not been requested.
std::string LanguageDownloadStatus(const std::string& language);
// Integrity status for the locale's files in the cached manifest; includes the first missing/damaged relative path.
std::string LanguagePackageStatus(const std::string& language);
std::string LanguagePackageDetails(const std::string& language);

}  // namespace app
