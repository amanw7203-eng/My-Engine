#pragma once

#include "Scene/Light.h"
#include "Scene/Transform.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

class Mesh;
struct Material;

// One object in the scene. Entities are created, destroyed and re-parented
// through their Scene, which owns them.
class Entity
{
public:
    explicit Entity(std::string name) : name(std::move(name)) {}

    std::string name;
    Transform transform;              // relative to the parent
    const Mesh* mesh = nullptr;       // not owned; nullptr = nothing drawn
    std::optional<PointLight> light;  // set if the entity is (also) a light
    Material* material = nullptr;     // not owned, may be shared; nullptr = default grey
    bool visible = true;              // false also hides all children

    Entity* GetParent() const { return m_Parent; }
    const std::vector<Entity*>& GetChildren() const { return m_Children; }

    // Local transform combined with every parent's, i.e. where the entity
    // actually is in the world.
    glm::mat4 GetWorldMatrix() const;

    // True if `other` is a child, grandchild, ... of this entity.
    bool IsAncestorOf(const Entity& other) const;

    // False if this entity or any parent is hidden, i.e. it isn't drawn.
    bool IsVisibleInHierarchy() const;

private:
    friend class Scene; // keeps parent/child links consistent

    Entity* m_Parent = nullptr;
    std::vector<Entity*> m_Children;
};
