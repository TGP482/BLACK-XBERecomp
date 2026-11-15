#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

#include "app/app.h"
#include "kernel/crypto.h"
#include "kernel/exports.h"
#include "xbe/xbe.h"

namespace kernel {

extern const xbe::Image* g_image;

namespace {

KERNEL_DATA(156, KeTickCount, 4);
KERNEL_DATA(164, LaunchDataPage, 4);
KERNEL_DATA(322, XboxHardwareInfo, 8);
KERNEL_DATA(324, XboxKrnlVersion, 8);
KERNEL_DATA(326, XeImageFileName, 8 + 64);
KERNEL_DATA(41, HalDiskModelNumber, 8 + 48);
KERNEL_DATA(42, HalDiskSerialNumber, 8 + 32);
KERNEL_DATA(40, HalDiskCachePartitionCount, 4);
KERNEL_DATA(356, HalBootSMCVideoMode, 4);
KERNEL_DATA(323, XboxHDKey, 16);
KERNEL_DATA(325, XboxSignatureKey, 16);
KERNEL_DATA(353, XboxLANKey, 16);
KERNEL_DATA(354, XboxAlternateSignatureKeys, 16 * 16);
KERNEL_DATA(355, XePublicKeyData, 284);
KERNEL_DATA(16, ExEventObjectType, 0x20);
KERNEL_DATA(71, IoFileObjectType, 0x20);
KERNEL_DATA(259, PsThreadObjectType, 0x20);
KERNEL_DATA(357, IdexChannelObject, 0x40);
KERNEL_DATA(22, ExMutantObjectType, 0x20);
KERNEL_DATA(30, ExSemaphoreObjectType, 0x20);
KERNEL_DATA(31, ExTimerObjectType, 0x20);
KERNEL_DATA(64, IoCompletionObjectType, 0x20);
KERNEL_DATA(70, IoDeviceObjectType, 0x20);
KERNEL_DATA(240, ObDirectoryObjectType, 0x20);
KERNEL_DATA(249, ObSymbolicLinkObjectType, 0x20);
KERNEL_DATA(88, KdDebuggerEnabled, 4);
KERNEL_DATA(89, KdDebuggerNotPresent, 4);
KERNEL_DATA(120, KeInterruptTime, 12);
KERNEL_DATA(154, KeSystemTime, 12);
KERNEL_DATA(157, KeTimeIncrement, 4);
KERNEL_DATA(321, XboxEEPROMKey, 16);

void PutAnsi(uint32_t str, const char* text) {
    uint32_t buf = str + 8;
    size_t n = std::strlen(text);
    std::memcpy(HostPtr(buf), text, n + 1);
    W16(str, uint16_t(n));
    W16(str + 2, uint16_t(n + 1));
    W32(str + 4, buf);
}

uint8_t& Irql() { return G<uint8_t>(Current()->kpcr + 0x24); }

int64_t TimeoutArg(uint32_t ptr) { return ptr ? int64_t(R64(ptr)) : INT64_MAX; }

// KSYSTEM_TIME: readers retry until High1Time equals High2Time
void PutSystemTime(uint32_t va, uint64_t t) {
    W32(va + 8, uint32_t(t >> 32));
    W32(va, uint32_t(t));
    W32(va + 4, uint32_t(t >> 32));
}

uint64_t InterruptTime() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return uint64_t(duration_cast<nanoseconds>(steady_clock::now() - start).count() / 100);
}

}  // namespace

uint32_t g_tick_count_va = 0;
uint32_t g_launch_data_page_va = 0;

void InitKernelData() {
    g_tick_count_va = DataAddress(156);
    g_launch_data_page_va = DataAddress(164);
    W32(DataAddress(164), 0);
    uint32_t hw = DataAddress(322);
    W32(hw, 0x00000020);
    G<uint8_t>(hw + 4) = 0xD3;  // GPU revision
    G<uint8_t>(hw + 5) = 0xB1;  // MCP revision
    uint32_t ver = DataAddress(324);
    W16(ver, 1);
    W16(ver + 2, 0);
    W16(ver + 4, 5838);
    W16(ver + 6, 1);
    PutAnsi(DataAddress(326), "\\Device\\CdRom0\\default.xbe");
    PutAnsi(DataAddress(41), "RECOMP HARD DISK");
    PutAnsi(DataAddress(42), "0000000000000000");
    W32(DataAddress(40), 3);
    W32(DataAddress(356), 0);
    // IDE_CHANNEL_OBJECT DeviceQueue.DeviceListHead, XAPI walks it to cancel file I/O
    uint32_t queue = DataAddress(357) + 0x28;
    W32(queue, queue);
    W32(queue + 4, queue);
    G<uint8_t>(DataAddress(89)) = 1;
    W32(DataAddress(157), 10000);  // 1 ms in 100 ns units
    // the start of the clock
    std::thread([] {
        auto start = std::chrono::steady_clock::now();
        for (;;) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            W32(g_tick_count_va, uint32_t(ms));
            PutSystemTime(DataAddress(120), InterruptTime());
            PutSystemTime(DataAddress(154), uint64_t(Now100ns()));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
}

// critical sections

KERNEL_EXPORT(291, RtlInitializeCriticalSection, 1) {
    uint32_t cs = a[0];
    G<uint8_t>(cs) = 1;  // SynchronizationEvent
    G<uint8_t>(cs + 1) = 0;
    G<uint8_t>(cs + 2) = 4;  // Header.Size in dwords
    G<uint8_t>(cs + 3) = 0;
    W32(cs + 4, 0);
    W32(cs + 8, cs + 8);  // empty WaitListHead
    W32(cs + 12, cs + 8);
    W32(cs + 0x10, 0xFFFFFFFF);  // LockCount
    W32(cs + 0x14, 0);           // RecursionCount
    W32(cs + 0x18, 0);           // OwningThread
    return 0;
}

KERNEL_EXPORT(277, RtlEnterCriticalSection, 1) {
    uint32_t cs = a[0];
    uint32_t me = Current()->ethread;
    auto* lock_count = reinterpret_cast<int32_t*>(HostPtr(cs + 0x10));
    if (__atomic_add_fetch(lock_count, 1, __ATOMIC_SEQ_CST) == 0) {
        W32(cs + 0x18, me);
        W32(cs + 0x14, 1);
        return 0;
    }
    if (R32(cs + 0x18) == me) {
        W32(cs + 0x14, R32(cs + 0x14) + 1);
        return 0;
    }
    ObjectRef ev = FromPointer(cs);
    // a wait past 2 s is logged once with the owner, so a starved or stuck section shows in the log
    if (Wait({ev}, false, -20000000, false) == kTimeout) {
        LOGW("critical section %08X: thread %08X waits over 2 s, owner %08X, recursion %u, lock count %d, return %08X",
             cs, me, R32(cs + 0x18), R32(cs + 0x14), *lock_count, R32(ctx.esp));
        LogGuestStack(ctx, 24);
        for (auto& t : Threads())  // the owner's stack, read while it runs: a diagnostic, not exact
            if (t->ethread == R32(cs + 0x18) && t->ctx) {
                LOGW("critical section %08X: owner is thread %X, its stack:", cs, t->id);
                LogGuestStack(*t->ctx, 96);
            }
        Wait({ev}, false, INT64_MAX, false);
    }
    W32(cs + 0x18, me);
    W32(cs + 0x14, 1);
    return 0;
}

KERNEL_EXPORT(294, RtlLeaveCriticalSection, 1) {
    uint32_t cs = a[0];
    auto* lock_count = reinterpret_cast<int32_t*>(HostPtr(cs + 0x10));
    uint32_t rec = R32(cs + 0x14) - 1;
    W32(cs + 0x14, rec);
    if (rec) {
        __atomic_sub_fetch(lock_count, 1, __ATOMIC_SEQ_CST);
        return 0;
    }
    W32(cs + 0x18, 0);
    if (__atomic_sub_fetch(lock_count, 1, __ATOMIC_SEQ_CST) >= 0) SetEvent(*FromPointer(cs));
    return 0;
}

KERNEL_EXPORT(306, RtlTryEnterCriticalSection, 1) {
    uint32_t cs = a[0];
    uint32_t me = Current()->ethread;
    int32_t idle = -1;  // LockCount of a free section
    if (__atomic_compare_exchange_n(reinterpret_cast<int32_t*>(HostPtr(cs + 0x10)), &idle, 0, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        W32(cs + 0x18, me);
        W32(cs + 0x14, 1);
        return 1;
    }
    if (R32(cs + 0x18) != me) return 0;
    __atomic_add_fetch(reinterpret_cast<int32_t*>(HostPtr(cs + 0x10)), 1, __ATOMIC_SEQ_CST);
    W32(cs + 0x14, R32(cs + 0x14) + 1);
    return 1;
}

// critical regions only hold off kernel APCs, which the host has none of
KERNEL_EXPORT(278, RtlEnterCriticalSectionAndRegion, 1) { return kimpl_RtlEnterCriticalSection(ctx, a); }
KERNEL_EXPORT(295, RtlLeaveCriticalSectionAndRegion, 1) { return kimpl_RtlLeaveCriticalSection(ctx, a); }
KERNEL_EXPORT(101, KeEnterCriticalRegion, 0) { return 0; }
KERNEL_EXPORT(122, KeLeaveCriticalRegion, 0) { return 0; }

// irql, time

KERNEL_FASTCALL(160, KfRaiseIrql) {
    uint8_t old = Irql();
    Irql() = uint8_t(ctx.ecx);
    return old;
}

KERNEL_FASTCALL(161, KfLowerIrql) {
    Irql() = uint8_t(ctx.ecx);
    return 0;
}

KERNEL_EXPORT(129, KeRaiseIrqlToDpcLevel, 0) {
    uint8_t old = Irql();
    Irql() = 2;
    return old;
}

// one processor: synch level is dispatch level
KERNEL_EXPORT(130, KeRaiseIrqlToSynchLevel, 0) {
    uint8_t old = Irql();
    Irql() = 2;
    return old;
}

KERNEL_EXPORT(103, KeGetCurrentIrql, 0) { return Irql(); }
KERNEL_EXPORT(121, KeIsExecutingDpc, 0) { return 0; }

KERNEL_EXPORT(125, KeQueryInterruptTime, 0) {
    uint64_t t = InterruptTime();
    ctx.edx = uint32_t(t >> 32);
    return uint32_t(t);
}

KERNEL_EXPORT(128, KeQuerySystemTime, 1) {
    W64(a[0], uint64_t(Now100ns()));
    return 0;
}

KERNEL_EXPORT(228, NtSetSystemTime, 2) {
    if (a[1]) W64(a[1], uint64_t(Now100ns()));
    return kSuccess;
}

// the console's ACPI timer
constexpr uint64_t kPerfFrequency = 3375000;

KERNEL_EXPORT(126, KeQueryPerformanceCounter, 0) {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    uint64_t ns = uint64_t(duration_cast<nanoseconds>(steady_clock::now() - start).count());
    uint64_t v = ns / 1000000000ull * kPerfFrequency + ns % 1000000000ull * kPerfFrequency / 1000000000ull;
    ctx.edx = uint32_t(v >> 32);
    return uint32_t(v);
}

KERNEL_EXPORT(127, KeQueryPerformanceFrequency, 0) {
    ctx.edx = 0;
    return uint32_t(kPerfFrequency);
}

KERNEL_EXPORT(151, KeStallExecutionProcessor, 1) {
    auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(a[0]);
    while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
    return 0;
}

KERNEL_EXPORT(99, KeDelayExecutionThread, 3) {
    int64_t interval = int64_t(R64(a[2]));
    bool alertable = a[1] & 0xFF;
    Thread* t = Current();
    if (alertable && t && RunApcs(*t)) return kUserApc;
    int64_t deadline = ToDeadline(interval);
    auto now = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    SuspendPoint();
    if (interval == 0) {
        std::this_thread::yield();
        return kSuccess;
    }
    while (now() < deadline) {
        int64_t left = deadline - now();
        std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<int64_t>(left, 2000000)));
        RestartPoint();
        if (alertable && t && RunApcs(*t)) return kUserApc;
    }
    return kSuccess;
}

static int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

KERNEL_EXPORT(304, RtlTimeFieldsToTime, 2) {
    uint32_t tf = a[0];
    int64_t days = DaysFromCivil(int16_t(R16(tf)), R16(tf + 2), R16(tf + 4));
    int64_t secs = days * 86400 + R16(tf + 6) * 3600 + R16(tf + 8) * 60 + R16(tf + 10);
    int64_t t = (secs + 11644473600ll) * 10000000ll + int64_t(R16(tf + 12)) * 10000;
    W64(a[1], uint64_t(t));
    return 1;
}

KERNEL_EXPORT(305, RtlTimeToTimeFields, 2) {
    int64_t t = int64_t(R64(a[0]));
    int64_t ms = (t / 10000) % 1000;
    int64_t secs = t / 10000000 - 11644473600ll;
    int64_t days = secs >= 0 ? secs / 86400 : (secs - 86399) / 86400;
    int64_t rem = secs - days * 86400;
    // civil from days
    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = int64_t(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    uint32_t tf = a[1];
    W16(tf, uint16_t(y));
    W16(tf + 2, uint16_t(m));
    W16(tf + 4, uint16_t(d));
    W16(tf + 6, uint16_t(rem / 3600));
    W16(tf + 8, uint16_t(rem / 60 % 60));
    W16(tf + 10, uint16_t(rem % 60));
    W16(tf + 12, uint16_t(ms));
    // weekday, 1 Jan 1970 was a Thursday
    W16(tf + 14, uint16_t((days + 4) % 7 < 0 ? (days + 4) % 7 + 7 : (days + 4) % 7));
    return 0;
}

// memory

KERNEL_EXPORT(184, NtAllocateVirtualMemory, 5) {
    uint32_t base = R32(a[0]), size = R32(a[2]);
    uint32_t st = mem::AllocateVirtual(base, size, a[3], a[4]);
    if (st == 0) {
        W32(a[0], base);
        W32(a[2], size);
    } else {
        LOGW("NtAllocateVirtualMemory %08X+%X type %X failed %08X", R32(a[0]), R32(a[2]), a[3], st);
    }
    return st;
}

KERNEL_EXPORT(199, NtFreeVirtualMemory, 3) {
    uint32_t base = R32(a[0]), size = R32(a[1]);
    uint32_t st = mem::FreeVirtual(base, size, a[2]);
    if (st == 0) {
        W32(a[0], base);
        W32(a[1], size);
    }
    return st;
}

KERNEL_EXPORT(217, NtQueryVirtualMemory, 2) {
    mem::RegionInfo ri;
    if (!mem::QueryVirtual(a[0], ri)) return kInvalidParameter;
    uint32_t o = a[1];
    W32(o, ri.base);
    W32(o + 4, ri.alloc_base);
    W32(o + 8, ri.alloc_protect);
    W32(o + 12, ri.size);
    W32(o + 16, ri.state);
    W32(o + 20, ri.protect);
    W32(o + 24, ri.type);
    return kSuccess;
}

KERNEL_EXPORT(165, MmAllocateContiguousMemory, 1) { return mem::AllocContiguous(a[0], 0, 0xFFFFFFFF, 0); }
KERNEL_EXPORT(166, MmAllocateContiguousMemoryEx, 5) { return mem::AllocContiguous(a[0], a[1], a[2], a[3]); }
KERNEL_EXPORT(171, MmFreeContiguousMemory, 1) {
    mem::FreeContiguous(a[0]);
    return 0;
}
KERNEL_EXPORT(173, MmGetPhysicalAddress, 1) { return mem::PhysicalFromVirtual(a[0]); }
KERNEL_EXPORT(178, MmPersistContiguousMemory, 3) { return 0; }
KERNEL_EXPORT(175, MmLockUnlockBufferPages, 3) { return 0; }
KERNEL_EXPORT(176, MmLockUnlockPhysicalPage, 2) { return 0; }
KERNEL_EXPORT(180, MmQueryAllocationSize, 1) { return mem::AllocationSize(a[0]); }
KERNEL_EXPORT(182, MmSetAddressProtect, 3) { return 0; }
KERNEL_EXPORT(179, MmQueryAddressProtect, 1) { return 4; }  // PAGE_READWRITE
KERNEL_EXPORT(167, MmAllocateSystemMemory, 2) { return mem::AllocSystem(a[0], 0x1000); }
KERNEL_EXPORT(172, MmFreeSystemMemory, 2) {
    mem::FreeSystem(a[0]);
    return a[1];
}
// returns the end of the claimed block
KERNEL_EXPORT(168, MmClaimGpuInstanceMemory, 2) {
    if (a[1]) W32(a[1], 0);
    return mem::AllocContiguous(a[0], 0, 0xFFFFFFFF, 0x1000) + a[0];
}
KERNEL_EXPORT(169, MmCreateKernelStack, 2) { return mem::AllocStack(a[0]); }
KERNEL_EXPORT(170, MmDeleteKernelStack, 2) {
    mem::FreeStack(a[0]);
    return 0;
}
KERNEL_EXPORT(15, ExAllocatePoolWithTag, 2) { return mem::AllocSystem(a[0]); }
KERNEL_EXPORT(14, ExAllocatePool, 1) { return mem::AllocSystem(a[0]); }
KERNEL_EXPORT(174, MmIsAddressValid, 1) { return mem::IsCommitted(a[0]); }
// every page stays read write
KERNEL_EXPORT(204, NtProtectVirtualMemory, 4) {
    if (a[3]) W32(a[3], 4);
    return kSuccess;
}
KERNEL_EXPORT(17, ExFreePool, 1) {
    mem::FreeSystem(a[0]);
    return 0;
}
KERNEL_EXPORT(23, ExQueryPoolBlockSize, 1) { return mem::AllocationSize(a[0]); }

KERNEL_EXPORT(181, MmQueryStatistics, 1) {
    uint32_t o = a[0];
    if (R32(o) != 36) return kInvalidParameter;  // MM_STATISTICS.Length
    mem::Stats s = mem::Query();
    W32(o + 4, s.total_pages);
    W32(o + 8, s.available_pages);
    W32(o + 12, s.virtual_committed * mem::kPage);
    W32(o + 16, s.virtual_committed * mem::kPage);
    W32(o + 20, 0);
    W32(o + 24, s.system_used);
    W32(o + 28, 0);
    W32(o + 32, 0);
    return kSuccess;
}

// sec is an XBE section header, +0x18 its reference count
KERNEL_EXPORT(327, XeLoadSection, 1) {
    uint32_t sec = a[0];
    uint32_t refs = R32(sec + 0x18);
    if (refs == 0) {
        uint32_t va = R32(sec + 4), vsize = R32(sec + 8), raw = R32(sec + 12), rsize = R32(sec + 16);
        mem::Commit(va, vsize);
        std::memset(HostPtr(va), 0, vsize);
        if (g_image && raw + rsize <= g_image->file.size())
            std::memcpy(HostPtr(va), g_image->file.data() + raw, std::min(rsize, vsize));
        LOGI("XeLoadSection %s at %08X", ReadCString(R32(sec + 0x14)).c_str(), va);
    }
    W32(sec + 0x18, refs + 1);
    return kSuccess;
}

KERNEL_EXPORT(328, XeUnloadSection, 1) {
    uint32_t sec = a[0];
    uint32_t refs = R32(sec + 0x18);
    if (!refs) return kInvalidParameter;
    W32(sec + 0x18, refs - 1);
    return kSuccess;
}

// objects and waits

KERNEL_EXPORT(187, NtClose, 1) { return CloseHandle(a[0]) ? kSuccess : kInvalidHandle; }

KERNEL_EXPORT(197, NtDuplicateObject, 3) {
    uint32_t h = DuplicateHandle(a[0]);
    if (!h) return kInvalidHandle;
    W32(a[1], h);
    if (a[2] & 1) CloseHandle(a[0]);
    return kSuccess;
}

static ObjectRef Named(uint32_t attrs, std::string& name) {
    name = attrs ? ReadAnsiString(R32(attrs + 4)) : std::string();
    return name.empty() ? nullptr : FindNamed(name);
}

KERNEL_EXPORT(189, NtCreateEvent, 4) {
    std::string name;
    if (ObjectRef existing = Named(a[1], name)) {
        W32(a[0], NewHandle(existing));
        return kObjectNameExists;
    }
    auto o = std::make_shared<Object>(ObjType::Event);
    o->notification = a[2] == 0;
    o->signal = (a[3] & 0xFF) ? 1 : 0;
    uint32_t body = mem::AllocSystem(16);
    G<uint8_t>(body) = uint8_t(a[2]);  // NotificationEvent or SynchronizationEvent
    G<uint8_t>(body + 2) = 4;
    W32(body + 4, uint32_t(o->signal));
    Bind(o, body);
    if (!name.empty()) AddNamed(name, o);
    W32(a[0], NewHandle(o));
    return kSuccess;
}

KERNEL_EXPORT(225, NtSetEvent, 2) {
    ObjectRef o = FromHandle(a[0]);
    if (!o) return kInvalidHandle;
    int32_t prev = SetEvent(*o);
    if (a[1]) W32(a[1], uint32_t(prev));
    return kSuccess;
}

KERNEL_EXPORT(186, NtClearEvent, 1) {
    ObjectRef o = FromHandle(a[0]);
    if (!o) return kInvalidHandle;
    ResetEvent(*o);
    return kSuccess;
}

KERNEL_EXPORT(192, NtCreateMutant, 3) {
    std::string name;
    if (ObjectRef existing = Named(a[1], name)) {
        W32(a[0], NewHandle(existing));
        return kObjectNameExists;
    }
    auto o = std::make_shared<Object>(ObjType::Mutant);
    uint32_t body = mem::AllocSystem(32);
    G<uint8_t>(body) = 2;  // MutantObject
    G<uint8_t>(body + 2) = 8;
    if (a[2] & 0xFF) {
        o->owner = Current()->ethread;
        o->recursion = 1;
        o->signal = 0;
    } else {
        o->signal = 1;
    }
    W32(body + 4, uint32_t(o->signal));
    Bind(o, body);
    if (!name.empty()) AddNamed(name, o);
    W32(a[0], NewHandle(o));
    return kSuccess;
}

KERNEL_EXPORT(221, NtReleaseMutant, 2) {
    ObjectRef o = FromHandle(a[0]);
    if (!o) return kInvalidHandle;
    uint32_t st;
    int32_t prev = ReleaseMutant(*o, st);
    if (a[1]) W32(a[1], uint32_t(prev));
    return st;
}

KERNEL_EXPORT(193, NtCreateSemaphore, 4) {
    int32_t count = int32_t(a[2]), limit = int32_t(a[3]);
    if (limit <= 0 || count < 0 || count > limit) return kInvalidParameter;
    std::string name;
    if (ObjectRef existing = Named(a[1], name)) {
        W32(a[0], NewHandle(existing));
        return kObjectNameExists;
    }
    auto o = std::make_shared<Object>(ObjType::Semaphore);
    o->signal = count;
    o->limit = limit;
    uint32_t body = mem::AllocSystem(20);
    G<uint8_t>(body) = 5;  // SemaphoreObject
    G<uint8_t>(body + 2) = 5;
    W32(body + 4, uint32_t(count));
    W32(body + 0x10, uint32_t(limit));
    Bind(o, body);
    if (!name.empty()) AddNamed(name, o);
    W32(a[0], NewHandle(o));
    return kSuccess;
}

KERNEL_EXPORT(222, NtReleaseSemaphore, 3) {
    ObjectRef o = FromHandle(a[0]);
    if (!o) return kInvalidHandle;
    if (o->type != ObjType::Semaphore) return kObjectTypeMismatch;
    uint32_t st;
    int32_t prev = ReleaseSemaphore(*o, int32_t(a[1]), st);
    if (a[2] && st == kSuccess) W32(a[2], uint32_t(prev));
    return st;
}

// dispatcher headers the guest owns: written like the console does, FromPointer rebuilds its object from them
static void InitHeader(uint32_t p, uint8_t type, uint8_t dwords, int32_t state) {
    std::memset(HostPtr(p), 0, dwords * 4u);
    G<uint8_t>(p) = type;
    G<uint8_t>(p + 2) = dwords;
    W32(p + 4, uint32_t(state));
    W32(p + 8, p + 8);
    W32(p + 12, p + 8);
}

KERNEL_EXPORT(108, KeInitializeEvent, 3) {
    InitHeader(a[0], uint8_t(a[1]), 4, a[2] & 0xFF);
    return 0;
}
KERNEL_EXPORT(138, KeResetEvent, 1) { return uint32_t(ResetEvent(*FromPointer(a[0]))); }
KERNEL_EXPORT(123, KePulseEvent, 3) { return uint32_t(PulseEvent(*FromPointer(a[0]))); }
KERNEL_EXPORT(146, KeSetEventBoostPriority, 2) {
    SetEvent(*FromPointer(a[0]));
    return 0;
}

KERNEL_EXPORT(205, NtPulseEvent, 2) {
    ObjectRef o = FromHandle(a[0]);
    if (!o) return kInvalidHandle;
    int32_t prev = PulseEvent(*o);
    if (a[1]) W32(a[1], uint32_t(prev));
    return kSuccess;
}

KERNEL_EXPORT(112, KeInitializeSemaphore, 3) {
    InitHeader(a[0], 5, 5, int32_t(a[1]));  // SemaphoreObject
    W32(a[0] + 0x10, a[2]);  // Limit
    return 0;
}

KERNEL_EXPORT(132, KeReleaseSemaphore, 4) {
    uint32_t st;
    int32_t prev = ReleaseSemaphore(*FromPointer(a[0]), int32_t(a[2]), st);
    if (st != kSuccess) LOG_FIRST(8, "KeReleaseSemaphore %08X over its limit", a[0]);
    return uint32_t(prev);
}

KERNEL_EXPORT(110, KeInitializeMutant, 2) {
    bool owned = a[1] & 0xFF;
    InitHeader(a[0], 2, 8, owned ? 0 : 1);  // MutantObject, signalled while unowned
    if (owned) {
        ObjectRef o = FromPointer(a[0]);
        std::lock_guard<std::mutex> lock(DispatchLock());
        o->owner = Current()->ethread;
        o->recursion = 1;
    }
    return 0;
}

KERNEL_EXPORT(131, KeReleaseMutant, 4) {
    uint32_t st;
    return uint32_t(ReleaseMutant(*FromPointer(a[0]), st));
}

// signals by type: event set, semaphore released once, mutant released
KERNEL_EXPORT(230, NtSignalAndWaitForSingleObjectEx, 5) {
    ObjectRef s = FromHandle(a[0]), w = FromHandle(a[1]);
    if (!s || !w) return kInvalidHandle;
    uint32_t st = kSuccess;
    switch (s->type) {
        case ObjType::Event: SetEvent(*s); break;
        case ObjType::Semaphore: ReleaseSemaphore(*s, 1, st); break;
        case ObjType::Mutant: ReleaseMutant(*s, st); break;
        default: return kObjectTypeMismatch;
    }
    if (st != kSuccess) return st;
    return Wait({w}, false, TimeoutArg(a[4]), a[3] & 0xFF);
}

KERNEL_EXPORT(233, NtWaitForSingleObject, 3) {
    ObjectRef o = FromHandle(a[0]);
    if (!o) return kInvalidHandle;
    return Wait({o}, false, TimeoutArg(a[2]), a[1] & 0xFF);
}

KERNEL_EXPORT(234, NtWaitForSingleObjectEx, 4) {
    ObjectRef o = FromHandle(a[0]);
    if (!o) return kInvalidHandle;
    return Wait({o}, false, TimeoutArg(a[3]), a[2] & 0xFF);
}

KERNEL_EXPORT(235, NtWaitForMultipleObjectsEx, 6) {
    std::vector<ObjectRef> objs;
    for (uint32_t i = 0; i < a[0]; i++) {
        ObjectRef o = FromHandle(R32(a[1] + i * 4));
        if (!o) return kInvalidHandle;
        objs.push_back(o);
    }
    return Wait(objs, a[2] == 0, TimeoutArg(a[5]), a[4] & 0xFF);
}

KERNEL_EXPORT(159, KeWaitForSingleObject, 5) {
    ObjectRef o = FromPointer(a[0]);
    return Wait({o}, false, TimeoutArg(a[4]), a[3] & 0xFF);
}

KERNEL_EXPORT(158, KeWaitForMultipleObjects, 8) {
    std::vector<ObjectRef> objs;
    for (uint32_t i = 0; i < a[0]; i++) objs.push_back(FromPointer(R32(a[1] + i * 4)));
    return Wait(objs, a[2] == 0, TimeoutArg(a[6]), a[5] & 0xFF);
}

KERNEL_EXPORT(145, KeSetEvent, 3) { return uint32_t(SetEvent(*FromPointer(a[0]))); }

KERNEL_EXPORT(246, ObReferenceObjectByHandle, 3) {
    uint32_t h = a[0];
    ObjectRef o = h == 0xFFFFFFFE ? CurrentRef() : FromHandle(h);
    if (!o) return kInvalidHandle;
    if (!o->guest) {
        uint32_t body = mem::AllocSystem(32);
        Bind(o, body);
    }
    W32(a[2], o->guest);
    return kSuccess;
}

KERNEL_EXPORT(247, ObReferenceObjectByName, 5) {
    std::string name = ReadAnsiString(a[0]);
    LOG_FIRST(8, "ObReferenceObjectByName %s", name.c_str());
    return kObjectNameNotFound;
}

KERNEL_FASTCALL(250, ObfDereferenceObject) { return 0; }

// timers, DPCs, interrupts

KERNEL_EXPORT(113, KeInitializeTimerEx, 2) {
    uint32_t t = a[0];
    std::memset(HostPtr(t), 0, 0x28);
    G<uint8_t>(t) = uint8_t(8 + a[1]);  // TimerNotificationObject or TimerSynchronizationObject
    G<uint8_t>(t + 2) = 10;
    W32(t + 8, t + 8);
    W32(t + 12, t + 8);
    return 0;
}

KERNEL_EXPORT(150, KeSetTimerEx, 5) {
    ObjectRef t = FromPointer(a[0]);
    bool was = CancelTimer(*t);
    int64_t due = int64_t(uint64_t(a[1]) | uint64_t(a[2]) << 32);
    W32(a[0] + 0x20, a[4]);  // KTIMER.Dpc
    W32(a[0] + 0x24, a[3]);  // KTIMER.Period
    SetTimer(t, due, int32_t(a[3]), a[4]);
    return was;
}

KERNEL_EXPORT(149, KeSetTimer, 4) {
    ObjectRef t = FromPointer(a[0]);
    bool was = CancelTimer(*t);
    int64_t due = int64_t(uint64_t(a[1]) | uint64_t(a[2]) << 32);
    SetTimer(t, due, 0, a[3]);
    return was;
}

KERNEL_EXPORT(97, KeCancelTimer, 1) { return CancelTimer(*FromPointer(a[0])); }

KERNEL_EXPORT(194, NtCreateTimer, 3) {
    std::string name;
    if (ObjectRef existing = Named(a[1], name)) {
        W32(a[0], NewHandle(existing));
        return kObjectNameExists;
    }
    auto o = std::make_shared<Object>(ObjType::Timer);
    o->notification = a[2] == 0;
    uint32_t body = mem::AllocSystem(0x28);
    InitHeader(body, uint8_t(8 + (a[2] & 1)), 10, 0);  // TimerNotificationObject or TimerSynchronizationObject
    Bind(o, body);
    if (!name.empty()) AddNamed(name, o);
    W32(a[0], NewHandle(o));
    return kSuccess;
}

// timer, due, apc routine, apc mode, apc context, resume, period ms, previous state
KERNEL_EXPORT(229, NtSetTimerEx, 8) {
    ObjectRef o = FromHandle(a[0]);
    if (!o) return kInvalidHandle;
    if (o->type != ObjType::Timer) return kObjectTypeMismatch;
    if (a[7]) G<uint8_t>(a[7]) = o->signal > 0;
    CancelTimer(*o);
    SetTimer(o, int64_t(R64(a[1])), int32_t(a[6]), 0, a[2] ? CurrentRef() : nullptr, a[2], a[4]);
    return kSuccess;
}

KERNEL_EXPORT(185, NtCancelTimer, 2) {
    ObjectRef o = FromHandle(a[0]);
    if (!o) return kInvalidHandle;
    if (a[1]) G<uint8_t>(a[1]) = o->signal > 0;
    CancelTimer(*o);
    return kSuccess;
}

KERNEL_EXPORT(107, KeInitializeDpc, 3) {
    uint32_t d = a[0];
    std::memset(HostPtr(d), 0, 0x1C);
    W16(d, 19);  // DpcObject
    W32(d + 0x0C, a[1]);
    W32(d + 0x10, a[2]);
    return 0;
}

KERNEL_EXPORT(119, KeInsertQueueDpc, 3) {
    uint32_t d = a[0];
    if (G<uint8_t>(d + 2)) return 0;  // already Inserted
    G<uint8_t>(d + 2) = 1;
    QueueDpc(d, a[1], a[2]);
    return 1;
}

KERNEL_EXPORT(137, KeRemoveQueueDpc, 1) {
    uint8_t was = G<uint8_t>(a[0] + 2);
    G<uint8_t>(a[0] + 2) = 0;
    return was;
}

KERNEL_EXPORT(109, KeInitializeInterrupt, 7) { return 0; }
KERNEL_EXPORT(98, KeConnectInterrupt, 1) { return 1; }
KERNEL_EXPORT(100, KeDisconnectInterrupt, 1) { return 1; }
KERNEL_EXPORT(44, HalGetInterruptVector, 2) {
    if (a[1]) G<uint8_t>(a[1]) = uint8_t(a[0]);
    return a[0] + 0x30;  // PRIMARY_VECTOR_BASE
}
KERNEL_EXPORT(153, KeSynchronizeExecution, 3) { return CallGuest(ctx, a[1], {a[2]}) & 0xFF; }

// threads

KERNEL_EXPORT(255, PsCreateSystemThreadEx, 10) {
    uint32_t handle_out = a[0], stack = a[2], tls = a[3], id_out = a[4], start = a[5], context = a[6];
    bool suspended = a[7] & 0xFF;
    uint32_t system = a[9];
    ThreadRef t = CreateThread(stack ? stack : 0x10000, tls, system, start, context, suspended);
    if (handle_out) W32(handle_out, NewHandle(t));
    if (id_out) W32(id_out, t->id);
    StartThread(t);
    return kSuccess;
}

KERNEL_EXPORT(258, PsTerminateSystemThread, 1) { ExitThread(a[0]); }

KERNEL_EXPORT(224, NtResumeThread, 2) {
    ObjectRef o = FromHandle(a[0]);
    if (!o || o->type != ObjType::Thread) return kInvalidHandle;
    auto& t = static_cast<Thread&>(*o);
    if (a[1]) W32(a[1], uint32_t(t.suspend.load()));
    Resume(t);
    return kSuccess;
}

static Thread* ThreadAt(uint32_t kthread) {
    ObjectRef o = FromPointer(kthread);
    return o && o->type == ObjType::Thread ? static_cast<Thread*>(o.get()) : nullptr;
}

KERNEL_EXPORT(231, NtSuspendThread, 2) {
    ObjectRef o = FromHandle(a[0]);
    if (!o || o->type != ObjType::Thread) return kInvalidHandle;
    uint32_t prev = Suspend(static_cast<Thread&>(*o));
    if (a[1]) W32(a[1], prev);
    return kSuccess;
}

KERNEL_EXPORT(152, KeSuspendThread, 1) {
    Thread* t = ThreadAt(a[0]);
    return t ? Suspend(*t) : 0;
}

KERNEL_EXPORT(140, KeResumeThread, 1) {
    Thread* t = ThreadAt(a[0]);
    if (!t) return 0;
    uint32_t prev = uint32_t(std::max(t->suspend.load(), 0));
    Resume(*t);
    return prev;
}
KERNEL_EXPORT(92, KeAlertResumeThread, 1) { return kimpl_KeResumeThread(ctx, a); }

KERNEL_EXPORT(238, NtYieldExecution, 0) {
    SuspendPoint();
    std::this_thread::yield();
    return kSuccess;
}

KERNEL_EXPORT(104, KeGetCurrentThread, 0) { return Current()->ethread; }

KERNEL_EXPORT(254, PsCreateSystemThread, 5) {
    ThreadRef t = CreateThread(0x10000, 0, 0, a[2], a[3], false);
    if (a[0]) W32(a[0], NewHandle(t));
    if (a[1]) W32(a[1], t->id);
    StartThread(t);
    return kSuccess;
}

// user APCs run at the thread's next alertable wait
KERNEL_EXPORT(206, NtQueueApcThread, 5) {
    ObjectRef o = FromHandle(a[0]);
    if (!o || o->type != ObjType::Thread) return kInvalidHandle;
    QueueApc(static_cast<Thread&>(*o), a[1], a[2], a[3], a[4]);
    return kSuccess;
}

KERNEL_EXPORT(148, KeSetPriorityThread, 2) {
    Thread* t = ThreadAt(a[0]);
    if (!t) return 0;
    int old = t->priority;
    t->priority = int32_t(a[1]);
    return uint32_t(old);
}
KERNEL_EXPORT(124, KeQueryBasePriorityThread, 1) {
    Thread* t = ThreadAt(a[0]);
    return t ? uint32_t(t->priority) : 0;
}
KERNEL_EXPORT(144, KeSetDisableBoostThread, 2) { return 0; }

KERNEL_EXPORT(143, KeSetBasePriorityThread, 2) {
    ObjectRef o = FromPointer(a[0]);
    if (!o || o->type != ObjType::Thread) return 0;
    auto& t = static_cast<Thread&>(*o);
    int old = t.priority;
    t.priority = int32_t(a[1]);
    return uint32_t(old);
}

KERNEL_EXPORT(139, KeRestoreFloatingPointState, 1) { return kSuccess; }
KERNEL_EXPORT(142, KeSaveFloatingPointState, 1) { return kSuccess; }

// strings

KERNEL_EXPORT(289, RtlInitAnsiString, 2) {
    uint32_t d = a[0], s = a[1];
    size_t n = s ? std::strlen(reinterpret_cast<const char*>(HostPtr(s))) : 0;
    W16(d, uint16_t(n));
    W16(d + 2, uint16_t(s ? n + 1 : 0));
    W32(d + 4, s);
    return 0;
}

KERNEL_EXPORT(290, RtlInitUnicodeString, 2) {
    uint32_t d = a[0], s = a[1];
    size_t n = 0;
    if (s)
        while (R16(s + uint32_t(n) * 2)) n++;
    W16(d, uint16_t(n * 2));
    W16(d + 2, uint16_t(s ? n * 2 + 2 : 0));
    W32(d + 4, s);
    return 0;
}

KERNEL_EXPORT(260, RtlAnsiStringToUnicodeString, 3) {
    uint32_t d = a[0], s = a[1];
    uint16_t n = R16(s);
    uint32_t src = R32(s + 4);
    if (a[2] & 0xFF) {
        uint32_t buf = mem::AllocSystem(n * 2u + 2);
        W32(d + 4, buf);
        W16(d + 2, uint16_t(n * 2 + 2));
    } else if (R16(d + 2) < n * 2) {
        return kBufferTooSmall;
    }
    uint32_t buf = R32(d + 4);
    for (uint16_t i = 0; i < n; i++) W16(buf + i * 2u, G<uint8_t>(src + i));
    if (R16(d + 2) >= n * 2 + 2) W16(buf + n * 2u, 0);
    W16(d, uint16_t(n * 2));
    return kSuccess;
}

KERNEL_EXPORT(308, RtlUnicodeStringToAnsiString, 3) {
    uint32_t d = a[0], s = a[1];
    uint16_t n = R16(s) / 2;
    uint32_t src = R32(s + 4);
    if (a[2] & 0xFF) {
        uint32_t buf = mem::AllocSystem(n + 1u);
        W32(d + 4, buf);
        W16(d + 2, uint16_t(n + 1));
    } else if (R16(d + 2) < n) {
        return kBufferTooSmall;
    }
    uint32_t buf = R32(d + 4);
    for (uint16_t i = 0; i < n; i++) {
        uint16_t c = R16(src + i * 2u);
        G<uint8_t>(buf + i) = c < 0x100 ? uint8_t(c) : '?';
    }
    if (R16(d + 2) > n) G<uint8_t>(buf + n) = 0;
    W16(d, n);
    return kSuccess;
}

KERNEL_EXPORT(286, RtlFreeAnsiString, 1) {
    uint32_t buf = R32(a[0] + 4);
    if (buf) mem::FreeSystem(buf);
    W32(a[0] + 4, 0);
    W16(a[0], 0);
    W16(a[0] + 2, 0);
    return 0;
}

static int CompareStrings(uint32_t s1, uint32_t s2, bool ci) {
    std::string a1 = ReadAnsiString(s1), a2 = ReadAnsiString(s2);
    size_t n = std::min(a1.size(), a2.size());
    for (size_t i = 0; i < n; i++) {
        int c1 = uint8_t(a1[i]), c2 = uint8_t(a2[i]);
        if (ci) {
            c1 = std::toupper(c1);
            c2 = std::toupper(c2);
        }
        if (c1 != c2) return c1 - c2;
    }
    return int(a1.size()) - int(a2.size());
}

KERNEL_EXPORT(270, RtlCompareString, 3) { return uint32_t(CompareStrings(a[0], a[1], a[2] & 0xFF)); }
KERNEL_EXPORT(279, RtlEqualString, 3) { return CompareStrings(a[0], a[1], a[2] & 0xFF) == 0; }
KERNEL_EXPORT(316, RtlUpperChar, 1) { return uint32_t(std::toupper(int(a[0] & 0xFF))) & 0xFF; }
KERNEL_EXPORT(313, RtlUpcaseUnicodeChar, 1) {
    uint16_t c = uint16_t(a[0]);
    return c < 0x80 ? uint32_t(std::toupper(c)) : c;
}

KERNEL_EXPORT(269, RtlCompareMemoryUlong, 3) {
    uint32_t src = a[0], len = a[1] & ~3u, pat = a[2], n = 0;
    while (n < len && R32(src + n) == pat) n += 4;
    return n;
}

static uint32_t DosError(uint32_t status) {
    switch (status) {
        case kSuccess: return 0;
        case kPending: return 997;
        case kTimeout: return 1460;
        case kNoSuchFile:
        case kObjectNameNotFound: return 2;
        case kObjectPathNotFound: return 3;
        case kAccessDenied:
        case kCannotDelete:
        case kFileIsADirectory: return 5;
        case kInvalidHandle: return 6;
        case kNoMemory: return 8;
        case kNoMoreFiles: return 18;
        case kEndOfFile: return 38;
        case kNotSupported: return 50;
        case kInvalidParameter: return 87;
        case kNotImplemented: return 120;
        case kBufferTooSmall: return 122;
        case kObjectNameInvalid: return 123;
        case kObjectNameCollision: return 183;
        case kNotADirectory: return 267;
        case 0xC000007F: return 112;  // disk full
        case kDeviceNotConnected: return 1167;
        default: return (status & 0xC0000000) ? 317 : 0;
    }
}

KERNEL_EXPORT(301, RtlNtStatusToDosError, 1) { return DosError(a[0]); }

// (completion routine, iosb, reserved): the APC of XAPI's ReadFileEx / WriteFileEx. The iosb is the OVERLAPPED's
// first field, and as on Windows a failed transfer reports no bytes.
KERNEL_EXPORT(232, NtUserIoApcDispatcher, 3) {
    uint32_t status = R32(a[1]);
    bool failed = status >= 0xC0000000;  // NT_ERROR
    CallGuest(ctx, a[0], {failed ? DosError(status) : 0, failed ? 0 : R32(a[1] + 4), a[1]});
    return 0;
}

// exceptions, debug, shutdown

// Exceptions dispatch like NT on x86: each handler of the fs:[0] chain is called (record, frame, context,
// dispatcher context) until one returns ExceptionContinueExecution. A C++ catch never returns: the CRT jumps to the
// continuation, which unwinds the host stack (X86Resume).
namespace {
constexpr uint32_t kContextBytes = 0x400, kUnwinding = 2, kExitUnwind = 4, kContinueExecution = 0;

uint32_t CallHandler(X86Context& ctx, uint32_t rec, uint32_t frame, uint32_t scratch) {
    return CallGuest(ctx, R32(frame + 4), {rec, frame, scratch, scratch + kContextBytes - 8});
}
}  // namespace

KERNEL_EXPORT(302, RtlRaiseException, 1) {
    uint32_t rec = a[0];
    std::string text;
    // a C++ throw carrying a wide string: the text
    if (R32(rec) == 0xE06D7363 && R32(rec + 16) >= 2 && mem::IsCommitted(R32(rec + 24))) {
        for (uint32_t p = R32(R32(rec + 24)), n = 0; mem::IsCommitted(p) && n < 256; n++, p += 2) {
            uint16_t c = R16(p);
            if (!c) break;
            text += c < 0x80 ? char(c) : '?';
        }
        LOG_FIRST(64, "c++ throw: %s", text.c_str());
    }
    uint32_t esp = ctx.esp;
    ctx.esp = (ctx.esp - kContextBytes) & ~15u;
    uint32_t scratch = ctx.esp;
    std::memset(HostPtr(scratch), 0, kContextBytes);
    for (uint32_t frame = R32(ctx.fs_base); frame && frame != 0xFFFFFFFF; frame = R32(frame)) {
        if (CallHandler(ctx, rec, frame, scratch) == kContinueExecution) {
            ctx.esp = esp;
            return 0;
        }
    }
    ctx.esp = esp;
    LOGE("RtlRaiseException code %08X flags %X address %08X params %u, not handled", R32(rec), R32(rec + 4), R32(rec + 12),
         R32(rec + 16));
    for (uint32_t i = 0; i < std::min<uint32_t>(R32(rec + 16), 4); i++) LOGE("  param %u = %08X", i, R32(rec + 20 + i * 4));
    if (!text.empty()) LOGE("  thrown: %s", text.c_str());
    x86_trap(ctx, R32(ctx.esp), "guest raised an exception");
}

// RtlUnwind(target frame, target ip, record, return value): the handlers of the frames above the target run with
// the unwinding flag (destructors), each frame leaving the chain after its handler
KERNEL_EXPORT(312, RtlUnwind, 4) {
    uint32_t target = a[0], rec = a[2];
    uint32_t esp = ctx.esp;
    ctx.esp = (ctx.esp - kContextBytes - 0x60) & ~15u;
    uint32_t scratch = ctx.esp;
    std::memset(HostPtr(scratch), 0, kContextBytes + 0x60);
    if (!rec) {
        rec = scratch + kContextBytes;
        W32(rec, 0xC0000027);  // STATUS_UNWIND
    }
    W32(rec + 4, R32(rec + 4) | kUnwinding | (target ? 0 : kExitUnwind));
    for (uint32_t frame = R32(ctx.fs_base); frame && frame != 0xFFFFFFFF && frame != target; frame = R32(ctx.fs_base)) {
        CallHandler(ctx, rec, frame, scratch);
        W32(ctx.fs_base, R32(frame));
    }
    ctx.esp = esp;
    return a[3];
}

KERNEL_EXPORT(26, ExRaiseException, 1) { return kimpl_RtlRaiseException(ctx, a); }

static std::string FormatGuest(X86Context& ctx, uint32_t fmt_va, uint32_t args_va, bool trim = true) {
    std::string fmt = ReadCString(fmt_va), out;
    uint32_t ap = args_va;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') {
            out += fmt[i];
            continue;
        }
        std::string spec = "%";
        i++;
        while (i < fmt.size() && std::strchr("-+ #0123456789.lhI", fmt[i])) spec += fmt[i++];
        if (i >= fmt.size()) break;
        char c = fmt[i];
        char buf[512];
        if (c == '%') {
            out += '%';
            continue;
        }
        if (c == 's') {
            out += ReadCString(R32(ap), 256);
            ap += 4;
            continue;
        }
        if (c == 'f' || c == 'g' || c == 'e') {
            double d;
            std::memcpy(&d, HostPtr(ap), 8);
            ap += 8;
            std::snprintf(buf, sizeof(buf), (spec + c).c_str(), d);
            out += buf;
            continue;
        }
        std::string clean;
        for (char ch : spec)
            if (ch != 'l' && ch != 'h' && ch != 'I') clean += ch;
        std::snprintf(buf, sizeof(buf), (clean + (c == 'p' ? 'x' : c)).c_str(), R32(ap));
        ap += 4;
        out += buf;
    }
    (void)ctx;
    while (trim && !out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

static void DbgPrintEntry(X86Context& ctx, uint8_t*) {
    LOGI("DbgPrint: %s", FormatGuest(ctx, R32(ctx.esp + 4), ctx.esp + 8).c_str());
    ctx.eax = 0;
    ctx.esp += 4;  // cdecl
}
static const bool kreg_DbgPrint = (RegisterExport(8, "DbgPrint", DbgPrintEntry), true);

// cdecl: RtlSprintf(buffer, format, ...), RtlSnprintf(buffer, count, format, ...); a result longer than count
// fills count bytes unterminated and returns -1
static void SprintfEntry(X86Context& ctx, uint8_t*) {
    std::string s = FormatGuest(ctx, R32(ctx.esp + 8), ctx.esp + 12, false);
    std::memcpy(HostPtr(R32(ctx.esp + 4)), s.c_str(), s.size() + 1);
    ctx.eax = uint32_t(s.size());
    ctx.esp += 4;
}
static void SnprintfEntry(X86Context& ctx, uint8_t*) {
    std::string s = FormatGuest(ctx, R32(ctx.esp + 12), ctx.esp + 16, false);
    uint32_t count = R32(ctx.esp + 8);
    uint32_t n = std::min<uint32_t>(uint32_t(s.size()) + 1, count);
    std::memcpy(HostPtr(R32(ctx.esp + 4)), s.c_str(), n);
    ctx.eax = s.size() < count ? uint32_t(s.size()) : 0xFFFFFFFFu;
    ctx.esp += 4;
}
static const bool kreg_RtlSprintf = (RegisterExport(362, "RtlSprintf", SprintfEntry), true);
static const bool kreg_RtlSnprintf = (RegisterExport(361, "RtlSnprintf", SnprintfEntry), true);

KERNEL_EXPORT(6, DbgBreakPointWithStatus, 1) {
    LOGW("DbgBreakPointWithStatus %08X from %08X", a[0], R32(ctx.esp));
    return 0;
}

KERNEL_EXPORT(264, RtlAssert, 4) {
    LOGE("RtlAssert: %s at %s:%u %s", ReadCString(a[0]).c_str(), ReadCString(a[1]).c_str(), a[2],
         a[3] ? ReadCString(a[3]).c_str() : "");
    return 0;
}

KERNEL_EXPORT(352, RtlRip, 3) {
    LOGE("RtlRip: %s %s %s", a[0] ? ReadCString(a[0]).c_str() : "", a[1] ? ReadCString(a[1]).c_str() : "",
         a[2] ? ReadCString(a[2]).c_str() : "");
    x86_trap(ctx, R32(ctx.esp), "RtlRip");
}

KERNEL_EXPORT(5, DbgBreakPoint, 0) {
    LOGW("DbgBreakPoint from %08X", R32(ctx.esp));
    return 0;
}

KERNEL_EXPORT(95, KeBugCheck, 1) {
    LOGE("KeBugCheck %08X", a[0]);
    x86_trap(ctx, R32(ctx.esp), "KeBugCheck");
}

KERNEL_EXPORT(49, HalReturnToFirmware, 1) {
    LOGI("HalReturnToFirmware(%u): the title asked to leave", a[0]);
    // quick reboot into a recompiled image, the XAPI wrote the launch data page: restart into its executable with it
    uint32_t page = R32(g_launch_data_page_va);
    if (a[0] == 2 && page) {
        std::string path = ReadCString(page + 8, 520);
        if (const char* exe = ImageExe(path)) {
            if (FILE* f = std::fopen(g_launch_file.c_str(), "wb")) {
                std::fwrite(HostPtr(page), 1, 0x1000, f);
                std::fclose(f);
            }
            LOGI("relaunching into %s", path.c_str());
            Relaunch(exe);
        }
        LOGW("HalReturnToFirmware: %s is no recompiled image", path.c_str());
    }
    LogGuestStack(ctx, 256);
    Shutdown(0);
}

KERNEL_EXPORT(360, HalInitiateShutdown, 0) { Shutdown(0); }
KERNEL_EXPORT(358, HalIsResetOrShutdownPending, 0) { return 0; }
KERNEL_EXPORT(47, HalRegisterShutdownNotification, 2) { return 0; }
// the file cache in pages: nothing is cached here, the size only goes back to the title
static uint32_t g_cache_pages = 16;
KERNEL_EXPORT(35, FscGetCacheSize, 0) { return g_cache_pages; }
KERNEL_EXPORT(37, FscSetCacheSize, 1) {
    g_cache_pages = a[0];
    return kSuccess;
}
KERNEL_EXPORT(36, FscInvalidateIdleBlocks, 0) { return kSuccess; }
KERNEL_EXPORT(366, HalWriteSMCScratchRegister, 1) { return 0; }
KERNEL_EXPORT(365, HalEnableSecureTrayEject, 0) { return 0; }
KERNEL_EXPORT(90, IoDismountVolume, 1) { return kSuccess; }
KERNEL_EXPORT(91, IoDismountVolumeByName, 1) { return kSuccess; }
KERNEL_EXPORT(248, ObReferenceObjectByPointer, 2) { return kSuccess; }
KERNEL_FASTCALL(251, ObfReferenceObject) { return 0; }

// the settings are fixed, a write changes nothing
KERNEL_EXPORT(29, ExSaveNonVolatileSetting, 4) {
    LOG_FIRST(16, "ExSaveNonVolatileSetting %X ignored", a[0]);
    return kSuccess;
}

// settings and hardware

// widescreen (a 4:3 output plays the console's 4:3), 480p
static uint32_t VideoFlags() { return (app::Widescreen() ? 0x00010000 : 0) | 0x00080000; }

KERNEL_EXPORT(24, ExQueryNonVolatileSetting, 5) {
    uint32_t index = a[0], type_out = a[1], value = a[2], len = a[3], result_len = a[4];
    uint32_t v = 0;
    switch (index) {
        case 7: v = 1; break;                      // XC_LANGUAGE english
        case 8: v = VideoFlags(); break;          // XC_VIDEO_FLAGS
        case 9: v = 0; break;                      // XC_AUDIO_FLAGS stereo
        case 0x103: v = 0x00400100; break;         // XC_FACTORY_AV_REGION NTSC-M
        case 0x104: v = 1; break;                  // XC_FACTORY_GAME_REGION north america
        case 0x12: v = 1; break;                   // XC_DVD_REGION
        default: break;
    }
    if (value && len) std::memset(HostPtr(value), 0, len);
    if (index == 0xFFFF) {
        // the whole EEPROM image: user settings live at 0xC0 onwards
        if (type_out) W32(type_out, 3);  // REG_BINARY
        if (value && len >= 0x100) {
            W32(value + 0xE8, 1);                     // language
            W32(value + 0xEC, VideoFlags());          // video flags
            W32(value + 0xF0, 0);                     // audio flags, stereo
            W32(value + 0x58, 0x00400100);            // factory video standard
            W32(value + 0x2C, 1);                     // factory game region
        }
        if (result_len) W32(result_len, 0x100);
        return kSuccess;
    }
    if (type_out) W32(type_out, 4);  // REG_DWORD
    if (value && len >= 4) W32(value, v);
    if (result_len) W32(result_len, 4);
    LOG_FIRST(32, "ExQueryNonVolatileSetting %X -> %X", index, v);
    return kSuccess;
}

KERNEL_EXPORT(1, AvGetSavedDataAddress, 0) { return SavedDataAddress(); }
KERNEL_EXPORT(4, AvSetSavedDataAddress, 1) {
    SetSavedDataAddress(a[0]);
    return 0;
}
KERNEL_EXPORT(3, AvSetDisplayMode, 6) { return 0; }
KERNEL_EXPORT(2, AvSendTVEncoderOption, 4) {
    // AV_QUERY_AV_CAPABILITIES: component (HDTV) pack, NTSC-M, 60 Hz and the user video flags, which D3D filters its
    // mode list by; titles read the same flags through XGetVideoFlags and ask for a matching mode
    if (a[3]) W32(a[3], a[1] == 6 ? 0x00400104 | VideoFlags() : 0);
    return 0;
}
KERNEL_EXPORT(46, HalReadWritePCISpace, 6) {
    if (!(a[5] & 0xFF) && a[3]) std::memset(HostPtr(a[3]), 0, a[4]);
    return 0;
}
KERNEL_EXPORT(252, PhyGetLinkState, 1) { return 0; }
KERNEL_EXPORT(253, PhyInitialize, 2) { return kSuccess; }

// I/O manager, not used by the replaced libraries

KERNEL_EXPORT(65, IoCreateDevice, 6) { return kUnsuccessful; }
KERNEL_EXPORT(74, IoInvalidDeviceRequest, 2) { return 0xC0000010; }  // STATUS_INVALID_DEVICE_REQUEST
KERNEL_EXPORT(62, IoBuildSynchronousFsdRequest, 7) { return 0; }
KERNEL_EXPORT(85, IoSynchronousFsdRequest, 5) { return kUnsuccessful; }
KERNEL_EXPORT(84, IoSynchronousDeviceIoControlRequest, 8) {
    if (a[4] && a[5]) std::memset(HostPtr(a[4]), 0, a[5]);
    if (a[6]) W32(a[6], 0);
    return kUnsuccessful;
}
KERNEL_EXPORT(83, IoStartPacket, 3) { return 0; }
KERNEL_EXPORT(81, IoStartNextPacket, 1) { return 0; }
KERNEL_EXPORT(359, IoMarkIrpMustComplete, 1) { return 0; }
KERNEL_FASTCALL(86, IofCallDriver) { return kUnsuccessful; }
KERNEL_FASTCALL(87, IofCompleteRequest) { return 0; }

// crypto

KERNEL_EXPORT(335, XcSHAInit, 1) {
    crypto::ShaInit(HostPtr(a[0]));
    return 0;
}
KERNEL_EXPORT(336, XcSHAUpdate, 3) {
    crypto::ShaUpdate(HostPtr(a[0]), static_cast<const uint8_t*>(HostPtr(a[1])), a[2]);
    return 0;
}
KERNEL_EXPORT(337, XcSHAFinal, 2) {
    crypto::ShaFinal(HostPtr(a[0]), static_cast<uint8_t*>(HostPtr(a[1])));
    return 0;
}
KERNEL_EXPORT(340, XcHMAC, 7) {
    crypto::Hmac(static_cast<const uint8_t*>(HostPtr(a[0])), a[1], a[2] ? static_cast<const uint8_t*>(HostPtr(a[2])) : nullptr,
                 a[3], a[4] ? static_cast<const uint8_t*>(HostPtr(a[4])) : nullptr, a[5], static_cast<uint8_t*>(HostPtr(a[6])));
    return 0;
}
KERNEL_EXPORT(338, XcRC4Key, 3) {
    crypto::Rc4Key(HostPtr(a[0]), static_cast<const uint8_t*>(HostPtr(a[2])), a[1]);
    return 0;
}
KERNEL_EXPORT(339, XcRC4Crypt, 3) {
    crypto::Rc4Crypt(HostPtr(a[0]), static_cast<uint8_t*>(HostPtr(a[2])), a[1]);
    return 0;
}
KERNEL_EXPORT(346, XcDESKeyParity, 2) { return 0; }
KERNEL_EXPORT(347, XcKeyTable, 3) { return 0; }
KERNEL_EXPORT(349, XcBlockCryptCBC, 7) {
    // only the online stack encrypts with DES; plain copy keeps buffers defined
    if (a[2] != a[3]) std::memmove(HostPtr(a[2]), HostPtr(a[3]), a[1]);
    return 0;
}
KERNEL_EXPORT(345, XcModExp, 5) { return 0; }
KERNEL_EXPORT(344, XcVerifyPKCS1Signature, 3) { return 1; }

}  // namespace kernel
