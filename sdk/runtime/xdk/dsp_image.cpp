// The DSP effects image on the host. The image's code blocks are encrypted, its effect state is plain: each effect's
// state starts with a header, then its input and output buffer addresses and its gains. The GP DSP runs the effects
// in image order on 32 sample frames; a mix bin b is X memory 0x1400 + b * 0x20 to read and 0xC00 + b * 0x20 to
// write, the same storage. Effects are recognised by their decrypted code, the same in every 5849 image seen (HL2,
// Double Agent, Black, 007): gain, mixer, filter, I3DL2 reverb and crosstalk are reproduced, others (the sample rate
// converters that feed DMA outputs) are left out and logged.
#include "xdk/dsp_image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

#include "core/log.h"
#include "kernel/kernel.h"

using namespace kernel;

namespace dsp {
namespace {

constexpr uint32_t kFrame = 32;  // samples the DSP runs each effect on at once
using Buffer = std::array<float, kFrame>;

// X memory addresses
constexpr uint32_t kBinRead = 0x1400;
constexpr uint32_t kBinWrite = 0xC00;
constexpr uint32_t kBinStride = 0x20;
constexpr uint32_t kSpeakers = 6;

// effect descriptor entry: {code, code bytes, state, state bytes, Y, Y bytes, scratch, scratch bytes}
constexpr size_t kEntryBytes = 32;
constexpr size_t kKeyBytes = 8;

// state words of every effect
constexpr uint32_t kFlags = 4;
constexpr uint32_t kFirstPointer = 5;
constexpr uint32_t kAddToOutput = 2;  // flag bit 1: outputs are added to, not overwritten

// Image cipher (DSOUND's key schedule and block routine): a 64 bit LFSR with taps 0, 2, 3 and 63
struct Lfsr {
    uint32_t lo = 0;
    uint32_t hi = 0;
    void Step() {
        uint32_t taps = lo ^ (lo << 1) ^ (lo << 3);
        uint32_t top = (taps << 28) ^ hi;
        lo = (lo >> 1) | (hi << 31);
        hi = (top & 0x80000000u) | (hi >> 1);
    }
};

// the master key, the LFSR after a fixed seed and 81 steps
std::array<uint8_t, 8> MasterKey() {
    Lfsr g{0x49DE12BA, 0x7FA49BCA};
    for (int i = 0; i < 81; i++) g.Step();
    std::array<uint8_t, 8> k;
    std::memcpy(k.data(), &g.lo, 4);
    std::memcpy(k.data() + 4, &g.hi, 4);
    return k;
}

void Decrypt(const uint8_t* key, uint8_t* data, size_t n) {
    std::array<uint8_t, 8> m = MasterKey();
    Lfsr g;
    std::memcpy(&g.lo, m.data(), 4);
    std::memcpy(&g.hi, m.data() + 4, 4);
    g.Step();
    for (size_t i = 0; i < n; i++) {
        uint8_t mask = m[i & 7];
        uint8_t c = uint8_t(data[i] - key[i & 7] * mask);
        g.Step();
        data[i] = uint8_t(mask ^ (g.lo & 0xFF) ^ c);
    }
}

enum class Kind { None, Gain, Mixer, Filter, Reverb, Crosstalk };

// FNV-1a of the decrypted code with word 1, the effect's state address, left out
uint64_t CodeHash(const std::vector<uint8_t>& code) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < code.size(); i++) {
        uint8_t b = i >= 4 && i < 8 ? 0 : code[i];
        h = (h ^ b) * 0x100000001B3ull;
    }
    return h;
}

Kind KindOf(uint64_t hash) {
    switch (hash) {
        case 0xDDD7C789A714E88Bull: return Kind::Gain;       // 36 words
        case 0xFA00AFA73549BF9Bull: return Kind::Mixer;      // 42 words
        case 0xA8ACC4A41FE77751ull: return Kind::Filter;     // 48 words
        case 0x13CF05D0E014E22Eull: return Kind::Reverb;     // 1211 words
        case 0xF1E0A321578CFED9ull: return Kind::Crosstalk;  // 116 words
        default: return Kind::None;
    }
}

// a 24 bit fraction
float Frac(uint32_t word) { return float(int32_t(word << 8)) / 2147483648.0f; }
// the DSP's limiter on a write to memory
float Limit(float x) { return std::clamp(x, -1.0f, 1.0f); }

// The XDK's I3DL2 reverb as its DSP program runs it: a 24 kHz core, delays in samples at that rate
struct Reverb {
    struct Pole {
        float a = 0, b = 0, z = 0;
        float Run(float x) { return z = a * z + b * x + 1e-20f; }  // the offset keeps silence out of denormals
    };
    struct Line {
        std::vector<float> s;
        uint32_t w = 0;
        // a delay of the whole line reads its oldest sample
        float At(uint32_t delay) const { return s[(w + s.size() - delay % s.size()) % s.size()]; }
        void Put(float x) {
            s[w] = x;
            w = (w + 1) % uint32_t(s.size());
        }
    };
    struct Params {
        uint32_t refl, tap[4], in[4], chain[8], comb[4], echo;
        float early[4], late, diff[8], rest, cross;
        Pole in_lp, chain_lp[3], loop_lp[4], echo_lp[2];
    };
    static constexpr int kLen[14] = {6000, 6000, 6000, 6000, 680, 612, 640, 720, 1392, 1684, 2044, 2476, 24000, 24000};
    Line line[14];
    Params now, next;
    bool on = false, pending = false;
    float fade = 1, held[4] = {};
    uint32_t tick = 0;

    Reverb() {
        for (int i = 0; i < 14; i++) line[i].s.assign(kLen[i], 0.0f);
    }
    static float Lin(float mb) { return std::pow(10.0f, mb / 2000.0f); }
    // y = a * y1 + b * x: mb millibels at 0 Hz, hf more at ref Hz
    static Pole Design(int mb, int hf, float ref, float rate) {
        Pole p;
        if (mb <= -10000) return p;
        // 1.0 is past the DSP's fractions and comes out as -1
        p.b = mb ? Lin(float(mb)) : -1;
        if (hf >= 0) return p;
        p.b = Lin(float(mb));
        float g = Lin(2.0f * hf), c = std::cos(6.2831853f * ref / rate);
        p.a = (1 - g * c - std::sqrt(2 * g * (1 - c) - g * g * (1 - c * c))) / (1 - g);
        p.b *= 1 - p.a;
        return p;
    }
    // DSI3DL2LISTENER as 12 floats, the library's own conversion
    void Set(const float* p) {
        static const float kGain[4] = {0.57f, 0.52f, 0.54f, 0.6f}, kTap[4] = {0.1052f, 0.0673f, 0.1737f, 0.2404f};
        static const float kComb[4] = {0.2469f, 0.2209f, 0.2311f, 0.2608f};
        static const float kCombBase[4] = {0.003651f, 0.003515f, 0.003673f, 0.003946f};
        static const float kChain[8] = {0.040023f, 0.288186f, 0.603764f, 0.851905f,
                                        0.021224f, 0.37542f,  0.623583f, 0.759229f};
        static const float kSign[8] = {1, -1, -1, 1, 1, 1, -1, -1};
        static const int kInput[4] = {0, 58, 134, 232};
        auto al = [](int n) { return (n + 1) & ~3; };
        // taps are read in blocks of 4 samples, 16 back at least
        auto tap = [](int n) { return uint32_t(std::max((n + 3) & ~3, 16)); };
        float decay = std::max(p[3], 0.1f), ratio = std::max(p[4], 0.1f), d = std::clamp(p[9] * 0.01f, 0.0f, 1.0f);
        float sc = std::clamp((2 - p[10] * 0.01f) * (decay - 1) * 5, 1.0f, 60.0f);
        float f = sc >= 2.5f ? 1.0f : (sc - 0.8f) * 0.5882353f;
        // a loop of n samples loses 60 dB in the decay time, high frequencies sooner by the ratio
        auto loop = [&](int n) {
            float x = n / 24000.0f / decay * -6000.0f;
            int lf = int(x), hf = int(ratio < 1 ? x / ratio : x);
            return Design(lf, hf - lf, p[11], 24000);
        };
        Params q;
        q.in_lp = Design(0, int(p[1]), p[11], 48000);
        int refl = al(int(p[6] * 24000)), rev = int(p[8] * 24000), echo = int(f * 24000), prev = 0;
        q.refl = tap(refl);
        q.late = Lin(p[7] + p[0]);
        for (int k = 0; k < 4; k++) {
            q.early[k] = Lin(p[5] + p[0]) * kGain[k];
            q.tap[k] = tap(refl + al(int(rev * kTap[k])));
            q.in[k] = tap(refl + al(rev) + kInput[k]);
            q.comb[k] = tap(std::min(al(int((p[8] * kComb[k] + kCombBase[k]) * 24000)), kLen[4 + k] - 1));
            q.loop_lp[k] = loop(kLen[8 + k]);
            // echo times count from the input, a stage of the chain holds 250 ms
            int a = al(int(f * kChain[k] * 24000)), b = al(int(f * kChain[4 + k] * 24000)), m = std::max(a, b) - prev;
            float r = m > 5999 ? 5999.0f / m : 0;
            q.chain[k * 2] = tap(r ? al(int((a - prev) * r)) : a - prev);
            q.chain[k * 2 + 1] = tap(r ? al(int((b - prev) * r)) : b - prev);
            prev = b;
        }
        q.echo = tap(std::min(al(echo), 23999));
        for (int k = 0; k < 3; k++) q.chain_lp[k] = loop(echo >> 2);
        q.echo_lp[0] = q.echo_lp[1] = loop(echo);
        for (int k = 0; k < 8; k++) q.diff[k] = d * kSign[k];
        q.rest = d > 0 ? 1 - d : -1;
        q.cross = 0.2f * d;
        // a change fades out, swaps, fades in
        (on ? next : now) = q;
        pending = on;
        on = true;
    }
    // one 24 kHz sample in, front left, front right, back left, back right out
    void Frame(float x, float o[4]) {
        static const float kFeed[4] = {-0.4f, 0.4f, -0.4f, -0.4f};
        Params& q = now;
        Line* l = line;
        float t = l[0].At(q.refl), e[4], in[4], c[8], u[4], sum = 0;
        for (int k = 0; k < 4; k++) {
            e[k] = t + l[0].At(q.tap[k]) + kFeed[k] * l[4 + k].At(q.comb[k]);
            in[k] = l[0].At(q.in[k]);
            c[k * 2] = l[k].At(q.chain[k * 2]);
            c[k * 2 + 1] = l[k].At(q.chain[k * 2 + 1]);
        }
        // the echoes, front and back
        float sa = c[1] + c[2] + c[4] + c[7], sb = c[0] + c[3] + c[5] + c[6];
        float ea = q.rest * sa + l[12].At(q.echo), eb = q.rest * sb + l[13].At(q.echo);
        for (int j = 0; j < 4; j++)
            sum += u[j] = l[8 + j].At(0) + q.diff[j * 2] * in[j & 1 ? 2 : 0] + q.diff[j * 2 + 1] * in[j & 1 ? 3 : 1];
        for (int j = 0; j < 4; j++) {
            o[j] = q.early[j] * e[j] + q.late * u[j] + q.late * 0.5f * (j < 2 ? ea : eb);
            l[4 + j].Put(e[j]);
            l[8 + j].Put(q.loop_lp[j].Run(u[j] - 0.5f * sum + q.cross * (sa + sb)));
        }
        l[0].Put(x);
        for (int k = 0; k < 3; k++) l[k + 1].Put(q.chain_lp[k].Run(c[k * 2 + 1]));
        l[12].Put(q.echo_lp[0].Run(ea));
        l[13].Put(q.echo_lp[1].Run(eb));
    }
    // the input at 48 kHz, halved, filtered, every second sample run; each output sample is added twice
    void Run(const Buffer& in, Buffer* out[4], uint32_t n) {
        for (uint32_t i = 0; on && i < n; i++) {
            float x = now.in_lp.Run(in[i] * 0.5f);
            if (!(tick++ & 1)) {
                if (pending && (fade -= 1 / 128.0f) <= 0) {
                    now = next;
                    pending = false;
                    fade = 0;
                } else if (!pending) {
                    fade = std::min(fade + 1 / 128.0f, 1.0f);
                }
                float o[4];
                Frame(x, o);
                for (int k = 0; k < 4; k++) held[k] = o[k] * fade;
            }
            for (int k = 0; k < 4; k++) (*out[k])[i] = Limit((*out[k])[i] + held[k]);
        }
    }
};

// The crosstalk effect, per pair of channels: each side adds the other side's output 11 samples back through a
// one pole low pass (constants the effect sets on its first run)
struct Crosstalk {
    static constexpr uint32_t kDelay = 11;
    static constexpr float kPole = 0.924f;    // 0x7645A1
    static constexpr float kFeed = -0.0585f;  // 0xF88313
    float line[2][kDelay] = {};  // each side's output
    float lp[2] = {};
    uint32_t at = 0;

    // in and out: left then right; cross: flag bit 1, the cross term is kept
    void Run(const Buffer* in[2], Buffer* out[2], uint32_t frames, bool cross) {
        for (uint32_t i = 0; i < frames; i++) {
            lp[0] = kPole * lp[0] + kFeed * line[0][at];
            lp[1] = kPole * lp[1] + kFeed * line[1][at];
            float left = Limit((*in[0])[i] + lp[1]);
            float right = Limit((*in[1])[i] + lp[0]);
            line[0][at] = left;
            line[1][at] = right;
            if (!cross) {
                left = (*in[0])[i];
                right = (*in[1])[i];
            }
            (*out[0])[i] = Limit((*out[0])[i] + left);
            (*out[1])[i] = Limit((*out[1])[i] + right);
            at = (at + 1) % kDelay;
        }
    }
};

// The filter effect: a biquad whose coefficients are stored halved, run in scaling mode. Its Y memory holds the
// last two inputs and the last two outputs at full scale; the output buffer gets the half scale result.
struct Biquad {
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    // c: b0, b1, b2, a1, a2 as the state holds them
    float Run(const float c[5], float x) {
        float half = c[0] * x + c[1] * x1 + c[2] * x2 + c[3] * y1 + c[4] * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = Limit(2 * half);
        return half;
    }
};

struct Effect {
    Kind kind = Kind::None;
    uint32_t state = 0;  // guest address of its state words
    Crosstalk crosstalk[2];  // its two pairs
    Biquad biquad;
};

std::vector<Effect> g_effects;
std::map<uint32_t, Buffer> g_x;  // X memory buffers by address, as the effects name them
Reverb g_reverb;                 // the image's I3DL2 reverb, driven by SetI3DL2Listener
int32_t g_reverb_index = -1;

uint32_t Word(const Effect& e, uint32_t i) { return R32(e.state + i * 4) & 0xFFFFFF; }

// the buffer at an X memory address; a bin's write address is its read address
Buffer& At(uint32_t address) {
    if (address >= kBinWrite && address < kBinRead) address += kBinRead - kBinWrite;
    return g_x[address];
}
Buffer& Pointer(const Effect& e, uint32_t i) { return At(Word(e, kFirstPointer + i)); }

// one input to up to 8 outputs, a gain each: pointers in, out x8, then the output count and the gains
void RunGain(const Effect& e, uint32_t n) {
    constexpr uint32_t kOutputs = 1;
    constexpr uint32_t kCount = 14;
    constexpr uint32_t kGains = 15;
    constexpr uint32_t kMaxOutputs = 8;
    const Buffer& in = Pointer(e, 0);
    bool add = Word(e, kFlags) & kAddToOutput;
    uint32_t count = std::min(Word(e, kCount), kMaxOutputs);
    for (uint32_t k = 0; k < count; k++) {
        Buffer& out = Pointer(e, kOutputs + k);
        float g = Frac(Word(e, kGains + k));
        for (uint32_t i = 0; i < n; i++) out[i] = Limit((add ? out[i] : 0) + g * in[i]);
    }
}

// out = out + g0 * in0 + g1 * in1, saturating; without flag bit 1 the output and in0 are not read
void RunMixer(const Effect& e, uint32_t n) {
    constexpr uint32_t kGain0 = 8;
    constexpr uint32_t kGain1 = 9;
    const Buffer& in0 = Pointer(e, 0);
    const Buffer& in1 = Pointer(e, 1);
    Buffer& out = Pointer(e, 2);
    bool add = Word(e, kFlags) & kAddToOutput;
    float g0 = Frac(Word(e, kGain0));
    float g1 = Frac(Word(e, kGain1));
    for (uint32_t i = 0; i < n; i++) {
        float sum = g1 * in1[i];
        if (add) sum += out[i] + g0 * in0[i];
        out[i] = Limit(sum);
    }
}

// input and output pointers, then the five coefficients
void RunFilter(Effect& e, uint32_t n) {
    constexpr uint32_t kCoefficients = 7;
    const Buffer& in = Pointer(e, 0);
    Buffer& out = Pointer(e, 1);
    bool add = Word(e, kFlags) & kAddToOutput;
    float c[5];
    for (uint32_t k = 0; k < 5; k++) c[k] = Frac(Word(e, kCoefficients + k));
    for (uint32_t i = 0; i < n; i++) {
        float y = e.biquad.Run(c, in[i]);
        out[i] = Limit((add ? out[i] : 0) + y);
    }
}

// input pointer 0 (the second is never read), outputs front left, front right, back left, back right
void RunReverb(const Effect& e, uint32_t n) {
    Buffer* out[4];
    for (uint32_t k = 0; k < 4; k++) out[k] = &Pointer(e, 2 + k);
    g_reverb.Run(Pointer(e, 0), out, n);
}

// two pairs: inputs 0, 1 and 2, 3, outputs 4, 5 and 6, 7
void RunCrosstalk(Effect& e, uint32_t n) {
    bool cross = Word(e, kFlags) & kAddToOutput;
    for (uint32_t p = 0; p < 2; p++) {
        const Buffer* in[2] = {&Pointer(e, p * 2), &Pointer(e, p * 2 + 1)};
        Buffer* out[2] = {&Pointer(e, 4 + p * 2), &Pointer(e, 4 + p * 2 + 1)};
        e.crosstalk[p].Run(in, out, n, cross);
    }
}

void RunEffect(Effect& e, uint32_t index, uint32_t n) {
    switch (e.kind) {
        case Kind::Gain: RunGain(e, n); break;
        case Kind::Mixer: RunMixer(e, n); break;
        case Kind::Filter: RunFilter(e, n); break;
        case Kind::Reverb:
            if (int32_t(index) == g_reverb_index) RunReverb(e, n);
            break;
        case Kind::Crosstalk: RunCrosstalk(e, n); break;
        case Kind::None: break;
    }
}

const char* KindName(Kind k) {
    switch (k) {
        case Kind::Gain: return "gain";
        case Kind::Mixer: return "mixer";
        case Kind::Filter: return "filter";
        case Kind::Reverb: return "reverb";
        case Kind::Crosstalk: return "crosstalk";
        case Kind::None: break;
    }
    return "not reproduced";
}

uint32_t U32(const std::vector<uint8_t>& img, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= img.size()) std::memcpy(&v, &img[at], 4);
    return v;
}

}  // namespace

void Load(const std::vector<uint8_t>& img, size_t table, const std::vector<uint32_t>& states, int32_t reverb) {
    g_effects.clear();
    g_x.clear();
    g_reverb = Reverb();
    g_reverb_index = reverb;
    uint32_t count = uint32_t(states.size());
    size_t keys_at = table + 8 + count * kEntryBytes;
    if (keys_at + count * kKeyBytes > img.size()) return;
    std::vector<uint8_t> keys(img.begin() + long(keys_at), img.begin() + long(keys_at + count * kKeyBytes));
    std::array<uint8_t, 8> master = MasterKey();
    Decrypt(master.data(), keys.data(), keys.size());
    std::string kinds;
    for (uint32_t i = 0; i < count; i++) {
        size_t entry = table + 8 + i * kEntryBytes;
        uint32_t code_at = U32(img, entry);
        uint32_t code_bytes = U32(img, entry + 4);
        Effect e;
        e.state = states[i];
        if (code_at <= img.size() && code_bytes <= img.size() - code_at) {
            std::vector<uint8_t> code(img.begin() + code_at, img.begin() + code_at + code_bytes);
            Decrypt(&keys[i * kKeyBytes], code.data(), code.size());
            e.kind = KindOf(CodeHash(code));
        }
        kinds += " " + std::to_string(i) + " " + KindName(e.kind) + ",";
        g_effects.push_back(e);
    }
    LOGR("dsound: effects image, reverb %d, effects%s", reverb, kinds.c_str());
}

bool Loaded() { return !g_effects.empty(); }

void SetListener(const float* p) { g_reverb.Set(p); }

void Run(float* bins, uint32_t frames) {
    for (uint32_t start = 0; start < frames; start += kFrame) {
        uint32_t n = std::min(kFrame, frames - start);
        float* frame = bins + size_t(start) * kBins;
        // the voice processor writes every bin fresh each frame
        for (uint32_t b = 0; b < kBins; b++) {
            Buffer& bin = At(kBinRead + b * kBinStride);
            for (uint32_t i = 0; i < n; i++) bin[i] = Limit(frame[i * kBins + b]);
        }
        for (uint32_t k = 0; k < g_effects.size(); k++) RunEffect(g_effects[k], k, n);
        for (uint32_t b = 0; b < kSpeakers; b++) {
            const Buffer& bin = At(kBinRead + b * kBinStride);
            for (uint32_t i = 0; i < n; i++) frame[i * kBins + b] = bin[i];
        }
    }
}

}  // namespace dsp
