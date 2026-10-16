#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <json.hpp>

#include "app/app.h"
#include "common/language_download.h"
#include "common/options.h"
#include "core/log.h"

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr wchar_t kRawHost[] = L"raw.githubusercontent.com";
constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr size_t kMaxFileBytes = 16 * 1024 * 1024;
constexpr size_t kMaxManifestBytes = 2 * 1024 * 1024;

struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::string pending;
    bool update_pending = false;
    uint64_t generation = 0;
    std::string update_status;
    std::map<std::string, std::string> status;
    std::map<std::string, std::string> package_status;
    std::map<std::string, std::string> package_details;
};

State& Shared() {
    // Process-lifetime storage keeps the detached worker independent of static destruction order.
    static State* state = new State;
    return *state;
}

std::string LocaleFolder(const std::string& language) {
    if (language == "int") return "en-US";
    if (language == "esp") return "es";
    if (language == "fra") return "fr";
    if (language == "deu") return "de";
    if (language == "ita") return "it";
    if (language == "pt-BR" || language == "id" || language == "ru") return language;
    return {};
}

std::string Sha256(const std::vector<uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, received = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                          &received, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<uint8_t> object(object_size);
    std::array<uint8_t, 32> digest{};
    bool ok = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0 &&
              BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0 &&
              BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint8_t value : digest) out << std::setw(2) << unsigned(value);
    return out.str();
}

std::wstring UniqueFileSuffix(const wchar_t* purpose) {
    static std::atomic<uint64_t> next_file{0};
    return L"." + std::wstring(purpose) + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(next_file.fetch_add(1, std::memory_order_relaxed));
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& bytes, size_t max_size = kMaxFileBytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto size = file.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > max_size) return false;
    bytes.resize(static_cast<size_t>(size));
    file.seekg(0);
    return bytes.empty() || bool(file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())));
}

bool HttpGetUrl(const wchar_t* host, const std::wstring& target, size_t max_bytes,
                std::vector<uint8_t>& bytes, std::string& error) {
    HINTERNET session = WinHttpOpen(L"BLACK-XBERecomp language downloader/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = "Could not start Windows HTTP service"; return false; }
    WinHttpSetTimeouts(session, 7000, 7000, 15000, 15000);
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr,
                                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                          WINHTTP_FLAG_SECURE) : nullptr;
    bool ok = false;
    if (request && WinHttpAddRequestHeaders(request,
                                             L"Cache-Control: no-cache\r\nPragma: no-cache\r\nUser-Agent: BLACK-XBERecomp\r\n",
                                             -1L, WINHTTP_ADDREQ_FLAG_ADD) &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) && status == 200) {
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available)) { error = "GitHub download was interrupted"; break; }
                if (!available) { ok = true; break; }
                if (bytes.size() + available > max_bytes) { error = "GitHub file exceeds the safe size limit"; break; }
                const size_t offset = bytes.size();
                bytes.resize(offset + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, bytes.data() + offset, available, &read)) {
                    bytes.resize(offset);
                    error = "GitHub download was interrupted";
                    break;
                }
                bytes.resize(offset + read);
            }
        } else {
            error = status == 404 ? "A file listed in manifest.json was not found" : "GitHub returned an HTTP error";
        }
    } else if (error.empty()) {
        error = "Could not connect to GitHub; check your internet connection";
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

bool HttpGet(const std::string& path, const std::string& revision,
             std::vector<uint8_t>& bytes, std::string& error) {
    const size_t max_bytes = path == "manifest.json" ? kMaxManifestBytes : kMaxFileBytes;
    const std::wstring wide_path(path.begin(), path.end());
    const std::wstring wide_revision(revision.begin(), revision.end());
    return HttpGetUrl(kRawHost, L"/soldieg/BLACK-SUBTITLES/" + wide_revision + L"/" + wide_path,
                      max_bytes, bytes, error);
}

bool LatestRevision(std::string& revision, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!HttpGetUrl(kApiHost, L"/repos/soldieg/BLACK-SUBTITLES/commits/main", 1024 * 1024, bytes, error))
        return false;
    Json response;
    try { response = Json::parse(bytes.begin(), bytes.end()); }
    catch (...) { error = "GitHub did not return a valid commit response"; return false; }
    revision = response.value("sha", std::string{});
    if (revision.size() != 40 || !std::all_of(revision.begin(), revision.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (std::tolower(c) >= 'a' && std::tolower(c) <= 'f');
        })) {
        revision.clear();
        error = "GitHub returned an invalid translation revision";
        return false;
    }
    return true;
}

bool SafeRelativePath(const std::string& text, fs::path& path) {
    if (text.empty() || text.find_first_of("\\:#?%") != std::string::npos) return false;
    path = fs::path(text);
    if (path.is_absolute() || !path.root_name().empty() || !path.root_directory().empty()) return false;
    for (const fs::path& part : path)
        if (part.empty() || part == "." || part == "..") return false;
    return true;
}

bool SafeLocaleKey(const std::string& locale) {
    if (locale.empty() || locale.size() > 32) return false;
    return std::all_of(locale.begin(), locale.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-';
    });
}

bool ValidManifest(const Json& manifest) {
    if (!manifest.is_object() || manifest.value("manifest_version", 0) != 1 ||
        manifest.value("repository", std::string{}) != "soldieg/BLACK-SUBTITLES" ||
        !manifest.contains("locales") || !manifest["locales"].is_object()) return false;
    for (auto it = manifest["locales"].begin(); it != manifest["locales"].end(); ++it)
        if (!SafeLocaleKey(it.key())) return false;
    return true;
}

bool WriteAtomic(const fs::path& path, const std::vector<uint8_t>& bytes, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) { error = "The language folder is not writable"; return false; }
    auto temp = path;
    temp += UniqueFileSuffix(L"download") + L".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file || (!bytes.empty() && !file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())))) {
            error = "The language folder is not writable";
            fs::remove(temp, ec);
            return false;
        }
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Could not install the downloaded language file";
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

struct PendingInstall {
    fs::path output;
    fs::path backup;
    std::vector<uint8_t> bytes;
    bool backed_up = false;
    bool installed = false;
};

bool PrepareAsset(const fs::path& locale_root, const Json& asset, const std::string& revision,
                  std::string& error, PendingInstall& pending) {
    if (!asset.is_object() || !asset.contains("source_path") || !asset.contains("destination_path") ||
        !asset.contains("sha256") || !asset.contains("size_bytes") || !asset["source_path"].is_string() ||
        !asset["destination_path"].is_string() || !asset["sha256"].is_string() || !asset["size_bytes"].is_number_integer()) {
        error = "manifest.json has an invalid file entry";
        return false;
    }
    const std::string source = asset["source_path"].get<std::string>();
    const std::string expected = asset["sha256"].get<std::string>();
    uint64_t expected_size = 0;
    if (asset["size_bytes"].is_number_unsigned()) {
        expected_size = asset["size_bytes"].get<uint64_t>();
    } else {
        const int64_t signed_size = asset["size_bytes"].get<int64_t>();
        if (signed_size < 0) {
            error = "manifest.json contains an invalid file size";
            return false;
        }
        expected_size = static_cast<uint64_t>(signed_size);
    }
    fs::path source_relative;
    fs::path destination;
    if (!SafeRelativePath(source, source_relative) ||
        !SafeRelativePath(asset["destination_path"].get<std::string>(), destination) ||
        expected.size() != 64 || expected_size > kMaxFileBytes) {
        error = "manifest.json contains an unsafe or unsupported file entry";
        return false;
    }
    std::string normalized_hash = expected;
    std::transform(normalized_hash.begin(), normalized_hash.end(), normalized_hash.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (!std::all_of(normalized_hash.begin(), normalized_hash.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) {
        error = "manifest.json contains an invalid SHA-256 value";
        return false;
    }

    const fs::path output = locale_root / destination;
    std::vector<uint8_t> bytes;
    if (ReadFile(output, bytes) && bytes.size() == expected_size && Sha256(bytes) == normalized_hash) return true;
    bytes.clear();
    if (!HttpGet(source, revision, bytes, error)) return false;
    const std::string downloaded_hash = Sha256(bytes);
    if (bytes.size() != expected_size || downloaded_hash != normalized_hash) {
        error = "Integrity check failed for " + source + " (expected " + std::to_string(expected_size) +
                " bytes / " + normalized_hash + ", got " + std::to_string(bytes.size()) + " bytes / " +
                downloaded_hash + "). The manifest and source must come from the same Git commit.";
        return false;
    }
    pending.output = output;
    pending.bytes = std::move(bytes);
    return true;
}

bool InstallPreparedAssets(std::vector<PendingInstall>& pending, std::string& error) {
    for (PendingInstall& file : pending) {
        std::error_code ec;
        fs::create_directories(file.output.parent_path(), ec);
        if (ec) {
            error = "The language folder is not writable";
            return false;
        }
    }

    auto rollback = [&](size_t through) {
        bool ok = true;
        for (size_t i = through + 1; i > 0; --i) {
            PendingInstall& file = pending[i - 1];
            std::error_code ec;
            if (file.installed) {
                fs::remove(file.output, ec);
                if (ec) ok = false;
            }
            if (file.backed_up &&
                !MoveFileExW(file.backup.c_str(), file.output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                ok = false;
        }
        return ok;
    };

    for (size_t i = 0; i < pending.size(); ++i) {
        PendingInstall& file = pending[i];
        std::error_code ec;
        const bool exists = fs::exists(file.output, ec);
        if (ec) {
            error = "Could not inspect an existing language file";
            if (i && !rollback(i - 1)) error += "; rollback also failed";
            return false;
        }
        if (exists) {
            file.backup = file.output;
            file.backup += UniqueFileSuffix(L"rollback") + L".bak";
            if (!MoveFileExW(file.output.c_str(), file.backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
                error = "Could not preserve an existing language file before update";
                if (i && !rollback(i - 1)) error += "; rollback also failed";
                return false;
            }
            file.backed_up = true;
        }
        if (!WriteAtomic(file.output, file.bytes, error)) {
            const bool restored = rollback(i);
            if (!restored) error += "; rollback also failed";
            return false;
        }
        file.installed = true;
    }

    for (PendingInstall& file : pending) {
        if (!file.backed_up) continue;
        std::error_code ec;
        fs::remove(file.backup, ec);
        if (ec) LOGW("language update: could not remove backup file %s", file.backup.string().c_str());
    }
    return true;
}

bool ParseManifest(const std::vector<uint8_t>& bytes, Json& manifest, std::string& error) {
    try { manifest = Json::parse(bytes.begin(), bytes.end()); }
    catch (...) { error = "GitHub manifest.json is not valid JSON"; return false; }
    if (!ValidManifest(manifest)) { error = "GitHub manifest.json has an unsupported format"; return false; }
    return true;
}

bool InstallLocale(const Json& manifest, const fs::path& language_root, const std::string& locale,
                   const std::string& revision, std::string& error,
                   size_t* installed_assets = nullptr, bool* changed = nullptr) {
    if (!SafeLocaleKey(locale)) {
        error = "manifest.json contains an unsafe locale name";
        return false;
    }
    if (!manifest["locales"].contains(locale) || !manifest["locales"][locale].is_object() ||
        !manifest["locales"][locale].contains("files") || !manifest["locales"][locale]["files"].is_array()) {
        error = "The selected locale has no package in manifest.json";
        return false;
    }
    const fs::path locale_root = language_root / locale;
    size_t installed = 0;
    std::vector<PendingInstall> pending;
    for (const Json& asset : manifest["locales"][locale]["files"]) {
        if (!asset.is_object() || asset.value("asset_type", std::string{}) == "launcher_catalog") continue;
        PendingInstall prepared;
        if (!PrepareAsset(locale_root, asset, revision, error, prepared)) return false;
        if (!prepared.output.empty()) pending.push_back(std::move(prepared));
        ++installed;
    }
    if (!installed) { error = "The selected locale has no game assets in manifest.json"; return false; }
    if (!InstallPreparedAssets(pending, error)) return false;
    if (installed_assets) *installed_assets = installed;
    if (changed) *changed = !pending.empty();
    LOGI("language download: %s ready, %zu manifest assets checked%s", locale.c_str(), installed,
         pending.empty() ? "" : ", files updated");
    return true;
}

bool FetchManifest(Json& manifest, std::string& revision, std::string& error) {
    const fs::path language_root = app::ExeDir() / "language";
    const fs::path cached_manifest = language_root / "manifest.json";
    std::vector<uint8_t> bytes;
    std::string fetch_error;
    if (LatestRevision(revision, fetch_error) && HttpGet("manifest.json", revision, bytes, fetch_error)) {
        if (!ParseManifest(bytes, manifest, error)) return false;
        LOGI("language manifest: verified current main at commit %s", revision.c_str());
        manifest["_resolved_revision"] = revision;
        const std::string cached_json = manifest.dump(2);
        bytes.assign(cached_json.begin(), cached_json.end());
        if (!WriteAtomic(cached_manifest, bytes, error)) return false;
        return true;
    }
    error = fetch_error.empty() ? "Could not resolve the current GitHub translation revision" : fetch_error;
    return false;
}

bool Install(const std::string& language, std::string& error) {
    const std::string locale = LocaleFolder(language);
    if (locale.empty()) { error = "The selected language is not supported by the manifest"; return false; }
    Json manifest;
    std::string revision;
    if (!FetchManifest(manifest, revision, error)) return false;
    return InstallLocale(manifest, app::ExeDir() / "language", locale, revision, error);
}

bool UpdateAllLocales(std::string& result, std::string& error) {
    Json manifest;
    std::string revision;
    if (!FetchManifest(manifest, revision, error)) return false;
    const fs::path root = app::ExeDir() / "language";
    const Json& locales = manifest["locales"];
    if (!locales.is_object() || locales.empty()) {
        error = "manifest.json does not list any language packages";
        return false;
    }
    size_t updates = 0;
    size_t checked = 0;
    std::vector<std::string> failures;
    for (auto it = locales.begin(); it != locales.end(); ++it) {
        const std::string locale = it.key();
        bool changed = false;
        std::string package_error;
        if (!InstallLocale(manifest, root, locale, revision, package_error, nullptr, &changed)) {
            LOGR("language update: %s failed: %s", locale.c_str(), package_error.c_str());
            failures.push_back(locale + ": " + package_error);
            continue;
        }
        ++checked;
        updates += changed ? 1u : 0u;
    }
    if (!failures.empty()) {
        error = "Some language packages could not be updated";
        for (const std::string& failure : failures) error += "\n- " + failure;
        LOGR("language update: checked %zu/%zu locale package(s); %zu failed",
             checked, locales.size(), failures.size());
        return false;
    }
    result = updates ? "TRANSLATIONS UPDATED" : "TRANSLATIONS UP TO DATE";
    LOGR("language update: checked %zu locale package(s); changed %zu", checked, updates);
    return true;
}

void Worker() {
    State& state = Shared();
    std::unique_lock lock(state.mutex);
    while (true) {
        state.cv.wait(lock, [&] { return !state.pending.empty() || state.update_pending; });
        if (state.update_pending) {
            state.update_pending = false;
            state.update_status = "CHECKING TRANSLATIONS...";
            state.package_status.clear();
            state.package_details.clear();
            lock.unlock();
            std::string error, result;
            const bool ok = UpdateAllLocales(result, error);
            if (!ok) LOGR("language update: failed: %s", error.c_str());
            lock.lock();
            state.update_status = ok ? result : "UPDATE FAILED (SEE LOG)";
            state.package_status.clear();
            state.package_details.clear();
            continue;
        }
        const uint64_t requested_generation = state.generation;
        const std::string language = state.pending;
        if (state.cv.wait_for(lock, std::chrono::milliseconds(650), [&] { return state.generation != requested_generation; })) continue;
        state.pending.clear();
        state.status[language] = "DOWNLOADING...";
        // The downloaded manifest may have made every locale's cached inventory stale.
        state.package_status.clear();
        state.package_details.clear();
        lock.unlock();
        LOGR("language download: starting package check for %s", language.c_str());
        std::string error;
        const bool ok = Install(language, error);
        if (ok) LOGR("language download: %s package is ready", LocaleFolder(language).c_str());
        else LOGR("language download: %s failed: %s", language.c_str(), error.c_str());
        lock.lock();
        state.status[language] = ok ? (language == game::BootLanguage() ? "FILES READY" : "READY - RESTART")
                                   : "DOWNLOAD FAILED (SEE LOG)";
        state.package_status.clear();
        state.package_details.clear();
    }
}

void ClearSupersededQueuedRequest(State& state) {
    if (state.pending.empty()) return;
    auto status = state.status.find(state.pending);
    if (status != state.status.end() && status->second == "QUEUED...") state.status.erase(status);
    state.pending.clear();
}

}  // namespace

void app::RequestLanguageDownload(const std::string& language) {
    if (LocaleFolder(language).empty()) return;
    State& state = Shared();
    static std::once_flag start;
    std::call_once(start, [] { std::thread(Worker).detach(); });
    {
        std::lock_guard guard(state.mutex);
        if (!state.pending.empty() && state.pending != language) ClearSupersededQueuedRequest(state);
        state.pending = language;
        ++state.generation;
        state.status[language] = "QUEUED...";
        state.package_status.erase(language);
        state.package_details.erase(language);
    }
    LOGR("language download: queued %s", language.c_str());
    state.cv.notify_one();
}

void app::RequestTranslationUpdate() {
    State& state = Shared();
    static std::once_flag start;
    std::call_once(start, [] { std::thread(Worker).detach(); });
    {
        std::lock_guard guard(state.mutex);
        ClearSupersededQueuedRequest(state);
        ++state.generation;
        state.update_pending = true;
        state.update_status = "QUEUED...";
        state.package_status.clear();
        state.package_details.clear();
    }
    LOGR("language update: manual check queued");
    state.cv.notify_one();
}

std::string app::TranslationUpdateStatus() {
    State& state = Shared();
    std::lock_guard guard(state.mutex);
    return state.update_status;
}

std::string app::LanguageDownloadStatus(const std::string& language) {
    State& state = Shared();
    std::lock_guard guard(state.mutex);
    auto it = state.status.find(language);
    return it == state.status.end() ? std::string{} : it->second;
}

std::string app::LanguagePackageStatus(const std::string& language) {
    State& state = Shared();
    {
        std::lock_guard guard(state.mutex);
        auto active = state.status.find(language);
        if (active != state.status.end() && (active->second == "QUEUED..." || active->second == "DOWNLOADING..."))
            return active->second;
        auto cached = state.package_status.find(language);
        if (cached != state.package_status.end()) return cached->second;
    }

    const std::string locale = LocaleFolder(language);
    std::string result = "MANIFEST NEEDED";
    std::string details;
    if (!locale.empty()) {
        const fs::path root = app::ExeDir() / "language";
        std::vector<uint8_t> bytes;
        Json manifest;
        if (ReadFile(root / "manifest.json", bytes, kMaxManifestBytes)) {
            try { manifest = Json::parse(bytes.begin(), bytes.end()); } catch (...) {}
        }
        if (ValidManifest(manifest)) {
            if (!manifest["locales"].contains(locale) || !manifest["locales"][locale].is_object() ||
                !manifest["locales"][locale].contains("files") || !manifest["locales"][locale]["files"].is_array()) {
                result = "NOT IN MANIFEST";
            } else {
                std::vector<std::string> missing, damaged;
                size_t checked_assets = 0;
                for (const Json& asset : manifest["locales"][locale]["files"]) {
                    if (!asset.is_object() || asset.value("asset_type", std::string{}) == "launcher_catalog") continue;
                    ++checked_assets;
                    if (!asset.contains("destination_path") || !asset["destination_path"].is_string()) continue;
                    fs::path relative;
                    if (!SafeRelativePath(asset["destination_path"].get<std::string>(), relative)) continue;
                    std::vector<uint8_t> local;
                    const fs::path path = root / locale / relative;
                    if (!ReadFile(path, local)) {
                        missing.push_back(relative.generic_string());
                        continue;
                    }
                    const uint64_t expected_size = asset.value("size_bytes", uint64_t(0));
                    const std::string expected_hash = asset.value("sha256", std::string{});
                    std::string normalized_hash = expected_hash;
                    std::transform(normalized_hash.begin(), normalized_hash.end(), normalized_hash.begin(),
                                   [](unsigned char c) { return char(std::tolower(c)); });
                    if (local.size() != expected_size || Sha256(local) != normalized_hash)
                        damaged.push_back(relative.generic_string());
                }
                const auto summarize = [&](const char* label, const std::vector<std::string>& paths) {
                    std::string text = std::string(label) + " (" + std::to_string(paths.size()) + ")";
                    details = std::string(label) + " files in language/" + locale + ":\n";
                    for (const std::string& path : paths)
                    {
                        LOGW("language files: %s/%s/%s", locale.c_str(), label, path.c_str());
                        details += "- language/" + locale + "/" + path + "\n";
                    }
                    return text;
                };
                if (!damaged.empty()) result = summarize("DAMAGED", damaged);
                else if (!missing.empty() && missing.size() == checked_assets)
                    result = summarize("NOT DOWNLOADED", missing);
                else if (!missing.empty()) result = summarize("MISSING", missing);
                else {
                    result = "READY";
                    details = "All files listed in the manifest are present and match their SHA-256 checks.";
                }
            }
        }
    }
    std::lock_guard guard(state.mutex);
    auto status = state.package_status.emplace(language, std::move(result)).first;
    if (!details.empty()) state.package_details[language] = std::move(details);
    if (!state.package_details.contains(language)) {
        if (status->second == "MANIFEST NEEDED")
            state.package_details[language] = "The manifest is not available yet. Connect to the internet and select a language to download its package.";
        else if (status->second == "NOT IN MANIFEST")
            state.package_details[language] = "This locale is not listed in the cached manifest.";
    }
    return status->second;
}

std::string app::LanguagePackageDetails(const std::string& language) {
    (void)LanguagePackageStatus(language);
    State& state = Shared();
    std::lock_guard guard(state.mutex);
    auto found = state.package_details.find(language);
    return found == state.package_details.end() ? "The package check is in progress." : found->second;
}
