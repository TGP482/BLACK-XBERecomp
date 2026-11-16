// The game menus' cursor: cursor.png or cursor.dds beside the exe, untinted, its top-left pixel on the pointer. Its
// visible part is sized, not the image, so padding and resolution don't matter: the longer side is 17 units of the 480
// line page. Without a file the system arrow stays.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "app/app.h"
#include "common/aspect.h"
#include "common/menu.h"
#include "core/log.h"

using Microsoft::WRL::ComPtr;

namespace app {

namespace {

constexpr float kPageHeight = 480.0f;
constexpr float kVisibleSize = 17.0f;
constexpr uint32_t kVisibleAlpha = 32;  // a fainter pixel is edge fringe, not part of the shape
constexpr UINT kPixelBytes = 4;  // BGRA
constexpr const wchar_t* kFiles[] = {L"cursor.png", L"cursor.dds"};

ComPtr<IWICImagingFactory> g_wic;
ComPtr<IWICBitmap> g_image;  // premultiplied BGRA
UINT g_extent = 0;  // the longer side of the visible part, image pixels
bool g_read = false;
HCURSOR g_cursor = nullptr;
HCURSOR g_previous = nullptr;
int g_height = 0;  // the frame height g_cursor is sized for

// the visible part's bounds in image pixels, end exclusive; empty when no pixel is visible
struct VisibleBox {
    UINT x0;
    UINT y0;
    UINT x1;
    UINT y1;
};

VisibleBox MeasureVisible(UINT w, UINT h) {
    std::vector<uint32_t> pixels(size_t(w) * h);
    g_image->CopyPixels(nullptr, w * kPixelBytes, UINT(pixels.size() * kPixelBytes),
                        reinterpret_cast<BYTE*>(pixels.data()));
    VisibleBox box{w, h, 0, 0};
    for (UINT y = 0; y < h; y++) {
        for (UINT x = 0; x < w; x++) {
            if (pixels[size_t(y) * w + x] >> 24 < kVisibleAlpha) continue;
            box.x0 = std::min(box.x0, x);
            box.x1 = std::max(box.x1, x + 1);
            box.y0 = std::min(box.y0, y);
            box.y1 = std::max(box.y1, y + 1);
        }
    }
    return box;
}

void ReadCursorFile() {
    g_read = true;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (const wchar_t* name : kFiles) {
        auto path = ExeDir() / name;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        HRESULT hr = S_OK;
        if (!g_wic) hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
        if (SUCCEEDED(hr)) {
            hr = g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                  &decoder);
        }
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        if (SUCCEEDED(hr)) hr = g_wic->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) {
            hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom);
        }
        if (SUCCEEDED(hr)) hr = g_wic->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, &g_image);
        UINT w = 0;
        UINT h = 0;
        if (SUCCEEDED(hr)) hr = g_image->GetSize(&w, &h);
        if (FAILED(hr)) {
            g_image.Reset();
            LOGW("cursor: %s could not be read (%08X), trying the next", path.string().c_str(), unsigned(hr));
            continue;
        }
        VisibleBox v = MeasureVisible(w, h);
        g_extent = v.x1 > v.x0 ? std::max(v.x1 - v.x0, v.y1 - v.y0) : std::max(w, h);
        LOGI("cursor: %s, %ux%u, visible %u..%u x %u..%u", path.string().c_str(), w, h, v.x0, v.x1, v.y0, v.y1);
        return;
    }
}

// premultiplied to straight alpha, as cursor bitmaps want it
void Unpremultiply(std::vector<uint32_t>& pixels) {
    for (uint32_t& p : pixels) {
        uint32_t a = p >> 24;
        if (!a || a == 255) continue;
        uint32_t c = 0;
        for (int s = 0; s < 24; s += 8) c |= std::min(255u, ((p >> s & 0xFF) * 255 + a / 2) / a) << s;
        p = a << 24 | c;
    }
}

// a top down 32 bit cursor, its hot spot the top left pixel
HCURSOR CursorFromPixels(const std::vector<uint32_t>& pixels, UINT cw, UINT ch) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = LONG(cw);
    header.bV5Height = -LONG(ch);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;
    std::copy(pixels.begin(), pixels.end(), static_cast<uint32_t*>(bits));
    HBITMAP mask = CreateBitmap(int(cw), int(ch), 1, 1, nullptr);
    ICONINFO info{FALSE, 0, 0, mask, color};
    HCURSOR cursor = CreateIconIndirect(&info);
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    return cursor;
}

// the image scaled to the frame
HCURSOR Build(int frame_height) {
    UINT w = 0;
    UINT h = 0;
    g_image->GetSize(&w, &h);
    float scale = frame_height * kVisibleSize / kPageHeight / float(g_extent);
    UINT cw = std::max(1u, UINT(std::lround(w * scale)));
    UINT ch = std::max(1u, UINT(std::lround(h * scale)));
    ComPtr<IWICBitmapScaler> scaler;
    std::vector<uint32_t> pixels(size_t(cw) * ch);
    BYTE* bytes = reinterpret_cast<BYTE*>(pixels.data());
    if (FAILED(g_wic->CreateBitmapScaler(&scaler))) return nullptr;
    if (FAILED(scaler->Initialize(g_image.Get(), cw, ch, WICBitmapInterpolationModeHighQualityCubic))) return nullptr;
    if (FAILED(scaler->CopyPixels(nullptr, cw * kPixelBytes, UINT(pixels.size() * kPixelBytes), bytes))) return nullptr;
    Unpremultiply(pixels);
    return CursorFromPixels(pixels, cw, ch);
}

}  // namespace

void* ArrowCursor() {
    static HCURSOR arrow = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    return arrow;
}

void ShowCursorImage(void* cursor) { SetCursor(static_cast<HCURSOR>(cursor)); }

void* CursorImage() {
    HCURSOR arrow = static_cast<HCURSOR>(ArrowCursor());
    if (!g_read) ReadCursorFile();
    RECT r;
    HWND hwnd = static_cast<HWND>(Window());
    if (!g_image || !hwnd || !GetClientRect(hwnd, &r)) return arrow;
    // the frame is shown in the middle of the window
    float aspect = game::Aspect();
    int frame_height = int(std::min(float(r.bottom), r.right / aspect));
    if (frame_height <= 0) return arrow;
    if (frame_height != g_height || !g_cursor) {
        HCURSOR cursor = Build(frame_height);
        if (!cursor) return arrow;
        // the one on screen goes a rebuild later, never while it is shown
        if (g_previous) DestroyCursor(g_previous);
        g_previous = g_cursor;
        g_cursor = cursor;
        g_height = frame_height;
    }
    return g_cursor;
}

}  // namespace app
