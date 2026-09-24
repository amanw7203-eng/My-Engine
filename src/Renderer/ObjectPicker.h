#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <cstdint>

// Finds what is under a pixel by drawing the scene into a 1x1 integer
// render target, each object writing its own ID number instead of a color,
// and reading that one pixel back. Pixel-exact for any mesh, and only costs
// anything on the frames where it is used (e.g. a mouse click).
// Like the other GL owners, it must be destroyed before the GL context.
//
// Use:  BeginRender() -> draw with projection = GetPickMatrix(...) * projection,
//       each object writing its ID -> EndRender() returns the ID (0 = none)
class ObjectPicker
{
public:
    ObjectPicker();
    ~ObjectPicker();

    // One ObjectPicker owns its GL objects: copying would double-delete them.
    ObjectPicker(const ObjectPicker&) = delete;
    ObjectPicker& operator=(const ObjectPicker&) = delete;

    // Zooms a projection in on one pixel of the view, so the 1x1 target sees
    // exactly what that pixel shows. `pixel` is in framebuffer pixels from
    // the top-left, `viewSize` is the full view's size in pixels.
    static glm::mat4 GetPickMatrix(const glm::vec2& pixel, const glm::vec2& viewSize);

    // Binds and clears the 1x1 target and sets the viewport to it.
    void BeginRender() const;

    // Reads the ID that ended up in the pixel and switches back to the
    // window's framebuffer; the caller must restore the viewport.
    std::uint32_t EndRender() const;

private:
    GLuint m_Framebuffer = 0;
    GLuint m_IdTexture = 0;
    GLuint m_DepthBuffer = 0;
};
