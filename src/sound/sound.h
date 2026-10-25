// DirectSound (XDK 5849) on a host software mixer. Buffers and streams are read from guest memory as they play.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include "audio/output.h"
#include "sound/filter.h"
#include "sound/reverb.h"
#include "xdk/hle.h"

namespace sound {
using namespace kernel;

// WAVEFORMATEX tags the mixer reads
constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatXboxAdpcm = 0x69;
// an Xbox ADPCM block holds 64 played frames in 36 bytes per channel
constexpr uint32_t kAdpcmFrames = 64;
constexpr uint32_t kAdpcmBlockBytes = 36;

// XMEDIAPACKET as the title queued it
struct Packet {
    uint32_t data;
    uint32_t size;
    uint32_t completed_ptr;
    uint32_t status_ptr;
    uint32_t context;
    uint32_t used = 0;
};

constexpr uint32_t kEarLength = 64;  // past the longest interaural delay, 31 samples
// a voice's levels: rows are its channels, or for a mono 3D voice the two ears and the voice itself
constexpr uint32_t kRows = 3;
constexpr uint32_t kLeftEar = 0;
constexpr uint32_t kRightEar = 1;
constexpr uint32_t kDry = 2;
constexpr uint32_t kOuts = audio::kChannels;  // the speakers, in audio::Speaker order

// levels of a voice's rows on the host outputs
struct Gains {
    float out[kRows][kOuts] = {};
    float send[kRows] = {};  // into the reverb
};

struct Voice {
    uint32_t obj = 0;
    bool stream = false;
    int refs = 1;
    // format
    uint16_t tag = kFormatPcm;
    uint16_t channels = 1;
    uint16_t bits = 16;
    uint16_t align = 2;
    uint32_t rate = 48000;
    // buffer
    // play region in bytes from data, loop region in bytes from the play region
    uint32_t data = 0;
    uint32_t bytes = 0;
    uint32_t loop_start = 0;
    uint32_t loop_len = 0;
    uint32_t play_start = 0;
    uint32_t play_len = 0;
    uint32_t owned = 0;  // the buffer's own memory, made with it: freed by SetBufferData and Release
    bool playing = false;
    bool looping = false;
    bool paused = false;
    // stream: took a packet since it went quiet, quiet again once they run out
    bool active = false;
    bool ending = false;
    bool sync = false;  // held until IDirectSound_SynchPlayback
    double pos = 0;  // frames
    uint32_t freq = 0;
    // stream
    uint32_t max_packets = 1;
    uint32_t callback = 0;
    uint32_t context = 0;
    std::deque<Packet> packets;
    // levels
    float volume = 1;
    float headroom = 1;
    VoiceFilter filter;
    uint8_t nbins = 0;
    uint8_t bin[8] = {};
    int32_t bin_mb[32] = {};  // by mix bin
    std::vector<float> curve;  // rolloff
    bool is3d = false;
    uint32_t mode = 0;  // DS3DMODE: 1 head relative, 2 disabled
    float p[3] = {};
    float min_dist = 1;
    float max_dist = 1e9f;
    Gains gain;  // the last block's levels, the next block ramps from them
    bool has_gain = false;
    // a mono 3D voice's interaural delay in samples and its recent samples
    float itd = 0;
    float ear[kEarLength] = {};
    uint32_t ear_pos = 0;
    // ADPCM block cache: the block read last, and the block read before it when this mix block decoded that one
    int64_t block = -1;
    int64_t spare_block = -1;
    bool block_decoded_now = false;  // this mix block decoded block
    uint8_t slot = 0;                // decoded[slot] holds block
    int16_t decoded[2][65 * 8];
};

// a 3D voice as the library's 3D pass sees it
struct Place {
    bool on = false;
    float att = 1;
    float azimuth = 0;
    float front = 1;  // crosstalk front pair
    float back = 0;   // crosstalk back pair, silent on a stereo console
};

struct Done {
    uint32_t voice;
    uint32_t callback;
    uint32_t context;
    uint32_t packet_context;
    uint32_t status;
};

inline std::recursive_mutex g_lock;
inline std::map<uint32_t, Voice> g_voices;
inline float g_listener[3] = {};
inline float g_front[3] = {0, 0, 1};
inline float g_top[3] = {0, 1, 0};
// Completed packets' callbacks, run as the XDK runs them: in DirectSoundDoWork on the title's thread, so they never
// overlap the title's own stream code. A stream flushed or released runs its own at once, before it is gone.
inline std::deque<Done> g_done;
inline std::condition_variable_any g_done_cv;
inline std::atomic<int64_t> g_pumped{0};          // the last DirectSoundDoWork, steady ms
inline bool g_started = false;
inline uint32_t g_ds = 0;
inline bool g_headphones = false;  // IDirectSound_EnableHeadphones
// BkXboxFx.bin's reverb (effect 16) reads its input from mix bin 11 alone
constexpr uint32_t kSends = 1u << 11;
// IDirectSound_SetMixBinHeadroom, a step halves the bin
inline uint8_t g_bin_headroom[32] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0};

inline float Db(int32_t hundredths) { return hundredths <= -10000 ? 0.0f : std::pow(10.0f, hundredths / 2000.0f); }

inline uint32_t FramesOf(const Voice& v, uint32_t bytes) {
    if (v.tag == kFormatXboxAdpcm) return bytes / (kAdpcmBlockBytes * v.channels) * kAdpcmFrames;
    return bytes / std::max<uint32_t>(v.align, 1);
}
inline uint32_t BytesOf(const Voice& v, uint32_t frames) {
    if (v.tag == kFormatXboxAdpcm) return frames / kAdpcmFrames * kAdpcmBlockBytes * v.channels;
    return frames * v.align;
}

constexpr uint32_t kCaps3d = 0x10;  // DSBCAPS_CTRL3D
constexpr uint32_t kErrorFail = 0x80004005u;  // E_FAIL

// a stream goes quiet: a started one loses its pause
inline void Deactivate(Voice& v) {
    if (v.active) v.active = v.paused = v.ending = false;
}

inline Voice* Find(uint32_t obj) {
    auto it = g_voices.find(obj);
    return it == g_voices.end() ? nullptr : &it->second;
}

inline float F32(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Start();
void Complete(Voice& v, Packet& pk, uint32_t status);
void Run(X86Context& ctx, const std::vector<Done>& done);
int64_t NowMs();
void ReadFormat(Voice& v, uint32_t wfx);
uint32_t SetFilter(Voice& v, uint32_t desc);
void SetBins(Voice& v, uint32_t mixbins);
void SetBinVolumes(Voice& v, uint32_t mixbins);
float DefaultHeadroom(uint32_t flags);

}  // namespace sound
