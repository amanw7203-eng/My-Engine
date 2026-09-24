#pragma once

#include <glad/gl.h>
#include <cuda_runtime.h>
#include <vector_types.h>

// An RGBA8 OpenGL texture that CUDA fills (CUDA-GL interop), so the ray
// traced image never makes a round trip through the CPU. CUDA writes into a
// GL pixel buffer, which is then copied into the texture on the GPU. Like
// the other GL owners, it must be destroyed before the GL context.
//
// Per frame:  Resize() -> Map() -> launch writing the pixels -> Unmap()
//             -> BlitToScreen()
class InteropTexture
{
public:
    InteropTexture() = default;
    ~InteropTexture();

    // One InteropTexture owns its GL/CUDA objects: copying would double-free them.
    InteropTexture(const InteropTexture&) = delete;
    InteropTexture& operator=(const InteropTexture&) = delete;

    // Makes the texture width x height, recreating it only if the size
    // changed. Returns false if it couldn't be created.
    bool Resize(int width, int height);

    // Hands the pixels to CUDA: width * height RGBA8 values, row by row
    // from the bottom (nullptr on failure). Work queued on `stream` after
    // Map may write them; Unmap waits for that work, then copies the pixels
    // into the texture.
    uchar4* Map(cudaStream_t stream);
    void Unmap(cudaStream_t stream);

    // Copies the texture onto the window's framebuffer, filling the
    // current width x height.
    void BlitToScreen() const;

    int GetWidth() const { return m_Width; }
    int GetHeight() const { return m_Height; }

private:
    void Release();

    GLuint m_PixelBuffer = 0; // what CUDA writes
    GLuint m_Texture = 0;
    GLuint m_Framebuffer = 0; // for glBlitFramebuffer, which reads framebuffers
    cudaGraphicsResource_t m_Resource = nullptr;
    bool m_Mapped = false;
    int m_Width = 0;
    int m_Height = 0;
};
