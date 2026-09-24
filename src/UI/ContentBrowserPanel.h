#pragma once

#include <SDL3/SDL.h>
#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class AssetLibrary;
class Texture;

// The "Content Browser": the project's asset files as they are on disk,
// like Unity's Project window or Unreal's Content Browser. Docked along the
// bottom of the editor.
//   Left:  the folder tree.   Right: the open folder as a grid of thumbnails.
//   - Double-click: open a folder, import an image, or open any other file
//     in its default app
//   - Drag an image onto a material's texture slot; drag anything onto a
//     folder (in the grid or the tree) to move it there
//   - Right-click: rename (F2), delete (Del; to the Recycle Bin), show in
//     Explorer. On empty space: new folder, import files
//   - Files dropped from Explorer onto the panel are copied into the open folder
//   - Search finds matching names in the open folder and everything below it
// Renaming, moving and deleting keep the AssetLibrary in step: a texture
// follows its file, and one whose file is deleted is removed.
class ContentBrowserPanel
{
public:
    // `root` is the top folder shown; nothing above it can be browsed.
    ContentBrowserPanel(SDL_Window* window, std::filesystem::path root);
    ~ContentBrowserPanel();

    ContentBrowserPanel(const ContentBrowserPanel&) = delete;
    ContentBrowserPanel& operator=(const ContentBrowserPanel&) = delete;

    // Call between ImGuiLayer::BeginFrame and EndFrame.
    void Draw(AssetLibrary& assets);

    // True if the panel covered this point (window coordinates) last frame,
    // i.e. a file dropped there from the OS was dropped onto the panel.
    bool Contains(float x, float y) const;

    // Copies a file or folder into the open folder (under a new name if one
    // is taken) and imports it if it is an image. Safe to call from any
    // thread (the OS file dialog answers on its own); the copy happens
    // during the next Draw.
    void QueueCopyIn(std::filesystem::path source);

private:
    // One entry in the open folder (or in the search results).
    struct Item
    {
        std::filesystem::path path;
        std::string name; // UTF-8 file name, for display
        bool isFolder = false;
        bool isImage = false;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified;
    };

    // A small copy of an image, for its grid cell.
    struct Thumbnail
    {
        std::unique_ptr<Texture> texture; // nullptr if the image couldn't be read
        std::filesystem::file_time_type modified;
    };

    void Navigate(const std::filesystem::path& folder);
    void Refresh();
    void ProcessCopyQueue(AssetLibrary& assets);

    void DrawToolbar();
    void DrawFolderTree(const std::filesystem::path& folder, AssetLibrary& assets);
    void DrawGrid(AssetLibrary& assets);
    // Returns true if the item was double-clicked (opened).
    bool DrawItem(const Item& item, AssetLibrary& assets, float cellWidth);
    void DrawItemMenu(const Item& item, AssetLibrary& assets);
    void DrawDeleteConfirmation(AssetLibrary& assets);

    void Open(const Item& item, AssetLibrary& assets);
    void StartRename(const std::filesystem::path& path);
    void FinishRename(AssetLibrary& assets);
    void CreateFolder();
    void OpenImportDialog();
    // Moves `source` into `folder`. Reports problems in the status line.
    void MoveInto(const std::filesystem::path& source, const std::filesystem::path& folder, AssetLibrary& assets);
    // A folder accepting files dragged onto it (call after drawing it).
    void FolderDropTarget(const std::filesystem::path& folder, AssetLibrary& assets);

    // The image to show for an image file: its loaded texture if it is in
    // the library, else a small copy loaded on demand (nullptr until then).
    const Texture* GetThumbnail(const Item& item, const AssetLibrary& assets);

    // Shows `message` at the bottom of the panel for a few seconds.
    void SetStatus(std::string message, bool isError);

    std::string DisplayPath(const std::filesystem::path& path) const; // relative to the root

    SDL_Window* m_Window;
    std::filesystem::path m_Root;
    std::filesystem::path m_Current;

    // The open folder's contents (or the search results), re-read on every
    // change and once a second, so changes made outside the editor show up.
    std::vector<Item> m_Items;
    std::map<std::filesystem::path, std::vector<std::filesystem::path>> m_Subfolders; // folder tree cache
    std::chrono::steady_clock::time_point m_LastRefresh;
    bool m_NeedsRefresh = true;
    bool m_RevealInTree = true; // expand the tree down to m_Current (after navigating)

    std::string m_Search;
    std::string m_SearchUsed; // the search m_Items was built for
    float m_ThumbnailScale = 5.0f; // thumbnail size, in font heights

    std::filesystem::path m_Selected;
    std::filesystem::path m_Renaming; // empty when not renaming
    std::string m_RenameBuffer;
    bool m_FocusRename = false;
    std::filesystem::path m_PendingDelete; // waiting for the confirmation dialog

    std::map<std::filesystem::path, Thumbnail> m_Thumbnails;
    // Thumbnails dropped from the cache this frame. Freed at the start of the
    // next Draw: this frame's UI may already show them.
    std::vector<std::unique_ptr<Texture>> m_RetiredThumbnails;
    // Drops cached thumbnails of files at or under `path` (all if empty).
    void RetireThumbnails(const std::filesystem::path& path = {});
    int m_ThumbnailLoadsLeft = 0; // per frame, so a big folder doesn't stall

    std::mutex m_CopyMutex;
    std::vector<std::filesystem::path> m_CopyQueue;

    std::string m_Status;
    bool m_StatusIsError = false;
    std::chrono::steady_clock::time_point m_StatusUntil;

    ImVec2 m_WindowMin{ 0.0f, 0.0f };
    ImVec2 m_WindowMax{ 0.0f, 0.0f };
};
