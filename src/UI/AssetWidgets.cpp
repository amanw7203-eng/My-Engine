#include "UI/AssetWidgets.h"

#include "Assets/AssetLibrary.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <functional>

// GL textures start at the bottom row, ImGui draws from the top: flip V.
static constexpr ImVec2 kUvTop(0.0f, 1.0f);
static constexpr ImVec2 kUvBottom(1.0f, 0.0f);

static ImTextureID ToImGui(const Texture& texture)
{
    return static_cast<ImTextureID>(texture.GetId());
}

void TextureThumbnail(const Texture& texture, float size)
{
    ImGui::Image(ToImGui(texture), ImVec2(size, size), kUvTop, kUvBottom);
}

bool TextureSlotPicker(const char* id, const Texture*& slot, const AssetLibrary& assets)
{
    bool changed = false;
    const float thumbSize = ImGui::GetFrameHeight();

    ImGui::PushID(id);
    const bool open = ImGui::BeginCombo("##slot", slot ? assets.GetName(slot) : "No texture",
                                        ImGuiComboFlags_HeightLarge);
    if (!open)
    {
        // Show which image is in the slot, and accept one dragged onto it.
        if (slot && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::BeginTooltip();
            TextureThumbnail(*slot, ImGui::GetFontSize() * 8.0f);
            ImGui::EndTooltip();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kTexturePayload))
            {
                slot = *static_cast<const Texture* const*>(payload->Data);
                changed = true;
            }
            ImGui::EndDragDropTarget();
        }
    }
    else
    {
        if (ImGui::Selectable("No texture", slot == nullptr))
        {
            slot = nullptr;
            changed = true;
        }
        for (const AssetLibrary::NamedTexture& entry : assets.GetTextures())
        {
            const Texture* texture = entry.texture.get();
            ImGui::PushID(texture);
            // An empty selectable row, with the thumbnail and name painted
            // on top of it (they're decoration; the row handles the click).
            if (ImGui::Selectable("##texture", slot == texture, 0, ImVec2(0.0f, thumbSize)))
            {
                slot = texture;
                changed = true;
            }
            const ImVec2 rowMin = ImGui::GetItemRectMin();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddImage(ToImGui(*texture), rowMin, ImVec2(rowMin.x + thumbSize, rowMin.y + thumbSize),
                               kUvTop, kUvBottom);
            drawList->AddText(ImVec2(rowMin.x + thumbSize + ImGui::GetStyle().ItemSpacing.x,
                                     rowMin.y + (thumbSize - ImGui::GetFontSize()) * 0.5f),
                              ImGui::GetColorU32(ImGuiCol_Text), entry.name.c_str());
            ImGui::PopID();
        }
        if (assets.GetTextures().empty())
            ImGui::TextDisabled("Import images in the Assets panel");
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

// One labelled row of the material table: the label on the left, then
// `value` (a slider/color) and optionally a texture slot side by side.
static void MaterialRow(const char* label, const std::function<void()>& value, const Texture** slot,
                        const AssetLibrary& assets)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);

    ImGui::TableSetColumnIndex(1);
    ImGui::PushID(label);
    const float width = ImGui::GetContentRegionAvail().x;
    if (value)
    {
        ImGui::SetNextItemWidth(slot ? width * 0.45f : width);
        value();
    }
    if (slot)
    {
        if (value)
            ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        TextureSlotPicker("map", *slot, assets);
    }
    ImGui::PopID();
}

void DrawMaterialEditor(Material& material, const AssetLibrary& assets)
{
    ImGui::PushID(&material);

    if (!ImGui::BeginTable("##material", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::PopID();
        return;
    }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.5f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

    MaterialRow("Base Color", [&] {
        ImGui::ColorEdit3("##value", &material.baseColor.x, ImGuiColorEditFlags_NoInputs);
    }, &material.baseColorMap, assets);
    MaterialRow("Metallic", [&] {
        ImGui::SliderFloat("##value", &material.metallic, 0.0f, 1.0f, "%.2f");
    }, &material.metallicMap, assets);
    MaterialRow("Roughness", [&] {
        ImGui::SliderFloat("##value", &material.roughness, 0.0f, 1.0f, "%.2f");
    }, &material.roughnessMap, assets);
    MaterialRow("Normal", [&] {
        ImGui::SliderFloat("##value", &material.normalStrength, 0.0f, 3.0f, "%.2f");
        ImGui::SetItemTooltip("Normal map strength");
    }, &material.normalMap, assets);
    MaterialRow("AO", [&] {
        ImGui::SliderFloat("##value", &material.aoStrength, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("Ambient occlusion strength");
    }, &material.aoMap, assets);
    MaterialRow("Emission", [&] {
        ImGui::ColorEdit3("##value", &material.emissionColor.x, ImGuiColorEditFlags_NoInputs);
    }, &material.emissionMap, assets);
    MaterialRow("Emit Strength", [&] {
        ImGui::DragFloat("##value", &material.emissionStrength, 0.05f, 0.0f, 100.0f, "%.2f");
    }, nullptr, assets);

    MaterialRow("Tiling", [&] {
        ImGui::DragFloat2("##value", &material.tiling.x, 0.01f, 0.0f, 0.0f, "%.2f");
    }, nullptr, assets);
    MaterialRow("Offset", [&] {
        ImGui::DragFloat2("##value", &material.offset.x, 0.01f, 0.0f, 0.0f, "%.2f");
    }, nullptr, assets);
    MaterialRow("Double Sided", [&] {
        ImGui::Checkbox("##value", &material.doubleSided);
    }, nullptr, assets);

    ImGui::EndTable();
    ImGui::PopID();
}
