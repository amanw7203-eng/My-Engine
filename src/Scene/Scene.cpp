#include "Scene/Scene.h"

#include "Renderer/Mesh.h"
#include "Renderer/Shader.h"
#include "Renderer/Texture.h"

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

void Scene::Draw(const Shader& shader, const Texture& defaultTexture) const
{
    for (const Entity* root : m_Roots)
        DrawEntity(*root, glm::mat4(1.0f), shader, defaultTexture);
}

void Scene::DrawEntity(const Entity& entity, const glm::mat4& parentWorld,
                       const Shader& shader, const Texture& defaultTexture) const
{
    if (!entity.visible)
        return; // hides the whole subtree

    // Walking down the tree, each entity's world matrix is its parent's
    // world matrix times its own local one.
    const glm::mat4 world = parentWorld * entity.transform.GetMatrix();

    if (entity.mesh)
    {
        shader.SetMat4("uModel", world);
        shader.SetVec3("uTint", entity.tint);
        (entity.texture ? *entity.texture : defaultTexture).Bind(0);
        entity.mesh->Draw();
    }

    for (const Entity* child : entity.m_Children)
        DrawEntity(*child, world, shader, defaultTexture);
}

void Scene::Detach(Entity& entity)
{
    std::erase(entity.m_Parent ? entity.m_Parent->m_Children : m_Roots, &entity);
    entity.m_Parent = nullptr;
}
