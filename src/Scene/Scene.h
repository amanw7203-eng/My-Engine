#pragma once

#include "Scene/Entity.h"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

class Shader;
class Texture;

// Owns every entity and the parent/child tree between them.
class Scene
{
public:
    // Adds an entity as a child of `parent`, or at the top level if nullptr.
    Entity& CreateEntity(std::string name, Entity* parent = nullptr);

    // Destroys the entity and all of its children. Any pointers to them
    // become invalid.
    void DestroyEntity(Entity& entity);

    // Moves `entity` under `newParent` (nullptr = top level), keeping its
    // local transform. Refused (returns false) if it would create a loop,
    // i.e. parenting an entity to itself or to one of its own children.
    bool SetParent(Entity& entity, Entity* newParent);

    const std::vector<Entity*>& GetRootEntities() const { return m_Roots; }

    // Draws every visible entity that has a mesh. The shader must already be
    // bound with its view/projection set. Entities without a texture use
    // `defaultTexture`.
    void Draw(const Shader& shader, const Texture& defaultTexture) const;

private:
    void DrawEntity(const Entity& entity, const glm::mat4& parentWorld,
                    const Shader& shader, const Texture& defaultTexture) const;

    // Removes the entity from its parent's (or the root) child list.
    void Detach(Entity& entity);

    // unique_ptr keeps each Entity at a stable address, so the raw pointers
    // in the tree stay valid as entities are added and removed.
    std::vector<std::unique_ptr<Entity>> m_Entities;
    std::vector<Entity*> m_Roots;
};
