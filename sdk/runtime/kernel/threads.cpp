#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "kernel/kernel.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace kernel {

namespace {

thread_local ThreadRef t_current;
std::mutex g_threads_lock;
std::vector<ThreadRef> g_threads;
std::atomic<uint32_t> g_next_id{0x10};
std::atomic<int> g_host_guest_threads{0};

// guest structures per thread
constexpr uint32_t kKpcrSize = 0x400;
constexpr uint32_t kEthreadSize = 0x200;

void SetupGuestBlocks(Thread& t, uint32_t stack_size, uint32_t tls_size) {
    stack_size = std::max<uint32_t>(stack_size, 0x10000);
    tls_size = (tls_size + 3) & ~3u;
    t.stack_top = mem::AllocStack(stack_size + tls_size + 0x1000);
    t.stack_size = stack_size;
    t.kpcr = mem::AllocSystem(kKpcrSize, 64);
    t.ethread = mem::AllocSystem(kEthreadSize, 64);
    // TLS sits at the top of the stack: StackBase + tls_index * 4 must equal TlsData
    t.tls = t.stack_top - tls_size;
    uint32_t k = t.kpcr;
    W32(k + 0x00, 0xFFFFFFFF);                  // NtTib.ExceptionList
    W32(k + 0x04, t.stack_top);                 // NtTib.StackBase
    W32(k + 0x08, t.stack_top - stack_size);    // NtTib.StackLimit
    W32(k + 0x18, k);                           // NtTib.Self
    W32(k + 0x1C, k);                           // SelfPcr
    W32(k + 0x20, k + 0x28);                    // Prcb
    G<uint8_t>(k + 0x24) = 0;                   // Irql
    W32(k + 0x28, t.ethread);                   // PrcbData.CurrentThread
    uint32_t e = t.ethread;
    G<uint8_t>(e + 0) = 6;                      // dispatcher type ThreadObject
    G<uint8_t>(e + 2) = uint8_t(kEthreadSize / 4);  // Header.Size in dwords
    W32(e + 0x1C, t.stack_top);                 // KTHREAD.StackBase
    W32(e + 0x20, t.stack_top - stack_size);    // KTHREAD.StackLimit
    W32(e + 0x28, t.tls);                       // KTHREAD.TlsData
    W32(e + 0x12C, t.id);                       // ETHREAD.UniqueThread
}

void InitContext(Thread& t, X86Context& ctx) {
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.esp = (t.tls & ~15u) - 16;
    ctx.fs_base = t.kpcr;
    ctx.fcw = 0x27F;  // x87 53 bit precision, exceptions masked
    ctx.mxcsr = 0x1F80;  // SSE exceptions masked, round to nearest
    ctx.thread_handle = 0;
    t.ctx = &ctx;
}

void Register(const ThreadRef& t) {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    g_threads.push_back(t);
}

void Unregister(Thread* t) {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    g_threads.erase(std::remove_if(g_threads.begin(), g_threads.end(), [t](const ThreadRef& r) { return r.get() == t; }),
                    g_threads.end());
}

void MarkExited(Thread& t, uint32_t code) {
    t.exit_code = code;
    std::lock_guard<std::mutex> lock(DispatchLock());
    SetSignal(t, 1);
    SignalChanged();
}

void NameHostThread(const char* name) {
#ifdef _WIN32
    wchar_t w[64];
    size_t i = 0;
    for (; name[i] && i < 63; i++) w[i] = wchar_t(name[i]);
    w[i] = 0;
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static Fn set = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (set) set(GetCurrentThread(), w);
#elif defined(__APPLE__)
    pthread_setname_np(name);  // Apple's names the calling thread only
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

// recompiled code nests host frames as deep as the guest's calls, give it room
void Spawn(std::function<void()> fn) {
    auto* f = new std::function<void()>(std::move(fn));
#ifdef _WIN32
    auto entry = [](void* p) -> DWORD {
        std::unique_ptr<std::function<void()>> g(static_cast<std::function<void()>*>(p));
        (*g)();
        return 0;
    };
    HANDLE h = ::CreateThread(nullptr, 64u << 20, entry, f, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (h) ::CloseHandle(h);
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t th;
    pthread_create(&th, &attr, [](void* p) -> void* {
        std::unique_ptr<std::function<void()>> g(static_cast<std::function<void()>*>(p));
        (*g)();
        return nullptr;
    }, f);
    pthread_detach(th);
    pthread_attr_destroy(&attr);
#endif
}

}  // namespace

Thread* Current() { return t_current.get(); }
ThreadRef CurrentRef() { return t_current; }
X86Context& CurrentContext() { return *t_current->ctx; }

std::vector<ThreadRef> Threads() {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    return g_threads;
}

struct StartInfo {
    uint32_t system_routine, start_routine, start_context;
};
static std::unordered_map<Thread*, StartInfo> g_start;
static std::mutex g_start_lock;

ThreadRef CreateThread(uint32_t stack_size, uint32_t tls_size, uint32_t system_routine, uint32_t start_routine,
                       uint32_t start_context, bool suspended) {
    auto t = std::make_shared<Thread>();
    t->id = g_next_id.fetch_add(4);
    SetupGuestBlocks(*t, stack_size, tls_size);
    Bind(t, t->ethread);
    if (suspended) t->suspend = 1;
    std::lock_guard<std::mutex> lock(g_start_lock);
    g_start[t.get()] = {system_routine, start_routine, start_context};
    return t;
}

void StartThread(ThreadRef t) {
    Register(t);
    Spawn([t] {
        t_current = t;
        char name[32];
        std::snprintf(name, sizeof(name), "guest %X", t->id);
        NameHostThread(name);
        X86Context ctx;
        InitContext(*t, ctx);
        {
            std::unique_lock<std::mutex> lock(t->apc_lock);
            t->resume_cv.wait(lock, [&] { return t->suspend.load() <= 0 || g_restarting.load(); });
        }
        StartInfo si;
        {
            std::lock_guard<std::mutex> lock(g_start_lock);
            si = g_start[t.get()];
            g_start.erase(t.get());
        }
        LOGI("thread %X starts at %08X (%08X, %08X)", t->id, si.system_routine ? si.system_routine : si.start_routine,
             si.start_routine, si.start_context);
        uint32_t code = 0;
        try {
            RestartPoint();
            if (si.system_routine)
                code = CallGuest(ctx, si.system_routine, {si.start_routine, si.start_context});
            else
                code = CallGuest(ctx, si.start_routine, {si.start_context});
            LOGI("thread %X returned %08X", t->id, code);
        } catch (const RestartUnwind&) {
            LOGI("thread %X stopped for the restart", t->id);
        }
        MarkExited(*t, code);
        Unregister(t.get());
        t_current.reset();
    });
}

void StartHostThread(const char* name, std::function<void(X86Context&)> fn) {
    auto t = std::make_shared<Thread>();
    t->id = g_next_id.fetch_add(4);
    SetupGuestBlocks(*t, 0x20000, 0x100);
    Bind(t, t->ethread);
    std::string n = name;
    g_host_guest_threads++;
    Spawn([t, fn, n] {
        t_current = t;
        NameHostThread(n.c_str());
        X86Context ctx;
        InitContext(*t, ctx);
        try {
            fn(ctx);
        } catch (const RestartUnwind&) {
            LOGI("%s stopped for the restart", n.c_str());
        }
        t_current.reset();
        g_host_guest_threads--;
    });
}

void ExitThread(uint32_t code) {
    Thread* t = Current();
    if (t) {
        LOGI("thread %X exits with %08X", t->id, code);
        MarkExited(*t, code);
        Unregister(t);
    }
    t_current.reset();
#ifdef _WIN32
    ::ExitThread(code);
#else
    pthread_exit(nullptr);
#endif
}

void Resume(Thread& t) {
    std::lock_guard<std::mutex> lock(t.apc_lock);
    if (t.suspend > 0) t.suspend--;
    t.resume_cv.notify_all();
}

uint32_t Suspend(Thread& t) {
    uint32_t prev = uint32_t(t.suspend++);
    if (&t == Current()) SuspendPoint();
    return prev;
}

void SuspendPoint() {
    RestartPoint();
    Thread* t = Current();
    if (!t || t->suspend.load() <= 0) return;
    {
        std::unique_lock<std::mutex> lock(t->apc_lock);
        t->resume_cv.wait(lock, [&] { return t->suspend.load() <= 0 || g_restarting.load(); });
    }
    RestartPoint();
}

void WakeThreadsForRestart() {
    for (const ThreadRef& t : Threads()) {
        std::lock_guard<std::mutex> lock(t->apc_lock);
        t->resume_cv.notify_all();
    }
    SignalChanged();
}

int HostGuestThreads() { return g_host_guest_threads.load(); }

void QueueApc(Thread& t, uint32_t routine, uint32_t a1, uint32_t a2, uint32_t a3) {
    {
        std::lock_guard<std::mutex> lock(t.apc_lock);
        t.apcs.push_back({routine, a1, a2, a3});
    }
    std::lock_guard<std::mutex> lock(DispatchLock());
    SignalChanged();
}

bool RunApcs(Thread& t) {
    bool ran = false;
    for (;;) {
        std::array<uint32_t, 4> a;
        {
            std::lock_guard<std::mutex> lock(t.apc_lock);
            if (t.apcs.empty()) break;
            a = t.apcs.front();
            t.apcs.pop_front();
        }
        CallGuest(*t.ctx, a[0], {a[1], a[2], a[3]});
        ran = true;
    }
    return ran;
}

}  // namespace kernel
