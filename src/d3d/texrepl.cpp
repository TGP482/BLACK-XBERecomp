// Texture replacement and dumping; see texrepl.h. Same key, folder layout and file rules as black-pc's loader.
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
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "d3d/texrepl.h"

using Microsoft::WRL::ComPtr;

namespace gpu::texrepl {

namespace {

constexpr uint32_t kMaxSize = 16384;  // D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION

std::mutex g_lock;
bool g_indexed = false;
std::unordered_map<uint64_t, std::wstring> g_files;  // key -> replacement file
std::unordered_set<uint64_t> g_failed;
std::unordered_set<uint64_t> g_dumped;

std::filesystem::path Root() { return app::ExeDir() / L"textures"; }

bool ParseKey(const std::wstring& name, uint64_t& k) {
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot < 16) return false;
    uint64_t v = 0;
    for (size_t i = dot - 16; i < dot; i++) {
        wchar_t c = name[i];
        int d = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | uint64_t(d);
    }
    k = v;
    return true;
}

bool HasExt(const std::wstring& n, const wchar_t* ext) {
    size_t l = wcslen(ext);
    return n.size() > l && _wcsicmp(n.c_str() + n.size() - l, ext) == 0;
}

void EnsureIndex() {
    if (g_indexed) return;
    g_indexed = true;
    std::error_code ec;
    std::filesystem::path dir = Root() / L"replacements";
    if (!std::filesystem::is_directory(dir, ec)) return;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::wstring n = it->path().filename().wstring();
        uint64_t k;
        if (!(HasExt(n, L".dds") || HasExt(n, L".png")) || !ParseKey(n, k)) continue;
        auto f = g_files.find(k);
        if (f == g_files.end() || HasExt(n, L".dds")) g_files[k] = it->path().wstring();  // .dds wins over .png
    }
    LOGI("texrepl: %u replacement textures indexed", unsigned(g_files.size()));
}

bool ReadFile(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    bool ok = n > 0 && fread(out.data(), 1, size_t(n), f) == size_t(n);
    fclose(f);
    return ok;
}

#pragma pack(push, 1)
struct DdsPixelFormat {
    uint32_t size, flags, fourcc, bits, r, g, b, a;
};
struct DdsHeader {
    uint32_t size, flags, height, width, pitch, depth, mips;
    uint32_t reserved1[11];
    DdsPixelFormat pf;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};
struct DdsHeader10 {
    uint32_t format, dimension, misc, array_size, misc2;
};
#pragma pack(pop)
constexpr uint32_t kDdsMagic = 0x20534444, kDdpfAlpha = 0x1, kDdpfFourcc = 0x4, kDdpfRgb = 0x40;
constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

// DXT1/3/5 (BC1/2/3, also as a DX10 header, BC7 too there), 32 bit A8R8G8B8 / X8R8G8B8 / A8B8G8R8
bool LoadDds(const std::vector<uint8_t>& f, Image& img) {
    if (f.size() < 4 + sizeof(DdsHeader) || *reinterpret_cast<const uint32_t*>(f.data()) != kDdsMagic) return false;
    const DdsHeader& hd = *reinterpret_cast<const DdsHeader*>(f.data() + 4);
    size_t off = 4 + sizeof(DdsHeader);
    uint32_t block = 0, bpp = 0;
    bool swap_rb = false, opaque = false;
    if (hd.pf.flags & kDdpfFourcc) {
        uint32_t cc = hd.pf.fourcc;
        if (cc == FourCC('D', 'X', 'T', '1')) { img.format = DXGI_FORMAT_BC1_UNORM; block = 8; }
        else if (cc == FourCC('D', 'X', 'T', '2') || cc == FourCC('D', 'X', 'T', '3')) { img.format = DXGI_FORMAT_BC2_UNORM; block = 16; }
        else if (cc == FourCC('D', 'X', 'T', '4') || cc == FourCC('D', 'X', 'T', '5')) { img.format = DXGI_FORMAT_BC3_UNORM; block = 16; }
        else if (cc == FourCC('D', 'X', '1', '0')) {
            if (f.size() < off + sizeof(DdsHeader10)) return false;
            const DdsHeader10& h10 = *reinterpret_cast<const DdsHeader10*>(f.data() + off);
            off += sizeof(DdsHeader10);
            switch (h10.format) {
                case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: img.format = DXGI_FORMAT_BC1_UNORM; block = 8; break;
                case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: img.format = DXGI_FORMAT_BC2_UNORM; block = 16; break;
                case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: img.format = DXGI_FORMAT_BC3_UNORM; block = 16; break;
                case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: img.format = DXGI_FORMAT_BC7_UNORM; block = 16; break;
                case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: img.format = DXGI_FORMAT_B8G8R8A8_UNORM; bpp = 4; break;
                case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: img.format = DXGI_FORMAT_B8G8R8A8_UNORM; bpp = 4; swap_rb = true; break;
                default: return false;
            }
        } else {
            return false;
        }
    } else if ((hd.pf.flags & kDdpfRgb) && hd.pf.bits == 32 && hd.pf.g == 0xFF00 &&
               ((hd.pf.r == 0xFF0000 && hd.pf.b == 0xFF) || (hd.pf.r == 0xFF && hd.pf.b == 0xFF0000))) {
        img.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bpp = 4;
        swap_rb = hd.pf.r == 0xFF;
        opaque = !(hd.pf.flags & kDdpfAlpha);
    } else {
        return false;
    }
    uint32_t w = hd.width, h = hd.height, mips = std::max(hd.mips, 1u);
    if (!w || !h || w > kMaxSize || h > kMaxSize) return false;
    // Direct3D 12 needs a BC texture's level 0 in whole 4x4 blocks
    if (block && ((w & 3) || (h & 3))) return false;
    img.width = w;
    img.height = h;
    for (uint32_t l = 0; l < mips; l++) {
        uint32_t lw = std::max(w >> l, 1u), lh = std::max(h >> l, 1u);
        Image::Level lv;
        lv.rows = block ? (lh + 3) / 4 : lh;
        lv.pitch = block ? ((lw + 3) / 4) * block : lw * bpp;
        size_t n = size_t(lv.rows) * lv.pitch;
        if (off + n > f.size()) {
            if (l == 0) return false;
            break;  // truncated chain: keep what is complete
        }
        lv.data.assign(f.data() + off, f.data() + off + n);
        off += n;
        if (bpp && (swap_rb || opaque)) {
            for (size_t i = 0; i < n; i += 4) {
                if (swap_rb) std::swap(lv.data[i], lv.data[i + 2]);
                if (opaque) lv.data[i + 3] = 0xFF;
            }
        }
        img.levels.push_back(std::move(lv));
    }
    return true;
}

// 2x2 box filter down to 1x1 for BGRA8 (Direct3D 12 has no GenerateMips)
void BuildMips(Image& img) {
    uint32_t w = img.width, h = img.height;
    while (w > 1 || h > 1) {
        const Image::Level& s = img.levels.back();
        uint32_t nw = std::max(w >> 1, 1u), nh = std::max(h >> 1, 1u);
        Image::Level d;
        d.pitch = nw * 4;
        d.rows = nh;
        d.data.resize(size_t(d.pitch) * nh);
        for (uint32_t y = 0; y < nh; y++)
            for (uint32_t x = 0; x < nw; x++) {
                uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                for (int c = 0; c < 4; c++) {
                    uint32_t sum = s.data[size_t(y0) * s.pitch + x0 * 4 + c] + s.data[size_t(y0) * s.pitch + x1 * 4 + c] +
                                   s.data[size_t(y1) * s.pitch + x0 * 4 + c] + s.data[size_t(y1) * s.pitch + x1 * 4 + c];
                    d.data[size_t(y) * d.pitch + x * 4 + c] = uint8_t((sum + 2) / 4);
                }
            }
        img.levels.push_back(std::move(d));
        w = nw;
        h = nh;
    }
}

// PNG (or anything WIC decodes) -> BGRA8 with a generated mip chain
bool LoadWic(const std::wstring& path, Image& img) {
    static ComPtr<IWICImagingFactory> wic;
    if (!wic) {
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        if (hr == CO_E_NOTINITIALIZED) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        }
        if (FAILED(hr)) return false;
    }
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(conv->GetSize(&w, &h)) || !w || !h || w > kMaxSize || h > kMaxSize)
        return false;
    Image::Level lv;
    lv.pitch = w * 4;
    lv.rows = h;
    lv.data.resize(size_t(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, lv.pitch, UINT(lv.data.size()), lv.data.data()))) return false;
    img.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    img.width = w;
    img.height = h;
    img.levels.push_back(std::move(lv));
    BuildMips(img);
    return true;
}

}  // namespace

bool Enabled() { return game::Opt().texture_replacement; }
bool DumpEnabled() { return game::Opt().texture_dump; }

uint64_t Key(const void* texels, uint32_t bytes, uint32_t xbox_format, uint32_t w, uint32_t h, const void* palette) {
    // identical to black-pc texrepl::key: FNV-1a 64, offset 14695981039346656037, prime 1099511628211
    uint64_t k = 14695981039346656037ull;
    auto mix = [&](const uint8_t* p, uint32_t n) {
        for (uint32_t i = 0; i < n; i++) k = (k ^ p[i]) * 1099511628211ull;
    };
    uint32_t hdr[3] = {xbox_format, w, h};
    mix(reinterpret_cast<const uint8_t*>(hdr), sizeof hdr);
    mix(static_cast<const uint8_t*>(texels), bytes);
    if (palette) mix(static_cast<const uint8_t*>(palette), 1024);
    return k;
}

bool Has(uint64_t key) {
    std::lock_guard<std::mutex> g(g_lock);
    EnsureIndex();
    return g_files.count(key) && !g_failed.count(key);
}

bool Load(uint64_t key, Image& out) {
    std::wstring path;
    {
        std::lock_guard<std::mutex> g(g_lock);
        EnsureIndex();
        auto f = g_files.find(key);
        if (f == g_files.end() || g_failed.count(key)) return false;
        path = f->second;
    }
    out = Image{};
    bool ok;
    if (HasExt(path, L".dds")) {
        std::vector<uint8_t> data;
        ok = ReadFile(path, data) && LoadDds(data, out);
    } else {
        ok = LoadWic(path, out);
    }
    std::u8string u8 = std::filesystem::path(path).u8string();
    std::string p8(u8.begin(), u8.end());
    if (!ok) {
        LOGW("texrepl: could not load %s (DDS: DXT1/3/5, BC7 or 32 bit RGB; PNG)", p8.c_str());
        std::lock_guard<std::mutex> g(g_lock);
        g_failed.insert(key);
        return false;
    }
    LOGI("texrepl: %016llx -> %s (%ux%u, %u levels)", static_cast<unsigned long long>(key), p8.c_str(), out.width,
         out.height, unsigned(out.levels.size()));
    return true;
}

bool DumpWanted(uint64_t key) {
    std::lock_guard<std::mutex> g(g_lock);
    return g_dumped.insert(key).second;
}

void Dump(uint64_t key, uint32_t xbox_format, const char* fmt_name, uint32_t w, uint32_t h, bool compressed,
          uint32_t block_bytes, const uint8_t* data, uint32_t pitch) {
    std::error_code ec;
    std::filesystem::path dir = Root() / L"dump";
    std::filesystem::create_directories(dir, ec);
    wchar_t name[128];
    swprintf_s(name, L"%ux%u_%02x_%hs_%016llx.dds", w, h, xbox_format, fmt_name ? fmt_name : "unknown",
               static_cast<unsigned long long>(key));
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir / name).c_str(), L"wb") || !f) return;
    DdsHeader hd = {};
    hd.size = 124;
    hd.flags = 0x1007;  // CAPS | HEIGHT | WIDTH | PIXELFORMAT
    hd.width = w;
    hd.height = h;
    hd.mips = 1;
    hd.pf.size = 32;
    hd.caps = 0x1000;  // TEXTURE
    uint32_t rows = compressed ? (h + 3) / 4 : h;
    uint32_t row = compressed ? ((w + 3) / 4) * block_bytes : w * 4;
    if (compressed) {
        hd.flags |= 0x80000;  // LINEARSIZE
        hd.pitch = row * rows;
        hd.pf.flags = kDdpfFourcc;
        hd.pf.fourcc = block_bytes == 8 ? FourCC('D', 'X', 'T', '1') : xbox_format == 0x0E ? FourCC('D', 'X', 'T', '3')
                                                                                            : FourCC('D', 'X', 'T', '5');
    } else {
        hd.flags |= 0x8;  // PITCH
        hd.pitch = row;
        hd.pf.flags = kDdpfRgb | kDdpfAlpha;
        hd.pf.bits = 32;
        hd.pf.r = 0xFF0000;
        hd.pf.g = 0xFF00;
        hd.pf.b = 0xFF;
        hd.pf.a = 0xFF000000;
    }
    uint32_t magic = kDdsMagic;
    fwrite(&magic, 4, 1, f);
    fwrite(&hd, sizeof hd, 1, f);
    for (uint32_t y = 0; y < rows; y++) fwrite(data + size_t(y) * pitch, 1, std::min(row, pitch), f);
    fclose(f);
}

}  // namespace gpu::texrepl
