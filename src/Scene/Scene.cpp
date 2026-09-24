#include "Scene/Scene.h"

#include "Renderer/Mesh.h"
#include "Renderer/Shader.h"
#include "Renderer/Texture.h"

#include <glad/gl.h>

Entity& Scene::CreateEntity(std::string name, Entity* parent)
{
    Entity& entity = *m_Entities.emplace_back(std::make_unique<Entity>(std::move(name)));
    entity.m_Parent = parent;
    (parent ? parent->m_Children : m_Roots).push_back(&entity);
    return entity;
}

void Scene::DestroyEntity(Entity& entity)
{
    // Each child removes itself from our child list as it is destroyed.
    while (!entity.m_Children.empty())
        DestroyEntity(*entity.m_Children.back());

    Detach(entity);
    std::erase_if(m_Entities, [&](const std::unique_ptr<Entity>& e) { return e.get() == &entity; });
}

bool Scene::SetParent(Entity& entity, Entity* newParent)
{
    if (newParent == &entity || (newParent && entity.IsAncestorOf(*newParent)))
        return false;
    if (entity.m_Parent == newParent)
        return true;

    Detach(entity);
    entity.m_Parent = newParent;
    (newParent ? newParent->m_Children : m_Roots).push_back(&entity);
    return true;
}

void Scene::Draw(const Shader& shader, const Texture& defaultTexture,
                 std::vector<Entity*>* drawnEntities) const
{
    for (Entity* root : m_Roots)
        DrawEntity(*root, glm::mat4(1.0f), shader, defaultTexture, drawnEntities);
}

void Scene::DrawEntity(Entity& entity, const glm::mat4& parentWorld, const Shader& shader,
                       const Texture& defaultTexture, std::vector<Entity*>* drawnEntities) const
{
    if (!entity.visible)
        return; // hides the whole subtree

    // Walking down the tree, each entity's world matrix is its parent's
    // world matrix times its own local one.
    const glm::mat4 world = parentWorld * entity.transform.GetMatrix();

    if (entity.mesh)
    {
        shader.SetMat4("uModel", world);
        // Inverse-transpose keeps normals perpendicular to the surface when
        // the entity (or a parent) is scaled unevenly.
        shader.SetMat3("uNormalMatrix", glm::transpose(glm::inverse(glm::mat3(world))));
        shader.SetVec3("uTint", entity.tint);
        shader.SetFloat("uSpecularStrength", entity.specularStrength);
        shader.SetFloat("uShininess", entity.shininess);
        (entity.texture ? *entity.texture : defaultTexture).Bind(0);
        if (drawnEntities)
        {
            drawnEntities->push_back(&entity);
            shader.SetUInt("uEntityId", static_cast<unsigned int>(drawnEntities->size()));
        }

        // A negative scale mirrors the mesh, which flips its triangles'
        // winding, so the side that counts as "front" must flip with it or
        // culling would hide the outside and show the inside.
        const bool mirrored = glm::determinant(glm::mat3(world)) < 0.0f;
        if (mirrored)
            glFrontFace(GL_CW);
        // Double-sided entities skip culling so their back is visible too.
        const bool pauseCulling = entity.doubleSided && glIsEnabled(GL_CULL_FACE);
        if (pauseCulling)
            glDisable(GL_CULL_FACE);

        entity.mesh->Draw();

        if (pauseCulling)
            glEnable(GL_CULL_FACE);
        if (mirrored)
            glFrontFace(GL_CCW);
    }

    for (Entity* child : entity.m_Children)
        DrawEntity(*child, world, shader, defaultTexture, drawnEntities);
}

void Scene::Detach(Entity& entity)
{
    std::erase(entity.m_Parent ? entity.m_Parent->m_Children : m_Roots, &entity);
    entity.m_Parent = nullptr;
}
