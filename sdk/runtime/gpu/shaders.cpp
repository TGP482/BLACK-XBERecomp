#include "gpu/shaders.h"
#include "gpu/vertex.h"

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gpu {

namespace {

std::string F(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

const char* kCommonHead = R"(
struct VOut {
    float4 pos : SV_Position;
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float4 b0 : COLOR2;
    float4 b1 : COLOR3;
    float4 tc0 : TEXCOORD0;
    float4 tc1 : TEXCOORD1;
    float4 tc2 : TEXCOORD2;
    float4 tc3 : TEXCOORD3;
    float fogamt : TEXCOORD4;
};
cbuffer VC : register(b0) { float4 c[192]; };
cbuffer DC : register(b1) {
    float4 surf;
    float4 fogp;
    float4 fogcolor;
    float4 misc;
    float4 pc[18];
    float4 tfactor;
    float4 texsize[4];
    float4 bumpmat[4];
    float4 bumplum[4];
    float4x4 wvp;
)";
const char* kCommonTail = R"(    float4 spread[4];
    float4x4 wv;
    float4x4 wvn;
    float4x4 tm[4];
    float4 mat[5];
    float4 amb;
    float4 lt[56];
    float4 rtshift;
    float4 zparams;
};
)";

const ShaderHooks& Hooks() {
    static const ShaderHooks plain;
    return g_shader_hooks ? *g_shader_hooks : plain;
}

std::string Common() { return kCommonHead + Hooks().Constants() + kCommonTail; }

// vertex programs

const char* kVsHelpers = R"(
float4 mac_mul(float4 a, float4 b) { return (a == 0 || b == 0) ? (float4)0 : a * b; }
int mac_arl(float v) { return (int)floor(v + 1e-3); }
// magnitude clamped between 1 / 2^64 and 2^64, sign kept
float magclamp(float v) { float m = clamp(abs(v), 5.42101e-20, 1.84467e+19); return v < 0 ? -m : m; }
float4 ilu_rcp(float v) { return (1.0 / v).xxxx; }
float4 ilu_rcc(float v) { return magclamp(1.0 / v).xxxx; }
float4 ilu_rsq(float v) { return v == 0 ? (1.#INF).xxxx : (rsqrt(abs(v))).xxxx; }
// 2^floor(v), fraction, 2^v
float4 ilu_exp(float v) {
    float whole = floor(v);
    return float4(exp2(whole), v - whole, exp2(v), 1);
}
// exponent, mantissa, log2 of |v|; zero logs to negative infinity
float4 ilu_log(float v) {
    float mag = abs(v), l2 = log2(mag), ex = floor(l2);
    return mag == 0 ? float4(-1.#INF, 1, -1.#INF, 1) : float4(ex, mag / exp2(ex), l2, 1);
}
// power clamped to 128 less 1/256
float4 ilu_lit(float4 s) {
    const float cap = 127.99609375;
    float diffuse = max(s.x, 0);
    float spec = exp2(clamp(s.w, -cap, cap) * log2(max(s.y, 0)));
    return float4(1, diffuse, diffuse > 0 ? spec : 0, 1);
}
float fogfactor(float d) {
    if (fogp.w == 3) return (fogp.y - d) / (fogp.y - fogp.x);
    if (fogp.w == 1) return exp(-d * fogp.z);
    if (fogp.w == 2) return exp(-(d * fogp.z) * (d * fogp.z));
    return 1;
}
)";

struct Tok {
    const uint32_t* t;
    uint32_t f(int sub, int bit, int len) const { return (t[sub] >> bit) & ((1u << len) - 1); }
};

const char* kMask[16] = {"", "w", "z", "zw", "y", "yw", "yz", "yzw", "x", "xw", "xz", "xzw", "xy", "xyw", "xyz", "xyzw"};
const char* kLanes[5] = {"", "x", "xy", "xyz", "xyzw"};  // oFog takes one lane per mask bit, from x
const char* kOut[16] = {"oPos", "", "", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1", "oT0", "oT1", "oT2", "oT3", "", "", ""};

// operand A 0, B 1, C 2
std::string Src(const Tok& k, int which, uint16_t regs) {
    uint32_t neg, sw[4], reg, mux;
    if (which == 0) {
        neg = k.f(1, 8, 1);
        sw[0] = k.f(1, 6, 2), sw[1] = k.f(1, 4, 2), sw[2] = k.f(1, 2, 2), sw[3] = k.f(1, 0, 2);
        reg = k.f(2, 28, 4), mux = k.f(2, 26, 2);
    } else if (which == 1) {
        neg = k.f(2, 25, 1);
        sw[0] = k.f(2, 23, 2), sw[1] = k.f(2, 21, 2), sw[2] = k.f(2, 19, 2), sw[3] = k.f(2, 17, 2);
        reg = k.f(2, 13, 4), mux = k.f(2, 11, 2);
    } else {
        neg = k.f(2, 10, 1);
        sw[0] = k.f(2, 8, 2), sw[1] = k.f(2, 6, 2), sw[2] = k.f(2, 4, 2), sw[3] = k.f(2, 2, 2);
        reg = k.f(2, 0, 2) << 2 | k.f(3, 30, 2), mux = k.f(3, 28, 2);
        uint32_t ilu = k.f(1, 25, 3);
        if (ilu >= 2 && ilu <= 6) sw[1] = sw[2] = sw[3] = sw[0];  // scalar ops read x
    }
    // mux: 1 temporary (R12 reads oPos), 2 input, 3 constant
    std::string base;
    if (mux == 1) {
        base = reg == 12 ? "oPos" : F("R%u", reg);
    } else if (mux == 2) {
        uint32_t v = k.f(1, 9, 4);
        base = (regs >> v) & 1 ? F("i.v%u", v) : (v == 3 ? "float4(1,1,1,1)" : "float4(0,0,0,1)");
    } else {
        uint32_t cr = k.f(1, 13, 8);
        base = k.f(3, 1, 1) ? F("c[clamp(a0 + %u, 0, 191)]", cr) : F("c[%u]", cr);
    }
    const char* xyzw = "xyzw";
    std::string s = F("(%s(%s).%c%c%c%c)", neg ? "-" : "", base.c_str(), xyzw[sw[0]], xyzw[sw[1]], xyzw[sw[2]], xyzw[sw[3]]);
    return s;
}

// MAC opcodes from 1: mov mul add mad dp3 dph dp4 dst min max slt sge arl
std::string MacExpr(uint32_t op, const std::string& a, const std::string& b, const std::string& c) {
    switch (op) {
        case 1: return a;
        case 2: return "mac_mul(" + a + ", " + b + ")";
        case 3: return "(" + a + " + " + c + ")";
        case 4: return "(mac_mul(" + a + ", " + b + ") + " + c + ")";
        case 5: return "dot(" + a + ".xyz, " + b + ".xyz).xxxx";
        case 6: return "dot(float4(" + a + ".xyz, 1), " + b + ").xxxx";
        case 7: return "dot(" + a + ", " + b + ").xxxx";
        case 8: return "float4(1, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)";
        case 9: return "min(" + a + ", " + b + ")";
        case 10: return "max(" + a + ", " + b + ")";
        case 11: return "(float4)(" + a + " < " + b + ")";
        case 12: return "(float4)(" + a + " >= " + b + ")";
        case 13: return a;
        default: return "(float4)0";
    }
}

// ILU opcodes from 1: mov rcp rcc rsq exp log lit
std::string IluExpr(uint32_t op, const std::string& c) {
    switch (op) {
        case 1: return c;
        case 2: return "ilu_rcp(" + c + ".x)";
        case 3: return "ilu_rcc(" + c + ".x)";
        case 4: return "ilu_rsq(" + c + ".x)";
        case 5: return "ilu_exp(" + c + ".x)";
        case 6: return "ilu_log(" + c + ".x)";
        case 7: return "ilu_lit(" + c + ")";
        default: return "(float4)0";
    }
}

std::string RegDest(uint32_t r) { return r == 12 ? "oPos" : F("R%u", r); }

std::string VsInputStruct(uint16_t regs) {
    std::string s = "struct VIn {\n";
    for (int r = 0; r < 16; r++)
        if ((regs >> r) & 1) s += F("    float4 v%d : ATTR%d;\n", r, r);
    if (!regs) s += "    uint id : SV_VertexID;\n";
    return s + "};\n";
}

// the input struct and the head of main: elements read raw are finished into float4 i.vN first
std::string VsInputs(uint16_t regs, uint64_t decode) {
    if (!decode) return VsInputStruct(regs) + "VOut main(VIn i) {\n";
    std::string raw = "struct VRaw {\n", in = "struct VIn {\n", body;
    bool packed = false;
    for (int r = 0; r < 16; r++) {
        if (!((regs >> r) & 1)) continue;
        uint32_t d = uint32_t(decode >> (r * 4)) & 15;
        const char* type = d == kDecodeInt || d == kDecodeIntW1 ? "int4" : d == kDecodePacked ? "uint" : "float4";
        raw += F("    %s v%d : ATTR%d;\n", type, r, r);
        in += F("    float4 v%d;\n", r);
        packed |= d == kDecodePacked;
        switch (d) {
            case kDecodeW1: body += F("    i.v%d = float4(ri.v%d.xyz, 1);\n", r, r); break;
            case kDecodeInt: body += F("    i.v%d = float4(ri.v%d);\n", r, r); break;
            case kDecodeIntW1: body += F("    i.v%d = float4(float3(ri.v%d.xyz), 1);\n", r, r); break;
            case kDecodePacked: body += F("    i.v%d = UnpackNp3(ri.v%d);\n", r, r); break;
            case kDecodeFloat2H: body += F("    i.v%d = float4(ri.v%d.xy, 0, ri.v%d.z);\n", r, r, r); break;
            default: body += F("    i.v%d = ri.v%d;\n", r, r); break;
        }
    }
    std::string s = raw + "};\n" + in + "};\n";
    if (packed)
        s += "float4 UnpackNp3(uint u) {\n"
             "    int3 v = int3(int(u << 21) >> 21, int(u << 10) >> 21, int(u) >> 22);\n"
             "    return float4(max(float3(v) / float3(1023, 1023, 511), -1), 1);\n"
             "}\n";
    return s + "VOut main(VRaw ri) {\n    VIn i;\n" + body;
}

const char* kVsEpilogue = R"(
    VOut o;
    precise float4 p = oPos;
    // w 0 is screen space; clamped to 2^-64 its clip coordinates near 1e-20 lose draws on AMD, w 1 draws the same
    p.w = p.w == 0 ? 1 : magclamp(p.w);
    precise float4 q;
    // full screen passes drawn half a pixel up-left for the console's integer pixel centers stop half a pixel short
    // of the far edges on D3D12's, which upscaled leaves the last host columns and rows undrawn
    // surf.w set: a point, which stays put since pulled out it would leave the target
    if (surf.w == 0 && abs(p.x - (surf.x - 0.5)) < 1e-3) p.x = surf.x;
    if (surf.w == 0 && abs(p.y - (surf.y - 0.5)) < 1e-3) p.y = surf.y;
    q.x = p.x * 2 / surf.x - 1;
    q.y = 1 - p.y * 2 / surf.y;
    q.z = p.z * surf.z;
    q.xyz *= p.w;
    q.w = p.w;
    // w - z of one vector's dp4s as one dot keeps the near plane's offset; then the viewport's z scale and offset
    precise float wz = q.w - q.z;
    if (all(clip_zv == clip_wv)) {
        float scale = c[58].z * surf.z;
        wz = scale * dot(clip_wv, clip_wr - clip_zr) + q.w * (1 - scale - c[59].z * surf.z);
    }
    o.pos = q;
    o.d0 = saturate(oD0);
    o.d1 = saturate(oD1);
    o.b0 = saturate(oB0);
    o.b1 = saturate(oB1);
    o.tc0 = oT0;
    o.tc1 = oT1;
    o.tc2 = oT2;
    o.tc3 = oT3;
    o.fogamt = fogfactor(oFog.x);
    return o;
}
)";

const char* kVsPrologue = R"(
    int a0 = 0;
    // precise: EQUAL depth passes need bit exact positions
    precise float4 R0 = 0, R1 = 0, R2 = 0, R3 = 0, R4 = 0, R5 = 0, R6 = 0, R7 = 0, R8 = 0, R9 = 0, R10 = 0, R11 = 0;
    precise float4 oPos = 0;
    precise float4 clip_zv = 0, clip_zr = 0, clip_wv = 1, clip_wr = 0;  // the vector and row of each dp4
    float4 oD0 = float4(0, 0, 0, 1), oD1 = 0, oB0 = float4(0, 0, 0, 1), oB1 = 0, oFog = 0, oPts = 1;
    float4 oT0 = float4(0, 0, 0, 1), oT1 = float4(0, 0, 0, 1), oT2 = float4(0, 0, 0, 1), oT3 = float4(0, 0, 0, 1);
)";

// combiners

struct In {
    uint32_t reg, chan, mod;
};
In ParseIn(uint32_t v) { return {v & 0xF, v & 0x10, v & 0xE0}; }

struct Ps {
    const PixelKey& k;
    int stage = 0;
    std::string e, f;  // final combiner E F for EF_PROD
    bool clamp_sum = false, inv_v1 = false, inv_r0 = false;
    std::string code;

    std::string Var(uint32_t reg) {
        uint32_t flags = k.count >> 8;  // PS_COMBINERCOUNT flags: 0x10 unique C0 per stage, 0x100 unique C1
        switch (reg) {
            case 0: return "float4(0,0,0,0)";
            case 1: return F("pc[%d]", stage == 8 ? 16 : ((flags & 0x10) ? stage * 2 : 0));
            case 2: return F("pc[%d]", stage == 8 ? 17 : ((flags & 0x100) ? stage * 2 + 1 : 1));
            case 3: return "fogin";
            case 4: return "v0";
            case 5: return "v1";
            case 8: return "t0";
            case 9: return "t1";
            case 10: return "t2";
            case 11: return "t3";
            case 12: return "r0";
            case 13: return "r1";
            case 14:
                return F("%s(float4(%s.rgb + %s.rgb, 0))", clamp_sum ? "saturate" : "", inv_v1 ? "(1 - v1)" : "v1",
                         inv_r0 ? "(1 - r0)" : "r0");
            case 15: return "float4(" + e + " * " + f + ", 0)";
            default: return "float4(0,0,0,0)";
        }
    }

    std::string Input(In in, bool alpha) {
        std::string r = Var(in.reg);
        if (!alpha) r += in.chan ? ".aaa" : ".rgb";
        else r += in.chan ? ".a" : ".b";
        switch (in.mod) {
            // PS_INPUTMAPPING
            case 0x00: return "max(" + r + ", 0)";
            case 0x20: return "(1 - saturate(" + r + "))";
            case 0x40: return "(2 * max(" + r + ", 0) - 1)";
            case 0x60: return "(-2 * max(" + r + ", 0) + 1)";
            case 0x80: return "(max(" + r + ", 0) - 0.5)";
            case 0xA0: return "(-max(" + r + ", 0) + 0.5)";
            case 0xC0: return r;
            default: return "-" + r;
        }
    }

    static std::string Map(const std::string& v, uint32_t m) {
        switch (m) {
            // PS_COMBINEROUTPUT bias and shift
            case 0x08: return "(" + v + " - 0.5)";
            case 0x10: return "(" + v + " * 2)";
            case 0x18: return "((" + v + " - 0.5) * 2)";
            case 0x20: return "(" + v + " * 4)";
            case 0x30: return "(" + v + " / 2)";
            default: return v;
        }
    }

    static const char* Dest(uint32_t reg) {
        switch (reg) {
            case 4: return "v0";
            case 5: return "v1";
            case 8: return "t0";
            case 9: return "t1";
            case 10: return "t2";
            case 11: return "t3";
            case 12: return "r0";
            case 13: return "r1";
            default: return nullptr;
        }
    }

    // rgb and alpha halves read the registers before either writes, so the writes come back to the caller
    std::string Stage(uint32_t inputs, uint32_t output, bool alpha) {
        In d = ParseIn(inputs & 0xFF), c = ParseIn((inputs >> 8) & 0xFF), b = ParseIn((inputs >> 16) & 0xFF),
           a = ParseIn(inputs >> 24);
        // fl: 1 cd dot, 2 ab dot, 4 mux, 0x38 mapping, 0x40 and 0x80 blue to alpha
        uint32_t cd_dst = output & 0xF, ab_dst = (output >> 4) & 0xF, sum_dst = (output >> 8) & 0xF, fl = output >> 12;
        const char* m = alpha ? "a" : "rgb";
        const char* T = alpha ? "float" : "float3";
        const char* n = alpha ? "a" : "c";
        std::string A = Input(a, alpha), B = Input(b, alpha), C = Input(c, alpha), D = Input(d, alpha);
        std::string ab = (fl & 2) ? "dot(" + A + ", " + B + ")" : "(" + A + " * " + B + ")";
        std::string cd = (fl & 1) ? "dot(" + C + ", " + D + ")" : "(" + C + " * " + D + ")";
        uint32_t map = fl & 0x38;
        code += F("%s ab%s = clamp((%s)%s, -1, 1);\n", T, n, T, Map(ab, map).c_str());
        code += F("%s cd%s = clamp((%s)%s, -1, 1);\n", T, n, T, Map(cd, map).c_str());
        std::string sum = (fl & 4) ? F("((%s) ? (%s)%s : (%s)%s)",
                                       ((k.count >> 8) & 1) ? "r0.a >= 0.5" : "(uint(r0.a * 255) & 1) == 1", T,
                                       cd.c_str(), T, ab.c_str())
                                   : "(" + ab + " + " + cd + ")";
        code += F("%s ms%s = clamp((%s)%s, -1, 1);\n", T, n, T, Map(sum, map).c_str());
        std::string post;
        if (const char* r = Dest(ab_dst)) {
            post += F("%s.%s = ab%s;\n", r, m, n);
            if (!alpha && (fl & 0x80)) post += F("%s.a = abc.b;\n", r);
        }
        if (const char* r = Dest(cd_dst)) {
            post += F("%s.%s = cd%s;\n", r, m, n);
            if (!alpha && (fl & 0x40)) post += F("%s.a = cdc.b;\n", r);
        }
        if (const char* r = Dest(sum_dst)) post += F("%s.%s = ms%s;\n", r, m, n);
        return post;
    }
};

// a dot stage's operand: the source stage's rgb, raw or read back as signed bytes
std::string DotInput(uint32_t map, uint32_t src) {
    switch (map) {
        case 1: return F("sbyte(t%u.rgb, 128, 128, 127)", src);        // offset by 128
        case 2: return F("sbyte(t%u.rgb, -0.5, 255.5, 127.5)", src);  // two's complement, (2b + 1) / 255
        case 3: return F("sbyte(t%u.rgb, 0, 256, 127)", src);         // two's complement
        default: return F("t%u.rgb", src);
    }
}

// stage i's coordinates as the title wrote them, over 0..1. The console draws half a pixel right and down of the
// host unless a program writes positions raw: a render target is read where the console's offset puts it
std::string TexUv(const TexInfo& t, int i, const std::string& uv) {
    std::string n = "(" + uv + ")";
    if (t.rect) n += F(" / texsize[%d].xy", i);
    n += F(" + texsize[%d].zw", i);
    if (!(t.flags & kTexTarget)) return n;
    std::string step = "ddx(" + n + ") + ddy(" + n + ")";
    return n + F(" + rtshift[%d] * (", i) + step + ")";
}

// texture shader stage i, per its mode
std::string TexFetch(const PixelKey& k, int i) {
    uint32_t mode = (k.tex_modes >> (i * 5)) & 0x1F;
    const TexInfo& t = k.tex[i];
    uint32_t in = i == 1 ? 0 : i == 2 ? (k.input_texture >> 16) & 0xF : (k.input_texture >> 20) & 0xF;
    uint32_t dm = i == 0 ? 0 : (k.dot_mapping >> ((i - 1) * 4)) & 0xF;
    const std::string tc = F("tc%d", i);
    const char* q = tc.c_str();
    auto put = [&](const std::string& v) { return F("float4 t%d = %s;\n", i, v.c_str()); };
    // dpN, or dpNn for stage N + 1's product taken early
    auto dp = [&](int n, int coord, uint32_t map, uint32_t src) {
        return F("float dp%d%s = dot(tc%d.xyz, %s);\n", n, coord == n ? "" : "n", coord, DotInput(map, src).c_str());
    };
    auto tex2d = [&](const std::string& uv) {
        if (t.flags & kTexWide) return F("spreadtap(tex%d, smp%d, %s, spread[%d], %s)", i, i, uv.c_str(), i, Hooks().ReadScale(t).c_str());
        return F("tex%d.Sample(smp%d, %s)", i, i, uv.c_str());
    };
    auto cube = [&](const std::string& dir) {
        std::string c = Hooks().Cube(t, i, dir);
        return c.empty() ? F("tex%d.Sample(smp%d, %s)", i, i, dir.c_str()) : c;
    };
    auto sample2 = [&](const std::string& uv) {
        if (t.kind == kTex2D) return tex2d(TexUv(t, i, uv));
        if (t.kind == kTexCube) return cube("float3(" + uv + ", 1)");
        if (t.kind == kTex3D) return F("tex%d.Sample(smp%d, float3(%s, 0))", i, i, uv.c_str());
        return std::string("float4(0,0,0,1)");
    };
    auto sample3 = [&](const std::string& uvw) {
        if (t.kind == kTex2D) return tex2d(TexUv(t, i, "(" + uvw + ").xy"));
        if (t.kind == kTexCube) return cube(uvw);
        if (t.kind == kTex3D) return F("tex%d.Sample(smp%d, %s)", i, i, uvw.c_str());
        return std::string("float4(0,0,0,1)");
    };
    auto depth = [&](const std::string& ref) {
        std::string uv = TexUv(t, i, F("%s.xy / %s.w", q, q));
        std::string rest = F(", %s, %d, i.pos.xy, texsize[%d].xy)", ref.c_str(), t.shadow, i);
        return put(F("shadow(tex%d, smp%d, ", i, i) + uv + rest);
    };
    std::string s;
    switch (mode) {
        // PS_TEXTUREMODES: 1 project2d, 2 project3d, 3 cubemap, 4 passthru, 5 clipplane, 6 and 7 bumpenvmap, 9 dot_st, 0xA dot_zw, 0xD dot_str_3d, 0xE dot_str_cube, 0xF dpndnt_ar, 0x10 dpndnt_gb, 0x11 dotproduct
        case 0: return put("float4(0,0,0,1)");
        case 1: return t.shadow ? depth("0") : put(sample2(F("%s.xy / %s.w", q, q)));
        case 2: return t.shadow ? depth(F("%s.z / %s.w", q, q)) : put(sample3(F("%s.xyz / %s.w", q, q)));
        case 3: return put(sample3(tc + ".xyz"));
        case 4: return put(tc);
        case 5:  // clip plane: a set compare_mode bit kills at >= 0, a clear one below 0
            s = put("0");
            for (int j = 0; j < 4; j++) s += F("if (%s.%c %s 0) discard;\n", q, "xyzw"[j], ((k.compare_mode >> (i * 4 + j)) & 1) ? ">=" : "<");
            return s;
        case 6:
        case 7:  // bump env map: signed du dv through the 2x2 matrix, 7 scales by luminance
            s = F("float2 duv%d = sbyte(t%u.bgr, 0, 256, 127).xy;\n", i, in);
            s += put(sample2(F("%s.xy + float2(dot(bumpmat[%d].xz, duv%d), dot(bumpmat[%d].yw, duv%d))", q, i, i, i, i)));
            if (mode == 7) s += F("t%d *= bumplum[%d].x * t%u.r + bumplum[%d].y;\n", i, i, in, i);
            return s;
        case 9: return dp(i, i, dm, in) + put(sample2(F("float2(dp%d, dp%d)", i - 1, i)));
        case 0xA:
        case 0x11: return dp(i, i, dm, in) + put("0");
        case 0xB:  // reflect diffuse at stage 2: stage 3's product early for the normal
            s = dp(i, i, dm, in) + dp(i, i + 1, (k.dot_mapping >> 8) & 0xF, (k.input_texture >> 20) & 0xF);
            return s + put(sample3(F("float3(dp%d, dp%d, dp%dn)", i - 1, i, i)));
        case 0xC:  // reflect specular at stage 3: eye from the three w
            s = dp(i, i, dm, in);
            s += F("float3 nrm%d = float3(dp%d, dp%d, dp%d), eye%d = float3(tc%d.w, tc%d.w, tc%d.w);\n", i, i - 2, i - 1, i, i, i - 2,
                   i - 1, i);
            s += F("float3 refl%d = 2 * nrm%d * dot(nrm%d, eye%d) / dot(nrm%d, nrm%d) - eye%d;\n", i, i, i, i, i, i, i);
            return s + put(sample3(F("refl%d", i)));
        case 0xD:
        case 0xE: return dp(i, i, dm, in) + put(sample3(F("float3(dp%d, dp%d, dp%d)", i - 2, i - 1, i)));
        case 0xF: return put(sample2(F("t%u.ar", in)));
        case 0x10: return put(sample2(F("t%u.gb", in)));
        default: return put("0");
    }
}

const char* kPsSbyte = R"(
// unorm to byte b, less lo under 128 or hi from 128, over d
float3 sbyte(float3 u, float lo, float hi, float d) { float3 b = u * 255; return (b - (b >= 128 ? hi : lo)) / d; }
)";
const char* kPsTail = R"(float4 spreadtap(Texture2D t, SamplerState s, float2 uv, float4 w, float m) {
    float4 c = 0;
    int n = (int)w.z;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float4 v = t.SampleLevel(s, uv + w.xy * (float2(x, y) - (n - 1) * 0.5), 0);
            c += float4(saturate(v.rgb * m), v.a);
        }
    return c / (n * n);
}
void alphatest(float a) {
    int f = (int)misc.y;
    int v = (int)round(a * 255), r = (int)misc.x;
    bool ok = f == 0 ? false : f == 1 ? v < r : f == 2 ? v == r : f == 3 ? v <= r : f == 4 ? v > r : f == 5 ? v != r : f == 6 ? v >= r : true;
    if (!ok) discard;
}
)";

std::string PsHelpers() { return kPsSbyte + Hooks().Shadow() + kPsTail; }

// fixed function texture stage arguments, D3DTA
std::string FfArg(uint32_t a, int stage, bool alpha) {
    std::string v;
    switch (a & 0xF) {
        case 0: v = "v0"; break;
        case 1: v = "cur"; break;
        case 2: v = F("t%d", stage); break;
        case 3: v = "tfactor"; break;
        case 4: v = "v1"; break;
        case 5: v = "tmp"; break;
        default: v = "cur"; break;
    }
    if (a & 0x20) v = "(" + v + ").aaaa";
    if (a & 0x10) v = "(1 - " + v + ")";
    return "(" + v + ")" + (alpha ? ".a" : ".rgb");
}

// D3DTOP
std::string FfOp(uint32_t op, const std::string& a1, const std::string& a2, const std::string& a0, int stage, bool alpha) {
    switch (op) {
        case 2: return a1;
        case 3: return a2;
        case 4: return a1 + " * " + a2;
        case 5: return a1 + " * " + a2 + " * 2";
        case 6: return a1 + " * " + a2 + " * 4";
        case 7: return a1 + " + " + a2;
        case 8: return a1 + " + " + a2 + " - 0.5";
        case 9: return "(" + a1 + " + " + a2 + " - 0.5) * 2";
        case 10: return a1 + " - " + a2;
        case 11: return a1 + " + " + a2 + " * (1 - " + a1 + ")";
        case 12: return "lerp(" + a2 + ", " + a1 + ", v0.a)";
        case 13: return "lerp(" + a2 + ", " + a1 + ", cur.a)";
        case 14: return F("lerp(%s, %s, t%d.a)", a2.c_str(), a1.c_str(), stage);
        case 15: return "lerp(" + a2 + ", " + a1 + ", tfactor.a)";
        case 16: return F("%s + %s * (1 - t%d.a)", a1.c_str(), a2.c_str(), stage);
        case 18: return alpha ? a1 : a1 + " + " + a2 + " * " + a1.substr(0, a1.size() - 4) + ".a";
        case 19: return alpha ? a1 : a1 + " * " + a2 + " + " + a1.substr(0, a1.size() - 4) + ".a";
        case 20: return alpha ? a1 : a1 + " + " + a2 + " * (1 - " + a1.substr(0, a1.size() - 4) + ".a)";
        case 21: return alpha ? a1 : "(1 - " + a1 + ") * " + a2 + " + " + a1.substr(0, a1.size() - 4) + ".a";
        case 22: return F("saturate(dot((%s - 0.5) * 2, (%s - 0.5) * 2))", alpha ? ("float3(" + a1 + ",0,0)").c_str() : a1.c_str(),
                          alpha ? ("float3(" + a2 + ",0,0)").c_str() : a2.c_str());
        case 23: return a0 + " + " + a1 + " * " + a2;
        case 24: return "lerp(" + a2 + ", " + a1 + ", " + a0 + ")";
        default: return alpha ? "cur.a" : "cur.rgb";
    }
}

}  // namespace

bool PixelKey::operator==(const PixelKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }

std::string ShaderHooks::Constants() const { return "    float4 ext;\n"; }

std::string ShaderHooks::Shadow() const {
    std::string r = g_reversed_depth ? "1 - clamp(z, 0, mx) / mx" : "clamp(z, 0, mx) / mx";
    return R"(// the sampler compares, a linear filter blends the four results like the console does
float4 shadow(Texture2D<float> t, SamplerComparisonState s, float2 uv, float z, int fmt, float2 px, float2 size) {
    float mx = fmt == 2 ? 65535.0 : 16777215.0;
    return t.SampleCmpLevelZero(s, uv, )" + r + R"().xxxx;
}
)";
}

std::string ShaderHooks::Output() const { return "outc.rgb = saturate(outc.rgb);\n"; }

uint64_t Hash(const void* p, size_t n, uint64_t h) {
    // 16 bytes a step through a 64 x 64 -> 128 bit multiply: it runs over textures every frame and keys every draw
    constexpr uint64_t k0 = 0x9E3779B97F4A7C15ull, k1 = 0xE7037ED1A0B428DBull;
    auto mix = [](uint64_t a, uint64_t b) {
        unsigned __int128 r = static_cast<unsigned __int128>(a) * b;
        return uint64_t(r) ^ uint64_t(r >> 64);
    };
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t seed = h ^ k0;
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        uint64_t x, y;
        std::memcpy(&x, b + i, 8);
        std::memcpy(&y, b + i + 8, 8);
        seed = mix(x ^ k1, y ^ seed);
    }
    if (size_t r = n - i) {
        uint64_t x = 0, y = 0;
        if (r >= 8) {
            std::memcpy(&x, b + i, 8);
            std::memcpy(&y, b + n - 8, 8);
        } else {
            std::memcpy(&x, b + i, r);
        }
        seed = mix(x ^ k1, y ^ seed ^ r);
    }
    return mix(seed ^ k0, uint64_t(n) ^ k1);
}

namespace {

// D3DTOP and D3DTA values PixelKey::ff holds
constexpr uint32_t kTopSelectArg1 = 2;
constexpr uint32_t kTopSelectArg2 = 3;
constexpr uint32_t kTopBlendTextureAlpha = 14;
constexpr uint32_t kTopBlendTextureAlphaPm = 16;
constexpr uint32_t kTopModulateAlphaAddColor = 18;
constexpr uint32_t kTopModulateInvColorAddAlpha = 21;
constexpr uint32_t kTopMultiplyAdd = 23;
constexpr uint32_t kTopLerp = 24;
constexpr uint32_t kTaCurrent = 1;
constexpr uint32_t kTaTexture = 2;

// args in PixelKey::ff order: arg1, arg2, arg0
bool ReadsTexture(uint32_t op, const uint32_t* args, bool alpha) {
    if (op == kTopBlendTextureAlpha || op == kTopBlendTextureAlphaPm) return true;
    bool alpha_arg1_only = alpha && op >= kTopModulateAlphaAddColor && op <= kTopModulateInvColorAddAlpha;
    bool uses_arg1 = op != kTopSelectArg2;
    bool uses_arg2 = op != kTopSelectArg1 && !alpha_arg1_only;
    bool uses_arg0 = op == kTopMultiplyAdd || op == kTopLerp;
    auto texture = [](uint32_t a) { return (a & 0xF) == kTaTexture; };
    return (uses_arg1 && texture(args[0])) || (uses_arg2 && texture(args[1])) || (uses_arg0 && texture(args[2]));
}

// the XDK's combiner builder turns a stage op that reads a missing texture into a pass of the current colour
void PassMissingTexture(uint32_t* f) {
    if (f[0] > 1 && ReadsTexture(f[0], f + 1, false)) {
        f[0] = kTopSelectArg1;
        f[1] = kTaCurrent;
    }
    if (f[4] > 1 && ReadsTexture(f[4], f + 5, true)) {
        f[4] = kTopSelectArg1;
        f[5] = kTaCurrent;
    }
}

}  // namespace

PixelKey MakePixelKey(const State& s, const TexInfo tex[4]) {
    PixelKey k;
    std::memset(&k, 0, sizeof(k));
    for (int i = 0; i < 4; i++) {
        k.tex[i] = tex[i];
        if (s.tss[size_t(i)][kTssAlphaKill] == 4) k.alphakill |= 1u << i;  // D3DTALPHAKILL_ENABLE
    }
    k.flat = s.simple[kMShadeMode >> 2] == 0x1D00;  // D3DSHADE_FLAT
    k.two_sided = s.rs[kRsTwoSidedLighting] != 0;
    k.alphatest = s.simple[kMAlphaTestEnable >> 2] != 0;
    if (s.ps) {
        const uint32_t* d = s.ps->def;
        k.programmable = 1;
        // D3DPIXELSHADERDEF dwords: alpha inputs 0, final 8 and 9, alpha outputs 26, rgb inputs 34, compare mode 42, rgb outputs 45, count 53, dot mapping 55, input texture 56
        k.count = d[53];
        for (int i = 0; i < 8; i++) {
            k.a_in[i] = d[i];
            k.a_out[i] = d[26 + i];
            k.rgb_in[i] = d[34 + i];
            k.rgb_out[i] = d[45 + i];
        }
        k.final_abcd = d[8];
        k.final_efg = d[9];
        // the stage modes follow the bound textures, as D3D's lazy shader stage setup does
        uint32_t modes = s.rs[kRsPsTextureModes];
        for (int i = 0; i < 4; i++) {
            uint32_t m = (modes >> (i * 5)) & 0x1F;
            const TexInfo& t = tex[i];
            if (!t.kind && m != 4 && m != 5 && m != 0xA && m != 0x11) m = 0;
            else if (m >= 1 && m <= 3) m = t.kind == kTexCube ? 3 : (t.kind == kTex3D || t.shadow) ? 2 : 1;
            else if (m == 0xD || m == 0xE) m = t.kind == kTexCube ? 0xE : 0xD;
            k.tex_modes |= m << (i * 5);
        }
        k.dot_mapping = d[55];
        k.input_texture = d[56];
        k.compare_mode = d[42];
        return k;
    }
    // the console's stages divide by q; a vertex program's q is its own
    bool program = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    for (int i = 0; i < 4; i++) {
        const auto& t = s.tss[size_t(i)];
        uint32_t v[9] = {t[kTssColorOp], t[kTssColorArg1], t[kTssColorArg2], t[kTssColorArg0], t[kTssAlphaOp],
                         t[kTssAlphaArg1],  t[kTssAlphaArg2], t[kTssAlphaArg0], t[kTssResultArg]};
        std::memcpy(k.ff[i], v, sizeof(v));
        if (!tex[i].kind) PassMissingTexture(k.ff[i]);
        if (program || (t[kTssTextureTransformFlags] & 0x100)) k.proj |= 1u << i;  // D3DTTFF_PROJECTED
        if (t[kTssColorOp] <= 1) break;
    }
    return k;
}

constexpr uint32_t kMuxConstant = 3;
constexpr uint32_t kViewportOffsetConstant = 59;  // c-37: constants start at c-96

constexpr uint32_t kViewportScaleConstant = 58;  // c-38

// the constant an instruction reads without a0, through any of its operands: at most one
bool ReadsConstant(const Tok& k, uint32_t index) {
    bool constant = k.f(2, 26, 2) == kMuxConstant || k.f(2, 11, 2) == kMuxConstant || k.f(3, 28, 2) == kMuxConstant;
    bool relative = k.f(3, 1, 1);
    return constant && !relative && k.f(1, 13, 8) == index;
}

bool ReadsViewportOffset(const std::vector<uint32_t>& prog) {
    for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
        if (ReadsConstant(Tok{&prog[n]}, kViewportOffsetConstant)) return true;
    }
    return false;
}

constexpr uint32_t kMacDph = 6;
constexpr uint32_t kMacDp4 = 7;
constexpr uint32_t kMacArl = 13;
constexpr uint32_t kLaneZ = 2;  // kMask bits
constexpr uint32_t kLaneW = 1;

// the oPos lanes an instruction writes: by a dp4 or dph, and by anything else
struct PosLanes {
    uint32_t dot = 0;
    uint32_t other = 0;
};

PosLanes WrittenPosLanes(const Tok& k) {
    uint32_t mac = k.f(1, 21, 4), ilu = k.f(1, 25, 3);
    uint32_t r = k.f(3, 20, 4), omask = k.f(3, 12, 4);
    bool mac_writes = mac && mac != kMacArl;
    uint32_t mac_lanes = mac_writes && r == 12 ? k.f(3, 24, 4) : 0;
    uint32_t ilu_lanes = ilu && (mac ? 1 : r) == 12 ? k.f(3, 16, 4) : 0;
    if (omask && k.f(3, 11, 1) && (k.f(3, 3, 8) & 0xF) == 0) {
        bool use_ilu = k.f(3, 2, 1) == 1;
        if (use_ilu && ilu) ilu_lanes |= omask;
        if (!use_ilu && mac_writes) mac_lanes |= omask;
    }
    bool dot = mac == kMacDph || mac == kMacDp4;
    return {dot ? mac_lanes : 0, ilu_lanes | (dot ? 0 : mac_lanes)};
}

// clip z and w each from one dp4 or dph, z later only taken into the viewport: their w - z can be one dot
bool SeparableClipDepth(const std::vector<uint32_t>& prog) {
    int z = 0, w = 0;
    for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
        Tok k{&prog[n]};
        PosLanes p = WrittenPosLanes(k);
        bool viewport = ReadsConstant(k, kViewportScaleConstant) || ReadsConstant(k, kViewportOffsetConstant);
        if ((p.dot & (kLaneZ | kLaneW)) == (kLaneZ | kLaneW) || (p.other & kLaneW)) return false;
        if ((p.other & kLaneZ) && !viewport) return false;
        z += (p.dot & kLaneZ) != 0;
        w += (p.dot & kLaneW) != 0;
        if (k.f(3, 0, 1)) break;
    }
    return z == 1 && w == 1;
}

// at the dp4 or dph of clip z or w, the epilogue's copy of its vector and row
std::string ClipSnapshot(const Tok& k, const std::string& a, const std::string& b) {
    PosLanes p = WrittenPosLanes(k);
    std::string v = k.f(1, 21, 4) == kMacDph ? "float4(" + a + ".xyz, 1)" : a;
    if (p.dot & kLaneZ) return "        clip_zv = " + v + ";\n        clip_zr = " + b + ";\n";
    if (p.dot & kLaneW) return "        clip_wv = " + v + ";\n        clip_wr = " + b + ";\n";
    return "";
}

std::string VertexProgramHlsl(const std::vector<uint32_t>& prog, uint16_t regs, uint64_t decode) {
    std::string s = Common();
    s += kVsHelpers;
    s += VsInputs(regs, decode);
    s += kVsPrologue;
    bool separable = SeparableClipDepth(prog);
    for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
        Tok k{&prog[n]};
        uint32_t mac = k.f(1, 21, 4), ilu = k.f(1, 25, 3);
        if (mac || ilu) {
            std::string a = Src(k, 0, regs), b = Src(k, 1, regs), c = Src(k, 2, regs);
            s += "    {\n";
            if (mac) s += "        precise float4 mr = " + MacExpr(mac, a, b, c) + ";\n";
            if (separable) s += ClipSnapshot(k, a, b);
            if (ilu) s += "        precise float4 ir = " + IluExpr(ilu, c) + ";\n";
            uint32_t r = k.f(3, 20, 4), omask = k.f(3, 12, 4);
            if (mac == 13) {
                s += "        a0 = mac_arl(mr.x);\n";
            } else if (mac) {
                uint32_t m = k.f(3, 24, 4);
                if (ilu && r == 1) m = 0;
                if (m && r <= 12) s += F("        %s.%s = mr.%s;\n", RegDest(r).c_str(), kMask[m], kMask[m]);
            }
            if (ilu) {
                uint32_t m = k.f(3, 16, 4), ir = mac ? 1 : r;
                if (m && ir <= 12) s += F("        %s.%s = ir.%s;\n", RegDest(ir).c_str(), kMask[m], kMask[m]);
            }
            bool use_ilu = k.f(3, 2, 1) == 1;
            if (omask && k.f(3, 11, 1) && ((use_ilu && ilu) || (!use_ilu && mac && mac != 13))) {
                uint32_t o = k.f(3, 3, 8) & 0xF;
                const char* src = use_ilu ? "ir" : "mr";
                if (kOut[o][0]) {
                    const char* msk = o == 5 ? kLanes[std::popcount(omask)] : kMask[omask];
                    s += F("        %s.%s = %s.%s;\n", kOut[o], msk, src, msk);
                }
            }
            s += "    }\n";
        }
        if (k.f(3, 0, 1)) break;  // last instruction
    }
    s += kVsEpilogue;
    return s;
}

FixedKey MakeFixedKey(const State& s) {
    FixedKey f;
    if (s.rs[kRsLighting]) {
        f.light = 1 | (s.rs[kRsSpecularEnable] ? 2 : 0) | (s.rs[kRsLocalViewer] ? 4 : 0) | (s.rs[kRsNormalizeNormals] ? 8 : 0);
        if (s.rs[kRsColorVertex])
            f.light |= (s.rs[kRsDiffuseMaterialSource] & 3) << 8 | (s.rs[kRsSpecularMaterialSource] & 3) << 10 |
                       (s.rs[kRsAmbientMaterialSource] & 3) << 12 | (s.rs[kRsEmissiveMaterialSource] & 3) << 14;
        for (int i = 0; i < 8; i++)
            if ((s.light_enable >> i) & 1) f.types |= (s.lights[size_t(i)][0] & 3) << (i * 2);
    }
    for (int t = 0; t < 4; t++) {
        uint32_t tci = s.tss[size_t(t)][kTssTexCoordIndex], ttf = s.tss[size_t(t)][kTssTextureTransformFlags];
        uint32_t set = tci & 3, size = 2;
        if (s.vs && (s.vs_handle & 1)) {
            uint32_t ty = s.vs->inputs[9 + set].type;
            size = ty == 0x16 ? 3 : std::min<uint32_t>(ty >> 4, 4);  // 0x16 NORMPACKED3
            if (ty >> 4 > 4) size = 3;
            if (!size) size = 2;
        } else {
            uint32_t code = (s.vs_handle >> (16 + set * 2)) & 3;
            size = code == 0 ? 2 : code == 1 ? 3 : code == 2 ? 4 : 1;
        }
        f.tex[t] = set | ((tci >> 16) & 7) << 4 | (ttf & 7) << 8 | (ttf & 0x100 ? 0x800 : 0) | size << 12;
    }
    return f;
}

// D3D's fixed function vertex pipeline: transform, lighting in view space, texture coordinate generation and transforms
std::string FixedVertexHlsl(uint16_t regs, bool pretransformed, uint64_t decode, const FixedKey& f) {
    std::string s = Common();
    s += kVsHelpers;
    s += VsInputs(regs, decode);
    s += kVsPrologue;
    auto v = [&](int r, const char* def) { return (regs >> r) & 1 ? F("i.v%d", r) : std::string(def); };
    bool lit = !pretransformed && (f.light & 1);
    if (pretransformed) {
        s += "    float4 rhw = " + v(0, "float4(0,0,0,1)") + ";\n";
        s += "    float w = rhw.w == 0 ? 1 : 1 / rhw.w;\n";
        s += "    oPos = float4(rhw.xy, rhw.z / surf.z, w);\n";
    } else {
        s += "    float4 ip = float4(" + v(0, "float4(0,0,0,1)") + ".xyz, 1);\n";
        s += "    precise float4 cp = mul(ip, wvp);\n";
        s += "    float3 vp = mul(ip, wv).xyz;\n";
        s += "    float3 vn = mul(float4(" + v(2, "float4(0,0,0,0)") + ".xyz, 0), wvn).xyz;\n";
        if (f.light & 8) s += "    vn = dot(vn, vn) > 0 ? normalize(vn) : vn;\n";
    }
    if (lit) {
        // D3DMCS: 1 the vertex diffuse, 2 its specular, else the material
        auto src = [&](int field, const char* mat) {
            uint32_t m = (f.light >> (8 + field * 2)) & 3;
            if (m == 1 && ((regs >> 3) & 1)) return std::string("i.v3");
            if (m == 2 && ((regs >> 4) & 1)) return std::string("i.v4");
            return std::string(mat);
        };
        s += "    float4 md = " + src(0, "mat[0]") + ", ms = " + src(1, "mat[2]") + ", ma = " + src(2, "mat[1]") +
             ", me = " + src(3, "mat[3]") + ";\n";
        s += "    float3 la = 0, ld = 0, ls = 0;\n";
        s += (f.light & 4) ? "    float3 eye = -normalize(vp);\n" : "    float3 eye = float3(0, 0, 1);\n";
        for (int n = 0; n < 8; n++) {
            uint32_t type = (f.types >> (n * 2)) & 3;  // D3DLIGHTTYPE: 1 point, 2 spot, 3 directional
            if (!type) continue;
            int b = n * 7;
            s += "    {\n        float3 l;\n        float att = 1;\n";
            if (type == 3) {
                s += F("        l = -normalize(lt[%d].xyz);\n", b + 4);
            } else {
                s += F("        float3 d = lt[%d].xyz - vp;\n        float dist = length(d);\n        l = d / max(dist, 1e-6);\n", b + 3);
                s += F("        att = dist > lt[%d].w ? 0 : 1 / max(lt[%d].x + lt[%d].y * dist + lt[%d].z * dist * dist, 1e-6);\n", b + 3, b + 5,
                       b + 5, b + 5);
                if (type == 2) {
                    s += F("        float rho = dot(-l, normalize(lt[%d].xyz));\n", b + 4);
                    s += F("        att *= rho > lt[%d].x ? 1 : rho <= lt[%d].y ? 0 : pow(saturate((rho - lt[%d].y) / (lt[%d].x - lt[%d].y)), lt[%d].w);\n",
                           b + 6, b + 6, b + 6, b + 6, b + 6, b + 4);
                }
            }
            s += F("        la += att * lt[%d].rgb;\n", b + 2);
            s += "        float nl = dot(vn, l);\n";
            s += F("        ld += att * lt[%d].rgb * max(nl, 0);\n", b);
            if (f.light & 2) s += F("        if (nl > 0) ls += att * lt[%d].rgb * pow(max(dot(vn, normalize(l + eye)), 0), mat[4].x);\n", b + 1);
            s += "    }\n";
        }
        s += "    oD0 = float4(me.rgb + ma.rgb * (amb.rgb + la) + md.rgb * ld, md.a);\n";
        s += (f.light & 2) ? "    oD1 = float4(ms.rgb * ls, ms.a);\n" : "    oD1 = float4(0,0,0,0);\n";
    } else {
        s += "    oD0 = " + v(3, "float4(1,1,1,1)") + ";\n";
        s += "    oD1 = " + v(4, "float4(0,0,0,0)") + ";\n";
    }
    s += "    oB0 = oD0;\n    oB1 = oD1;\n";
    for (int t = 0; t < 4; t++) {
        uint32_t u = f.tex[t], set = u & 3, gen = (u >> 4) & 7, count = (u >> 8) & 7, size = (u >> 12) & 7;
        std::string in = v(9 + int(set), "float4(0,0,0,1)");
        if (pretransformed || (!gen && !count)) {
            s += F("    oT%d = %s;\n", t, in.c_str());
            continue;
        }
        // generated coordinates are view space; a transform takes the set with 1 after its last element
        std::string c = gen == 1 ? "float4(vn, 1)" : gen == 2 ? "float4(vp, 1)" : gen == 3 ? "float4(reflect(normalize(vp), vn), 1)"
                        : size == 1 ? F("float4(%s.x, 1, 0, 0)", in.c_str()) : size == 2 ? F("float4(%s.xy, 1, 0)", in.c_str())
                        : size == 3 ? F("float4(%s.xyz, 1)", in.c_str()) : in;
        if (!count) {
            s += F("    oT%d = %s;\n", t, c.c_str());
            continue;
        }
        s += F("    {\n        float4 tc = mul(%s, tm[%d]);\n", c.c_str(), t);
        if (u & 0x800) s += F("        oT%d = float4(tc.xy, tc.z, tc[%u]);\n", t, std::min<uint32_t>(count, 4) - 1);
        else s += F("        oT%d = float4(tc.xy, %s, 1);\n", t, count >= 3 ? "tc.z" : "0");
        s += "    }\n";
    }
    s += "    oFog = " + v(5, "float4(0,0,0,0)") + ";\n";
    if (pretransformed) return s + kVsEpilogue;
    s += R"(
    VOut o;
    // like the pretransformed ones, target sized passes end half a console pixel short of the far edges
    float2 sp = (float2(cp.x, -cp.y) / cp.w + 1) * 0.5 * surf.xy;
    if (surf.w == 0 && abs(sp.x - (surf.x - 0.5)) < 1e-3) cp.x = cp.w;
    if (surf.w == 0 && abs(sp.y - (surf.y - 0.5)) < 1e-3) cp.y = -cp.w;
    o.pos = cp;
    o.d0 = saturate(oD0);
    o.d1 = saturate(oD1);
    o.b0 = o.d0;
    o.b1 = o.d1;
    o.tc0 = oT0;
    o.tc1 = oT1;
    o.tc2 = oT2;
    o.tc3 = oT3;
    o.fogamt = fogfactor(abs(cp.z));
    return o;
}
)";
    return s;
}

std::string PixelHlsl(const PixelKey& k) {
    std::string s = Common();
    s += PsHelpers();
    for (int i = 0; i < 4; i++) {
        const TexInfo& t = k.tex[i];
        const char* type = t.shadow ? "Texture2D<float>" : t.kind == kTex3D ? "Texture3D" : t.kind == kTexCube ? "TextureCube" : "Texture2D";
        s += F("%s tex%d : register(t%d);\n%s smp%d : register(s%d);\n", type, i, i, t.shadow ? "SamplerComparisonState" : "SamplerState", i, i);
    }
    s += F("float4 main(VOut i, bool front : SV_IsFrontFace) : SV_Target {\n");
    s += "    float4 tc0 = i.tc0, tc1 = i.tc1, tc2 = i.tc2, tc3 = i.tc3;\n";
    s += k.two_sided ? "    float4 v0 = front ? i.d0 : i.b0, v1 = front ? i.d1 : i.b1;\n" : "    float4 v0 = i.d0, v1 = i.d1;\n";
    s += "    if (misc.w == 0) v1 = float4(0, 0, 0, 1);\n";
    s += "    float4 fogin = float4(fogcolor.rgb, saturate(i.fogamt));\n";
    std::string body;
    if (k.programmable) {
        for (int i = 0; i < 4; i++) {
            body += TexFetch(k, i);
            body += Hooks().Fetched(k.tex[i], i);
        }
        for (int i = 0; i < 4; i++) {
            uint32_t m = (k.tex_modes >> (i * 5)) & 0x1F;
            bool fetch = m && m != 4 && m != 5 && m != 0xA && m != 0x11;
            if (fetch && ((k.alphakill >> i) & 1)) body += F("if (t%d.a == 0) discard;\n", i);
        }
        uint32_t mode0 = k.tex_modes & 0x1F;
        body += F("float4 r0 = float4(0, 0, 0, %s), r1 = 0;\n", mode0 ? "t0.a" : "1");
        Ps ps{k};
        uint32_t n = std::min<uint32_t>(k.count & 0xFF, 8);
        for (uint32_t st = 0; st < n; st++) {
            ps.stage = int(st);
            ps.code += "{\n";
            std::string wc = ps.Stage(k.rgb_in[st], k.rgb_out[st], false);
            std::string wa = ps.Stage(k.a_in[st], k.a_out[st], true);
            ps.code += wc + wa + "}\n";
        }
        body += ps.code;
        if (k.final_abcd || k.final_efg) {
            ps.stage = 8;
            uint32_t fl = k.final_efg & 0xFF;
            ps.clamp_sum = fl & 0x80;
            ps.inv_v1 = fl & 0x40;
            ps.inv_r0 = fl & 0x20;
            ps.e = ps.Input(ParseIn(k.final_efg >> 24), false);
            ps.f = ps.Input(ParseIn((k.final_efg >> 16) & 0xFF), false);
            std::string A = ps.Input(ParseIn(k.final_abcd >> 24), false), B = ps.Input(ParseIn((k.final_abcd >> 16) & 0xFF), false),
                        C = ps.Input(ParseIn((k.final_abcd >> 8) & 0xFF), false), D = ps.Input(ParseIn(k.final_abcd & 0xFF), false),
                        G = ps.Input(ParseIn((k.final_efg >> 8) & 0xFF), true);
            body += "float4 outc;\n";
            body += "outc.rgb = " + D + " + lerp(" + C + ", " + B + ", " + A + ");\n";
            body += "outc.a = " + G + ";\n";
        } else {
            // the final combiner reads r0 unsigned, a negative alpha reaches the alpha test as 0
            body += "float4 outc = float4(lerp(fogin.rgb, saturate(r0.rgb), fogin.a), saturate(r0.a));\n";
        }
    } else {
        body += "float4 cur = v0, tmp = 0;\n";
        for (int i = 0; i < 4; i++) {
            const uint32_t* f = k.ff[i];
            if (f[0] <= 1) break;
            const TexInfo& t = k.tex[i];
            std::string uv = TexUv(t, i, (k.proj >> i) & 1 ? F("tc%d.xy / tc%d.w", i, i) : F("tc%d.xy", i));
            std::string cube;
            if (t.shadow)
                body += F("float4 t%d = shadow(tex%d, smp%d, ", i, i, i) + TexUv(t, i, F("tc%d.xy / tc%d.w", i, i)) +
                        F(", tc%d.z / tc%d.w, %d, i.pos.xy, texsize[%d].xy);\n", i, i, t.shadow, i);
            else if (t.kind == kTex2D && (t.flags & kTexWide))
                body += F("float4 t%d = spreadtap(tex%d, smp%d, %s, spread[%d], %s);\n", i, i, i, uv.c_str(), i, Hooks().ReadScale(t).c_str());
            else if (t.kind == kTex2D) body += F("float4 t%d = tex%d.Sample(smp%d, %s);\n", i, i, i, uv.c_str());
            else if (t.kind == kTexCube && !(cube = Hooks().Cube(t, i, F("tc%d.xyz", i))).empty()) body += F("float4 t%d = %s;\n", i, cube.c_str());
            else if (t.kind) body += F("float4 t%d = tex%d.Sample(smp%d, tc%d.xyz);\n", i, i, i, i);
            else body += F("float4 t%d = float4(1,1,1,1);\n", i);
            body += Hooks().Fetched(t, i);
            if ((k.alphakill >> i) & 1) body += F("if (t%d.a == 0) discard;\n", i);
            const char* dst = f[8] == 5 ? "tmp" : "cur";
            std::string rgb = FfOp(f[0], FfArg(f[1], i, false), FfArg(f[2], i, false), FfArg(f[3], i, false), i, false);
            std::string a = f[4] <= 1 ? std::string("cur.a") : FfOp(f[4], FfArg(f[5], i, true), FfArg(f[6], i, true), FfArg(f[7], i, true), i, true);
            body += F("{ float3 c3 = saturate(%s); float c1 = saturate(%s); %s = float4(c3, c1); }\n", rgb.c_str(), a.c_str(), dst);
        }
        body += "cur.rgb = saturate(cur.rgb + v1.rgb);\n";
        body += "float4 outc = float4(lerp(fogin.rgb, cur.rgb, fogin.a), cur.a);\n";
    }
    if (k.alphatest) body += "alphatest(outc.a);\n";
    body += Hooks().Output() + "return outc;\n}\n";
    s += body;
    return k.quad ? QuadHlsl(std::move(s)) : s;
}

// the vertex shader hands on the pixel position as drawn and as pulled (e), the pixel shader takes the stretch back out
std::string QuadHlsl(std::string s) {
    auto sub = [&](const char* at, const char* before, const char* after) {
        size_t p = s.find(at);
        if (p != std::string::npos) s.replace(p, std::strlen(at), std::string(before) + at + after);
    };
    sub("    float fogamt : TEXCOORD4;\n", "", "    noperspective float4 e : TEXCOORD5;\n");
    sub("    if (surf.w == 0 && abs(p.x - (surf.x - 0.5)) < 1e-3) p.x = surf.x;\n", "    o.e.xy = p.xy;\n", "");
    sub("    if (surf.w == 0 && abs(p.y - (surf.y - 0.5)) < 1e-3) p.y = surf.y;\n", "", "    o.e.zw = p.xy;\n");
    sub("    o.pos = cp;\n", "    o.e = float4(sp, (float2(cp.x, -cp.y) / cp.w + 1) * 0.5 * surf.xy);\n", "");
    sub("float4 main(VOut i, bool front", R"(float4 unstretch(float4 t, float4 e) {
    float2 d = e.zw - e.xy, g = float2(ddx(e.x), ddy(e.y));
    return t + (d.x ? d.x / g.x * ddx(t) : 0) + (d.y ? d.y / g.y * ddy(t) : 0);
}
)", "");
    for (int i = 0; i < 4; i++) sub(F("i.tc%d", i).c_str(), "unstretch(", ", i.e)");
    return s;
}

namespace {

// A w buffer's reversed depth comes from the clip w alone, zparams.x * (zparams.y / w - 1), exact where z / w had
// already rounded away; a z buffer's is 1 - z, given as w - z. zparams.z is the polygon offset in the units of each
const char* kReversedClipHlsl = R"(float ReversedClip(float wz, float w) {
    if (zparams.x != 0) return zparams.x * (zparams.y - (w + zparams.z));
    return wz - zparams.z * w;
}
)";

bool InsertAfter(std::string& s, const char* at, const char* text) {
    size_t p = s.find(at);
    if (p == std::string::npos) return false;
    s.insert(p + std::strlen(at), text);
    return true;
}

}  // namespace

std::string ReversedDepthHlsl(std::string s) {
    size_t entry = s.find("VOut main(");
    if (entry != std::string::npos) s.insert(entry, kReversedClipHlsl);
    InsertAfter(s, "    o.pos = q;\n", "    o.pos.z = ReversedClip(wz, q.w);\n");
    // w and z rounded apart lose the near plane's offset to cancellation once the far plane is distant, so fixed
    // function takes w - z from one column; the matrix's own rounding then moves every surface alike
    InsertAfter(s, "    o.pos = cp;\n",
                "    precise float wz = dot(ip, wvp._m03_m13_m23_m33 - wvp._m02_m12_m22_m32);\n"
                "    o.pos.z = ReversedClip(wz, cp.w);\n");
    return s;
}

// a point covers its console pixel, several host pixels on an upscaled target: drawn as a quad of that size
std::string PointHlsl() {
    return Common() + R"(
[maxvertexcount(4)]
void main(point VOut v[1], inout TriangleStream<VOut> strip) {
    float2 half_pixel = v[0].pos.w / surf.xy;  // in clip units, the target spans 2 across surf.xy pixels
    // a target is read half a host pixel back (surf.w host pixels per console pixel): left and top reach that far
    float2 back = (0.5 - 1e-3) * 2 * half_pixel / surf.w;
    for (int i = 0; i < 4; i++) {
        VOut o = v[0];
        o.pos.x += (i & 1) ? half_pixel.x : -half_pixel.x - back.x;
        o.pos.y += (i & 2) ? -half_pixel.y : half_pixel.y + back.y;
        strip.Append(o);
    }
}
)";
}

}  // namespace gpu
