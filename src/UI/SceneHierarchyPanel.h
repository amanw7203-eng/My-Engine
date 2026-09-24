#pragma once

#include <functional>
#include <vector>

class AssetLibrary;
class Entity;
class Mesh;
class Scene;

// Two editor panels sharing one selection:
//   Hierarchy - the entity tree: select, create, delete, drag to re-parent
//   Inspector - edit the selected entity's name, transform, mesh, light, material...
class SceneHierarchyPanel
{
public:
    // Call between ImGuiLayer::BeginFrame and EndFrame. The library isn't
    // const because the Inspector can create materials.
    void Draw(Scene& scene, AssetLibrary& assets);

    Entity* GetSelected() const { return m_Selected; }
    // e.g. from clicking the entity in the 3D view; nullptr deselects.
    void SetSelected(Entity* entity) { m_Selected = entity; }

private:
    void DrawHierarchy(Scene& scene, const AssetLibrary& assets);
    void DrawEntityNode(Scene& scene, const AssetLibrary& assets, Entity& entity);
    void DrawCreateMenuItems(Scene& scene, const AssetLibrary& assets, Entity* parent);
    void DrawInspector(const Scene& scene, AssetLibrary& assets);
    void DrawMaterialSection(const Scene& scene, AssetLibrary& assets, Entity& entity);
    void DrawLightSection(Entity& entity);

    // isLight: the new entity is a point light.
    void QueueCreate(Scene& scene, const char* name, const Mesh* mesh, Entity* parent, bool isLight = false);
    void QueueDelete(Scene& scene, Entity& entity);

    Entity* m_Selected = nullptr;

    // Changes to the tree (create/delete/re-parent) requested while drawing
    // it. They run after the tree is drawn, because changing a child list
    // while looping over it would invalidate the loop.
    std::vector<std::function<void()>> m_Deferred;
};
