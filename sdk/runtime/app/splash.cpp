// The boot splash: GameInfo::splash .bmp or .png beside the exe, centred in a borderless window. The game boots behind
// it with its window hidden and waits at its first frame until the splash has been up kMinimum, since this boot takes
// a fraction of a second. No file, or a relaunch: no splash.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/log.h"

using Microsoft::WRL::ComPtr;

namespace app {

namespace {

constexpr auto kMinimum = std::chrono::seconds(1);

HWND g_splash = nullptr;
std::atomic<bool> g_waiting{false};
std::chrono::steady_clock::time_point g_until;

// premultiplied BGRA, as UpdateLayeredWindow wants it; a picture without alpha comes out opaque
bool Read(const std::filesystem::path& path, std::vector<uint32_t>& pixels, UINT& w, UINT& h) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = converter->GetSize(&w, &h);
    if (SUCCEEDED(hr) && w && h) {
        pixels.resize(size_t(w) * h);
        hr = converter->CopyPixels(nullptr, w * 4, UINT(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data()));
    }
    if (FAILED(hr) || !w || !h) {
        LOGW("splash: %s could not be read (%08X)", path.string().c_str(), unsigned(hr));
        return false;
    }
    return true;
}

}  // namespace

bool OpenSplash() {
    if (std::wcsstr(GetCommandLineW(), L"--relaunched")) return false;
    std::vector<uint32_t> pixels;
    UINT w = 0, h = 0;
    std::filesystem::path path;
    for (const wchar_t* ext : {L".bmp", L".png"}) {
        std::error_code ec;
        path = ExeDir() / (std::wstring(kGame.splash) + ext);
        if (std::filesystem::exists(path, ec) && Read(path, pixels, w, h)) break;
        pixels.clear();
    }
    if (pixels.empty()) return false;

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32514));  // IDC_WAIT
    std::wstring cls = std::wstring(kGame.name) + L"Splash";
    wc.lpszClassName = cls.c_str();
    RegisterClassExW(&wc);
    // centred on the primary screen at one image pixel per screen pixel
    POINT at{(GetSystemMetrics(SM_CXSCREEN) - int(w)) / 2, (GetSystemMetrics(SM_CYSCREEN) - int(h)) / 2};
    g_splash = CreateWindowExW(WS_EX_LAYERED, wc.lpszClassName, kGame.title, WS_POPUP,
                               at.x, at.y, int(w), int(h), nullptr, nullptr, inst, nullptr);
    if (!g_splash) return false;

    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), LONG(w), -LONG(h), 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) {
        std::memcpy(bits, pixels.data(), pixels.size() * 4);
        HGDIOBJ old = SelectObject(dc, bitmap);
        SIZE size{LONG(w), LONG(h)};
        POINT origin{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(g_splash, screen, &at, &size, dc, &origin, 0, &blend, ULW_ALPHA);
        SelectObject(dc, old);
    }
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    // to the front once, without staying on top
    SetWindowPos(g_splash, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(g_splash, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    g_until = std::chrono::steady_clock::now() + kMinimum;
    g_waiting = true;
    LOGI("splash: %s, %ux%u", path.string().c_str(), w, h);
    return true;
}

void SplashFirstFrame() {
    if (!g_waiting.exchange(false)) return;
    std::this_thread::sleep_until(g_until);
    OpenGameWindow();
}

void CloseSplash() {
    if (g_splash) DestroyWindow(g_splash);
    g_splash = nullptr;
}

}  // namespace app
