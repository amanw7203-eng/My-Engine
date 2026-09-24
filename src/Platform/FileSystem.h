#pragma once

#include <filesystem>
#include <string>

// File system helpers, including operations that need the operating
// system's help.
namespace Platform
{
    // Absolute, with "." and ".." resolved, without touching the disk (so it
    // also works for a file that has just been moved or deleted).
    std::filesystem::path Normalized(const std::filesystem::path& path);

    // True if `path` is `folder` itself or somewhere inside it. Ignores
    // case, as Windows paths do.
    bool IsWithin(const std::filesystem::path& path, const std::filesystem::path& folder);

    // A path as UTF-8 text, and back. (path::string() uses the Windows code
    // page, which can't hold every file name.)
    std::string ToUtf8(const std::filesystem::path& path);
    std::filesystem::path FromUtf8(const std::string& text);

    // Moves a file or folder to the Recycle Bin (Trash), so a delete from the
    // editor can be undone. Returns false and fills `error` on failure.
    bool MoveToTrash(const std::filesystem::path& path, std::string& error);

    // Opens the OS file browser at `path`: a folder is opened, a file is
    // shown selected in its folder.
    void ShowInFileBrowser(const std::filesystem::path& path);

    // Opens a file with whatever app the OS associates with its type.
    void OpenWithDefaultApp(const std::filesystem::path& path);
}
