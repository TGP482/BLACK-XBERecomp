#pragma once
// XDK data layouts come from the image config (sdk/xdk/<build>/layout.toml), data addresses from its [data]. One the
// config lacks is unknown and traps where it is first needed rather than being guessed.
#include <cstdint>
#include <cstdlib>
#include <iterator>

#include "core/log.h"
#include "image_symbols.h"

namespace xdk {
constexpr uint32_t kUnknown = 0xFFFFFFFFu;
struct LayoutKey {
    const char* name;
    uint32_t value;
};
template <size_t N>
constexpr bool AllKnown(const LayoutKey (&keys)[N]) {
    for (auto& k : keys)
        if (k.value == kUnknown) return false;
    return true;
}
[[noreturn]] inline void LayoutMissing(const char* lib, const LayoutKey* keys, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (keys[i].value == kUnknown) LOGE("%s: %s is unknown for this image, add it to the config", lib, keys[i].name);
    logx::Flush();
    std::abort();
}
}  // namespace xdk

#define XDK_KEY(name) ::xdk::LayoutKey{#name, XDK_##name}
#define XDK_DATA(name) ::xdk::LayoutKey{#name, HAS(name) ? uint32_t(SYM(name)) : ::xdk::kUnknown}
#ifdef IMAGE_NAMED
// the loaded image's data addresses are known at run time only
#define XDK_REQUIRE(lib, ...)                                                                  \
    do {                                                                                       \
        const ::xdk::LayoutKey keys_[] = {__VA_ARGS__};                                        \
        if (!::xdk::AllKnown(keys_)) ::xdk::LayoutMissing(lib, keys_, std::size(keys_));        \
    } while (0)
#else
#define XDK_REQUIRE(lib, ...)                                                                  \
    do {                                                                                       \
        static constexpr ::xdk::LayoutKey keys_[] = {__VA_ARGS__};                             \
        if constexpr (!::xdk::AllKnown(keys_)) ::xdk::LayoutMissing(lib, keys_, std::size(keys_)); \
    } while (0)
#endif

#ifndef XDK_CDevice_Push
#define XDK_CDevice_Push 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_PushThreshold
#define XDK_CDevice_PushThreshold 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_BaseVertex
#define XDK_CDevice_BaseVertex 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_IndexBuffer
#define XDK_CDevice_IndexBuffer 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_ZScale
#define XDK_CDevice_ZScale 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_SuperSampleX
#define XDK_CDevice_SuperSampleX 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_SuperSampleY
#define XDK_CDevice_SuperSampleY 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_Transforms
#define XDK_CDevice_Transforms 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_Textures
#define XDK_CDevice_Textures 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_Palettes
#define XDK_CDevice_Palettes 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_RenderTarget
#define XDK_CDevice_RenderTarget 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_Depth
#define XDK_CDevice_Depth 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_BackBuffer
#define XDK_CDevice_BackBuffer 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_FrontBuffer
#define XDK_CDevice_FrontBuffer 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_SwapCount
#define XDK_CDevice_SwapCount 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_Size
#define XDK_CDevice_Size 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_Flags
#define XDK_CDevice_Flags 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_Time
#define XDK_CDevice_Time 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_PixelShader
#define XDK_CDevice_PixelShader 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_VertexShader
#define XDK_CDevice_VertexShader 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_BackBufferScale
#define XDK_CDevice_BackBufferScale 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_Viewport
#define XDK_CDevice_Viewport 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_ScreenSpaceOffset
#define XDK_CDevice_ScreenSpaceOffset 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_AutoDepth
#define XDK_CDevice_AutoDepth 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_VBlankCallback
#define XDK_CDevice_VBlankCallback 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_VBlankEvent
#define XDK_CDevice_VBlankEvent 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_DisplayFlags
#define XDK_CDevice_DisplayFlags 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_VBlankCount
#define XDK_CDevice_VBlankCount 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_Field
#define XDK_CDevice_Field 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_GammaRamps
#define XDK_CDevice_GammaRamps 0xFFFFFFFFu
#endif
#ifndef XDK_CDevice_GammaPending
#define XDK_CDevice_GammaPending 0xFFFFFFFFu
#endif
#ifndef XDK_PixelShader_Definition
#define XDK_PixelShader_Definition 0xFFFFFFFFu
#endif
#ifndef XDK_DirtyFlags_Texture
#define XDK_DirtyFlags_Texture 0xFFFFFFFFu
#endif
#ifndef XDK_TextureState_Stage
#define XDK_TextureState_Stage 0xFFFFFFFFu
#endif
#ifndef XDK_Stream_Size
#define XDK_Stream_Size 0xFFFFFFFFu
#endif
#ifndef XDK_Stream_Stride
#define XDK_Stream_Stride 0xFFFFFFFFu
#endif
#ifndef XDK_Stream_Buffer
#define XDK_Stream_Buffer 0xFFFFFFFFu
#endif
#ifndef XDK_CDirectSoundStream_Vtable
#define XDK_CDirectSoundStream_Vtable 0xFFFFFFFFu
#endif
#ifndef XDK_CDirectSoundStream_Vtable2
#define XDK_CDirectSoundStream_Vtable2 0xFFFFFFFFu
#endif
#ifndef XDK_EffectsImage_Skip0
#define XDK_EffectsImage_Skip0 0xFFFFFFFFu
#endif
#ifndef XDK_EffectsImage_Skip1
#define XDK_EffectsImage_Skip1 0xFFFFFFFFu
#endif
#ifndef XDK_EffectsImage_Desc
#define XDK_EffectsImage_Desc 0xFFFFFFFFu
#endif
#ifndef XDK_XPP_Current
#define XDK_XPP_Current 0xFFFFFFFFu
#endif
#ifndef XDK_XPP_Changed
#define XDK_XPP_Changed 0xFFFFFFFFu
#endif
#ifndef XDK_XPP_Previous
#define XDK_XPP_Previous 0xFFFFFFFFu
#endif
