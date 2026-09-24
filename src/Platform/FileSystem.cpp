#include "Platform/FileSystem.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace
{
    // A file:/// URL for SDL_OpenURL.
    std::string FileUrl(const std::filesystem::path& path)
    {
        const std::u8string utf8 = std::filesystem::absolute(path).generic_u8string();
        return "file:///" + std::string(utf8.begin(), utf8.end());
    }
}

namespace Platform
{
    std::filesystem::path Normalized(const std::filesystem::path& path)
    {
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(path, error);
        return (error ? path : absolute).lexically_normal();
    }

    bool IsWithin(const std::filesystem::path& path, const std::filesystem::path& folder)
    {
        auto lower = [](const std::filesystem::path& part) {
            std::string text = ToUtf8(part);
            for (char& c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return text;
        };

        const std::filesystem::path p = Normalized(path);
        const std::filesystem::path f = Normalized(folder);
        auto pi = p.begin();
        for (auto fi = f.begin(); fi != f.end(); ++fi)
        {
            if (fi->empty()) // trailing separator
                continue;
            if (pi == p.end() || lower(*pi) != lower(*fi))
                return false;
            ++pi;
        }
        return true;
    }

    std::string ToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    std::filesystem::path FromUtf8(const std::string& text)
    {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    bool MoveToTrash(const std::filesystem::path& path, std::string& error)
    {
#ifdef _WIN32
        // SHFileOperation wants the path double-null-terminated.
        std::wstring from = std::filesystem::absolute(path).wstring();
        from.push_back(L'\0');

        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = from.c_str();
        // FOF_ALLOWUNDO sends it to the Recycle Bin instead of deleting it.
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        const int result = SHFileOperationW(&operation);
        if (result != 0 || operation.fAnyOperationsAborted)
        {
            error = "Couldn't move to the Recycle Bin (error " + std::to_string(result) + ")";
            return false;
        }
        return true;
#else
        // No portable trash: delete for real.
        std::error_code code;
        std::filesystem::remove_all(path, code);
        if (code)
        {
            error = code.message();
            return false;
        }
        return true;
#endif
    }

    void ShowInFileBrowser(const std::filesystem::path& path)
    {
#ifdef _WIN32
        if (!std::filesystem::is_directory(path))
        {
            // Explorer opens the folder with the file selected.
            const std::wstring arguments = L"/select,\"" + std::filesystem::absolute(path).wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
            return;
        }
#endif
        const std::filesystem::path folder = std::filesystem::is_directory(path) ? path : path.parent_path();
        SDL_OpenURL(FileUrl(folder).c_str());
    }

    void OpenWithDefaultApp(const std::filesystem::path& path)
    {
        SDL_OpenURL(FileUrl(path).c_str());
    }
}
