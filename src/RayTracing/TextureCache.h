#pragma once

#include <cuda_runtime.h>

#include <cstdint>
#include <unordered_map>

class Texture;

// CUDA copies of the engine's textures, so the OptiX programs can sample
// them (the GL textures themselves are out of CUDA's reach). A copy is made
// the first time a texture is asked for: every mip level is read back from
// OpenGL into a CUDA mipmapped array, sampled by the hardware texture units
// with trilinear filtering and wrapping, like the GL texture.
//
// Must be used on the GL thread, and destroyed while the CUDA context lives.
class TextureCache
{
public:
    TextureCache() = default;
    ~TextureCache();

    // One TextureCache owns its CUDA objects: copying would double-free them.
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    // A texture as the programs see it.
    struct Entry
    {
        cudaTextureObject_t srgb = 0;   // decodes sRGB to linear (color images)
        cudaTextureObject_t linear = 0; // values as stored (data: normal, roughness, ...)
        float log2Size = 0.0f;          // log2 of the larger side, for choosing mip levels
    };

    // The CUDA copy of `texture`, made now if it isn't yet. All zero if it
    // couldn't be made (the reason has been logged).
    const Entry& Get(const Texture& texture);

    // Call once per frame, after the Get calls for that frame: frees copies
    // not asked for in a while (their texture was removed, or no drawn
    // material uses it any more).
    void EndFrame();

private:
    struct Cached
    {
        Entry entry;
        cudaMipmappedArray_t array = nullptr;
        std::uint64_t lastUsedFrame = 0;
    };

    static void Release(Cached& cached);

    std::unordered_map<std::uint64_t, Cached> m_Cache; // by Texture::GetSerial()
    std::uint64_t m_Frame = 0;
};
