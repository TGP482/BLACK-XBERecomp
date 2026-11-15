#include "gpu/vk_shaders.h"

#include <dlfcn.h>
#include <unistd.h>

#include <dxc/dxcapi.h>

#include <climits>
#include <filesystem>
#include <mutex>

namespace gpu::vk {

namespace {

// libdxcompiler.so beside the executable first, then wherever the system keeps it
DxcCreateInstanceProc LoadDxc(std::string& error) {
    static DxcCreateInstanceProc create = nullptr;
    static std::string failed;
    static std::once_flag once;
    std::call_once(once, [] {
        char self[PATH_MAX] = {};
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        std::string beside = n > 0 ? (std::filesystem::path(std::string(self, size_t(n))).parent_path() / "libdxcompiler.so").string()
                                   : std::string();
        void* lib = beside.empty() ? nullptr : dlopen(beside.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib) lib = dlopen("libdxcompiler.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            failed = std::string("libdxcompiler.so not found: ") + dlerror();
            return;
        }
        create = reinterpret_cast<DxcCreateInstanceProc>(dlsym(lib, "DxcCreateInstance"));
        if (!create) failed = "libdxcompiler.so has no DxcCreateInstance";
    });
    if (!create) error = failed;
    return create;
}

const wchar_t* Profile(Stage s) {
    switch (s) {
        case Stage::Vertex: return L"vs_6_0";
        case Stage::Geometry: return L"gs_6_0";
        default: return L"ps_6_0";
    }
}

std::wstring Wide(const char* s) { return std::wstring(s, s + std::strlen(s)); }

}  // namespace

std::vector<uint32_t> CompileHlsl(const std::string& src, const char* entry, Stage stage, std::string& error) {
    DxcCreateInstanceProc create = LoadDxc(error);
    if (!create) return {};
    // one compiler per thread: DXC's objects are not shared across threads
    thread_local CComPtr<IDxcCompiler3> compiler;
    if (!compiler && FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
        error = "DxcCreateInstance failed";
        return {};
    }
    std::wstring e = Wide(entry);
    std::wstring b0 = std::to_wstring(kBindingConstants), t0 = std::to_wstring(kBindingTextures),
                 s0 = std::to_wstring(kBindingSamplers);
    // HLSL 2018: the generated code compares and selects whole vectors, which 2021 no longer allows
    const wchar_t* args[] = {
        L"-spirv", L"-HV", L"2018", L"-T", Profile(stage), L"-E", e.c_str(), L"-O3",
        L"-fspv-target-env=vulkan1.3",
        L"-fspv-entrypoint-name=main",  // every entry point is main in SPIR-V, whatever the HLSL calls it
        L"-fvk-use-dx-layout",         // constant buffers packed as Direct3D packs them
        L"-fvk-use-dx-position-w",     // a pixel shader's SV_Position.w is w, as Direct3D gives it
        L"-fvk-b-shift", b0.c_str(), L"0", L"-fvk-t-shift", t0.c_str(), L"0", L"-fvk-s-shift", s0.c_str(), L"0",
        L"-Wno-conversion", L"-Wno-parentheses-equality",
    };
    DxcBuffer buf{src.data(), src.size(), DXC_CP_UTF8};
    CComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(&buf, args, UINT32(std::size(args)), nullptr, IID_PPV_ARGS(&result)))) {
        error = "IDxcCompiler3::Compile failed";
        return {};
    }
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    CComPtr<IDxcBlobUtf8> messages;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&messages), nullptr);
    if (FAILED(status)) {
        error = messages && messages->GetStringLength() ? messages->GetStringPointer() : "compile failed";
        return {};
    }
    CComPtr<IDxcBlob> code;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr)) || !code) {
        error = "no SPIR-V";
        return {};
    }
    const auto* words = static_cast<const uint32_t*>(code->GetBufferPointer());
    return std::vector<uint32_t>(words, words + code->GetBufferSize() / 4);
}

bool CompilerAvailable(std::string& error) { return LoadDxc(error) != nullptr; }

}  // namespace gpu::vk
