#pragma once

#include "Scene/Transform.h"

#include <glm/glm.hpp>

#include <string>
#include <utility>
#include <vector>

class Mesh;
class Texture;

// One object in the scene. Entities are created, destroyed and re-parented
// through their Scene, which owns them.
class Entity
{
public:
    explicit Entity(std::string name) : name(std::move(name)) {}

    std::string name;
    Transform transform;              // relative to the parent
    const Mesh* mesh = nullptr;       // not owned; nullptr = nothing drawn
    const Texture* texture = nullptr; // not owned; nullptr = plain white
    glm::vec3 tint{ 1.0f };           // multiplied with the texture
    float specularStrength = 0.5f;    // 0 = matte, 1 = full-strength highlight
    float shininess = 32.0f;          // higher = smaller, sharper highlight
    bool visible = true;              // false also hides all children

    Entity* GetParent() const { return m_Parent; }
    const std::vector<Entity*>& GetChildren() const { return m_Children; }

    // Local transform combined with every parent's, i.e. where the entity
    // actually is in the world.
    glm::mat4 GetWorldMatrix() const;

    // True if `other` is a child, grandchild, ... of this entity.
    bool IsAncestorOf(const Entity& other) const;

private:
    friend class Scene; // keeps parent/child links consistent

    Entity* m_Parent = nullptr;
    std::vector<Entity*> m_Children;
};
