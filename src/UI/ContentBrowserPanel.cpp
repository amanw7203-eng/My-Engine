#include "UI/ContentBrowserPanel.h"

#include "Assets/AssetLibrary.h"
#include "Platform/FileSystem.h"
#include "Renderer/Texture.h"
#include "UI/AssetWidgets.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>

using Platform::FromUtf8;
using Platform::IsWithin;
using Platform::ToUtf8;

namespace
{
    constexpr std::chrono::seconds kRefreshInterval{ 1 };
    constexpr int kThumbnailLoadsPerFrame = 2;
    constexpr int kThumbnailPixels = 128; // longest side of a cached thumbnail

    std::string Lower(std::string text)
    {
        for (char& c : text)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return text;
    }

    // "1.4 MB" etc.
    std::string FormatSize(std::uintmax_t bytes)
    {
        char text[32];
        if (bytes < 1024)
            std::snprintf(text, sizeof(text), "%u B", static_cast<unsigned>(bytes));
        else if (bytes < 1024 * 1024)
            std::snprintf(text, sizeof(text), "%.1f KB", bytes / 1024.0);
        else
            std::snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
        return text;
    }

    // What to print on a file's icon, and in which color, by extension.
    struct FileKind
    {
        std::string label;
        ImU32 color;
    };

    FileKind KindOf(const std::filesystem::path& path)
    {
        const std::string extension = Lower(ToUtf8(path.extension()));
        auto is = [&](std::initializer_list<const char*> list) {
            return std::any_of(list.begin(), list.end(), [&](const char* e) { return extension == e; });
        };
        if (is({ ".vert", ".frag", ".glsl", ".comp", ".geom", ".tesc", ".tese" }))
            return { "GLSL", IM_COL32(150, 110, 220, 255) };
        if (is({ ".cu", ".cuh" }))
            return { "CUDA", IM_COL32(118, 185, 0, 255) };
        if (is({ ".obj", ".fbx", ".gltf", ".glb" }))
            return { "MESH", IM_COL32(230, 140, 60, 255) };
        if (is({ ".txt", ".md", ".json", ".ini", ".xml", ".yaml" }))
            return { "TEXT", IM_COL32(120, 140, 170, 255) };
        std::string label = extension.size() > 1 ? extension.substr(1, 4) : "FILE";
        std::transform(label.begin(), label.end(), label.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return { label, IM_COL32(130, 130, 130, 255) };
    }

    // A folder: a tab on top of a body, filling [min, max].
    void DrawFolderIcon(ImDrawList* draw, ImVec2 min, ImVec2 max)
    {
        const float w = max.x - min.x;
        const float h = max.y - min.y;
        const float rounding = w * 0.06f;
        const ImVec2 bodyMin(min.x + w * 0.06f, min.y + h * 0.26f);
        const ImVec2 bodyMax(max.x - w * 0.06f, max.y - h * 0.12f);
        draw->AddRectFilled(ImVec2(bodyMin.x, bodyMin.y - h * 0.08f), ImVec2(bodyMin.x + w * 0.38f, bodyMin.y + h * 0.1f),
                            IM_COL32(196, 146, 52, 255), rounding);
        draw->AddRectFilled(bodyMin, bodyMax, IM_COL32(226, 176, 72, 255), rounding);
    }

    // A page with a folded corner and a colored label band.
    void DrawFileIcon(ImDrawList* draw, ImVec2 min, ImVec2 max, const FileKind& kind)
    {
        const float w = max.x - min.x;
        const float h = max.y - min.y;
        const ImVec2 pageMin(min.x + w * 0.18f, min.y + h * 0.06f);
        const ImVec2 pageMax(max.x - w * 0.18f, max.y - h * 0.06f);
        const float fold = w * 0.18f;
        draw->AddRectFilled(pageMin, pageMax, IM_COL32(215, 218, 224, 255), w * 0.03f);
        draw->AddTriangleFilled(ImVec2(pageMax.x - fold, pageMin.y), ImVec2(pageMax.x, pageMin.y + fold),
                                ImVec2(pageMax.x - fold, pageMin.y + fold), IM_COL32(160, 164, 172, 255));

        const ImVec2 bandMin(pageMin.x, pageMin.y + (pageMax.y - pageMin.y) * 0.55f);
        const ImVec2 bandMax(pageMax.x, bandMin.y + ImGui::GetFontSize() * 1.3f);
        draw->AddRectFilled(bandMin, bandMax, kind.color);
        const ImVec2 textSize = ImGui::CalcTextSize(kind.label.c_str());
        draw->AddText(ImVec2((bandMin.x + bandMax.x - textSize.x) * 0.5f, (bandMin.y + bandMax.y - textSize.y) * 0.5f),
                      IM_COL32_WHITE, kind.label.c_str());
    }

    // `text` cut to fit `width` pixels, ending in "..." if it didn't.
    std::string Ellipsize(const std::string& text, float width)
    {
        if (ImGui::CalcTextSize(text.c_str()).x <= width)
            return text;
        std::string cut = text;
        while (!cut.empty() && ImGui::CalcTextSize((cut + "...").c_str()).x > width)
        {
            // Remove one whole UTF-8 character: its continuation bytes
            // (10xxxxxx), then its first byte.
            while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80)
                cut.pop_back();
            if (!cut.empty())
                cut.pop_back();
        }
        return cut + "...";
    }

    // `folder / name`, or "name (2)", "name (3)", ... if that is taken.
    std::filesystem::path UniquePath(const std::filesystem::path& folder, const std::filesystem::path& name)
    {
        std::filesystem::path candidate = folder / name;
        std::error_code error;
        for (int n = 2; std::filesystem::exists(candidate, error); ++n)
        {
            const std::string numbered = ToUtf8(name.stem()) + " (" + std::to_string(n) + ")" + ToUtf8(name.extension());
            candidate = folder / FromUtf8(numbered);
        }
        return candidate;
    }

    // A small copy of an image (longest side kThumbnailPixels), or nullptr
    // if it can't be read.
    std::unique_ptr<Texture> LoadThumbnail(const std::filesystem::path& path)
    {
        // Same orientation as Texture: rows bottom first.
        stbi_set_flip_vertically_on_load(true);
        int width = 0, height = 0, channels = 0;
#ifdef _WIN32
        // stbi_load takes a narrow path; open the file ourselves so any name works.
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0)
            file = nullptr;
#else
        FILE* file = std::fopen(path.c_str(), "rb");
#endif
        if (!file)
            return nullptr;
        unsigned char* pixels = stbi_load_from_file(file, &width, &height, &channels, 4);
        std::fclose(file);
        if (!pixels)
            return nullptr;

        // Box filter: each thumbnail pixel averages the block of image
        // pixels it covers.
        const float scale = std::min(1.0f, static_cast<float>(kThumbnailPixels) / std::max(width, height));
        const int tw = std::max(1, static_cast<int>(width * scale));
        const int th = std::max(1, static_cast<int>(height * scale));
        std::vector<unsigned char> small(static_cast<size_t>(tw) * th * 4);
        for (int y = 0; y < th; ++y)
        {
            const int y0 = y * height / th;
            const int y1 = std::max(y0 + 1, (y + 1) * height / th);
            for (int x = 0; x < tw; ++x)
            {
                const int x0 = x * width / tw;
                const int x1 = std::max(x0 + 1, (x + 1) * width / tw);
                unsigned sum[4] = {};
                for (int sy = y0; sy < y1; ++sy)
                    for (int sx = x0; sx < x1; ++sx)
                        for (int c = 0; c < 4; ++c)
                            sum[c] += pixels[(static_cast<size_t>(sy) * width + sx) * 4 + c];
                const unsigned count = static_cast<unsigned>((y1 - y0) * (x1 - x0));
                for (int c = 0; c < 4; ++c)
                    small[(static_cast<size_t>(y) * tw + x) * 4 + c] = static_cast<unsigned char>(sum[c] / count);
            }
        }
        stbi_image_free(pixels);
        return std::make_unique<Texture>(small.data(), tw, th);
    }
}

ContentBrowserPanel::ContentBrowserPanel(SDL_Window* window, std::filesystem::path root)
    : m_Window(window)
    , m_Root(Platform::Normalized(root))
    , m_Current(m_Root)
{
}

ContentBrowserPanel::~ContentBrowserPanel() = default;

bool ContentBrowserPanel::Contains(float x, float y) const
{
    return x >= m_WindowMin.x && y >= m_WindowMin.y && x < m_WindowMax.x && y < m_WindowMax.y;
}

void ContentBrowserPanel::QueueCopyIn(std::filesystem::path source)
{
    std::lock_guard lock(m_CopyMutex);
    m_CopyQueue.push_back(std::move(source));
}

void ContentBrowserPanel::SetStatus(std::string message, bool isError)
{
    if (isError)
        std::cerr << "Content Browser: " << message << '\n';
    m_Status = std::move(message);
    m_StatusIsError = isError;
    m_StatusUntil = std::chrono::steady_clock::now() + std::chrono::seconds(isError ? 6 : 3);
}

std::string ContentBrowserPanel::DisplayPath(const std::filesystem::path& path) const
{
    return ToUtf8(m_Root.filename() / path.lexically_relative(m_Root).lexically_normal());
}

void ContentBrowserPanel::Navigate(const std::filesystem::path& folder)
{
    // Never above the root.
    m_Current = IsWithin(folder, m_Root) ? Platform::Normalized(folder) : m_Root;
    m_Search.clear();
    m_NeedsRefresh = true;
    m_RevealInTree = true;
}

void ContentBrowserPanel::Refresh()
{
    m_NeedsRefresh = false;
    m_LastRefresh = std::chrono::steady_clock::now();
    m_Subfolders.clear();
    m_Items.clear();
    m_SearchUsed = m_Search;

    // The open folder was deleted or moved outside the editor: go up.
    std::error_code error;
    while (m_Current != m_Root && !std::filesystem::is_directory(m_Current, error))
        m_Current = m_Current.parent_path();

    auto addItem = [&](const std::filesystem::directory_entry& entry) {
        Item item;
        item.path = entry.path();
        item.name = ToUtf8(entry.path().filename());
        item.isFolder = entry.is_directory(error);
        item.isImage = !item.isFolder && AssetLibrary::IsImageFile(entry.path());
        item.size = item.isFolder ? 0 : entry.file_size(error);
        item.modified = entry.last_write_time(error);
        m_Items.push_back(std::move(item));
    };

    if (m_Search.empty())
    {
        for (const auto& entry : std::filesystem::directory_iterator(m_Current, error))
            addItem(entry);
    }
    else
    {
        // Search this folder and everything below it.
        const std::string needle = Lower(m_Search);
        for (auto it = std::filesystem::recursive_directory_iterator(
                 m_Current, std::filesystem::directory_options::skip_permission_denied, error);
             it != std::filesystem::recursive_directory_iterator(); it.increment(error))
        {
            if (error)
                break;
            if (Lower(ToUtf8(it->path().filename())).find(needle) != std::string::npos)
                addItem(*it);
        }
    }

    // Folders first, then by name, ignoring case.
    std::sort(m_Items.begin(), m_Items.end(), [](const Item& a, const Item& b) {
        if (a.isFolder != b.isFolder)
            return a.isFolder;
        return Lower(a.name) < Lower(b.name);
    });

    // Keep the thumbnail cache from growing without bound.
    if (m_Thumbnails.size() > 512)
        RetireThumbnails();
}

void ContentBrowserPanel::RetireThumbnails(const std::filesystem::path& path)
{
    for (auto it = m_Thumbnails.begin(); it != m_Thumbnails.end();)
    {
        if (path.empty() || IsWithin(it->first, path))
        {
            if (it->second.texture)
                m_RetiredThumbnails.push_back(std::move(it->second.texture));
            it = m_Thumbnails.erase(it);
        }
        else
            ++it;
    }
}

void ContentBrowserPanel::ProcessCopyQueue(AssetLibrary& assets)
{
    std::vector<std::filesystem::path> queued;
    {
        std::lock_guard lock(m_CopyMutex);
        queued.swap(m_CopyQueue);
    }
    for (const std::filesystem::path& source : queued)
    {
        std::error_code error;
        const std::filesystem::path target = UniquePath(m_Current, source.filename());
        std::filesystem::copy(source, target, std::filesystem::copy_options::recursive, error);
        if (error)
        {
            SetStatus("Couldn't copy " + ToUtf8(source.filename()) + ": " + error.message(), true);
            continue;
        }
        if (std::filesystem::is_directory(target))
            assets.ImportFolder(target);
        else if (AssetLibrary::IsImageFile(target))
            assets.ImportTexture(target);
        m_Selected = target;
        SetStatus("Added " + ToUtf8(target.filename()), false);
    }
    if (!queued.empty())
        m_NeedsRefresh = true;
}

void ContentBrowserPanel::Draw(AssetLibrary& assets)
{
    // Last frame's UI has been drawn, so nothing shows these any more.
    m_RetiredThumbnails.clear();

    ProcessCopyQueue(assets);
    if (m_NeedsRefresh || m_Search != m_SearchUsed ||
        std::chrono::steady_clock::now() - m_LastRefresh > kRefreshInterval)
        Refresh();
    m_ThumbnailLoadsLeft = kThumbnailLoadsPerFrame;

    const bool open = ImGui::Begin("Content Browser");
    m_WindowMin = ImGui::GetWindowPos();
    m_WindowMax = ImVec2(m_WindowMin.x + ImGui::GetWindowWidth(), m_WindowMin.y + ImGui::GetWindowHeight());
    if (!open)
    {
        m_WindowMax = m_WindowMin; // collapsed or hidden: drops don't land here
        ImGui::End();
        return;
    }

    // Folder tree | toolbar + grid, with a draggable divider.
    if (ImGui::BeginTable("##layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImGui::GetContentRegionAvail()))
    {
        ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 13.0f);
        ImGui::TableSetupColumn("Files", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::BeginChild("##tree"))
            DrawFolderTree(m_Root, assets);
        ImGui::EndChild();
        m_RevealInTree = false;

        ImGui::TableSetColumnIndex(1);
        DrawToolbar();
        const bool showStatus = std::chrono::steady_clock::now() < m_StatusUntil;
        const float statusHeight = showStatus ? ImGui::GetTextLineHeightWithSpacing() : 0.0f;
        if (ImGui::BeginChild("##grid", ImVec2(0.0f, -statusHeight)))
            DrawGrid(assets);
        ImGui::EndChild();
        if (showStatus)
        {
            ImGui::TextColored(m_StatusIsError ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.6f, 0.85f, 0.6f, 1.0f),
                               "%s", m_Status.c_str());
        }
        ImGui::EndTable();
    }

    DrawDeleteConfirmation(assets);
    ImGui::End();
}

void ContentBrowserPanel::DrawToolbar()
{
    // Up one folder.
    ImGui::BeginDisabled(m_Current == m_Root);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up))
        Navigate(m_Current.parent_path());
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Up one folder (Backspace)");

    // Breadcrumbs: each folder on the way down from the root, clickable
    // (and a drop target, to move things up the tree).
    std::vector<std::filesystem::path> crumbs;
    for (std::filesystem::path p = m_Current; ; p = p.parent_path())
    {
        crumbs.push_back(p);
        if (p == m_Root || p == p.parent_path())
            break;
    }
    std::reverse(crumbs.begin(), crumbs.end());
    std::filesystem::path navigateTo;
    for (size_t i = 0; i < crumbs.size(); ++i)
    {
        ImGui::SameLine(0.0f, i == 0 ? -1.0f : ImGui::GetStyle().ItemSpacing.x * 0.5f);
        if (i > 0)
        {
            ImGui::TextDisabled(">");
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 0.5f);
        }
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button(ToUtf8(crumbs[i].filename()).c_str()))
            navigateTo = crumbs[i];
        ImGui::PopID();
    }
    if (!navigateTo.empty())
        Navigate(navigateTo);

    // Search and thumbnail size, on the right.
    const float searchWidth = ImGui::GetFontSize() * 12.0f;
    const float sizeWidth = ImGui::GetFontSize() * 6.0f;
    const float rightWidth = searchWidth + sizeWidth + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine();
    // Right-aligned in this column (the region max would be the whole window's).
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - rightWidth));
    ImGui::SetNextItemWidth(searchWidth);
    ImGui::InputTextWithHint("##search", "Search", &m_Search);
    ImGui::SetItemTooltip("Find files by name in this folder and all folders inside it");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(sizeWidth);
    ImGui::SliderFloat("##size", &m_ThumbnailScale, 3.0f, 10.0f, "Size");
    ImGui::Separator();
}

void ContentBrowserPanel::DrawFolderTree(const std::filesystem::path& folder, AssetLibrary& assets)
{
    // Subfolders, cached until the next refresh.
    auto cached = m_Subfolders.find(folder);
    if (cached == m_Subfolders.end())
    {
        std::vector<std::filesystem::path> subfolders;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error))
            if (entry.is_directory(error))
                subfolders.push_back(entry.path());
        std::sort(subfolders.begin(), subfolders.end(),
                  [](const auto& a, const auto& b) { return Lower(ToUtf8(a.filename())) < Lower(ToUtf8(b.filename())); });
        cached = m_Subfolders.emplace(folder, std::move(subfolders)).first;
    }
    const std::vector<std::filesystem::path> subfolders = cached->second; // copy: drawing may refresh

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (subfolders.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (folder == m_Current)
        flags |= ImGuiTreeNodeFlags_Selected;
    if (folder == m_Root)
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    // After navigating, open the folders leading to the open one.
    if (m_RevealInTree && folder != m_Current && IsWithin(m_Current, folder))
        ImGui::SetNextItemOpen(true);

    const std::string label = ToUtf8(folder.filename());
    const bool open = ImGui::TreeNodeEx(ToUtf8(folder).c_str(), flags, "%s", label.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        Navigate(folder);
    FolderDropTarget(folder, assets);

    if (open)
    {
        for (const std::filesystem::path& subfolder : subfolders)
            DrawFolderTree(subfolder, assets);
        ImGui::TreePop();
    }
}

void ContentBrowserPanel::DrawGrid(AssetLibrary& assets)
{
    const float fontSize = ImGui::GetFontSize();
    const float thumbSize = fontSize * m_ThumbnailScale;
    const float padding = fontSize * 0.35f;
    const float cellWidth = thumbSize + padding * 2.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (cellWidth + spacing)));

    // Items are opened after the loop: opening a folder changes m_Items.
    const Item* toOpen = nullptr;
    int column = 0;
    for (const Item& item : m_Items)
    {
        if (column++ % columns != 0)
            ImGui::SameLine();
        if (DrawItem(item, assets, cellWidth))
            toOpen = &item;
    }
    if (m_Items.empty())
        ImGui::TextDisabled(m_Search.empty() ? "This folder is empty. Drop files here to add them."
                                             : "Nothing matches.");

    if (toOpen)
    {
        const Item item = *toOpen;
        Open(item, assets);
    }

    // Clicking empty space clears the selection.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
        m_Selected.clear();

    // Right-click on empty space.
    if (ImGui::BeginPopupContextWindow("##background", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::MenuItem("New Folder"))
            CreateFolder();
        if (ImGui::MenuItem("Import Files..."))
            OpenImportDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Show in Explorer"))
            Platform::ShowInFileBrowser(m_Current);
        if (ImGui::MenuItem("Refresh"))
            m_NeedsRefresh = true;
        ImGui::EndPopup();
    }

    // Keyboard shortcuts, while the grid has focus and no name is being edited.
    if (ImGui::IsWindowFocused() && m_Renaming.empty() && !ImGui::GetIO().WantTextInput)
    {
        const auto selected = std::find_if(m_Items.begin(), m_Items.end(),
                                           [&](const Item& item) { return item.path == m_Selected; });
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && m_Current != m_Root)
            Navigate(m_Current.parent_path());
        else if (selected != m_Items.end())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_F2))
                StartRename(selected->path);
            else if (ImGui::IsKeyPressed(ImGuiKey_Delete))
                m_PendingDelete = selected->path;
            else if (ImGui::IsKeyPressed(ImGuiKey_Enter))
            {
                const Item item = *selected;
                Open(item, assets);
            }
        }
    }
}

bool ContentBrowserPanel::DrawItem(const Item& item, AssetLibrary& assets, float cellWidth)
{
    const float fontSize = ImGui::GetFontSize();
    const float padding = fontSize * 0.35f;
    const float thumbSize = cellWidth - padding * 2.0f;
    const float nameHeight = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 cellSize(cellWidth, thumbSize + padding * 2.0f + nameHeight);

    ImGui::PushID(ToUtf8(item.path).c_str());
    ImGui::BeginGroup();
    const ImVec2 cellMin = ImGui::GetCursorScreenPos();
    const ImVec2 cellMax(cellMin.x + cellSize.x, cellMin.y + cellSize.y);
    ImGui::InvisibleButton("##cell", cellSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    bool opened = false;

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        m_Selected = item.path;
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        opened = true;

    // Drag it: onto a folder to move it, or (an image) onto a texture slot.
    if (ImGui::BeginDragDropSource())
    {
        AssetFilePayload payload{};
        const std::string path = ToUtf8(item.path);
        if (path.size() < sizeof(payload.path))
        {
            std::memcpy(payload.path, path.c_str(), path.size() + 1);
            if (item.isImage)
            {
                // Imported now, so a texture slot can take it on drop.
                payload.texture = assets.FindTexture(item.path);
                if (!payload.texture)
                    payload.texture = assets.ImportTexture(item.path);
            }
            ImGui::SetDragDropPayload(kAssetFilePayload, &payload, sizeof(payload));
        }
        ImGui::TextUnformatted(item.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (item.isFolder)
        FolderDropTarget(item.path, assets);

    if (ImGui::BeginPopupContextItem("##menu"))
    {
        DrawItemMenu(item, assets);
        ImGui::EndPopup();
    }

    const Texture* loaded = item.isImage ? assets.FindTexture(item.path) : nullptr;
    if (hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left) && ImGui::BeginItemTooltip())
    {
        ImGui::TextUnformatted(item.name.c_str());
        if (item.isFolder)
            ImGui::TextDisabled("Folder");
        else
            ImGui::TextDisabled("%s", FormatSize(item.size).c_str());
        if (loaded)
            ImGui::TextDisabled("%d x %d, imported as a texture", loaded->GetWidth(), loaded->GetHeight());
        else if (item.isImage)
            ImGui::TextDisabled("Not imported: double-click, or drag onto a texture slot");
        if (!m_SearchUsed.empty())
            ImGui::TextDisabled("in %s", DisplayPath(item.path.parent_path()).c_str());
        ImGui::EndTooltip();
    }

    // --- Drawing ---
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool selected = item.path == m_Selected;
    if (selected || hovered)
    {
        draw->AddRectFilled(cellMin, cellMax,
                            ImGui::GetColorU32(selected ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered),
                            fontSize * 0.3f);
    }

    const ImVec2 thumbMin(cellMin.x + padding, cellMin.y + padding);
    const ImVec2 thumbMax(thumbMin.x + thumbSize, thumbMin.y + thumbSize);
    const Texture* thumbnail = item.isImage ? GetThumbnail(item, assets) : nullptr;
    if (item.isFolder)
        DrawFolderIcon(draw, thumbMin, thumbMax);
    else if (thumbnail)
    {
        // Fit inside the square, keeping the image's shape.
        const float aspect = static_cast<float>(thumbnail->GetWidth()) / std::max(1, thumbnail->GetHeight());
        ImVec2 size(thumbSize, thumbSize);
        if (aspect > 1.0f)
            size.y = thumbSize / aspect;
        else
            size.x = thumbSize * aspect;
        const ImVec2 imageMin(thumbMin.x + (thumbSize - size.x) * 0.5f, thumbMin.y + (thumbSize - size.y) * 0.5f);
        // Flipped vertically: textures store the bottom row first.
        draw->AddImage(static_cast<ImTextureID>(thumbnail->GetId()), imageMin,
                       ImVec2(imageMin.x + size.x, imageMin.y + size.y), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
    }
    else
        DrawFileIcon(draw, thumbMin, thumbMax, item.isImage ? FileKind{ "IMG", IM_COL32(70, 140, 200, 255) } : KindOf(item.path));

    // A dot on images that are already loaded as textures.
    if (loaded)
    {
        const float r = fontSize * 0.28f;
        draw->AddCircleFilled(ImVec2(thumbMax.x - r, thumbMin.y + r), r, IM_COL32(90, 200, 120, 255));
    }

    // The name, or the box for editing it.
    const ImVec2 namePos(cellMin.x + padding, thumbMax.y + padding * 0.5f);
    if (m_Renaming == item.path)
    {
        ImGui::SetCursorScreenPos(namePos);
        ImGui::SetNextItemWidth(thumbSize);
        if (m_FocusRename)
        {
            ImGui::SetKeyboardFocusHere();
            m_FocusRename = false;
        }
        if (ImGui::InputText("##rename", &m_RenameBuffer,
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            FinishRename(assets);
        else if (ImGui::IsItemDeactivated())
        {
            // Escape cancels; clicking elsewhere keeps the new name.
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                m_Renaming.clear();
            else
                FinishRename(assets);
        }
    }
    else
    {
        const std::string name = Ellipsize(item.name, thumbSize);
        const float nameWidth = ImGui::CalcTextSize(name.c_str()).x;
        draw->AddText(ImVec2(namePos.x + (thumbSize - nameWidth) * 0.5f, namePos.y),
                      ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
    }

    ImGui::EndGroup();
    ImGui::PopID();
    return opened;
}

void ContentBrowserPanel::DrawItemMenu(const Item& item, AssetLibrary& assets)
{
    if (ImGui::MenuItem("Open"))
        Open(item, assets);
    if (item.isImage)
    {
        const bool imported = assets.FindTexture(item.path) != nullptr;
        if (ImGui::MenuItem("Import as Texture", nullptr, false, !imported))
            assets.ImportTexture(item.path);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Rename", "F2"))
        StartRename(item.path);
    if (ImGui::MenuItem("Delete", "Del"))
        m_PendingDelete = item.path;
    ImGui::Separator();
    if (ImGui::MenuItem("Show in Explorer"))
        Platform::ShowInFileBrowser(item.path);
    if (ImGui::MenuItem("Copy Path"))
        ImGui::SetClipboardText(ToUtf8(item.path).c_str());
}

void ContentBrowserPanel::DrawDeleteConfirmation(AssetLibrary& assets)
{
    constexpr const char* kPopup = "Delete?##contentbrowser";
    if (!m_PendingDelete.empty() && !ImGui::IsPopupOpen(kPopup))
        ImGui::OpenPopup(kPopup);

    if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const bool isFolder = std::filesystem::is_directory(m_PendingDelete);
    const std::string name = ToUtf8(m_PendingDelete.filename());
    if (isFolder)
        ImGui::Text("Delete the folder \"%s\" and everything in it?", name.c_str());
    else
        ImGui::Text("Delete \"%s\"?", name.c_str());
    ImGui::TextDisabled("It goes to the Recycle Bin, so it can be restored from there.");
    if (const int textures = assets.CountTexturesUnder(m_PendingDelete); textures > 0)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "%d loaded texture%s will be removed, and materials using %s lose %s.", textures,
                           textures == 1 ? "" : "s", textures == 1 ? "it" : "them", textures == 1 ? "it" : "them");
    }

    ImGui::Spacing();
    if (ImGui::Button("Delete") || ImGui::IsKeyPressed(ImGuiKey_Enter))
    {
        std::string error;
        if (Platform::MoveToTrash(m_PendingDelete, error))
        {
            assets.RemoveTexturesUnder(m_PendingDelete);
            RetireThumbnails(m_PendingDelete);
            if (IsWithin(m_Current, m_PendingDelete))
                Navigate(m_PendingDelete.parent_path());
            SetStatus("Moved " + ToUtf8(m_PendingDelete.filename()) + " to the Recycle Bin", false);
            m_Selected.clear();
            m_NeedsRefresh = true;
        }
        else
            SetStatus(error, true);
        m_PendingDelete.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        m_PendingDelete.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ContentBrowserPanel::Open(const Item& item, AssetLibrary& assets)
{
    if (item.isFolder)
        Navigate(item.path);
    else if (item.isImage)
    {
        if (assets.FindTexture(item.path))
            SetStatus(item.name + " is already imported", false);
        else if (assets.ImportTexture(item.path))
            SetStatus("Imported " + item.name + " as a texture", false);
        else
            SetStatus("Couldn't load " + item.name, true);
    }
    else
        Platform::OpenWithDefaultApp(item.path);
}

void ContentBrowserPanel::StartRename(const std::filesystem::path& path)
{
    m_Renaming = path;
    m_RenameBuffer = ToUtf8(path.filename());
    m_FocusRename = true;
    m_Selected = path;
}

void ContentBrowserPanel::FinishRename(AssetLibrary& assets)
{
    const std::filesystem::path from = m_Renaming;
    m_Renaming.clear();
    if (from.empty() || m_RenameBuffer == ToUtf8(from.filename()))
        return;

    if (m_RenameBuffer.empty() || m_RenameBuffer.find_first_of("\\/:*?\"<>|") != std::string::npos)
    {
        SetStatus("A name can't be empty or contain \\ / : * ? \" < > |", true);
        return;
    }
    const std::filesystem::path to = from.parent_path() / FromUtf8(m_RenameBuffer);
    std::error_code error;
    // Allow changing only the case of a name, which Windows sees as the same file.
    if (std::filesystem::exists(to, error) && !std::filesystem::equivalent(from, to, error))
    {
        SetStatus("There is already something called " + m_RenameBuffer + " here", true);
        return;
    }
    std::filesystem::rename(from, to, error);
    if (error)
    {
        SetStatus("Couldn't rename: " + error.message(), true);
        return;
    }
    assets.OnPathMoved(from, to);
    RetireThumbnails(from);
    m_Selected = to;
    m_NeedsRefresh = true;
}

void ContentBrowserPanel::CreateFolder()
{
    const std::filesystem::path folder = UniquePath(m_Current, "New Folder");
    std::error_code error;
    if (!std::filesystem::create_directory(folder, error))
    {
        SetStatus("Couldn't create a folder: " + error.message(), true);
        return;
    }
    Refresh();
    StartRename(folder); // name it straight away
}

void ContentBrowserPanel::OpenImportDialog()
{
    // SDL answers on its own thread once files are picked, so the callback
    // only queues them; they are copied in during the next Draw. The filter
    // list must outlive the dialog, hence static.
    static const SDL_DialogFileFilter kFilters[] = {
        { "All files", "*" },
        { "Images", "png;jpg;jpeg;tga;bmp" },
    };
    const SDL_DialogFileCallback onChosen = [](void* userdata, const char* const* files, int /*filter*/) {
        if (!files)
        {
            SDL_Log("File dialog failed: %s", SDL_GetError());
            return;
        }
        auto* panel = static_cast<ContentBrowserPanel*>(userdata);
        for (const char* const* file = files; *file; ++file)
            panel->QueueCopyIn(std::filesystem::path(reinterpret_cast<const char8_t*>(*file)));
    };
    SDL_ShowOpenFileDialog(onChosen, this, m_Window, kFilters, 2, nullptr, true);
}

void ContentBrowserPanel::MoveInto(const std::filesystem::path& source, const std::filesystem::path& folder,
                                   AssetLibrary& assets)
{
    if (source.parent_path() == folder)
        return; // already there
    if (IsWithin(folder, source))
    {
        SetStatus("Can't move a folder into itself", true);
        return;
    }
    const std::filesystem::path target = folder / source.filename();
    std::error_code error;
    if (std::filesystem::exists(target, error))
    {
        SetStatus(ToUtf8(folder.filename()) + " already has something called " + ToUtf8(source.filename()), true);
        return;
    }
    std::filesystem::rename(source, target, error);
    if (error)
    {
        SetStatus("Couldn't move: " + error.message(), true);
        return;
    }

    assets.OnPathMoved(source, target);
    RetireThumbnails(source);
    // The open folder was the one moved (or inside it): follow it.
    if (IsWithin(m_Current, source))
        Navigate(target / Platform::Normalized(m_Current).lexically_relative(Platform::Normalized(source)));
    m_Selected = target;
    SetStatus("Moved " + ToUtf8(source.filename()) + " to " + DisplayPath(folder), false);
    m_NeedsRefresh = true;
}

void ContentBrowserPanel::FolderDropTarget(const std::filesystem::path& folder, AssetLibrary& assets)
{
    if (!ImGui::BeginDragDropTarget())
        return;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetFilePayload))
    {
        const auto* file = static_cast<const AssetFilePayload*>(payload->Data);
        MoveInto(FromUtf8(file->path), folder, assets);
    }
    ImGui::EndDragDropTarget();
}

const Texture* ContentBrowserPanel::GetThumbnail(const Item& item, const AssetLibrary& assets)
{
    if (const Texture* loaded = assets.FindTexture(item.path))
        return loaded;

    auto cached = m_Thumbnails.find(item.path);
    if (cached != m_Thumbnails.end() && cached->second.modified == item.modified)
        return cached->second.texture.get();

    // Not loaded yet (or the file changed): a few per frame.
    if (m_ThumbnailLoadsLeft <= 0)
        return cached != m_Thumbnails.end() ? cached->second.texture.get() : nullptr;
    --m_ThumbnailLoadsLeft;
    Thumbnail& thumbnail = m_Thumbnails[item.path];
    if (thumbnail.texture)
        m_RetiredThumbnails.push_back(std::move(thumbnail.texture)); // the file changed
    thumbnail.texture = LoadThumbnail(item.path);
    thumbnail.modified = item.modified;
    return thumbnail.texture.get();
}
