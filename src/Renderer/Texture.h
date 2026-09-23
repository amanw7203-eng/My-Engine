#pragma once

#include <glad/gl.h>

#include <filesystem>

// Owns an OpenGL 2D texture loaded from an image file, with a full mipmap
// chain. Like Shader and Mesh, it must be destroyed before the GL context.
class Texture
{
public:
    // If the file can't be loaded, the texture becomes a magenta/black
    // checkerboard so the problem is obvious on screen instead of silent.
    explicit Texture(const std::filesystem::path& path);

    // Creates a texture from raw pixels: width * height * 4 bytes, RGBA,
    // bottom row first.
    Texture(const unsigned char* rgbaPixels, int width, int height);

    ~Texture();

    // One Texture owns one GL texture: copying would double-delete it.
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    // Binds to texture unit `slot` (0, 1, 2, ...). The shader's sampler
    // uniform must be set to the same slot number.
    void Bind(unsigned int slot = 0) const;

    // False if the image failed to load and the fallback is in use.
    bool IsLoaded() const { return m_Loaded; }
    int GetWidth() const { return m_Width; }
    int GetHeight() const { return m_Height; }

private:
    void Upload(const unsigned char* pixels, int width, int height);

    GLuint m_Texture = 0;
    int m_Width = 0;
    int m_Height = 0;
    bool m_Loaded = false;
};
