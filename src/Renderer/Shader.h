#pragma once

#include <glad/gl.h>
#include <glm/mat4x4.hpp>

#include <filesystem>

// Owns an OpenGL shader program built from a vertex + fragment shader file.
// The program is deleted in the destructor, so a Shader must be destroyed
// before the OpenGL context is.
class Shader
{
public:
    Shader(const std::filesystem::path& vertexPath, const std::filesystem::path& fragmentPath);
    ~Shader();

    // One Shader owns one GL program: copying would double-delete it.
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    // False if a file failed to load or a stage failed to compile/link.
    bool IsValid() const { return m_Program != 0; }

    void Bind() const;

    // Uniform setters. The shader must be bound first.
    void SetInt(const char* name, int value) const;
    void SetMat4(const char* name, const glm::mat4& value) const;

private:
    GLuint m_Program = 0;
};
