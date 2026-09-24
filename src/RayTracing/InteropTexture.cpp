#include "RayTracing/InteropTexture.h"

#include "RayTracing/CudaCheck.h"

#include <cuda_gl_interop.h>

InteropTexture::~InteropTexture()
{
    Release();
}

bool InteropTexture::Resize(int width, int height)
{
    if (width == m_Width && height == m_Height && m_Resource)
        return true;

    Release();
    if (width <= 0 || height <= 0)
        return false;

    glGenBuffers(1, &m_PixelBuffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_PixelBuffer);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(width) * height * 4, nullptr, GL_STREAM_DRAW);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    glGenTextures(1, &m_Texture);
    glBindTexture(GL_TEXTURE_2D, m_Texture);
    // Plain RGBA8, not SRGB8_ALPHA8: the ray tracer writes sRGB-encoded bytes
    // itself, like lit.frag, and the blit must copy them unchanged.
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &m_Framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_Framebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_Texture, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    // CUDA overwrites every pixel each frame, so it never needs the old contents.
    if (!CUDA_CHECK(cudaGraphicsGLRegisterBuffer(&m_Resource, m_PixelBuffer, cudaGraphicsRegisterFlagsWriteDiscard)))
    {
        m_Resource = nullptr;
        Release();
        return false;
    }

    m_Width = width;
    m_Height = height;
    return true;
}

uchar4* InteropTexture::Map(cudaStream_t stream)
{
    if (!m_Resource || !CUDA_CHECK(cudaGraphicsMapResources(1, &m_Resource, stream)))
        return nullptr;
    m_Mapped = true;

    void* pixels = nullptr;
    size_t size = 0;
    if (!CUDA_CHECK(cudaGraphicsResourceGetMappedPointer(&pixels, &size, m_Resource)))
    {
        Unmap(stream);
        return nullptr;
    }
    return static_cast<uchar4*>(pixels);
}

void InteropTexture::Unmap(cudaStream_t stream)
{
    if (!m_Mapped)
        return;
    m_Mapped = false;
    CUDA_CHECK(cudaGraphicsUnmapResources(1, &m_Resource, stream));

    // Pixel buffer -> texture, on the GPU. With a buffer bound to
    // GL_PIXEL_UNPACK_BUFFER, the "pixels" argument is an offset into it.
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_PixelBuffer);
    glBindTexture(GL_TEXTURE_2D, m_Texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_Width, m_Height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

void InteropTexture::BlitToScreen() const
{
    if (!m_Framebuffer)
        return;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_Framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, m_Width, m_Height, 0, 0, m_Width, m_Height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void InteropTexture::Release()
{
    if (m_Resource)
        CUDA_CHECK(cudaGraphicsUnregisterResource(m_Resource));
    m_Resource = nullptr;
    m_Mapped = false;

    // Deleting name 0 is a no-op.
    glDeleteFramebuffers(1, &m_Framebuffer);
    glDeleteTextures(1, &m_Texture);
    glDeleteBuffers(1, &m_PixelBuffer);
    m_Framebuffer = m_Texture = m_PixelBuffer = 0;
    m_Width = m_Height = 0;
}
