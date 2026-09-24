#include "UI/SceneHierarchyPanel.h"

#include "Assets/AssetLibrary.h"
#include "Scene/Scene.h"
#include "UI/AssetWidgets.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <string>

// Drag-and-drop payload type for dragging entities around the tree.
static constexpr const char* kEntityPayload = "ENTITY";

void SceneHierarchyPanel::Draw(Scene& scene, AssetLibrary& assets)
{
    DrawHierarchy(scene, assets);

    // Apply tree changes now that nothing is iterating over the tree.
    for (const std::function<void()>& change : m_Deferred)
        change();
    m_Deferred.clear();

    DrawInspector(scene, assets);
}

void SceneHierarchyPanel::DrawHierarchy(Scene& scene, const AssetLibrary& assets)
{
    const float fontSize = ImGui::GetFontSize();
    ImGui::Begin("Hierarchy");

    if (ImGui::Button("+ Add"))
        ImGui::OpenPopup("AddEntity");
    if (ImGui::BeginPopup("AddEntity"))
    {
        DrawCreateMenuItems(scene, assets, nullptr);
        ImGui::EndPopup();
    }
    ImGui::Separator();

    for (Entity* root : scene.GetRootEntities())
        DrawEntityNode(scene, assets, *root);

    // The empty space below the tree: click to deselect, right-click to
    // create, and drop an entity here to move it back to the top level.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##HierarchyBackground",
                           ImVec2(std::max(avail.x, 1.0f), std::max(avail.y, fontSize * 2)));
    if (ImGui::IsItemClicked())
        m_Selected = nullptr;
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload))
        {
            Entity* dragged = *static_cast<Entity* const*>(payload->Data);
            m_Deferred.push_back([&scene, dragged] { scene.SetParent(*dragged, nullptr); });
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("HierarchyBackgroundMenu"))
    {
        DrawCreateMenuItems(scene, assets, nullptr);
        ImGui::EndPopup();
    }

    // Delete key removes the selection, unless the user is typing somewhere.
    if (m_Selected && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete))
    {
        QueueDelete(scene, *m_Selected);
    }

    ImGui::End();
}

void SceneHierarchyPanel::DrawEntityNode(Scene& scene, const AssetLibrary& assets, Entity& entity)
{
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (entity.GetChildren().empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (&entity == m_Selected)
        flags |= ImGuiTreeNodeFlags_Selected;

    // Hidden entities are greyed out. The entity's address is its ImGui ID,
    // so two entities with the same name don't clash.
    if (!entity.visible)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool open = ImGui::TreeNodeEx(&entity, flags, "%s", entity.name.c_str());
    if (!entity.visible)
        ImGui::PopStyleColor();

    // Clicking the arrow only expands/collapses; clicking the label selects.
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        m_Selected = &entity;

    // Drag this entity...
    if (ImGui::BeginDragDropSource())
    {
        Entity* dragged = &entity;
        ImGui::SetDragDropPayload(kEntityPayload, &dragged, sizeof(dragged));
        ImGui::Text("%s", entity.name.c_str());
        ImGui::EndDragDropSource();
    }
    // ...or drop another one onto it to make it a child. Scene::SetParent
    // refuses drops that would create a loop (onto itself or a descendant).
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload))
        {
            Entity* dragged = *static_cast<Entity* const*>(payload->Data);
            Entity* target = &entity;
            m_Deferred.push_back([&scene, dragged, target] { scene.SetParent(*dragged, target); });
        }
        // A material dragged from the Assets panel is applied to this entity.
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMaterialPayload))
            entity.material = *static_cast<Material* const*>(payload->Data);
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem())
    {
        m_Selected = &entity;
        if (ImGui::BeginMenu("Create Child"))
        {
            DrawCreateMenuItems(scene, assets, &entity);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Delete", "Del"))
            QueueDelete(scene, entity);
        ImGui::EndPopup();
    }

    if (open)
    {
        for (Entity* child : entity.GetChildren())
            DrawEntityNode(scene, assets, *child);
        ImGui::TreePop();
    }
}

void SceneHierarchyPanel::DrawCreateMenuItems(Scene& scene, const AssetLibrary& assets, Entity* parent)
{
    if (ImGui::MenuItem("Empty"))
        QueueCreate(scene, "Empty", nullptr, parent);
    ImGui::Separator();
    for (const AssetLibrary::NamedMesh& entry : assets.GetMeshes())
    {
        if (ImGui::MenuItem(entry.name.c_str()))
            QueueCreate(scene, entry.name.c_str(), entry.mesh.get(), parent);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Point Light"))
        QueueCreate(scene, "Point Light", nullptr, parent, /*isLight*/ true);
}

void SceneHierarchyPanel::DrawInspector(const Scene& scene, AssetLibrary& assets)
{
    ImGui::Begin("Inspector");

    if (!m_Selected)
    {
        ImGui::TextDisabled("Select an object in the Hierarchy.");
        ImGui::End();
        return;
    }

    Entity& entity = *m_Selected;

    ImGui::InputText("Name", &entity.name);
    ImGui::Checkbox("Visible", &entity.visible);

    ImGui::SeparatorText("Transform");
    ImGui::DragFloat3("Position", &entity.transform.position.x, 0.05f);
    ImGui::DragFloat3("Rotation", &entity.transform.rotation.x, 0.5f, 0.0f, 0.0f, "%.1f deg");
    ImGui::DragFloat3("Scale", &entity.transform.scale.x, 0.02f);
    if (ImGui::Button("Reset Transform"))
        entity.transform = Transform{};

    ImGui::SeparatorText("Rendering");
    if (ImGui::BeginCombo("Mesh", assets.GetName(entity.mesh)))
    {
        if (ImGui::Selectable("None", entity.mesh == nullptr))
            entity.mesh = nullptr;
        for (const AssetLibrary::NamedMesh& entry : assets.GetMeshes())
        {
            if (ImGui::Selectable(entry.name.c_str(), entity.mesh == entry.mesh.get()))
                entity.mesh = entry.mesh.get();
        }
        ImGui::EndCombo();
    }

    DrawLightSection(entity);
    // A light on its own has no surface for a material to apply to.
    if (entity.mesh || !entity.light)
        DrawMaterialSection(scene, assets, entity);

    ImGui::End();
}

void SceneHierarchyPanel::DrawLightSection(Entity& entity)
{
    ImGui::SeparatorText("Light");
    ImGui::PushID("light");
    if (!entity.light)
    {
        if (ImGui::Button("Add Point Light"))
            entity.light.emplace();
        ImGui::SetItemTooltip("Make this object give off light, like a lamp");
        ImGui::PopID();
        return;
    }

    PointLight& light = *entity.light;
    ImGui::ColorEdit3("Color", &light.color.x);
    ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 10000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("How bright a white surface facing the light looks from 1 unit away.\n"
                          "It gets dimmer with the square of the distance, like real light.");
    ImGui::DragFloat("Range", &light.range, 0.05f, 0.1f, 1000.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("Beyond this distance the light has no effect (it fades out before\n"
                          "it). Smaller is faster, as fewer surfaces need to consider it.");
    ImGui::Checkbox("Cast Shadows", &light.castShadows);
    ImGui::SetItemTooltip("In the ray traced view. The raster view has shadows for the sun only.");
    if (ImGui::SmallButton("Remove Light"))
        entity.light.reset();
    ImGui::PopID();
}

void SceneHierarchyPanel::DrawMaterialSection(const Scene& scene, AssetLibrary& assets, Entity& entity)
{
    ImGui::SeparatorText("Material");

    // Material picker, plus a New button, like Blender's material slot row.
    const float buttonWidth = ImGui::CalcTextSize("New").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##material", entity.material ? entity.material->name.c_str() : "Default"))
    {
        if (ImGui::Selectable("Default", entity.material == nullptr))
            entity.material = nullptr;
        for (const std::unique_ptr<Material>& material : assets.GetMaterials())
        {
            ImGui::PushID(material.get());
            if (ImGui::Selectable(material->name.c_str(), entity.material == material.get()))
                entity.material = material.get();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    else if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMaterialPayload))
            entity.material = *static_cast<Material* const*>(payload->Data);
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    if (ImGui::Button("New"))
        entity.material = assets.CreateMaterial();
    ImGui::SetItemTooltip("Create a new material for this object");

    if (!entity.material)
    {
        ImGui::TextDisabled("Using the default grey material.");
        return;
    }

    Material& material = *entity.material;
    ImGui::InputText("Name##material", &material.name);

    // Shared materials change every object using them. "Make Single User"
    // gives this object its own copy to edit, as in Blender.
    const int users = scene.CountUsers(&material);
    if (users > 1)
    {
        ImGui::TextDisabled("Shared by %d objects", users);
        ImGui::SameLine();
        if (ImGui::SmallButton("Make Single User"))
            entity.material = assets.DuplicateMaterial(material);
    }

    DrawMaterialEditor(*entity.material, assets);
}

void SceneHierarchyPanel::QueueCreate(Scene& scene, const char* name, const Mesh* mesh, Entity* parent, bool isLight)
{
    m_Deferred.push_back([this, &scene, name = std::string(name), mesh, parent, isLight] {
        Entity& created = scene.CreateEntity(name, parent);
        created.mesh = mesh;
        if (isLight)
        {
            created.light.emplace();
            // Off the floor, where it lights things rather than sitting in them.
            if (!parent)
                created.transform.position = glm::vec3(0.0f, 1.5f, 0.0f);
        }
        m_Selected = &created;
    });
}

void SceneHierarchyPanel::QueueDelete(Scene& scene, Entity& entity)
{
    Entity* target = &entity;
    m_Deferred.push_back([this, &scene, target] {
        // Deleting an entity deletes its children too, so drop the selection
        // if it is the entity or anywhere below it.
        if (m_Selected && (m_Selected == target || target->IsAncestorOf(*m_Selected)))
            m_Selected = nullptr;
        scene.DestroyEntity(*target);
    });
}
