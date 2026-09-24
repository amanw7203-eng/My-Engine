#include "Renderer/ObjectPicker.h"

#include <glm/gtc/matrix_transform.hpp>

#include <iostream>

ObjectPicker::ObjectPicker()
{
    // One unsigned-integer texel for the ID...
    glGenTextures(1, &m_IdTexture);
    glBindTexture(GL_TEXTURE_2D, m_IdTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, 1, 1, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    // ...and a depth buffer, so the nearest object wins as usual.
    glGenRenderbuffers(1, &m_DepthBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, m_DepthBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 1, 1);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &m_Framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_IdTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_DepthBuffer);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        std::cerr << "Object picker framebuffer is incomplete\n";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

ObjectPicker::~ObjectPicker()
{
    glDeleteFramebuffers(1, &m_Framebuffer);
    glDeleteRenderbuffers(1, &m_DepthBuffer);
    glDeleteTextures(1, &m_IdTexture);
}

glm::mat4 ObjectPicker::GetPickMatrix(const glm::vec2& pixel, const glm::vec2& viewSize)
{
    // The pixel's position in normalized device coordinates (-1..1, y up).
    const float ndcX = pixel.x / viewSize.x * 2.0f - 1.0f;
    const float ndcY = 1.0f - pixel.y / viewSize.y * 2.0f;

    // Move that point to the centre, then scale so one pixel (2 / size in
    // NDC) fills the whole -1..1 range of the 1x1 target. Applied after the
    // projection, in clip space, the translate is multiplied by w, which is
    // exactly what keeps it correct under perspective.
    return glm::scale(glm::mat4(1.0f), glm::vec3(viewSize.x, viewSize.y, 1.0f))
         * glm::translate(glm::mat4(1.0f), glm::vec3(-ndcX, -ndcY, 0.0f));
}

void ObjectPicker::BeginRender() const
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_Framebuffer);
    glViewport(0, 0, 1, 1);
    const GLuint noEntity[] = { 0, 0, 0, 0 };
    glClearBufferuiv(GL_COLOR, 0, noEntity);
    glClear(GL_DEPTH_BUFFER_BIT);
}

std::uint32_t ObjectPicker::EndRender() const
{
    // Waits for the GPU to finish drawing, but it's one pixel on one frame.
    GLuint id = 0;
    glReadPixels(0, 0, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &id);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return id;
}
