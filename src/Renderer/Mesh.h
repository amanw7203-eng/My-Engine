#pragma once

#include <glad/gl.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <span>
#include <vector>

// One vertex as stored in the vertex buffer. Must match the attribute
// locations in the shaders: 0 = position, 1 = color, 2 = uv, 3 = normal.
struct Vertex
{
    glm::vec3 position;
    glm::vec3 color;  // multiplied with the texture; white = texture as-is
    glm::vec2 uv;     // texture coordinate: (0,0) bottom-left, (1,1) top-right
    glm::vec3 normal; // unit vector pointing out of the surface, for lighting
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

    // CPU copy of the geometry, kept for the ray tracer, which builds its
    // own GPU buffers from it.
    std::span<const Vertex> GetVertices() const { return m_Vertices; }
    std::span<const unsigned int> GetIndices() const { return m_Indices; }

    // A 1x1 upright rectangle centred on the origin, facing +Z, UVs 0..1.
    static Mesh CreateQuad();

    // A flat size x size square on the XZ plane (a floor), centred on the
    // origin. The texture repeats uvRepeat times across each side.
    static Mesh CreatePlane(float size, float uvRepeat);

    // A 1x1x1 cube centred on the origin. Each face has its own 4 vertices
    // so it gets the full texture (a shared corner can't have 3 UVs).
    static Mesh CreateCube();

private:
    void Release();

    GLuint m_Vao = 0;
    GLuint m_Vbo = 0;
    GLuint m_Ebo = 0;
    GLsizei m_IndexCount = 0;

    std::vector<Vertex> m_Vertices;
    std::vector<unsigned int> m_Indices;
};
