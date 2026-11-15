#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/memory.h"

namespace kernel {

// NTSTATUS values the kernel hands back
enum : uint32_t {
    kSuccess = 0x00000000,
    kWait0 = 0x00000000,
    kAbandoned = 0x00000080,
    kUserApc = 0x000000C0,
    kAlerted = 0x00000101,
    kTimeout = 0x00000102,
    kPending = 0x00000103,
    kObjectNameExists = 0x40000000,
    kNoMoreFiles = 0x80000006,
    kUnsuccessful = 0xC0000001,
    kNotImplemented = 0xC0000002,
    kInvalidHandle = 0xC0000008,
    kInvalidParameter = 0xC000000D,
    kNoSuchFile = 0xC000000F,
    kEndOfFile = 0xC0000011,
    kNoMemory = 0xC0000017,
    kAccessDenied = 0xC0000022,
    kBufferTooSmall = 0xC0000023,
    kObjectTypeMismatch = 0xC0000024,
    kObjectNameInvalid = 0xC0000033,
    kObjectNameNotFound = 0xC0000034,
    kObjectNameCollision = 0xC0000035,
    kObjectPathNotFound = 0xC000003A,
    kMutantNotOwned = 0xC0000046,
    kSemaphoreLimitExceeded = 0xC0000047,
    kNotADirectory = 0xC0000103,
    kFileIsADirectory = 0xC00000BA,
    kNotSupported = 0xC00000BB,
    kDeviceNotConnected = 0xC000009D,
    kCannotDelete = 0xC0000121,
};

extern uint8_t* g_base;

template <typename T>
inline T& G(uint32_t va) {
    return *reinterpret_cast<T*>(g_base + va);
}
inline uint32_t R32(uint32_t va) {
    uint32_t v;
    std::memcpy(&v, g_base + va, 4);
    return v;
}
inline void W32(uint32_t va, uint32_t v) { std::memcpy(g_base + va, &v, 4); }
inline uint16_t R16(uint32_t va) {
    uint16_t v;
    std::memcpy(&v, g_base + va, 2);
    return v;
}
inline void W16(uint32_t va, uint16_t v) { std::memcpy(g_base + va, &v, 2); }
inline uint64_t R64(uint32_t va) {
    uint64_t v;
    std::memcpy(&v, g_base + va, 8);
    return v;
}
inline void W64(uint32_t va, uint64_t v) { std::memcpy(g_base + va, &v, 8); }
inline void* HostPtr(uint32_t va) { return g_base + va; }

// ANSI_STRING / OBJECT_STRING: USHORT Length, USHORT MaximumLength, PCHAR Buffer
std::string ReadAnsiString(uint32_t str);
std::string ReadCString(uint32_t va, size_t max = 4096);

// stdcall argument access for host implementations of guest functions
struct Args {
    X86Context& ctx;
    uint32_t operator[](int i) const { return R32(ctx.esp + 4 + 4 * uint32_t(i)); }
};
inline void Return(X86Context& ctx, uint32_t value, int stack_args) {
    ctx.eax = value;
    ctx.esp += 4 + 4 * uint32_t(stack_args);
}

// runs guest code from the host on the current guest thread
uint32_t CallGuest(X86Context& ctx, uint32_t fn, const uint32_t* args, size_t count);
inline uint32_t CallGuest(X86Context& ctx, uint32_t fn, std::initializer_list<uint32_t> args) {
    return CallGuest(ctx, fn, args.begin(), args.size());
}

// objects
enum class ObjType : uint8_t { Event, Semaphore, Mutant, Timer, Thread, File, SymLink, Device, Queue, Other };

struct Waiter;

struct Object {
    ObjType type;
    uint32_t guest = 0;  // body in guest memory, dispatcher header first for waitables
    // dispatcher state, protected by the dispatcher lock
    int32_t signal = 0;
    int32_t limit = 0;
    bool notification = false;  // manual reset event or notification timer
    uint32_t owner = 0;         // mutant owner thread
    uint32_t recursion = 0;
    std::vector<Waiter*> waiters;  // Waits blocked on it
    std::string name;
    explicit Object(ObjType t) : type(t) {}
    virtual ~Object() = default;
};
using ObjectRef = std::shared_ptr<Object>;

uint32_t NewHandle(ObjectRef obj);
ObjectRef FromHandle(uint32_t handle);
bool CloseHandle(uint32_t handle);
uint32_t DuplicateHandle(uint32_t handle);
ObjectRef FromPointer(uint32_t guest);  // dispatcher object in guest memory, created on first use
void Bind(ObjectRef obj, uint32_t guest);
ObjectRef FindNamed(const std::string& name);
void AddNamed(const std::string& name, ObjectRef obj);

// dispatcher
std::mutex& DispatchLock();
void SignalChanged();  // wakes every waiter, caller holds the lock
void SetSignal(Object& o, int32_t v);
uint32_t Wait(const std::vector<ObjectRef>& objs, bool wait_all, int64_t timeout_100ns, bool alertable);
int32_t SetEvent(Object& o);
int32_t ResetEvent(Object& o);
int32_t PulseEvent(Object& o);
int32_t ReleaseSemaphore(Object& o, int32_t count, uint32_t& status);
int32_t ReleaseMutant(Object& o, uint32_t& status);
int64_t Now100ns();                           // system time, 100 ns since 1601
int64_t ToDeadline(int64_t due_100ns);        // relative or absolute to a steady clock deadline in ns
void QueueDpc(uint32_t dpc, uint32_t arg1, uint32_t arg2);
struct Thread;
// apc_routine runs on apc_thread with (apc_context, due low, due high) each time the timer fires
void SetTimer(ObjectRef timer, int64_t due, int32_t period_ms, uint32_t dpc, std::shared_ptr<Thread> apc_thread = nullptr,
              uint32_t apc_routine = 0, uint32_t apc_context = 0);
bool CancelTimer(Object& timer);

// threads
struct Thread : Object {
    uint32_t id = 0;
    uint32_t kpcr = 0, ethread = 0, stack_top = 0, stack_size = 0, tls = 0;
    uint32_t exit_code = 0;
    int priority = 0;
    std::atomic<int> suspend{0};
    std::mutex apc_lock;
    std::deque<std::array<uint32_t, 4>> apcs;  // routine, arg1, arg2, arg3
    std::condition_variable resume_cv;
    X86Context* ctx = nullptr;
    Thread() : Object(ObjType::Thread) {}
};
using ThreadRef = std::shared_ptr<Thread>;

Thread* Current();
ThreadRef CurrentRef();
void StartHostThread(const char* name, std::function<void(X86Context&)> fn);
void RunDpcs(X86Context& ctx);
void InitKernelData();
void LogGuestStack(X86Context& ctx, int words);
X86Context& CurrentContext();
ThreadRef CreateThread(uint32_t stack_size, uint32_t tls_size, uint32_t system_routine, uint32_t start_routine,
                       uint32_t start_context, bool suspended);
void StartThread(ThreadRef t);
[[noreturn]] void ExitThread(uint32_t code);
void Resume(Thread& t);
uint32_t Suspend(Thread& t);  // previous count; another thread stops at its next kernel wait, delay or yield
void SuspendPoint();           // blocks the current thread while it is suspended
bool RunApcs(Thread& t);
void QueueApc(Thread& t, uint32_t routine, uint32_t a1, uint32_t a2, uint32_t a3);
std::vector<ThreadRef> Threads();

// files
void MountDefaults(const std::string& game_dir, const std::string& data_dir);
bool CreateSymbolicLink(const std::string& link, const std::string& target);
bool DeleteSymbolicLink(const std::string& link);
std::string QuerySymbolicLink(const std::string& link);
std::string ResolvePath(const std::string& xbox_path, uint32_t root_handle);  // host path or empty
bool ReadGameFile(const std::string& xbox_path, std::vector<uint8_t>& out);  // from a host folder or the disc image

// boot
bool Boot(std::vector<uint8_t> xbe, const std::string& xbe_name, const std::string& game_dir, const std::string& data_dir);
[[noreturn]] void Shutdown(int code);
// the executable recompiled from an Xbox image path (D:\\default.xbe), null when there is none
const char* ImageExe(std::string xbe_path);
[[noreturn]] void Relaunch(const char* exe);
extern std::string g_launch_file;
// The display a title keeps up through a quick reboot (D3DDevice_PersistDisplay): its front buffer goes to a file
// beside launch.bin, and the next instance finds it as a surface at AvGetSavedDataAddress.
void SavePersistedDisplay(uint32_t common, uint32_t format, uint32_t size, const void* pixels, uint32_t bytes);
uint32_t SavedDataAddress();
void SetSavedDataAddress(uint32_t surface);
// one bit of note carried with the kept display to the next instance
void MarkPersistedDisplay();
bool KeptDisplayMarked();
// the path of the picture the title last showed, raw guest string with its terminator, carried along too
void SetShownPicture(std::string path);
const std::string& KeptPicture();
// A relaunch hands over without a gap: the new instance's window opens once it has presented, then it signals this
// event and the old instance, waiting in Relaunch with its window still up, exits.
std::wstring HandoverEvent(unsigned long process);
inline std::atomic<bool> g_relaunching{false};

// In-process restart, for hosts that cannot exec (iOS; XBE_RESTART_IN_PROCESS=1 elsewhere): every guest thread, and
// every host thread running guest code, unwinds at its next kernel call, wait, sleep or indirect call; the kernel,
// memory and libraries go back to their first-boot state, and Boot runs again with the launch data page kept.
struct RestartUnwind {};
inline std::atomic<bool> g_restarting{false};
inline void RestartPoint() {
    if (g_restarting.load(std::memory_order_relaxed)) throw RestartUnwind{};
}
bool RestartInProcess();
[[noreturn]] void Restart();
// threads.cpp: wakes suspended threads so they reach a restart point; host threads running guest code still alive
void WakeThreadsForRestart();
int HostGuestThreads();
// first-boot state per part, run by Restart once no guest code runs
void ResetObjects();
void ResetFiles();
void ResetD3D();
void ResetDSound();
void ResetXapi();
void ResetBink();

using KernelFn = void (*)(X86Context& ctx, uint8_t* base);
void RegisterExport(uint32_t ordinal, const char* name, KernelFn fn);
uint32_t RegisterData(uint32_t ordinal, const char* name, uint32_t size);  // allocates kernel data, returns address
uint32_t DataAddress(uint32_t ordinal);
const char* ExportName(uint32_t ordinal);
bool HasExport(uint32_t ordinal);       // implemented as a function or data
const char* OrdinalName(uint32_t ordinal);  // public name, "?" past the table

// guest kernel variables updated by the host
extern uint32_t g_tick_count_va;
extern uint32_t g_launch_data_page_va;

}  // namespace kernel
