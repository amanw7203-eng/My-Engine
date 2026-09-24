#pragma once

#include <glm/glm.hpp>

// A free-flying first-person camera. It knows nothing about input: callers
// tell it which way to move and how far the mouse moved.
class Camera
{
public:
    explicit Camera(const glm::vec3& position = glm::vec3(0.0f, 0.0f, 3.0f));

    // direction is in camera-relative terms: x = right, y = world up,
    // z = forward. Values are usually -1, 0 or 1 per axis.
    void Move(const glm::vec3& direction, float deltaTime);

    // Mouse movement in pixels. Positive dx looks right, positive dy looks down.
    void Rotate(float dx, float dy);

    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix(float aspect) const;

    const glm::vec3& GetPosition() const { return m_Position; }
    glm::vec3 GetForward() const; // unit vector the camera looks along

    void SetPosition(const glm::vec3& position) { m_Position = position; }
    // Turns the camera to face `target` (no roll, like the mouse look).
    void LookAt(const glm::vec3& target);

    float moveSpeed = 3.0f;          // units per second
    float sprintMultiplier = 3.0f;   // applied while sprinting
    float mouseSensitivity = 0.1f;   // degrees per pixel
    float fieldOfView = 45.0f;       // vertical, in degrees
    bool sprinting = false;

private:
    glm::vec3 m_Position;
    float m_Yaw = -90.0f;  // degrees; -90 looks down -Z
    float m_Pitch = 0.0f;  // degrees; clamped so the view never flips
};
