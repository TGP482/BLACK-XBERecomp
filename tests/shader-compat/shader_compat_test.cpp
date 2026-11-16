#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

#include "d3d/shader_compat.h"

using Microsoft::WRL::ComPtr;

// Exercise the SDK's zero-input RSQ/LOG branches, including both infinity signs.
// Keep the input dynamic so optimization cannot discard those branches.
constexpr char kHelpers[] = R"(
float4 ilu_rsq(float v) { return v == 0 ? (1.#INF).xxxx : (rsqrt(abs(v))).xxxx; }
float4 ilu_log(float v) {
    float mag = abs(v), l2 = log2(mag), ex = floor(l2);
    return mag == 0 ? float4(-1.#INF, 1, -1.#INF, 1) : float4(ex, mag / exp2(ex), l2, 1);
}
)";

ComPtr<ID3DBlob> Compile(const std::string& source, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
                           "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
    if (FAILED(hr)) {
        std::fprintf(stderr, "%s: %s\n", profile,
                     errors ? static_cast<const char*>(errors->GetBufferPointer()) : "D3DCompile failed");
        return {};
    }
    return code;
}

int main(int argc, char** argv) {
    const bool compare = argc == 2 && std::strcmp(argv[1], "--compare-legacy") == 0;
    for (const char* profile : {"vs_5_0", "ps_5_0"}) {
        std::string source = kHelpers;
        source += "float4 main(float4 v : TEXCOORD0) : ";
        source += profile[0] == 'v' ? "SV_Position" : "SV_Target";
        source += " { precise float4 r = ilu_rsq(v.x) + ilu_log(v.y); return r; }";
        const std::string portable = gpu::PortableShaderHlsl(source);
        if (portable.find("#INF") != std::string::npos ||
            portable.find("asfloat(0xff800000u)") == std::string::npos ||
            portable.find("asfloat(0x7f800000u)") == std::string::npos ||
            gpu::PortableShaderHlsl(portable) != portable)
            return 1;
        auto code = Compile(portable, profile);
        if (!code) return 1;
        if (compare) {
            auto legacy = Compile(source, profile);
            if (!legacy || code->GetBufferSize() != legacy->GetBufferSize() ||
                std::memcmp(code->GetBufferPointer(), legacy->GetBufferPointer(), code->GetBufferSize())) {
                std::fprintf(stderr, "%s: legacy and portable DXBC differ\n", profile);
                return 1;
            }
        }
        std::printf("%s: PASS (%zu bytes%s)\n", profile, code->GetBufferSize(),
                    compare ? ", identical to legacy DXBC" : "");
    }
    return 0;
}
