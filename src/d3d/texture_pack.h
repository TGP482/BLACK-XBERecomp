#pragma once
#include "gpu/texture.h"

namespace texture_pack {
bool Enabled();
// Optional host textures; guest memory and the original disc remain unchanged.
bool Load(const gpu::TextureDesc& original, const uint8_t* pixels, const uint32_t* palette, gpu::TextureDesc& replacement,
          std::vector<uint8_t>& blocks);
}  // namespace texture_pack
