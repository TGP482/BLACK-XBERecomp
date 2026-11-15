#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <thread>

#include "app/app.h"
#include "kernel/kernel.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#endif
#endif
#include "xbe/xbe.h"

namespace dispatch {
void Init();
uint32_t EntryPoint();
#ifndef _WIN32
bool LoadSelf();  // the generated code is in this image, not a dylib
bool Load(const std::filesystem::path& dll);  // cpu/dispatch.cpp, as on Windows
#endif
uint32_t KernelThunkAddress(uint32_t ordinal);
}  // namespace dispatch

namespace xnet {
void CloseSockets();  // xdk/xnet.cpp
}

namespace kernel {

uint8_t* g_base = nullptr;
std::string g_launch_file;

namespace {
uint32_t g_saved_data = 0;
bool g_mark = false, g_kept_marked = false;
std::string g_picture, g_kept_picture;  // the last picture the title showed, raw path string with its terminator
constexpr uint32_t kPersistMagic = 0x50535044;  // "DPSP"

std::string PersistFile() { return g_launch_file.substr(0, g_launch_file.find_last_of("/\\") + 1) + "display.bin"; }

// surface header (Common, Data, Lock, Format, Size) then the pixels at +0x40, as the XDK lays out its persisted copy
void LoadPersistedDisplay() {
    std::ifstream f(PersistFile(), std::ios::binary);
    uint32_t head[7] = {};
    if (!f.read(reinterpret_cast<char*>(head), sizeof head) || head[0] != kPersistMagic || !head[4] || head[4] > 0x800000 ||
        head[6] > 1040)
        return;
    uint32_t block = mem::AllocContiguous(0x40 + head[4], 0, 0xFFFFFFFF, 0x1000);
    if (!block || !f.read(static_cast<char*>(HostPtr(block + 0x40)), head[4])) return;
    g_kept_picture.resize(head[6]);
    if (!f.read(g_kept_picture.data(), head[6])) g_kept_picture.clear();
    W32(block, (head[1] & 0xFFFF0000) | 1);
    W32(block + 4, mem::PhysicalFromVirtual(block + 0x40));
    W32(block + 8, 0);
    W32(block + 0xC, head[2]);
    W32(block + 0x10, head[3]);
    g_saved_data = block;
    g_kept_marked = head[5] != 0;
    LOGI("boot: display kept from the previous run, %u bytes%s", head[4], g_kept_marked ? ", marked" : "");
}
}  // namespace

void SavePersistedDisplay(uint32_t common, uint32_t format, uint32_t size, const void* pixels, uint32_t bytes) {
    if (FILE* f = std::fopen(PersistFile().c_str(), "wb")) {
        uint32_t head[7] = {kPersistMagic, common, format, size, bytes, g_mark, uint32_t(g_picture.size())};
        std::fwrite(head, sizeof head, 1, f);
        std::fwrite(pixels, 1, bytes, f);
        std::fwrite(g_picture.data(), 1, g_picture.size(), f);
        std::fclose(f);
    }
}

void MarkPersistedDisplay() { g_mark = true; }
bool KeptDisplayMarked() { return g_kept_marked; }
void SetShownPicture(std::string path) { g_picture = std::move(path); }
const std::string& KeptPicture() { return g_kept_picture; }

uint32_t SavedDataAddress() { return g_saved_data; }
void SetSavedDataAddress(uint32_t surface) { g_saved_data = surface; }
const xbe::Image* g_image = nullptr;
static xbe::Image g_xbe;

// what Boot was given, for an in-process restart
static struct {
    std::vector<uint8_t> xbe;
    std::string name, game_dir, data_dir;
} g_boot;

bool Boot(std::vector<uint8_t> xbe, const std::string& xbe_name, const std::string& game_dir, const std::string& data_dir) {
    if (g_boot.xbe.empty()) g_boot = {xbe, xbe_name, game_dir, data_dir};
    if (!mem::Init()) return false;
    g_base = mem::Base();

#ifndef _WIN32
    if (dispatch::EntryPoint() == 0) {
        // a release ships the host only: the first start builds the game's code beside it
        std::error_code ec;
        std::filesystem::path code = app::ExeDir() / "BLACK.dylib";
        if (!std::filesystem::exists(code, ec)) code = app::ExeDir() / "compiler" / "BLACK.dylib";
        if (!std::filesystem::exists(code, ec) || !dispatch::Load(code)) {
            if (!dispatch::LoadSelf()) {
                LOGE("boot: no recompiled code: run compiler/build.sh, or put BLACK.dylib beside the game");
                return false;
            }
        }
    }
#endif
    std::string err;
    if (!g_xbe.Parse(std::move(xbe), err)) {
        LOGE("boot: %s: %s", xbe_name.c_str(), err.c_str());
        return false;
    }
    g_image = &g_xbe;
    const xbe::Image& img = g_xbe;
    if (img.entry != dispatch::EntryPoint()) {
        LOGE("boot: %s entry %08X does not match the recompiled entry %08X, wrong executable", xbe_name.c_str(), img.entry,
             dispatch::EntryPoint());
        return false;
    }
    LOGI("boot: title %08X, entry %08X, %zu sections", img.title_id, img.entry, img.sections.size());
    for (auto& l : img.libraries) LOGI("boot: library %-8s %u.%u.%u", l.name.c_str(), l.major, l.minor, l.build);

    mem::ReserveImage(img.base, img.image_size);
    std::memcpy(g_base + img.base, img.file.data(), std::min<size_t>(img.header_size, img.file.size()));
    // XBE header 0x120 points at the section headers, 56 bytes each, +0x18 is the reference count that marks one loaded
    uint32_t sec_hdr = img.U32(0x120);
    for (size_t i = 0; i < img.sections.size(); i++) {
        auto& s = img.sections[i];
        if (!(s.flags & xbe::kPreload)) continue;
        std::memcpy(g_base + s.va, img.file.data() + s.raw, std::min(s.rsize, s.vsize));
        W32(sec_hdr + uint32_t(i) * 56 + 0x18, 1);
    }
    if (app::kGame.loaded) app::kGame.loaded();

    InitKernelData();
    dispatch::Init();

    uint32_t missing = 0;
    for (uint32_t slot = img.kernel_thunk;; slot += 4) {
        uint32_t v = R32(slot);
        if (!v) break;
        uint32_t ord = v & 0x7FFFFFFF;  // high bit flags an import by ordinal
        uint32_t data = DataAddress(ord);
        W32(slot, data ? data : dispatch::KernelThunkAddress(ord));
        if (!HasExport(ord)) {
            LOGW("boot: kernel import %u (%s) has no implementation", ord, ExportName(ord));
            missing++;
        }
    }
    LOGI("boot: kernel imports resolved, %u missing", missing);

    MountDefaults(game_dir, data_dir);

    // a quick reboot into this title left its launch data page behind
    g_launch_file = data_dir + "/launch.bin";
    std::ifstream lf(g_launch_file, std::ios::binary);
    if (lf) {
        uint32_t page = mem::AllocContiguous(0x1000, 0, 0xFFFFFFFF, 0x1000);
        lf.read(static_cast<char*>(HostPtr(page)), 0x1000);
        lf.close();
        std::remove(g_launch_file.c_str());
        W32(g_launch_data_page_va, page);
        LOGI("boot: launch data from the previous run, type %u", R32(page));
        LoadPersistedDisplay();
    }
    std::remove(PersistFile().c_str());

    uint32_t tls = 0;
    if (img.tls_addr) tls = ((img.tls.data_end - img.tls.data_start + img.tls.zero_fill + 15) & ~15u) + 4;
    ThreadRef t = CreateThread(std::max<uint32_t>(img.stack_commit, 0x10000), tls, 0, img.entry, 0, false);
    StartThread(t);
    return true;
}

const char* ImageExe(std::string xbe_path) {
    for (char& c : xbe_path) c = char(std::tolower(uint8_t(c)));
    // Xbox paths ignore case; a row keeps the xbe's own name, which the code DLL and config lookups compare exactly
    for (auto& [xbe, exe] : app::kGame.images) {
        std::string name = xbe;
        for (char& c : name) c = char(std::tolower(uint8_t(c)));
        size_t n = name.size();
        if (xbe_path.size() >= n && xbe_path.compare(xbe_path.size() - n, n, name) == 0 &&
            (xbe_path.size() == n || xbe_path[xbe_path.size() - n - 1] == '\\' || xbe_path[xbe_path.size() - n - 1] == ';'))
            return exe;
    }
    return nullptr;
}

std::wstring HandoverEvent(unsigned long process) {
    return L"Local\\" + std::wstring(app::kGame.name) + L"Handover-" + std::to_wstring(process);
}

bool RestartInProcess() {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    return true;  // iOS has no exec
#else
    const char* e = std::getenv("XBE_RESTART_IN_PROCESS");
    return e && *e == '1';
#endif
}

// On the guest thread that asked for the reboot: every other thread unwinds at its next restart point, this one
// unwinds from here. A host thread waits until no guest code runs, puts everything back to its first-boot state and
// boots the image again; launch.bin and display.bin, written before, carry the launch data and the kept display.
void Restart() {
    LOGI("restart: in process");
    logx::Flush();
    g_restarting = true;
    WakeThreadsForRestart();
    std::thread([] {
        using clock = std::chrono::steady_clock;
        auto t0 = clock::now();
        while ((!Threads().empty() || HostGuestThreads() > 0) && clock::now() - t0 < std::chrono::seconds(5)) {
            WakeThreadsForRestart();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
        if (!Threads().empty() || HostGuestThreads() > 0) {
            LOGE("restart: %zu guest threads and %d host threads still run after %lld ms", Threads().size(),
                 HostGuestThreads(), (long long)ms);
            for (const ThreadRef& t : Threads()) LOGE("restart: thread %X still runs", t->id);
            Shutdown(1);
        }
#ifndef __APPLE__
        // the mac port relaunches by exec, so the replaced D3D and sound backends carry no in-process reset
        ResetD3D();
        ResetDSound();
#endif
        ResetXapi();
        ResetBink();
        ResetObjects();
        ResetFiles();
        xnet::CloseSockets();
        mem::Reset();
        g_saved_data = 0;
        g_mark = false;
        g_picture.clear();
        g_restarting = false;
        LOGI("restart: the guest stopped in %lld ms, booting %s again", (long long)ms, g_boot.name.c_str());
        if (!Boot(g_boot.xbe, g_boot.name, g_boot.game_dir, g_boot.data_dir)) Shutdown(1);
    }).detach();
    throw RestartUnwind{};
}

void Relaunch(const char* exe) {
    if (RestartInProcess()) Restart();
    logx::Flush();
#ifdef _WIN32
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring path = self;
    path = path.substr(0, path.find_last_of(L'\\') + 1) + std::wstring(exe, exe + std::strlen(exe)) + L".exe";
    // the command line keeps its arguments (--instance) behind the new executable
    std::wstring line = GetCommandLineW();
    size_t args = line[0] == L'"' ? line.find(L'"', 1) + 1 : line.find(L' ');
    line = L"\"" + path + L"\"" + (args == std::wstring::npos ? L"" : line.substr(args));
    if (line.find(L"--relaunched") == std::wstring::npos) line += L" --relaunched";  // no boot splash again
    // this process lives on through the handover: the new one binds the same ports
    xnet::CloseSockets();
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(path.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        // this window stays up until the new instance shows its first frames (HandoverEvent), so no gap shows
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, HandoverEvent(pi.dwProcessId).c_str());
        AllowSetForegroundWindow(pi.dwProcessId);
        g_relaunching = true;
        ResumeThread(pi.hThread);
        HANDLE wait[] = {ready, pi.hProcess};
        if (ready) WaitForMultipleObjects(2, wait, FALSE, 15000);
        if (ready) ::CloseHandle(ready);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    } else {
        LOGE("relaunch: cannot start %s.exe (%lu)", exe, GetLastError());
    }
    std::_Exit(0);
#else
    char self[4096] = {};
#ifdef __APPLE__
    uint32_t size = sizeof(self);
    ssize_t n = _NSGetExecutablePath(self, &size) == 0 ? ssize_t(std::strlen(self)) : -1;
#else
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
#endif
    std::string path = std::string(self, n > 0 ? size_t(n) : 0);
    path = path.substr(0, path.find_last_of('/') + 1) + exe;
    execl(path.c_str(), path.c_str(), nullptr);
    std::_Exit(0);
#endif
}

void Shutdown(int code) {
    LOGR("Exit (%d)", code);
    logx::Flush();
    std::_Exit(code);
}

}  // namespace kernel
