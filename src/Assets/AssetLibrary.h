#pragma once

#include "Renderer/Mesh.h"
#include "Renderer/Texture.h"

#include <memory>
#include <string>
#include <vector>

// Owns the meshes and textures entities can use, each under a display name
// so the editor can list them. Like the GL objects it holds, it must be
// destroyed before the OpenGL context.
class AssetLibrary
{
public:
    struct NamedMesh
    {
        std::string name;
        std::unique_ptr<Mesh> mesh;
    };

    struct NamedTexture
    {
        std::string name;
        std::unique_ptr<Texture> texture;
    };

    // Returned pointers stay valid for the library's lifetime.
    const Mesh* AddMesh(std::string name, Mesh mesh);
    const Texture* AddTexture(std::string name, Texture texture);

    const std::vector<NamedMesh>& GetMeshes() const { return m_Meshes; }
    const std::vector<NamedTexture>& GetTextures() const { return m_Textures; }

    // Display name for an asset, or "None" for nullptr / unknown.
    const char* GetName(const Mesh* mesh) const;
    const char* GetName(const Texture* texture) const;

private:
    std::vector<NamedMesh> m_Meshes;
    std::vector<NamedTexture> m_Textures;
};
