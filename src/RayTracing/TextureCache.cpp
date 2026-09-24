#include "RayTracing/TextureCache.h"

#include "RayTracing/CudaCheck.h"
#include "Renderer/Texture.h"

#include <glad/gl.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    // Copies nobody has asked for in this many frames are freed. Long enough
    // that briefly unassigning a texture doesn't mean uploading it again.
    constexpr std::uint64_t kUnusedFramesBeforeRelease = 300;

    cudaTextureObject_t CreateTextureObject(cudaMipmappedArray_t array, unsigned int levels, bool srgb)
    {
        cudaResourceDesc resource{};
        resource.resType = cudaResourceTypeMipmappedArray;
        resource.res.mipmap.mipmap = array;

        cudaTextureDesc description{};
        // Repeat outside 0..1 (a tiled floor), blend within and between mip
        // levels: the same as the GL texture.
        description.addressMode[0] = cudaAddressModeWrap;
        description.addressMode[1] = cudaAddressModeWrap;
        description.filterMode = cudaFilterModeLinear;
        description.mipmapFilterMode = cudaFilterModeLinear;
        description.maxMipmapLevelClamp = static_cast<float>(levels - 1);
        description.maxAnisotropy = 8;
        description.normalizedCoords = 1;
        // 8-bit values read as 0..1 floats; for color images, decoded from
        // sRGB before filtering (the correct order).
        description.readMode = cudaReadModeNormalizedFloat;
        description.sRGB = srgb ? 1 : 0;

        cudaTextureObject_t object = 0;
        if (!CUDA_CHECK(cudaCreateTextureObject(&object, &resource, &description, nullptr)))
            return 0;
        return object;
    }
}

TextureCache::~TextureCache()
{
    for (auto& [serial, cached] : m_Cache)
        Release(cached);
}

const TextureCache::Entry& TextureCache::Get(const Texture& texture)
{
    static const Entry kNone{};

    auto found = m_Cache.find(texture.GetSerial());
    if (found != m_Cache.end())
    {
        found->second.lastUsedFrame = m_Frame;
        return found->second.entry;
    }

    Cached& cached = m_Cache[texture.GetSerial()];
    cached.lastUsedFrame = m_Frame;

    // The GL texture already has its full mip chain; read every level back.
    const GLuint id = texture.GetId();
    GLint width = 0, height = 0;
    glGetTextureLevelParameteriv(id, 0, GL_TEXTURE_WIDTH, &width);
    glGetTextureLevelParameteriv(id, 0, GL_TEXTURE_HEIGHT, &height);
    if (width <= 0 || height <= 0)
        return kNone;
    const unsigned int levels = 1 + static_cast<unsigned int>(std::floor(std::log2((std::max)(width, height))));

    const cudaChannelFormatDesc format = cudaCreateChannelDesc<uchar4>();
    const cudaExtent extent = make_cudaExtent(static_cast<size_t>(width), static_cast<size_t>(height), 0);
    if (!CUDA_CHECK(cudaMallocMipmappedArray(&cached.array, &format, extent, levels)))
    {
        cached.array = nullptr;
        return kNone;
    }

    std::vector<unsigned char> pixels;
    for (unsigned int level = 0; level < levels; ++level)
    {
        GLint levelWidth = 0, levelHeight = 0;
        glGetTextureLevelParameteriv(id, static_cast<GLint>(level), GL_TEXTURE_WIDTH, &levelWidth);
        glGetTextureLevelParameteriv(id, static_cast<GLint>(level), GL_TEXTURE_HEIGHT, &levelHeight);
        pixels.resize(static_cast<size_t>(levelWidth) * levelHeight * 4);
        // RGBA8 rows are always a multiple of 4 bytes, so the default pack
        // alignment adds no padding. Rows come bottom first, as UV v = 0
        // expects in both GL and CUDA.
        glGetTextureImage(id, static_cast<GLint>(level), GL_RGBA, GL_UNSIGNED_BYTE,
                          static_cast<GLsizei>(pixels.size()), pixels.data());

        cudaArray_t levelArray = nullptr;
        const size_t rowBytes = static_cast<size_t>(levelWidth) * 4;
        if (!CUDA_CHECK(cudaGetMipmappedArrayLevel(&levelArray, cached.array, level)) ||
            !CUDA_CHECK(cudaMemcpy2DToArray(levelArray, 0, 0, pixels.data(), rowBytes, rowBytes,
                                            static_cast<size_t>(levelHeight), cudaMemcpyHostToDevice)))
        {
            Release(cached);
            return kNone;
        }
    }

    cached.entry.srgb = CreateTextureObject(cached.array, levels, true);
    cached.entry.linear = CreateTextureObject(cached.array, levels, false);
    cached.entry.log2Size = std::log2(static_cast<float>((std::max)(width, height)));
    if (!cached.entry.srgb || !cached.entry.linear)
    {
        Release(cached);
        return kNone;
    }
    return cached.entry;
}

void TextureCache::EndFrame()
{
    for (auto it = m_Cache.begin(); it != m_Cache.end();)
    {
        if (m_Frame - it->second.lastUsedFrame > kUnusedFramesBeforeRelease)
        {
            Release(it->second);
            it = m_Cache.erase(it);
        }
        else
            ++it;
    }
    ++m_Frame;
}

void TextureCache::Release(Cached& cached)
{
    if (cached.entry.srgb)
        cudaDestroyTextureObject(cached.entry.srgb);
    if (cached.entry.linear)
        cudaDestroyTextureObject(cached.entry.linear);
    if (cached.array)
        cudaFreeMipmappedArray(cached.array);
    cached = Cached{};
}
