#include "UI/AssetsPanel.h"

#include "Assets/AssetLibrary.h"
#include "Scene/Scene.h"
#include "UI/AssetWidgets.h"

#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName, for docking beside the Hierarchy
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <string>

AssetsPanel::AssetsPanel(SDL_Window* window, std::filesystem::path textureFolder)
    : m_Window(window)
    , m_TextureFolder(std::move(textureFolder))
{
}

void AssetsPanel::OpenImportDialog(AssetLibrary& assets)
{
    // SDL answers on its own thread once the user picks files, so the
    // callback only queues the paths; AssetLibrary loads them on the main
    // (GL) thread. The filter list must outlive the dialog, hence static.
    static const SDL_DialogFileFilter kFilters[] = {
        { "Images", "png;jpg;jpeg;tga;bmp" },
    };
    const SDL_DialogFileCallback onChosen = [](void* userdata, const char* const* files, int /*filter*/) {
        if (!files)
        {
            SDL_Log("File dialog failed: %s", SDL_GetError());
            return;
        }
        auto* library = static_cast<AssetLibrary*>(userdata);
        for (const char* const* file = files; *file; ++file)
            library->QueueImport(std::filesystem::path(reinterpret_cast<const char8_t*>(*file)));
    };
    SDL_ShowOpenFileDialog(onChosen, &assets, m_Window, kFilters, 1, nullptr, true);
}

void AssetsPanel::Draw(AssetLibrary& assets, Scene& scene, Entity* selected)
{
    // The first time this panel appears in a saved layout that predates it,
    // open it as a tab beside the Hierarchy rather than floating.
    if (const ImGuiWindow* hierarchy = ImGui::FindWindowByName("Hierarchy"); hierarchy && hierarchy->DockId)
        ImGui::SetNextWindowDockID(hierarchy->DockId, ImGuiCond_FirstUseEver);

    ImGui::Begin("Assets");
    DrawTextures(assets);
    ImGui::Spacing();
    DrawMaterials(assets, scene, selected);
    ImGui::End();
}

void AssetsPanel::DrawTextures(AssetLibrary& assets)
{
    ImGui::SeparatorText("Textures");

    if (ImGui::Button("Import Image..."))
        OpenImportDialog(assets);
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
        assets.ImportFolder(m_TextureFolder);
    ImGui::SetItemTooltip("Load new images from %s", m_TextureFolder.string().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    ImGui::SetItemTooltip("You can also drop image files onto the window.\n"
                          "Drag a thumbnail onto a material's texture slot.");

    // A grid of thumbnails with names underneath, wrapping to the width.
    const float fontSize = ImGui::GetFontSize();
    const float thumbSize = fontSize * 4.0f;
    const float cellWidth = thumbSize + ImGui::GetStyle().ItemSpacing.x;
    const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cellWidth));

    const Texture* toRemove = nullptr;
    int column = 0;
    for (const AssetLibrary::NamedTexture& entry : assets.GetTextures())
    {
        const Texture* texture = entry.texture.get();
        if (column++ % columns != 0)
            ImGui::SameLine();

        ImGui::PushID(texture);
        ImGui::BeginGroup();
        const bool isSelected = texture == m_SelectedTexture;
        if (isSelected)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
        if (ImGui::ImageButton("##thumb", static_cast<ImTextureID>(texture->GetId()),
                               ImVec2(thumbSize - 4.0f, thumbSize - 4.0f), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f)))
            m_SelectedTexture = texture;
        ImGui::PopStyleVar();
        if (isSelected)
            ImGui::PopStyleColor();

        // Drag the thumbnail onto a texture slot.
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload(kTexturePayload, &texture, sizeof(texture));
            TextureThumbnail(*texture, fontSize * 3.0f);
            ImGui::SameLine();
            ImGui::TextUnformatted(entry.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem("Remove"))
                toRemove = texture;
            ImGui::EndPopup();
        }

        // Name under the thumbnail, cut off at the cell width.
        const ImVec2 namePos = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(namePos, ImVec2(namePos.x + thumbSize, namePos.y + fontSize * 2.0f), true);
        ImGui::TextUnformatted(entry.name.c_str());
        ImGui::PopClipRect();
        ImGui::EndGroup();
        ImGui::SetItemTooltip("%s", entry.name.c_str());
        ImGui::PopID();
    }
    if (assets.GetTextures().empty())
        ImGui::TextDisabled("No textures yet.");

    // Details of the selected texture.
    const auto& textures = assets.GetTextures();
    const auto selected = std::find_if(textures.begin(), textures.end(), [&](const AssetLibrary::NamedTexture& e) {
        return e.texture.get() == m_SelectedTexture;
    });
    if (selected != textures.end())
    {
        Texture& texture = *selected->texture;
        ImGui::Spacing();

        std::string name = selected->name;
        if (ImGui::InputText("Name##texture", &name, ImGuiInputTextFlags_EnterReturnsTrue))
            assets.RenameTexture(&texture, name);

        int colorSpace = texture.colorSpace == Texture::ColorSpace::Srgb ? 0 : 1;
        if (ImGui::Combo("Color Space", &colorSpace, "sRGB\0Non-Color\0"))
            texture.colorSpace = colorSpace == 0 ? Texture::ColorSpace::Srgb : Texture::ColorSpace::NonColor;
        ImGui::SetItemTooltip("sRGB for colors (base color, emission).\n"
                              "Non-Color for data (normal, roughness, metallic, AO).");

        ImGui::TextDisabled("%d x %d", texture.GetWidth(), texture.GetHeight());
        if (!texture.GetPath().empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", texture.GetPath().string().c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // Removed after drawing, so nothing above was using it mid-loop.
    if (toRemove)
    {
        if (m_SelectedTexture == toRemove)
            m_SelectedTexture = nullptr;
        assets.RemoveTexture(toRemove);
    }
}

void AssetsPanel::DrawMaterials(AssetLibrary& assets, Scene& scene, Entity* selected)
{
    ImGui::SeparatorText("Materials");

    if (ImGui::Button("New Material"))
        m_SelectedMaterial = assets.CreateMaterial();

    Material* toDelete = nullptr;
    Material* duplicated = nullptr;
    for (const std::unique_ptr<Material>& owned : assets.GetMaterials())
    {
        Material* material = owned.get();
        ImGui::PushID(material);

        // Blender shows how many objects use a material; so do we.
        const int users = scene.CountUsers(material);
        const std::string label = users > 0 ? material->name + "  (" + std::to_string(users) + ")" : material->name;
        if (ImGui::Selectable(label.c_str(), material == m_SelectedMaterial))
            m_SelectedMaterial = material;
        ImGui::SetItemTooltip("Used by %d object%s. Drag onto an object in the Hierarchy.", users,
                              users == 1 ? "" : "s");

        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload(kMaterialPayload, &material, sizeof(material));
            ImGui::TextUnformatted(material->name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem("Assign to Selected", nullptr, false, selected != nullptr))
                selected->material = material;
            if (ImGui::MenuItem("Duplicate"))
                duplicated = material;
            if (ImGui::MenuItem("Delete"))
                toDelete = material;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // Changes to the material list wait until the loop over it is done.
    if (duplicated)
        m_SelectedMaterial = assets.DuplicateMaterial(*duplicated);
    if (toDelete)
    {
        if (m_SelectedMaterial == toDelete)
            m_SelectedMaterial = nullptr;
        scene.ClearMaterial(toDelete);
        assets.RemoveMaterial(toDelete);
    }

    if (m_SelectedMaterial)
    {
        ImGui::Spacing();
        ImGui::InputText("Name##material", &m_SelectedMaterial->name);
        DrawMaterialEditor(*m_SelectedMaterial, assets);
    }
}
