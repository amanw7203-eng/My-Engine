#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

class Camera;

// A depth texture rendered from the light's point of view. The lit shader
// compares each pixel's distance from the light against it: if something
// closer to the light was recorded there, the pixel is in shadow.
// Like the other GL owners, it must be destroyed before the GL context.
//
// Per frame:  FitToView() -> GetLightSpaceMatrix() -> BeginRender() -> draw the scene with
//             the depth shader -> EndRender() -> BindTexture() for the lit pass
class ShadowMap
{
public:
    explicit ShadowMap(int resolution = 2048);
    ~ShadowMap();

    // One ShadowMap owns its GL objects: copying would double-delete them.
    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    // A sphere in world space that the shadow map should cover.
    struct Bounds
    {
        glm::vec3 center;
        float radius;
    };

    // The sphere around what the camera can see out to `distance`, so no
    // shadow-map texels are spent behind the camera.
    static Bounds FitToView(const Camera& camera, float aspect, float distance);

    // Projection for a directional light shining along `lightDir`, covering
    // `bounds`. The box moves in whole-texel steps so shadow edges don't
    // shimmer as the camera moves.
    glm::mat4 GetLightSpaceMatrix(const glm::vec3& lightDir, const Bounds& bounds) const;

    // Renders into the shadow map: sets its framebuffer and viewport and
    // clears it. EndRender switches back to the window's framebuffer; the
    // caller must restore the viewport.
    void BeginRender() const;
    void EndRender() const;

    // Binds the depth texture to texture unit `slot`.
    void BindTexture(unsigned int slot) const;

    GLuint GetTextureId() const { return m_DepthTexture; }
    int GetResolution() const { return m_Resolution; }

private:
    GLuint m_Framebuffer = 0;
    GLuint m_DepthTexture = 0;
    int m_Resolution = 0;
};
