// decoding, levels and the mix
#include <chrono>

#include "sound/sound.h"

namespace sound {

const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21,
    23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};

// one 36 byte per channel IMA block, the voice plays 64 of its 65 frames
void DecodeBlock(const uint8_t* b, int ch, int16_t* out) {
    for (int c = 0; c < ch; c++) {
        // header per channel: 16 bit first sample, step index
        int pred = int16_t(b[c * 4] | b[c * 4 + 1] << 8);
        int idx = std::min<int>(b[c * 4 + 2], 88);
        out[c] = int16_t(pred);
        const uint8_t* d = b + ch * 4;
        for (int k = 0; k < 8; k++) {
            // 4 bytes of 8 nibbles per channel, channels interleaved
            for (int j = 0; j < 8; j++) {
                int n = (d[(k * ch + c) * 4 + j / 2] >> ((j & 1) * 4)) & 15;
                int step = kStep[idx];
                int diff = step >> 3;
                if (n & 1) diff += step >> 2;
                if (n & 2) diff += step >> 1;
                if (n & 4) diff += step;
                pred = std::clamp(n & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = std::clamp(idx + kIndex[n], 0, 88);
                out[(1 + k * 8 + j) * ch + c] = int16_t(pred);
            }
        }
    }
}

// The decoded ADPCM block at guest address at. A frame between two blocks reads both, so the one before stays too.
const int16_t* DecodedBlock(Voice& v, int64_t at) {
    if (at == v.block) return v.decoded[v.slot];
    int64_t before = v.block_decoded_now ? v.block : -1;
    if (at != v.spare_block) {
        int16_t* into = v.decoded[v.slot ^ 1];
        DecodeBlock(static_cast<const uint8_t*>(HostPtr(uint32_t(at))), std::min<int>(v.channels, 8), into);
    }
    v.spare_block = before;
    v.block = at;
    v.slot ^= 1;
    v.block_decoded_now = true;
    return v.decoded[v.slot];
}

// sample of channel c at frame f of memory starting at data
float Sample(Voice& v, uint32_t data, uint32_t f, int c) {
    c = std::min<int>(c, v.channels - 1);
    const uint8_t* p = static_cast<const uint8_t*>(HostPtr(data));
    if (v.tag == kFormatXboxAdpcm) {
        int64_t at = int64_t(data) + int64_t(f / kAdpcmFrames) * kAdpcmBlockBytes * v.channels;
        return DecodedBlock(v, at)[(f % kAdpcmFrames) * v.channels + c] / 32768.0f;
    }
    if (v.bits == 8) return (p[f * v.align + c] - 128) / 128.0f;
    int16_t s;
    std::memcpy(&s, p + f * v.align + c * 2, 2);
    return s / 32768.0f;
}

void Complete(Voice& v, Packet& pk, uint32_t status) {
    if (pk.completed_ptr) W32(pk.completed_ptr, status == 0 ? pk.size : pk.used);
    if (pk.status_ptr) W32(pk.status_ptr, status);
    if (v.callback) {
        g_done.push_back({v.obj, v.callback, v.context, pk.context, status});
        g_done_cv.notify_one();
    } else if (pk.context) {
        if (ObjectRef e = FromHandle(pk.context)) SetEvent(*e);
    }
}

// Degrees from straight ahead, plus to the right. The library's octant estimate (0x21BC01), not atan2: it picks
// the 3 degree HRTF entry and the interaural delay.
float Azimuth(const float d[3], const float front[3], const float right[3]) {
    float ahead = d[0] * front[0] + d[1] * front[1] + d[2] * front[2];
    float side = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
    float a = std::fabs(ahead);
    float s = std::fabs(side);
    float deg = 0;
    if (a > s) deg = 45 * s / a;
    else if (s > 0) deg = 90 - 45 * a / s;
    if (ahead < 0) deg = 180 - deg;
    return side < 0 ? -deg : deg;
}

// Interaural delay in samples, above 0 the left ear hears later. Black's light HRTF table (0x2233B0) holds it
// in the last byte of the far ear's filter, one entry every 3 degrees. With speakers behind the listener a sound
// behind takes the entry of its mirror in front (0x4D4D6C in Double Agent).
float Itd(float azimuth, bool surround) {
    if (surround && std::fabs(azimuth) > 90) azimuth = std::copysign(180 - std::fabs(azimuth), azimuth);
    static const uint8_t kDelay[61] = {0,  2,  3,  4,  4,  5,  6,  9,  10, 11, 12, 12, 13, 14, 17, 18,
                                       19, 20, 21, 21, 22, 23, 24, 25, 26, 28, 29, 29, 31, 30, 30, 31,
                                       30, 28, 27, 28, 27, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15,
                                       14, 13, 13, 12, 11, 8,  7,  6,  6,  3,  2,  2,  0};
    if (!(std::fabs(azimuth) <= 180)) return 0;
    int deg = int(std::fabs(azimuth) + 1.5f) / 3 * 3;
    float delay = kDelay[deg / 3];
    return azimuth < 0 ? -delay : delay;
}

// distance attenuation, 1.0 inside the minimum distance
float Attenuation(const Voice& v, float dist) {
    if (dist <= v.min_dist) return 1.0f;
    if (v.curve.empty()) return v.min_dist / std::min(dist, v.max_dist);
    if (v.max_dist <= v.min_dist) return 1.0f;
    // points spread evenly from min to max distance, 1.0 at min
    float pos = (std::min(dist, v.max_dist) - v.min_dist) / (v.max_dist - v.min_dist) * v.curve.size();
    size_t i = std::min(size_t(pos), v.curve.size() - 1);
    float from = i ? v.curve[i - 1] : 1.0f;
    return from + (v.curve[i] - from) * (pos - i);
}

// Share of a 3D voice for the crosstalk pair behind, by power (0x4D1986 in Double Agent): none to 45 degrees,
// half at 90, all from 135, drawn towards half and half within half a unit of the head. Black turns sounds on the
// horizontal plane only, so no elevation.
float BackShare(float azimuth, float dist) {
    float back = std::clamp(std::fabs(azimuth) / 90 - 0.5f, 0.0f, 1.0f);
    if (dist < 0.5f) back = ((back - 0.5f) * dist + 1) * 0.5f;
    return back;
}

// Where a 3D voice sits for the library's 3D pass (DSOUND 0x219074): distance attenuation and azimuth
Place Locate(const Voice& v) {
    Place at;
    if (!v.is3d || v.mode == 2) return at;
    at.on = true;
    bool rel = v.mode == 1;  // the position is already in listener space
    float d[3] = {v.p[0], v.p[1], v.p[2]};
    for (int i = 0; i < 3 && !rel; i++) d[i] -= g_listener[i];
    float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    at.att = Attenuation(v, dist);
    static const float kHeadFront[3] = {0, 0, 1};
    static const float kHeadRight[3] = {1, 0, 0};
    float right[3] = {g_top[1] * g_front[2] - g_top[2] * g_front[1], g_top[2] * g_front[0] - g_top[0] * g_front[2],
                      g_top[0] * g_front[1] - g_top[1] * g_front[0]};
    float level[3] = {d[0], 0, d[2]};  // the library turns the sound on the horizontal plane only
    const float* front = rel ? kHeadFront : g_front;
    const float* side = rel ? kHeadRight : right;
    at.azimuth = Azimuth(level, front, side);
    // surround without headphones splits the crosstalk pairs (0x4CF3F7 in Double Agent)
    if (audio::IsSurround() && !g_headphones) {
        float back = BackShare(at.azimuth, dist);
        at.front = std::sqrt(1 - back);
        at.back = std::sqrt(back);
    }
    return at;
}

// mix bins 6 to 9 are the crosstalk front left, front right, back left and back right
constexpr uint32_t kCrosstalkFrontLeft = 6;
constexpr uint32_t kCrosstalkFrontRight = 7;
constexpr uint32_t kCrosstalkBackLeft = 8;
constexpr uint32_t kCrosstalkBackRight = 9;

// The library's level of a 3D voice in a bin (0x21E94D): the crosstalk pairs take their shares, every bin takes the
// distance attenuation. Left and right come from the HRTF.
float BinScale(const Place& at, uint32_t bin) {
    if (!at.on) return 1.0f;
    if (bin == kCrosstalkFrontLeft || bin == kCrosstalkFrontRight) return at.att * at.front;
    if (bin == kCrosstalkBackLeft || bin == kCrosstalkBackRight) return at.att * at.back;
    return at.att;
}

// Row of a voice channel's level. A mono 3D voice leaves the HRTF as two ears: the left ear feeds the left
// crosstalk bins, the right ear the right ones, every other bin hears the voice as it is.
uint32_t RowOf(const Place& at, const Voice& v, uint32_t c, uint32_t bin) {
    if (!at.on || v.channels != 1) return c;
    if (bin == kCrosstalkFrontLeft || bin == kCrosstalkBackLeft) return kLeftEar;
    if (bin == kCrosstalkFrontRight || bin == kCrosstalkBackRight) return kRightEar;
    return kDry;
}

// The speaker each mix bin reaches. Bins 0 to 5 are the speakers, the crosstalk bins join the front and back pairs.
// Black's effects image adds bins 19 to 24 to FL FR BL BR C LFE (effects 7 to 12) and bins 25 and 13 to 16 to
// FL FR BL BR C inverted (effects 2 to 6), with the filters the game sets at boot (0xB8950 through SetEffectData).
// The game's pan law sends a negative gain to the inverted bins (0x8D480). The rest are effect sends.
constexpr int8_t kNoSpeaker = -1;
constexpr int8_t kBinSpeaker[32] = {
    audio::FrontLeft, audio::FrontRight, audio::Centre, audio::Sub, audio::BackLeft, audio::BackRight,
    audio::FrontLeft, audio::FrontRight, audio::BackLeft, audio::BackRight,
    kNoSpeaker, kNoSpeaker, kNoSpeaker,
    audio::FrontRight, audio::BackLeft, audio::BackRight, audio::Centre,
    kNoSpeaker, kNoSpeaker,
    audio::FrontLeft, audio::FrontRight, audio::BackLeft, audio::BackRight, audio::Centre, audio::Sub,
    audio::FrontLeft,
    kNoSpeaker, kNoSpeaker, kNoSpeaker, kNoSpeaker, kNoSpeaker, kNoSpeaker};
constexpr uint32_t kInvertedBins = 1u << 13 | 1u << 14 | 1u << 15 | 1u << 16 | 1u << 25;

// slots that reach one speaker add up, as the voice processor and the effects add into the bins
void PlaceBin(uint32_t bin, float x, float g[kOuts]) {
    bool inverted = kInvertedBins >> bin & 1;
    g[kBinSpeaker[bin]] += inverted ? -x : x;
}

// Gain of each row on host left and right and into the reverb.
// Rows are the voice's channels, or the two ears and the voice itself for a mono 3D voice. A hardware voice
// holds two channels and owns a run of slots, which alternate between them.
Gains Levels(const Voice& v, const Place& at) {
    static const uint8_t kFront[2] = {0, 1};
    const uint8_t* bin = v.nbins ? v.bin : kFront;
    uint32_t n = v.nbins ? v.nbins : 2;
    uint32_t voices = (std::max<uint32_t>(v.channels, 1) - 1) / 2 + 1;
    uint32_t per = std::max<uint32_t>(n / voices, 2);
    uint32_t step = v.channels == 1 ? 1 : 2;
    bool speakers = false;
    // muted speaker bins do not count: such a voice is heard through its effect send
    for (uint32_t s = 0; s < n; s++) {
        uint32_t b = bin[s];
        int8_t to = kBinSpeaker[b];
        bool speaker = to != kNoSpeaker && to != audio::Sub;
        speakers |= speaker && v.bin_mb[b] > -10000;
    }
    Gains g;
    for (uint32_t c = 0; c < v.channels && c < 2; c++) {
        for (uint32_t s = c; s < per && s < n; s += step) {
            uint32_t b = bin[s];
            float x = Db(v.bin_mb[b]) * v.volume * v.headroom * BinScale(at, b);
            uint32_t r = RowOf(at, v, c, b);
            if (kSends >> b & 1) {
                g.send[r] += std::ldexp(x, -g_bin_headroom[b]);
                continue;
            }
            if (speakers && kBinSpeaker[b] == kNoSpeaker) continue;  // other effect sends
            float* row = g.out[r];
            if (speakers) {
                PlaceBin(b, x, row);
                continue;
            }
            // a voice with no speaker bin plays on the front pair
            row[audio::FrontLeft] = std::max(row[audio::FrontLeft], x);
            row[audio::FrontRight] = std::max(row[audio::FrontRight], x);
        }
    }
    // a sound placed at NaN goes silent alone; its NaN gain would silence the whole mix
    for (uint32_t r = 0; r < kRows; r++) {
        for (uint32_t o = 0; o < kOuts; o++)
            if (!std::isfinite(g.out[r][o])) g.out[r][o] = 0;
        if (!std::isfinite(g.send[r])) g.send[r] = 0;
    }
    return g;
}

// 3D levels follow the listener once a game frame: stepped per block, a fast turn clicks
Gains UpdateRamp(Voice& v, const Gains& g, uint32_t frames) {
    if (!v.has_gain) v.gain = g;
    v.has_gain = true;
    Gains step;
    for (uint32_t r = 0; r < kRows; r++) {
        for (uint32_t o = 0; o < kOuts; o++) step.out[r][o] = (g.out[r][o] - v.gain.out[r][o]) / float(frames);
        step.send[r] = (g.send[r] - v.gain.send[r]) / float(frames);
    }
    return step;
}

// a row's level on one output or on the reverb send, ramped across the block
struct Lane {
    uint32_t row;
    uint32_t out;
    float gain;
    float step;
};

// The levels a block can hear. A level that is zero at both ends of its ramp only ever adds zero to a mix that
// starts at +0, so leaving it out changes no bit.
struct Lanes {
    Lane out[kRows * kOuts];
    uint32_t outs = 0;
    Lane send[kRows];
    uint32_t sends = 0;
};

Lanes AudibleLanes(const Gains& from, const Gains& step) {
    Lanes l;
    for (uint32_t r = 0; r < kRows; r++) {
        for (uint32_t o = 0; o < kOuts; o++) {
            if (from.out[r][o] != 0 || step.out[r][o] != 0) l.out[l.outs++] = {r, o, from.out[r][o], step.out[r][o]};
        }
        if (from.send[r] != 0 || step.send[r] != 0) l.send[l.sends++] = {r, 0, from.send[r], step.send[r]};
    }
    return l;
}

// one frame of the voice's rows into the mix, each level a step further along its ramp
void MixFrame(Lanes& l, const float s[kRows], float* frame, float& send) {
    for (uint32_t k = 0; k < l.outs; k++) {
        Lane& a = l.out[k];
        a.gain += a.step;
        frame[a.out] += s[a.row] * a.gain;
    }
    for (uint32_t k = 0; k < l.sends; k++) {
        Lane& a = l.send[k];
        a.gain += a.step;
        send += s[a.row] * a.gain;
    }
}

// the 3D voice's recent samples, the ear further from the sound reads them later
void PushEar(Voice& v, float x) {
    v.ear[v.ear_pos % kEarLength] = x;
    v.ear_pos++;
}
float EarAt(const Voice& v, float delay) {
    float back = std::max(delay, 0.0f);
    uint32_t whole = uint32_t(back);
    float part = back - float(whole);
    float now = v.ear[(v.ear_pos - 1 - whole) % kEarLength];
    float before = v.ear[(v.ear_pos - 2 - whole) % kEarLength];
    return now + (before - now) * part;
}

// The stream's two channels at its position, between this frame and the next. Finished packets complete on the
// way; false once it has run dry.
bool StreamFrame(Voice& v, float s[2]) {
    uint32_t f = uint32_t(v.pos);
    while (!v.packets.empty() && f >= FramesOf(v, v.packets.front().size)) {
        Packet pk = v.packets.front();
        v.packets.pop_front();
        pk.used = pk.size;
        Complete(v, pk, 0);
        v.pos -= FramesOf(v, pk.size);
        f = uint32_t(v.pos);
    }
    if (v.packets.empty()) {
        v.pos = 0;
        if (v.ending) Deactivate(v);
        return false;
    }
    Packet& pk = v.packets.front();
    float t = float(v.pos - f);
    int channels = std::min<int>(v.channels, 2);
    for (int c = 0; c < channels; c++) {
        float now = Sample(v, pk.data, f, c);
        float next = now;
        if (f + 1 < FramesOf(v, pk.size)) next = Sample(v, pk.data, f + 1, c);
        else if (v.packets.size() > 1) next = Sample(v, v.packets[1].data, 0, c);
        s[c] = now + (next - now) * t;
    }
    if (channels == 1) s[1] = s[0];  // a mono voice reads its one channel as both
    return true;
}

// a buffer's frames it plays to, or loops between; the mix holds the lock, so they stay for the block
struct Region {
    uint32_t start;
    uint32_t end;
};

Region RegionOf(const Voice& v) {
    Region at;
    at.start = v.looping ? FramesOf(v, v.loop_start) : 0;
    at.end = FramesOf(v, v.looping ? v.loop_start + v.loop_len : v.play_len);
    return at;
}

// The buffer's two channels at its position, looping or stopping at the end of its region; false once stopped.
bool BufferFrame(Voice& v, const Region& region, float s[2]) {
    uint32_t f = uint32_t(v.pos);
    uint32_t start = region.start;
    uint32_t end = region.end;
    if (f >= end) {
        if (!v.looping || end <= start) {
            v.playing = false;
            v.pos = 0;
            return false;
        }
        v.pos = start + std::fmod(v.pos - start, double(end - start));
        f = uint32_t(v.pos);
    }
    if (!v.data) return false;
    uint32_t base = v.data + v.play_start;
    // the frame after the last is the loop start, or the last again
    uint32_t next = f;
    if (f + 1 < end) next = f + 1;
    else if (v.looping) next = start;
    float t = float(v.pos - f);
    int channels = std::min<int>(v.channels, 2);
    for (int c = 0; c < channels; c++) {
        float now = Sample(v, base, f, c);
        s[c] = now + (Sample(v, base, next, c) - now) * t;
    }
    if (channels == 1) s[1] = s[0];  // a mono voice reads its one channel as both
    return true;
}

// one voice's block into the mix and the reverb send
void MixVoice(Voice& v, float* out, float* send, uint32_t frames) {
    Place at = Locate(v);
    Gains g = Levels(v, at);
    bool ears = at.on && v.channels == 1;
    float itd = Itd(at.azimuth, audio::IsSurround());
    if (!v.has_gain) v.itd = itd;
    float itd_step = (itd - v.itd) / float(frames);
    Gains dg = UpdateRamp(v, g, frames);
    Lanes lanes = AudibleLanes(v.gain, dg);
    Region region = RegionOf(v);
    double step = double(v.freq ? v.freq : v.rate) / audio::kRate;
    // the game may have rewritten sample memory since the last block: a block left behind is decoded again
    v.spare_block = -1;
    v.block_decoded_now = false;
    for (uint32_t i = 0; i < frames; i++) {
        float s[kRows];
        bool more = v.stream ? StreamFrame(v, s) : BufferFrame(v, region, s);
        if (!more) break;
        // before the ears, the bins and the reverb send, as the voice processor does
        v.filter.Run(s, v.channels);
        s[kDry] = s[0];
        if (ears) {
            v.itd += itd_step;
            PushEar(v, s[0]);
            s[kLeftEar] = EarAt(v, v.itd);
            s[kRightEar] = EarAt(v, -v.itd);
        }
        MixFrame(lanes, s, out + i * kOuts, send[i]);
        v.pos += step;
    }
    v.gain = g;
    v.itd = itd;
}

// the device clips once, after the fold to stereo sums the speakers
void Render(float* out, uint32_t frames) {
    std::memset(out, 0, frames * kOuts * sizeof(float));
    static std::vector<float> send;  // the output thread alone renders
    send.assign(frames, 0.0f);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (auto& [obj, v] : g_voices) {
        if (v.paused || v.sync || (!v.stream && !v.playing)) continue;
        MixVoice(v, out, send.data(), frames);
    }
    g_reverb.Run(send.data(), out, frames);
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Run(X86Context& ctx, const std::vector<Done>& done) {
    for (const Done& d : done) CallGuest(ctx, d.callback, {d.context, d.packet_context, d.status});
}

void Start() {
    if (g_started) return;
    g_started = true;
    audio::StartOutput(Render);
    // for a title that never calls DirectSoundDoWork: its callbacks come from here instead
    StartHostThread("dsound", [](X86Context& ctx) {
        constexpr int64_t kPumpMs = 250;
        for (;;) {
            std::vector<Done> done;
            {
                std::unique_lock<std::recursive_mutex> lock(g_lock);
                g_done_cv.wait_for(lock, std::chrono::milliseconds(50));
                if (NowMs() - g_pumped < kPumpMs) continue;
                done.assign(g_done.begin(), g_done.end());
                g_done.clear();
            }
            Run(ctx, done);
        }
    });
}

// WAVEFORMATEX
constexpr uint32_t kWfxChannels = 2;
constexpr uint32_t kWfxRate = 4;
constexpr uint32_t kWfxBlockAlign = 12;
constexpr uint32_t kWfxBits = 14;

void ReadFormat(Voice& v, uint32_t wfx) {
    if (!wfx) return;
    v.filter.Reset();
    v.tag = R16(wfx);
    v.channels = std::max<uint16_t>(R16(wfx + kWfxChannels), 1);
    v.rate = R32(wfx + kWfxRate);
    v.align = std::max<uint16_t>(R16(wfx + kWfxBlockAlign), 1);
    v.bits = R16(wfx + kWfxBits);
    if (v.tag != kFormatPcm && v.tag != kFormatXboxAdpcm) LOG_FIRST(8, "dsound: format tag %X not supported", v.tag);
}

// DSFILTERDESC: mode, PEQ Q, four coefficients. Null resends the last one (XDK 0x21F205), so nothing changes
constexpr uint32_t kFilterDescMode = 0;
constexpr uint32_t kFilterDescCoefficients = 8;

uint32_t SetFilter(Voice& v, uint32_t desc) {
    if (!desc) return 0;
    uint32_t mode = R32(desc + kFilterDescMode) & 3;
    uint32_t coefficients[4];
    for (int i = 0; i < 4; i++) coefficients[i] = R32(desc + kFilterDescCoefficients + i * 4);
    bool applied = v.filter.Set(mode, coefficients, v.is3d, v.channels);
    if (!applied) LOG_FIRST(8, "dsound: filter mode %u not supported", mode);
    return 0;
}

// DSMIXBINS: count, then a pointer to (bin, volume) pairs of 8 bytes
constexpr uint32_t kMixBinsPairs = 4;
constexpr uint32_t kMixBinPairSize = 8;
constexpr uint32_t kMixBinPairVolume = 4;

// DSMIXBINS: the voice's bins and their volumes
void SetBins(Voice& v, uint32_t mixbins) {
    v.nbins = 0;  // none listed: front left and right
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 8);
    uint32_t pairs = R32(mixbins + kMixBinsPairs);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t pair = pairs + i * kMixBinPairSize;
        v.bin[i] = R32(pair) & 31;
        v.bin_mb[v.bin[i]] = int32_t(R32(pair + kMixBinPairVolume));
    }
    v.nbins = uint8_t(n);
}
// volumes only, the bins stay
void SetBinVolumes(Voice& v, uint32_t mixbins) {
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 32);
    uint32_t pairs = R32(mixbins + kMixBinsPairs);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t pair = pairs + i * kMixBinPairSize;
        v.bin_mb[R32(pair) & 31] = int32_t(R32(pair + kMixBinPairVolume));
    }
}
// 600 mB unless 3D or a mix in or effect in buffer
float DefaultHeadroom(uint32_t flags) { return flags & 0x382010 ? 1.0f : Db(-600); }

}  // namespace sound
