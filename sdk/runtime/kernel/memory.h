#pragma once
#include <cstdint>

namespace mem {

constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kUserLow = 0x00010000;
constexpr uint32_t kUserHigh = 0x7FFE0000;
constexpr uint32_t kContiguousBase = 0x80000000;  // guest VA = kContiguousBase | physical
constexpr uint32_t kPhysicalSize = 64u << 20;
constexpr uint32_t kWriteCombineBase = 0xF0000000;
constexpr uint32_t kKernelDataBase = 0x80000000;   // first physical megabyte holds kernel variables
constexpr uint32_t kKernelDataSize = 0x00100000;
constexpr uint32_t kSystemBase = 0xB0000000;       // pool, system memory, thread blocks
constexpr uint32_t kSystemSize = 0x10000000;
constexpr uint32_t kStackBase = 0xD0000000;
constexpr uint32_t kStackSize = 0x10000000;

// NT allocation flags
constexpr uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000, kMemRelease = 0x8000,
                   kMemTopDown = 0x100000, kMemNoZero = 0x800000;

bool Init();  // once; a second call keeps the mapping
void Reset();  // every guest page zero and unused again, as before the first boot (kernel::Restart)
uint8_t* Base();
bool Commit(uint32_t va, uint32_t size);
void Decommit(uint32_t va, uint32_t size);
bool IsCommitted(uint32_t va);

// user virtual space, NTSTATUS results
uint32_t AllocateVirtual(uint32_t& base, uint32_t& size, uint32_t type, uint32_t protect);
uint32_t FreeVirtual(uint32_t& base, uint32_t& size, uint32_t type);
struct RegionInfo {
    uint32_t base, alloc_base, alloc_protect, size, state, protect, type;
};
bool QueryVirtual(uint32_t addr, RegionInfo& out);
void ReserveImage(uint32_t base, uint32_t size);

uint32_t AllocContiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t align);
void FreeContiguous(uint32_t va);
uint32_t AllocationSize(uint32_t va);  // any allocator, 0 if unknown

uint32_t AllocSystem(uint32_t size, uint32_t align = 16);
void FreeSystem(uint32_t va);

uint32_t AllocStack(uint32_t size);  // returns the top, guard below the bottom
void FreeStack(uint32_t top);

uint32_t PhysicalFromVirtual(uint32_t va);
uint32_t VirtualFromPhysical(uint32_t pa);

struct Stats {
    uint32_t total_pages, available_pages, virtual_committed, contiguous_used, system_used;
};
Stats Query();

}  // namespace mem
