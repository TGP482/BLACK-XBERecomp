#include "common/save_signature.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

#include "app/app.h"
#include "core/log.h"
#include "kernel/crypto.h"

namespace save {

namespace {

namespace fs = std::filesystem;

constexpr size_t kProfileSize = 6144;
constexpr size_t kSigSize = 20;
constexpr size_t kSignedSize = kProfileSize - kSigSize;  // the signature covers everything before it
constexpr size_t kBlocks = 3;
constexpr size_t kBlockSize = 0x778;  // the size word at +4 of each of the three profile blocks
constexpr size_t kChecksumAt = 0x770;  // the sum of the block's first 476 words
constexpr size_t kEmptyAt = 0x76D;     // non zero: the slot is unused
constexpr size_t kBlockWords = 476;
constexpr uint32_t kMagic = 0x26;  // the first word of a valid slot

// the Xbox kernel's XboxHDKey and XboxSignatureKey (runtime/kernel/xboxkrnl.cpp): never set, so all zero
constexpr uint8_t kHdKey[16] = {};
constexpr uint8_t kSigKey[16] = {};

uint32_t Word(const std::vector<uint8_t>& d, size_t at) {
    return uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24;
}

void SetWord(std::vector<uint8_t>& d, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) d[at + i] = uint8_t(v >> (8 * i));
}

// the game's checksum of a slot: the sum of its first 476 words
uint32_t BlockSum(const std::vector<uint8_t>& d, size_t block) {
    uint32_t sum = 0;
    for (size_t i = 0; i < kBlockWords; i++) sum += Word(d, block * kBlockSize + i * 4);
    return sum;
}

// what the game's XCalculateSignature gives: the HD-key HMAC of the signature-key HMAC of the data
void Sign(const std::vector<uint8_t>& d, uint8_t out[kSigSize]) {
    uint8_t inner[kSigSize];
    crypto::Hmac(kSigKey, sizeof kSigKey, d.data(), kSignedSize, nullptr, 0, inner);
    crypto::Hmac(kHdKey, sizeof kHdKey, inner, sizeof inner, nullptr, 0, out);
}

}  // namespace

Slots ClassifySlots(const std::vector<uint8_t>& d) {
    Slots slots;
    slots.fill(Slot::Damaged);
    if (d.size() != kProfileSize) return slots;
    for (size_t b = 0; b < kBlocks; b++) {
        size_t at = b * kBlockSize;
        if (Word(d, at + 4) != kBlockSize) continue;  // Damaged
        uint32_t magic = Word(d, at);
        uint32_t stored = Word(d, at + kChecksumAt);
        uint32_t sum = BlockSum(d, b);
        if (magic == kMagic)
            slots[b] = sum != stored ? Slot::BadChecksum : (d[at + kEmptyAt] ? Slot::Empty : Slot::InUse);
        else if (magic == 0 && stored == sum + kMagic)  // 32 bit sums: wraps like the game's
            slots[b] = Slot::Restorable;
    }
    return slots;
}

namespace {

bool ReadAll(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return !f.bad();
}

bool WriteAll(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    f.flush();
    return bool(f);
}

// "Profile", and "Profilo" which the Europe release uses
bool IsProfileName(const fs::path& name) {
    std::string s = name.string();
    if (s.size() != 7) return false;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s == "profile" || s == "profilo";
}

Result ResignOne(const fs::path& file) {
    Result r;
    r.file = file;
    std::vector<uint8_t> data;
    if (!ReadAll(file, data) || data.size() != kProfileSize) return r;

    // a file with at least one slot the game would read is a profile; the other slots may be blank or damaged
    Slots slots = ClassifySlots(data);
    bool any = false;
    for (Slot sl : slots) any = any || sl != Slot::Damaged;
    if (!any) return r;

    // a slot the game zeroed while it was damaged comes back with its first word; its own checksum was kept
    for (size_t b = 0; b < kBlocks; b++)
        if (slots[b] == Slot::Restorable) {
            SetWord(data, b * kBlockSize, kMagic);
            r.restored++;
        }
    r.slots = ClassifySlots(data);

    uint8_t want[kSigSize];
    Sign(data, want);
    r.signature = std::memcmp(want, data.data() + kSignedSize, kSigSize) != 0;
    if (!r.signature && !r.restored) {
        r.outcome = Outcome::AlreadyValid;
        return r;
    }

    // the original, kept once: the first copy is the one that came from the Xbox
    fs::path backup = file;
    backup += ".xbox.bak";
    std::error_code ec;
    if (!fs::exists(backup, ec)) {
        fs::copy_file(file, backup, fs::copy_options::none, ec);
        if (ec) {
            r.outcome = Outcome::Failed;
            return r;
        }
    }

    // written beside the file and moved over it, so a failed write cannot leave half a profile
    std::memcpy(data.data() + kSignedSize, want, kSigSize);
    fs::path temp = file;
    temp += ".new";
    if (!WriteAll(temp, data)) {
        fs::remove(temp, ec);
        r.outcome = Outcome::Failed;
        return r;
    }
    fs::rename(temp, file, ec);  // replaces the file, on Windows too
    if (ec) {
        fs::remove(temp, ec);
        r.outcome = Outcome::Failed;
        return r;
    }
    r.outcome = Outcome::Signed;
    return r;
}

}  // namespace

std::vector<Result> ResignProfiles(const fs::path& title_dir) {
    std::vector<Result> results;
    std::error_code ec;
    if (!fs::is_directory(title_dir, ec)) return results;

    std::vector<fs::path> folders;
    for (fs::directory_iterator it(title_dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) folders.push_back(it->path());
    std::sort(folders.begin(), folders.end());

    for (const fs::path& folder : folders) {
        std::vector<fs::path> files;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && IsProfileName(it->path().filename())) files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) results.push_back(ResignOne(file));
    }
    return results;
}

void ResignSaves() {
    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    fs::path title_dir = app::ExeDir() / hdd / "E" / "UDATA" / "45410083";
    for (const Result& r : ResignProfiles(title_dir)) {
        if (r.outcome == Outcome::NotProfile) continue;
        std::string name = (r.file.parent_path().filename() / r.file.filename()).string();
        int in_use = 0, empty = 0, bad = 0;
        for (Slot sl : r.slots) {
            if (sl == Slot::InUse) in_use++;
            else if (sl == Slot::Empty) empty++;
            else bad++;  // BadChecksum or Damaged: the game will call it damaged
        }
        std::string text = std::to_string(in_use) + (in_use == 1 ? " profile" : " profiles") + " in use";
        if (empty) text += ", " + std::to_string(empty) + " empty";
        if (bad) text += ", " + std::to_string(bad) + " damaged";
        if (r.restored) text += ", " + std::to_string(r.restored) + " restored";
        if (r.signature) text += ", signature replaced";
        std::string keep = r.file.filename().string() + ".xbox.bak";
        switch (r.outcome) {
            case Outcome::Signed:
                LOGR("Save %s: %s (original kept as %s)", name.c_str(), text.c_str(), keep.c_str());
                break;
            case Outcome::AlreadyValid: LOGR("Save %s: %s", name.c_str(), text.c_str()); break;
            case Outcome::Failed:
                LOGE("Save %s: %s, but it could not be changed", name.c_str(), text.c_str());
                break;
            case Outcome::NotProfile: break;
        }
    }
}

}  // namespace save
