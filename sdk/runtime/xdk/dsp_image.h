#pragma once
#include <cstdint>
#include <vector>

// The title's DSP effects image (XAudioDownloadEffectsImage) run on the host: the effects the image chains after the
// voice processor, recognised by their code. Called under DirectSound's lock.
namespace dsp {

constexpr uint32_t kBins = 32;  // mix bins, DSMIXBIN 0..31

// table: offset of the image's effect descriptor table; states: each effect's state segment in guest memory, the
// image's state copied in; reverb: the I3DL2 reverb's effect index from DSEFFECTIMAGELOC, -1 for none
void Load(const std::vector<uint8_t>& img, size_t table, const std::vector<uint32_t>& states, int32_t reverb);
bool Loaded();
// DSI3DL2LISTENER as 12 floats
void SetListener(const float* p);
// frames of kBins interleaved: the voice processor's mix in, the speaker bins 0..5 out
void Run(float* bins, uint32_t frames);

}  // namespace dsp
