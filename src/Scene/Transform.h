#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

// Position, rotation and scale of an object relative to its parent (or the
// world, if it has no parent).
struct Transform
{
    glm::vec3 position{ 0.0f };
    glm::vec3 rotation{ 0.0f }; // Euler angles in degrees around X, Y, Z
    glm::vec3 scale{ 1.0f };

    // Scale first, then rotate, then move (matrices apply right to left).
    glm::mat4 GetMatrix() const
    {
        return glm::translate(glm::mat4(1.0f), position)
             * glm::mat4_cast(glm::quat(glm::radians(rotation)))
             * glm::scale(glm::mat4(1.0f), scale);
    }
};
