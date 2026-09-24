#pragma once

#include "Assets/Material.h"
#include "Renderer/Mesh.h"
#include "Renderer/Texture.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Owns the meshes, textures and materials entities can use, each under a
// display name so the editor can list them. Like the GL objects it holds, it
// must be destroyed before the OpenGL context.
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

    // Returned pointers stay valid until the asset is removed (or, for
    // meshes, for the library's lifetime).
    const Mesh* AddMesh(std::string name, Mesh mesh);
    Texture* AddTexture(std::string name, Texture texture);

    // Loads an image file as a texture named after the file. If that file is
    // already loaded, returns the existing texture. Returns nullptr if it
    // can't be read. Normal/roughness/metallic/AO maps (guessed from the file
    // name) start out as Non-Color data.
    Texture* ImportTexture(const std::filesystem::path& path);

    // Imports every image in `folder` that isn't loaded yet.
    void ImportFolder(const std::filesystem::path& folder);

    // Imports can be requested from any thread (the OS file dialog answers
    // on its own thread), but textures must be created on the GL thread:
    // QueueImport stores the path, ProcessQueuedImports (called once per
    // frame on the main thread) loads them.
    void QueueImport(std::filesystem::path path);
    void ProcessQueuedImports();

    // Removes the texture and clears it from every material slot using it.
    void RemoveTexture(const Texture* texture);

    // A new material with default settings, named uniquely from `name`
    // (Blender style: "Material", "Material.001", ...).
    Material* CreateMaterial(const std::string& name = "Material");
    Material* DuplicateMaterial(const Material& source);
    // Entities still pointing at it must be cleared first (Scene::ClearMaterial).
    void RemoveMaterial(const Material* material);

    const std::vector<NamedMesh>& GetMeshes() const { return m_Meshes; }
    const std::vector<NamedTexture>& GetTextures() const { return m_Textures; }
    const std::vector<std::unique_ptr<Material>>& GetMaterials() const { return m_Materials; }

    // Display name for an asset, or "None" for nullptr / unknown.
    const char* GetName(const Mesh* mesh) const;
    const char* GetName(const Texture* texture) const;
    const char* GetName(const Material* material) const;

    // Sets a texture's display name, made unique among the textures.
    void RenameTexture(const Texture* texture, const std::string& name);

    // `base` if no existing name matches, else "base.001", "base.002", ...
    std::string MakeUniqueTextureName(const std::string& base) const;
    std::string MakeUniqueMaterialName(const std::string& base) const;

private:
    std::vector<NamedMesh> m_Meshes;
    std::vector<NamedTexture> m_Textures;
    std::vector<std::unique_ptr<Material>> m_Materials;

    std::mutex m_QueueMutex;
    std::vector<std::filesystem::path> m_ImportQueue;
};
