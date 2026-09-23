#include "Assets/AssetLibrary.h"

const Mesh* AssetLibrary::AddMesh(std::string name, Mesh mesh)
{
    auto& entry = m_Meshes.emplace_back(std::move(name), std::make_unique<Mesh>(std::move(mesh)));
    return entry.mesh.get();
}

const Texture* AssetLibrary::AddTexture(std::string name, Texture texture)
{
    auto& entry = m_Textures.emplace_back(std::move(name), std::make_unique<Texture>(std::move(texture)));
    return entry.texture.get();
}

const char* AssetLibrary::GetName(const Mesh* mesh) const
{
    for (const NamedMesh& entry : m_Meshes)
    {
        if (entry.mesh.get() == mesh)
            return entry.name.c_str();
    }
    return "None";
}

const char* AssetLibrary::GetName(const Texture* texture) const
{
    for (const NamedTexture& entry : m_Textures)
    {
        if (entry.texture.get() == texture)
            return entry.name.c_str();
    }
    return "None";
}
