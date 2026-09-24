#include "Renderer/ShadowMap.h"

#include "Renderer/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>

ShadowMap::ShadowMap(int resolution)
    : m_Resolution(resolution)
{
    glGenTextures(1, &m_DepthTexture);
    glBindTexture(GL_TEXTURE_2D, m_DepthTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, resolution, resolution, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    // Nearest: the shader does its own filtering (PCF) over several texels.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Outside the map, read depth 1.0 (as far as possible), so anything the
    // map doesn't cover counts as lit rather than shadowed.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float border[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    // Depth reads back in the red channel only; copy it to green and blue
    // too so the debug view in the UI shows grey instead of red.
    const GLint swizzle[] = { GL_RED, GL_RED, GL_RED, GL_ONE };
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
    glBindTexture(GL_TEXTURE_2D, 0);

    // A framebuffer with only a depth attachment: no color is written.
    glGenFramebuffers(1, &m_Framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_DepthTexture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        std::cerr << "Shadow map framebuffer is incomplete\n";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

ShadowMap::~ShadowMap()
{
    glDeleteFramebuffers(1, &m_Framebuffer);
    glDeleteTextures(1, &m_DepthTexture);
}

glm::mat4 ShadowMap::GetLightSpaceMatrix(const glm::vec3& lightDir, const Bounds& bounds) const
{
    // Rotation only: look along the light. lookAt needs an up vector that
    // isn't parallel to the view direction, so switch when the sun is
    // (almost) straight overhead.
    const glm::vec3 dir = glm::normalize(lightDir);
    const glm::vec3 up = std::abs(dir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 lightView = glm::lookAt(glm::vec3(0.0f), dir, up);

    // Centre the box on the bounds, in light space, snapped to the size of
    // one shadow-map texel.
    glm::vec3 center = glm::vec3(lightView * glm::vec4(bounds.center, 1.0f));
    const float texelSize = 2.0f * bounds.radius / static_cast<float>(m_Resolution);
    center.x = std::floor(center.x / texelSize) * texelSize;
    center.y = std::floor(center.y / texelSize) * texelSize;

    // The view looks down -Z, so depths in front are -z. The depth range
    // only has to span the sphere: casters between it and the sun are
    // outside the range but still recorded, because BeginRender turns on
    // depth clamping.
    const float r = bounds.radius;
    const glm::mat4 lightProjection = glm::ortho(center.x - r, center.x + r,
                                                 center.y - r, center.y + r,
                                                 -center.z - r, -center.z + r);
    return lightProjection * lightView;
}

ShadowMap::Bounds ShadowMap::FitToView(const Camera& camera, float aspect, float distance)
{
    // The smallest sphere around the part of the view that gets shadows: a
    // pyramid from the camera out to `distance`. Its centre sits on the view
    // axis, at the point equally far from the camera and from the corners
    // of the far end. With a very wide view that point would be past the
    // far end, so it is clamped there.
    const float tanHalfFov = std::tan(glm::radians(camera.fieldOfView) * 0.5f);
    // (far-corner offset from the view axis / distance), squared
    const float cornerSq = tanHalfFov * tanHalfFov * (1.0f + aspect * aspect);
    const float along = std::min(distance * (1.0f + cornerSq) * 0.5f, distance);
    const float radius = std::max(along, std::sqrt((distance - along) * (distance - along)
                                                   + distance * distance * cornerSq));

    // The radius depends only on the lens, not the camera's direction, so it
    // stays constant while looking around and the texel snapping holds.
    return { camera.GetPosition() + camera.GetForward() * along, radius };
}

void ShadowMap::BeginRender() const
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_Framebuffer);
    glViewport(0, 0, m_Resolution, m_Resolution);
    glClear(GL_DEPTH_BUFFER_BIT);
    // Geometry nearer the sun than the box's near plane would normally be
    // clipped away and cast no shadow; clamping squashes it onto the near
    // plane instead, which still blocks the light.
    glEnable(GL_DEPTH_CLAMP);
}

void ShadowMap::EndRender() const
{
    glDisable(GL_DEPTH_CLAMP);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ShadowMap::BindTexture(unsigned int slot) const
{
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, m_DepthTexture);
}
