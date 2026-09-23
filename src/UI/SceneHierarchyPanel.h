#pragma once

#include <functional>
#include <vector>

class AssetLibrary;
class Entity;
class Mesh;
class Scene;

// Two editor panels sharing one selection:
//   Hierarchy - the entity tree: select, create, delete, drag to re-parent
//   Inspector - edit the selected entity's name, transform, mesh, texture...
class SceneHierarchyPanel
{
public:
    // Call between ImGuiLayer::BeginFrame and EndFrame.
    void Draw(Scene& scene, const AssetLibrary& assets);

    Entity* GetSelected() const { return m_Selected; }

private:
    void DrawHierarchy(Scene& scene, const AssetLibrary& assets);
    void DrawEntityNode(Scene& scene, const AssetLibrary& assets, Entity& entity);
    void DrawCreateMenuItems(Scene& scene, const AssetLibrary& assets, Entity* parent);
    void DrawInspector(const AssetLibrary& assets);

    void QueueCreate(Scene& scene, const char* name, const Mesh* mesh, Entity* parent);
    void QueueDelete(Scene& scene, Entity& entity);

    Entity* m_Selected = nullptr;

    // Changes to the tree (create/delete/re-parent) requested while drawing
    // it. They run after the tree is drawn, because changing a child list
    // while looping over it would invalidate the loop.
    std::vector<std::function<void()>> m_Deferred;
};
