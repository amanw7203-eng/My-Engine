#pragma once

#include <SDL3/SDL.h>

#include <filesystem>

class AssetLibrary;
class Entity;
class Scene;
class Texture;
struct Material;

// The "Assets" panel: every texture (as thumbnails) and material.
//   Textures  - import from the file dialog, reload the textures folder,
//               drag onto material slots, set Color Space, rename, remove.
//               Image files dropped onto the window are imported too.
//   Materials - create, duplicate, delete, edit, and drag onto an entity in
//               the Hierarchy (or the Inspector's material box).
class AssetsPanel
{
public:
    // `textureFolder` is what Refresh re-scans for new images.
    AssetsPanel(SDL_Window* window, std::filesystem::path textureFolder);

    // Call between ImGuiLayer::BeginFrame and EndFrame. `selected` (may be
    // nullptr) is the entity "Assign to Selected" gives a material to.
    void Draw(AssetLibrary& assets, Scene& scene, Entity* selected);

    // Opens the OS file picker; chosen images are imported on a later frame.
    void OpenImportDialog(AssetLibrary& assets);

private:
    void DrawTextures(AssetLibrary& assets);
    void DrawMaterials(AssetLibrary& assets, Scene& scene, Entity* selected);

    SDL_Window* m_Window;
    std::filesystem::path m_TextureFolder;

    const Texture* m_SelectedTexture = nullptr;
    Material* m_SelectedMaterial = nullptr;
};
