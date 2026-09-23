#include "Renderer/Shader.h"

#include <glm/gtc/type_ptr.hpp>

#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

// Reads a whole text file; returns nullopt and prints the path on failure.
static std::optional<std::string> ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file)
    {
        std::cerr << "Failed to open shader file: " << path.string() << '\n';
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Compiles one shader stage; returns 0 and prints the log on failure.
static GLuint CompileShader(GLenum type, const std::string& source,
                            const std::filesystem::path& path)
{
    GLuint shader = glCreateShader(type);
    const char* sourcePtr = source.c_str();
    glShaderSource(shader, 1, &sourcePtr, nullptr);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::cerr << "Shader compile error in " << path.string() << ":\n" << log << '\n';
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

Shader::Shader(const std::filesystem::path& vertexPath, const std::filesystem::path& fragmentPath)
{
    std::optional<std::string> vertexSource = ReadFile(vertexPath);
    std::optional<std::string> fragmentSource = ReadFile(fragmentPath);
    if (!vertexSource || !fragmentSource)
        return;

    GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, *vertexSource, vertexPath);
    GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, *fragmentSource, fragmentPath);
    if (!vertexShader || !fragmentShader)
    {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    // Once linked, the individual shader objects are no longer needed.
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success)
    {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::cerr << "Shader link error (" << vertexPath.string() << ", "
                  << fragmentPath.string() << "):\n" << log << '\n';
        glDeleteProgram(program);
        return;
    }

    m_Program = program;
}

Shader::~Shader()
{
    // glDeleteProgram(0) is a no-op, so invalid/moved-from shaders are fine.
    glDeleteProgram(m_Program);
}

Shader::Shader(Shader&& other) noexcept
    : m_Program(std::exchange(other.m_Program, 0))
{
}

Shader& Shader::operator=(Shader&& other) noexcept
{
    if (this != &other)
    {
        glDeleteProgram(m_Program);
        m_Program = std::exchange(other.m_Program, 0);
    }
    return *this;
}

void Shader::Bind() const
{
    glUseProgram(m_Program);
}

void Shader::SetMat4(const char* name, const glm::mat4& value) const
{
    // GLM stores matrices column-major, same as OpenGL, so no transpose.
    glUniformMatrix4fv(glGetUniformLocation(m_Program, name), 1, GL_FALSE,
                       glm::value_ptr(value));
}
