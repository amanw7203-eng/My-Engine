#include "Renderer/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

static constexpr glm::vec3 kWorldUp(0.0f, 1.0f, 0.0f);

Camera::Camera(const glm::vec3& position)
    : m_Position(position)
{
}

void Camera::Move(const glm::vec3& direction, float deltaTime)
{
    const glm::vec3 forward = GetForward();
    const glm::vec3 right = glm::normalize(glm::cross(forward, kWorldUp));

    // Forward follows where the camera looks (including up/down), while
    // Q/E always move straight along the world's up axis.
    glm::vec3 velocity = forward * direction.z + right * direction.x + kWorldUp * direction.y;

    // Inputs can cancel out (e.g. looking straight down while holding W + E);
    // normalising a zero vector would produce NaN.
    const float length = glm::length(velocity);
    if (length < 0.0001f)
        return;

    // Normalise so diagonal movement is not faster than straight movement.
    velocity /= length;

    const float speed = moveSpeed * (sprinting ? sprintMultiplier : 1.0f);
    m_Position += velocity * speed * deltaTime;
}

void Camera::Rotate(float dx, float dy)
{
    m_Yaw += dx * mouseSensitivity;
    m_Pitch -= dy * mouseSensitivity; // screen Y grows downward

    // Looking straight up or down would make forward parallel to world up,
    // which breaks the right-vector cross product.
    m_Pitch = std::clamp(m_Pitch, -89.0f, 89.0f);
}

void Camera::LookAt(const glm::vec3& target)
{
    const glm::vec3 offset = target - m_Position;
    const float length = glm::length(offset);
    if (length < 0.0001f)
        return; // already there: any direction would do, so keep the current one
    // The inverse of GetForward: pitch from the height, yaw around it.
    const glm::vec3 direction = offset / length;
    m_Pitch = std::clamp(glm::degrees(std::asin(std::clamp(direction.y, -1.0f, 1.0f))), -89.0f, 89.0f);
    m_Yaw = glm::degrees(std::atan2(direction.z, direction.x));
}

glm::mat4 Camera::GetViewMatrix() const
{
    return glm::lookAt(m_Position, m_Position + GetForward(), kWorldUp);
}

glm::mat4 Camera::GetProjectionMatrix(float aspect) const
{
    return glm::perspective(glm::radians(fieldOfView), aspect, 0.1f, 100.0f);
}

glm::vec3 Camera::GetForward() const
{
    // Convert yaw/pitch angles into a unit direction vector.
    const float yaw = glm::radians(m_Yaw);
    const float pitch = glm::radians(m_Pitch);
    return glm::normalize(glm::vec3(std::cos(yaw) * std::cos(pitch),
                                    std::sin(pitch),
                                    std::sin(yaw) * std::cos(pitch)));
}
