#pragma once
// Xbox DVD file system images (.iso): a whole disc dump, its game partition behind the video one, or the game
// partition alone (xiso). Read only.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kernel {

class Disc {
public:
    struct Entry {
        std::string name;
        uint64_t offset = 0;  // bytes into the image file; a folder's is its entry table
        uint32_t size = 0;
        bool dir = false;
        std::vector<Entry> children;  // folders, sorted by name
    };
    Disc() = default;
    Disc(const Disc&) = delete;
    Disc& operator=(const Disc&) = delete;
    ~Disc();
    // false with the reason when the file is no Xbox disc image
    bool Open(const std::filesystem::path& path, std::string& error);
    // from the root, any case, ".." steps up; null when missing
    const Entry* Find(const std::vector<std::string>& parts) const;
    const Entry* Find(std::string_view path) const;  // backslash separated
    // at a position in the file, from any thread; the count read, short at the file's end
    uint32_t Read(const Entry& file, uint64_t pos, void* out, uint32_t len) const;
    bool Load(std::string_view path, std::vector<uint8_t>& out) const;
    uint64_t Time() const { return time_; }  // the volume's FILETIME, the only time the disc keeps
    const std::filesystem::path& Path() const { return path_; }

private:
    uint32_t ReadAt(uint64_t at, void* out, uint32_t len) const;
    bool ReadDir(Entry& dir, int depth);

    intptr_t file_ = -1;  // HANDLE or fd
    std::filesystem::path path_;
    uint64_t base_ = 0;  // the game partition
    uint64_t time_ = 0;
    Entry root_;
};

// D: from a disc image instead of a folder, set before Boot
void MountDisc(std::unique_ptr<Disc> disc);
const Disc* MountedDisc();

}  // namespace kernel
