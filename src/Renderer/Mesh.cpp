#include "Renderer/Mesh.h"

#include <cstddef>
#include <utility>
#include <vector>

Mesh::Mesh(std::span<const Vertex> vertices, std::span<const unsigned int> indices)
    : m_IndexCount(static_cast<GLsizei>(indices.size()))
    , m_Vertices(vertices.begin(), vertices.end())
    , m_Indices(indices.begin(), indices.end())
{
    glGenVertexArrays(1, &m_Vao);
    glGenBuffers(1, &m_Vbo);
    glGenBuffers(1, &m_Ebo);

    glBindVertexArray(m_Vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_Vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size_bytes(), vertices.data(), GL_STATIC_DRAW);

    // Binding the EBO while the VAO is bound stores it in the VAO.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_Ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size_bytes(), indices.data(), GL_STATIC_DRAW);

    // Describe the Vertex struct to OpenGL: which floats go to which location.
    const GLsizei stride = sizeof(Vertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(Vertex, position)));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(Vertex, color)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(Vertex, uv)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(Vertex, normal)));
    glEnableVertexAttribArray(3);

    // Unbind the VAO first: unbinding the EBO while it is bound would
    // remove the EBO from it.
    glBindVertexArray(0);
}

Mesh::~Mesh()
{
    Release();
}

Mesh::Mesh(Mesh&& other) noexcept
    : m_Vao(std::exchange(other.m_Vao, 0))
    , m_Vbo(std::exchange(other.m_Vbo, 0))
    , m_Ebo(std::exchange(other.m_Ebo, 0))
    , m_IndexCount(std::exchange(other.m_IndexCount, 0))
    , m_Vertices(std::move(other.m_Vertices))
    , m_Indices(std::move(other.m_Indices))
{
}

Mesh& Mesh::operator=(Mesh&& other) noexcept
{
    if (this != &other)
    {
        Release();
        m_Vao = std::exchange(other.m_Vao, 0);
        m_Vbo = std::exchange(other.m_Vbo, 0);
        m_Ebo = std::exchange(other.m_Ebo, 0);
        m_IndexCount = std::exchange(other.m_IndexCount, 0);
        m_Vertices = std::move(other.m_Vertices);
        m_Indices = std::move(other.m_Indices);
    }
    return *this;
}

void Mesh::Draw() const
{
    glBindVertexArray(m_Vao);
    glDrawElements(GL_TRIANGLES, m_IndexCount, GL_UNSIGNED_INT, nullptr);
}

Mesh Mesh::CreateQuad()
{
    const glm::vec3 white(1.0f);
    const glm::vec3 n(0.0f, 0.0f, 1.0f); // faces +Z
    const Vertex vertices[] = {
        { { -0.5f, -0.5f, 0.0f }, white, { 0.0f, 0.0f }, n }, // 0: bottom left
        { {  0.5f, -0.5f, 0.0f }, white, { 1.0f, 0.0f }, n }, // 1: bottom right
        { {  0.5f,  0.5f, 0.0f }, white, { 1.0f, 1.0f }, n }, // 2: top right
        { { -0.5f,  0.5f, 0.0f }, white, { 0.0f, 1.0f }, n }, // 3: top left
    };

    // Two triangles built from those corners, reusing vertices 0 and 2.
    const unsigned int indices[] = {
        0, 1, 2,
        2, 3, 0,
    };

    return Mesh(vertices, indices);
}

Mesh Mesh::CreatePlane(float size, float uvRepeat)
{
    const float h = size * 0.5f;
    const float r = uvRepeat;
    const glm::vec3 white(1.0f);
    const glm::vec3 n(0.0f, 1.0f, 0.0f); // faces up
    // Wound counter-clockwise when seen from above (+Y), same as CreateQuad
    // seen from the front, so both face the same way if culling is enabled.
    const Vertex vertices[] = {
        { { -h, 0.0f,  h }, white, { 0.0f, 0.0f }, n }, // 0: near left
        { {  h, 0.0f,  h }, white, { r,    0.0f }, n }, // 1: near right
        { {  h, 0.0f, -h }, white, { r,    r    }, n }, // 2: far right
        { { -h, 0.0f, -h }, white, { 0.0f, r    }, n }, // 3: far left
    };

    const unsigned int indices[] = {
        0, 1, 2,
        2, 3, 0,
    };

    return Mesh(vertices, indices);
}

Mesh Mesh::CreateCube()
{
    // Corners of each face as seen from outside the cube, in the order
    // bottom-left, bottom-right, top-right, top-left (counter-clockwise).
    constexpr float h = 0.5f;
    const glm::vec3 faces[6][4] = {
        { { -h, -h,  h }, {  h, -h,  h }, {  h,  h,  h }, { -h,  h,  h } }, // +Z front
        { {  h, -h, -h }, { -h, -h, -h }, { -h,  h, -h }, {  h,  h, -h } }, // -Z back
        { {  h, -h,  h }, {  h, -h, -h }, {  h,  h, -h }, {  h,  h,  h } }, // +X right
        { { -h, -h, -h }, { -h, -h,  h }, { -h,  h,  h }, { -h,  h, -h } }, // -X left
        { { -h,  h,  h }, {  h,  h,  h }, {  h,  h, -h }, { -h,  h, -h } }, // +Y top
        { { -h, -h, -h }, {  h, -h, -h }, {  h, -h,  h }, { -h, -h,  h } }, // -Y bottom
    };
    // Same order as `faces`. Every vertex of a face shares its normal, which
    // keeps the cube's edges sharp instead of smoothly shaded.
    const glm::vec3 normals[6] = {
        {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f, -1.0f },
        {  1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f,  0.0f },
        {  0.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f },
    };
    const glm::vec2 uvs[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    for (int face = 0; face < 6; ++face)
    {
        const unsigned int base = static_cast<unsigned int>(vertices.size());
        for (int corner = 0; corner < 4; ++corner)
            vertices.push_back({ faces[face][corner], glm::vec3(1.0f), uvs[corner], normals[face] });

        for (unsigned int i : { 0u, 1u, 2u, 2u, 3u, 0u })
            indices.push_back(base + i);
    }

    return Mesh(vertices, indices);
}

void Mesh::Release()
{
    // Deleting name 0 is a no-op, so moved-from meshes are fine.
    glDeleteVertexArrays(1, &m_Vao);
    glDeleteBuffers(1, &m_Vbo);
    glDeleteBuffers(1, &m_Ebo);
    m_Vao = m_Vbo = m_Ebo = 0;
    m_IndexCount = 0;
}
