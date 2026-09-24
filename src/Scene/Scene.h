#pragma once

#include "Scene/Entity.h"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

class Shader;
class Texture;
struct Material;

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

    // Draws every visible entity that has a mesh, binding its material's
    // values and textures. The shader must already be bound with its
    // view/projection set, and SetMaterialSamplers called on it. Entities
    // without a material use `defaultMaterial`; empty texture slots use
    // `whiteTexture`.
    //
    // For picking, pass `drawnEntities`: each drawn entity is appended to it,
    // and its 1-based position in the list is sent as the uEntityId uniform,
    // so an ID read back from the picker is drawnEntities[id - 1].
    void Draw(const Shader& shader, const Material& defaultMaterial, const Texture& whiteTexture,
              std::vector<Entity*>* drawnEntities = nullptr) const;

    // Points a material shader's texture samplers at the texture units Draw
    // binds each map to. The shader must be bound.
    static void SetMaterialSamplers(const Shader& shader);

    // Entities using `material` fall back to the default (before deleting it).
    void ClearMaterial(const Material* material);

    // How many entities use `material` (Blender's "users" count).
    int CountUsers(const Material* material) const;

private:
    struct DrawContext
    {
        const Shader& shader;
        const Material& defaultMaterial;
        const Texture& whiteTexture;
        std::vector<Entity*>* drawnEntities;
    };

    void DrawEntity(Entity& entity, const glm::mat4& parentWorld, const DrawContext& context) const;

    // Removes the entity from its parent's (or the root) child list.
    void Detach(Entity& entity);

    // unique_ptr keeps each Entity at a stable address, so the raw pointers
    // in the tree stay valid as entities are added and removed.
    std::vector<std::unique_ptr<Entity>> m_Entities;
    std::vector<Entity*> m_Roots;
};
