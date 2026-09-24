#include "Renderer/Texture.h"

#include <stb_image.h>

#include <atomic>
#include <iostream>
#include <utility>

Texture::Texture(const std::filesystem::path& path)
    : m_Path(path)
{
    // Image files store the top row first, but OpenGL expects UV (0,0) to be
    // the bottom-left, so flip rows while loading.
    stbi_set_flip_vertically_on_load(true);

    // Always ask for 4 channels (RGBA) so every texture has the same layout,
    // whether the file is RGB, greyscale, or already RGBA.
    int width = 0, height = 0, channelsInFile = 0;
    unsigned char* pixels = stbi_load(path.string().c_str(), &width, &height, &channelsInFile, 4);

    if (pixels)
    {
        Upload(pixels, width, height);
        stbi_image_free(pixels);
        m_Loaded = true;
        return;
    }

    std::cerr << "Failed to load texture " << path.string() << ": "
              << stbi_failure_reason() << '\n';

    // 2x2 magenta/black "missing texture" fallback.
    const unsigned char fallback[] = {
        255, 0, 255, 255,   0,   0, 0,   255,
          0, 0,   0, 255,   255, 0, 255, 255,
    };
    Upload(fallback, 2, 2);
}

Texture::Texture(const unsigned char* rgbaPixels, int width, int height)
{
    Upload(rgbaPixels, width, height);
    m_Loaded = true;
}

void Texture::Upload(const unsigned char* pixels, int width, int height)
{
    // Every upload is a new image.
    static std::atomic<std::uint64_t> nextSerial{ 1 };
    m_Serial = nextSerial++;

    m_Width = width;
    m_Height = height;

    glGenTextures(1, &m_Texture);
    glBindTexture(GL_TEXTURE_2D, m_Texture);

    // Repeat the image when UVs go outside 0..1 (lets a floor tile it).
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // Minification (texture shrunk, e.g. far away): blend between the two
    // nearest mip levels, and between pixels within each ("trilinear").
    // Magnification (texture enlarged, up close): mipmaps don't apply, so
    // just blend neighbouring pixels.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // Build every smaller level (half size each time, down to 1x1) from
    // level 0 on the GPU.
    glGenerateMipmap(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, 0);
}

Texture::~Texture()
{
    // glDeleteTextures ignores 0, so moved-from textures are fine.
    glDeleteTextures(1, &m_Texture);
}

Texture::Texture(Texture&& other) noexcept
    : colorSpace(other.colorSpace)
    , m_Path(std::move(other.m_Path))
    , m_Texture(std::exchange(other.m_Texture, 0))
    , m_Width(std::exchange(other.m_Width, 0))
    , m_Height(std::exchange(other.m_Height, 0))
    , m_Loaded(std::exchange(other.m_Loaded, false))
    , m_Serial(std::exchange(other.m_Serial, 0))
{
}

Texture& Texture::operator=(Texture&& other) noexcept
{
    if (this != &other)
    {
        glDeleteTextures(1, &m_Texture);
        colorSpace = other.colorSpace;
        m_Path = std::move(other.m_Path);
        m_Texture = std::exchange(other.m_Texture, 0);
        m_Width = std::exchange(other.m_Width, 0);
        m_Height = std::exchange(other.m_Height, 0);
        m_Loaded = std::exchange(other.m_Loaded, false);
        m_Serial = std::exchange(other.m_Serial, 0);
    }
    return *this;
}

void Texture::Bind(unsigned int slot) const
{
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, m_Texture);
}
