#pragma once
#include <string>
#include <string_view>

namespace gpu {

// The SDK emits FXC's infinity literals. Wine's HLSL parser rejects that syntax;
// asfloat preserves the exact IEEE values on both compilers, including the sign.
inline std::string PortableShaderHlsl(std::string source) {
    auto replace = [&](std::string_view from, std::string_view to) {
        for (size_t at = 0; (at = source.find(from, at)) != std::string::npos; at += to.size())
            source.replace(at, from.size(), to);
    };
    replace("-1.#INF", "asfloat(0xff800000u)");
    replace("1.#INF", "asfloat(0x7f800000u)");
    return source;
}

}  // namespace gpu
