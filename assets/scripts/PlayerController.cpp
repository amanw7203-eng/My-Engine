#include "Scripting/Script.h"

#include <glm/gtc/constants.hpp>

#include <cmath>

// Walks its entity around with the arrow keys, jumps with Space, and has
// the camera follow from behind. (WASD stays with the editor camera.)
class PlayerController : public Script
{
public:
    float moveSpeed = 3.0f;      // units per second
    float turnSpeed = 120.0f;    // degrees per second
    float jumpSpeed = 5.0f;      // upward speed when jumping
    float gravity = 15.0f;
    glm::vec3 cameraOffset{ 0.0f, 2.0f, 5.0f }; // behind and above, in the player's frame

    void OnStart() override
    {
        m_GroundY = GetTransform().position.y;
        Log("PlayerController on '%s': arrows move, Space jumps.", GetEntity().name.c_str());
    }

    void OnUpdate(float deltaTime) override
    {
        Transform& transform = GetTransform();

        // Turn, then move along the way it faces (-Z is forward at yaw 0).
        if (IsKeyDown(SDL_SCANCODE_LEFT))
            transform.rotation.y += turnSpeed * deltaTime;
        if (IsKeyDown(SDL_SCANCODE_RIGHT))
            transform.rotation.y -= turnSpeed * deltaTime;
        const float yaw = glm::radians(transform.rotation.y);
        const glm::vec3 forward(-std::sin(yaw), 0.0f, -std::cos(yaw));
        float move = 0.0f;
        if (IsKeyDown(SDL_SCANCODE_UP))
            move += 1.0f;
        if (IsKeyDown(SDL_SCANCODE_DOWN))
            move -= 1.0f;
        transform.position += forward * (move * moveSpeed * deltaTime);

        // Jump and fall back to the starting height.
        const bool grounded = transform.position.y <= m_GroundY + 1e-4f;
        if (grounded && WasKeyPressed(SDL_SCANCODE_SPACE))
            m_VerticalSpeed = jumpSpeed;
        m_VerticalSpeed -= gravity * deltaTime;
        transform.position.y += m_VerticalSpeed * deltaTime;
        if (transform.position.y < m_GroundY)
        {
            transform.position.y = m_GroundY;
            m_VerticalSpeed = 0.0f;
        }

        // Camera behind the player, looking at it.
        const glm::vec3 right(std::cos(yaw), 0.0f, -std::sin(yaw));
        const glm::vec3 offset = right * cameraOffset.x + glm::vec3(0.0f, cameraOffset.y, 0.0f) - forward * cameraOffset.z;
        SetCameraPosition(transform.position + offset);
        CameraLookAt(transform.position);
    }

private:
    float m_GroundY = 0.0f;
    float m_VerticalSpeed = 0.0f;
};
REGISTER_SCRIPT(PlayerController)
