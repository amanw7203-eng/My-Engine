#include "Scene/Scene.h"

#include "Assets/Material.h"
#include "Renderer/ColorSpace.h"
#include "Renderer/Mesh.h"
#include "Renderer/Shader.h"
#include "Renderer/Texture.h"

#include <glad/gl.h>

#include <algorithm>

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
    const DrawContext context{ shader, defaultMaterial, whiteTexture, drawnEntities };
    for (Entity* root : m_Roots)
        DrawEntity(*root, glm::mat4(1.0f), context);
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

void Scene::DrawEntity(Entity& entity, const glm::mat4& parentWorld, const DrawContext& context) const
{
    const Shader& shader = context.shader;
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
        const Material& material = entity.material ? *entity.material : context.defaultMaterial;
        BindMaterial(shader, material, context.whiteTexture);
        if (context.drawnEntities)
        {
            context.drawnEntities->push_back(&entity);
            shader.SetUInt("uEntityId", static_cast<unsigned int>(context.drawnEntities->size()));
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
    }

    for (Entity* child : entity.m_Children)
        DrawEntity(*child, world, context);
}

void Scene::Detach(Entity& entity)
{
    std::erase(entity.m_Parent ? entity.m_Parent->m_Children : m_Roots, &entity);
    entity.m_Parent = nullptr;
}
