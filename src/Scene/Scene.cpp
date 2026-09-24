#include "Scene/Scene.h"

#include "Assets/Material.h"
#include "Renderer/ColorSpace.h"
#include "Renderer/Mesh.h"
#include "Renderer/Shader.h"
#include "Renderer/Texture.h"

#include <glad/gl.h>

#include <algorithm>
#include <functional>

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
    // Kept rather than freed, so undo can bring back this very entity.
    const auto it = std::find_if(m_Entities.begin(), m_Entities.end(),
                                 [&](const std::unique_ptr<Entity>& e) { return e.get() == &entity; });
    if (it != m_Entities.end())
    {
        m_Destroyed.push_back(std::move(*it));
        m_Entities.erase(it);
    }
}

bool Scene::Contains(const Entity* entity) const
{
    return std::any_of(m_Entities.begin(), m_Entities.end(),
                       [&](const std::unique_ptr<Entity>& e) { return e.get() == entity; });
}

Scene::State Scene::CaptureState() const
{
    State state;
    state.reserve(m_Entities.size());
    // Depth first, children in order: restoring in this order appends each
    // entity to its parent's child list in the original order.
    std::function<void(Entity&)> visit = [&](Entity& entity) {
        EntityState& s = state.emplace_back();
        s.entity = &entity;
        s.parent = entity.m_Parent;
        s.name = entity.name;
        s.transform = entity.transform;
        s.mesh = entity.mesh;
        s.material = entity.material;
        s.light = entity.light;
        s.visible = entity.visible;
        for (Entity* child : entity.m_Children)
            visit(*child);
    };
    for (Entity* root : m_Roots)
        visit(*root);
    return state;
}

void Scene::RestoreState(const State& state)
{
    // Every entity object we have, alive or destroyed, gathered in one pool.
    std::vector<std::unique_ptr<Entity>> pool;
    pool.reserve(m_Entities.size() + m_Destroyed.size());
    for (auto* list : { &m_Entities, &m_Destroyed })
    {
        for (std::unique_ptr<Entity>& entity : *list)
            pool.push_back(std::move(entity));
        list->clear();
    }
    for (std::unique_ptr<Entity>& entity : pool)
    {
        entity->m_Parent = nullptr;
        entity->m_Children.clear();
    }
    m_Roots.clear();

    // The state's entities come back to life, rebuilt into the tree...
    for (const EntityState& s : state)
    {
        const auto it = std::find_if(pool.begin(), pool.end(),
                                     [&](const std::unique_ptr<Entity>& e) { return e.get() == s.entity; });
        if (it == pool.end())
            continue; // can't happen: entities are never freed
        Entity& entity = **it;
        entity.name = s.name;
        entity.transform = s.transform;
        entity.mesh = s.mesh;
        entity.material = s.material;
        entity.light = s.light;
        entity.visible = s.visible;
        entity.m_Parent = s.parent;
        (s.parent ? s.parent->m_Children : m_Roots).push_back(&entity);
        m_Entities.push_back(std::move(*it));
    }

    // ...and the rest are destroyed (kept for a later redo).
    for (std::unique_ptr<Entity>& entity : pool)
        if (entity)
            m_Destroyed.push_back(std::move(entity));
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

// Texture unit for each material map. Unit 1 is left for the shadow map.
namespace TextureUnit
{
    constexpr unsigned int BaseColor = 0;
    constexpr unsigned int Metallic = 2;
    constexpr unsigned int Roughness = 3;
    constexpr unsigned int Normal = 4;
    constexpr unsigned int Ao = 5;
    constexpr unsigned int Emission = 6;
}

void Scene::SetMaterialSamplers(const Shader& shader)
{
    shader.SetInt("uBaseColorMap", TextureUnit::BaseColor);
    shader.SetInt("uMetallicMap", TextureUnit::Metallic);
    shader.SetInt("uRoughnessMap", TextureUnit::Roughness);
    shader.SetInt("uNormalMap", TextureUnit::Normal);
    shader.SetInt("uAoMap", TextureUnit::Ao);
    shader.SetInt("uEmissionMap", TextureUnit::Emission);
}

// Binds one map (or white if the slot is empty, which leaves the value
// unchanged when multiplied) and tells the shader whether to decode it
// from sRGB.
static void BindMap(const Shader& shader, const Texture* map, const Texture& white,
                    unsigned int unit, const char* srgbUniform)
{
    const Texture& texture = map ? *map : white;
    texture.Bind(unit);
    shader.SetInt(srgbUniform, map && map->colorSpace == Texture::ColorSpace::Srgb);
}

static void BindMaterial(const Shader& shader, const Material& material, const Texture& white)
{
    // The color pickers show sRGB; lighting works in linear light.
    shader.SetVec3("uBaseColor", SrgbToLinear(material.baseColor));
    shader.SetFloat("uMetallic", material.metallic);
    shader.SetFloat("uRoughness", material.roughness);
    shader.SetInt("uHasNormalMap", material.normalMap != nullptr);
    shader.SetFloat("uNormalStrength", material.normalStrength);
    shader.SetFloat("uAoStrength", material.aoStrength);
    shader.SetVec3("uEmission", SrgbToLinear(material.emissionColor) * material.emissionStrength);
    shader.SetVec2("uUvTiling", material.tiling);
    shader.SetVec2("uUvOffset", material.offset);

    BindMap(shader, material.baseColorMap, white, TextureUnit::BaseColor, "uBaseColorMapSrgb");
    BindMap(shader, material.metallicMap, white, TextureUnit::Metallic, "uMetallicMapSrgb");
    BindMap(shader, material.roughnessMap, white, TextureUnit::Roughness, "uRoughnessMapSrgb");
    BindMap(shader, material.normalMap, white, TextureUnit::Normal, "uNormalMapSrgb");
    BindMap(shader, material.aoMap, white, TextureUnit::Ao, "uAoMapSrgb");
    BindMap(shader, material.emissionMap, white, TextureUnit::Emission, "uEmissionMapSrgb");
}

void Scene::Draw(const Shader& shader, const Material& defaultMaterial, const Texture& whiteTexture,
                 std::vector<Entity*>* drawnEntities) const
{
    ForEachDrawable([&](Entity& entity, const glm::mat4& world)
    {
        shader.SetMat4("uModel", world);
        // Inverse-transpose keeps normals perpendicular to the surface when
        // the entity (or a parent) is scaled unevenly.
        shader.SetMat3("uNormalMatrix", glm::transpose(glm::inverse(glm::mat3(world))));
        const Material& material = entity.material ? *entity.material : defaultMaterial;
        BindMaterial(shader, material, whiteTexture);
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
        const bool pauseCulling = material.doubleSided && glIsEnabled(GL_CULL_FACE);
        if (pauseCulling)
            glDisable(GL_CULL_FACE);

        entity.mesh->Draw();

        if (pauseCulling)
            glEnable(GL_CULL_FACE);
        if (mirrored)
            glFrontFace(GL_CCW);
    });
}

void Scene::ForEachVisible(const EntityCallback& fn) const
{
    for (Entity* root : m_Roots)
        VisitVisible(*root, glm::mat4(1.0f), fn);
}

void Scene::ForEachDrawable(const DrawableCallback& fn) const
{
    ForEachVisible([&](Entity& entity, const glm::mat4& world) {
        if (entity.mesh)
            fn(entity, world);
    });
}

std::vector<ScenePointLight> Scene::GatherPointLights() const
{
    std::vector<ScenePointLight> lights;
    ForEachVisible([&](Entity& entity, const glm::mat4& world) {
        if (!entity.light)
            return;
        const PointLight& light = *entity.light;
        ScenePointLight& placed = lights.emplace_back();
        placed.position = glm::vec3(world[3]);
        // The picker shows sRGB; lighting works in linear light.
        placed.radiance = SrgbToLinear(light.color) * std::max(light.intensity, 0.0f);
        placed.range = std::max(light.range, 0.01f);
        placed.castShadows = light.castShadows;
    });
    return lights;
}

void Scene::ClearMaterial(const Material* material)
{
    for (const std::unique_ptr<Entity>& entity : m_Entities)
    {
        if (entity->material == material)
            entity->material = nullptr;
    }
}

int Scene::CountUsers(const Material* material) const
{
    return static_cast<int>(std::count_if(m_Entities.begin(), m_Entities.end(),
                                          [&](const std::unique_ptr<Entity>& e) { return e->material == material; }));
}

void Scene::VisitVisible(Entity& entity, const glm::mat4& parentWorld, const EntityCallback& fn)
{
    if (!entity.visible)
        return; // hides the whole subtree

    // Walking down the tree, each entity's world matrix is its parent's
    // world matrix times its own local one.
    const glm::mat4 world = parentWorld * entity.transform.GetMatrix();
    fn(entity, world);

    for (Entity* child : entity.m_Children)
        VisitVisible(*child, world, fn);
}

void Scene::Detach(Entity& entity)
{
    std::erase(entity.m_Parent ? entity.m_Parent->m_Children : m_Roots, &entity);
    entity.m_Parent = nullptr;
}
