#pragma once

#include <glad/gl.h>
#include <glm/vec3.hpp>

#include <span>

// One vertex as stored in the vertex buffer. Must match the attribute
// locations in the shaders: location 0 = position, location 1 = color.
struct Vertex
{
    glm::vec3 position;
    glm::vec3 color;
};

// Owns the GPU buffers for a piece of indexed geometry (VAO + VBO + EBO).
// Like Shader, a Mesh must be destroyed before the OpenGL context is.
class Mesh
{
public:
    Mesh(std::span<const Vertex> vertices, std::span<const unsigned int> indices);
    ~Mesh();

    // One Mesh owns its GL buffers: copying would double-delete them.
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    // Draws the mesh with whichever shader is currently bound.
    void Draw() const;

    // A 1x1 rectangle centred on the origin with a different color per corner.
    static Mesh CreateQuad();

private:
    void Release();

    GLuint m_Vao = 0;
    GLuint m_Vbo = 0;
    GLuint m_Ebo = 0;
    GLsizei m_IndexCount = 0;
};
