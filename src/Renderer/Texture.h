#pragma once

#include <glad/gl.h>

#include <cstdint>
#include <filesystem>

// Owns an OpenGL 2D texture loaded from an image file, with a full mipmap
// chain. Like Shader and Mesh, it must be destroyed before the GL context.
class Texture
{
public:
    // How the pixel values should be read, as in Blender's image "Color
    // Space" setting:
    //   Srgb     - a picture/color (base color, emission); stored
    //              gamma-encoded, so the shader converts it to linear light.
    //   NonColor - data (normal, roughness, metallic, AO maps); used as-is.
    enum class ColorSpace
    {
        Srgb,
        NonColor,
    };

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
    GLuint GetId() const { return m_Texture; } // e.g. for ImGui::Image previews

    // Different for every image ever loaded (never reused, unlike the
    // Texture's address or GL name), so caches of it can't mix images up.
    // Moving a Texture keeps it.
    std::uint64_t GetSerial() const { return m_Serial; }

    // The file it was loaded from; empty for textures made from raw pixels.
    const std::filesystem::path& GetPath() const { return m_Path; }
    // The file was renamed or moved on disk (the pixels are already loaded).
    void SetPath(std::filesystem::path path) { m_Path = std::move(path); }

    // Only changes how shaders interpret the pixels, so it can be switched
    // at any time without reloading.
    ColorSpace colorSpace = ColorSpace::Srgb;

private:
    void Upload(const unsigned char* pixels, int width, int height);

    std::filesystem::path m_Path;
    GLuint m_Texture = 0;
    int m_Width = 0;
    int m_Height = 0;
    bool m_Loaded = false;
    std::uint64_t m_Serial = 0;
};
