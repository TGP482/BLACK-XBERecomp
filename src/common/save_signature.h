#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Saves copied from a real Xbox are signed with that console's keys, which the port does not have. The game finds the
// signature wrong and calls the profile damaged. Here every Profile (Europe: Profilo) is signed again with the port's
// keys when the port starts, so a save folder can simply be copied into hdd/E/UDATA/45410083/.
//
// A save file holds three profile slots. The signature covers the whole file, so one signature serves all of them; what
// differs from slot to slot is what the game finds in each, and that is what ClassifySlots reports.
namespace save {

enum class Outcome {
    Signed,        // the file was changed: its signature replaced and/or a slot restored
    AlreadyValid,  // nothing to do
    NotProfile,    // not 6144 bytes, or no slot shaped like a Black profile: left alone
    Failed,        // could not back up, write or replace the file: left as it was
};

// what the game's slot check (0x1F75D0) finds in one of the three slots
enum class Slot {
    Empty,        // a valid slot nobody has used; the game hides it
    InUse,        // a valid profile
    BadChecksum,  // the slot's own checksum is wrong (for example edited by hand): the game calls it damaged
    Restorable,   // the first word is 0 but the checksum is the one of a valid slot with 0x26 there: the game zeroed it
                  // when it found the slot damaged (for example under a wrong signature) and saved that
    Damaged,      // anything else: no size word, other first word
};

using Slots = std::array<Slot, 3>;

struct Result {
    std::filesystem::path file;
    Outcome outcome = Outcome::NotProfile;
    Slots slots{};            // as the file is now, so a restored slot shows as InUse or Empty
    unsigned restored = 0;    // slots brought back
    bool signature = false;   // the signature was replaced
};

// the state of each of the three slots of a 6144 byte profile file (all Damaged when the file has another size)
Slots ClassifySlots(const std::vector<uint8_t>& data);

// the Profile and Profilo files below title_dir (UDATA/45410083), one folder level down; a file that changes is saved
// beside itself as <name>.xbox.bak first, unless that copy already exists
std::vector<Result> ResignProfiles(const std::filesystem::path& title_dir);

// the port's save folder, logged; call after the settings load and before the game boots
void ResignSaves();

}  // namespace save
