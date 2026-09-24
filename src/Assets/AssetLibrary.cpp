#include "Assets/AssetLibrary.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <string_view>

static std::string ToLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Image formats stb_image can read that are worth offering.
static bool IsImageFile(const std::filesystem::path& path)
{
    static constexpr std::array<std::string_view, 5> kExtensions = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };
    const std::string extension = ToLower(path.extension().string());
    return std::find(kExtensions.begin(), kExtensions.end(), extension) != kExtensions.end();
}

// Textures that hold data rather than colors are usually named for their
// map type at the end (bricks_normal.png, Metal012_Roughness.jpg,
// rock-ao.png, rock_normal_gl.png). Only the last two words of the name are
// checked, so "metal_basecolor.png" stays a color texture.
static Texture::ColorSpace GuessColorSpace(const std::filesystem::path& path)
{
    static constexpr std::array<std::string_view, 19> kDataWords = {
        "normal", "normalgl", "nrm", "nor", "n", "roughness", "rough", "metallic", "metalness", "metal",
        "ao", "occlusion", "ambientocclusion", "orm", "arm", "height", "displacement", "disp", "glossiness",
    };

    // Split the name into words at '_', '-', ' ' and '.'.
    const std::string stem = ToLower(path.stem().string());
    std::vector<std::string> words;
    std::string word;
    for (char c : stem + '_')
    {
        if (c == '_' || c == '-' || c == ' ' || c == '.')
        {
            if (!word.empty())
                words.push_back(std::move(word));
            word.clear();
        }
        else
            word += c;
    }

    const size_t first = words.size() > 2 ? words.size() - 2 : 0;
    for (size_t i = first; i < words.size(); ++i)
    {
        if (std::find(kDataWords.begin(), kDataWords.end(), words[i]) != kDataWords.end())
            return Texture::ColorSpace::NonColor;
    }
    return Texture::ColorSpace::Srgb;
}

// Compares two paths as the same file where possible, even if spelled
// differently (relative vs absolute, different slashes).
static bool SameFile(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code error;
    const bool same = std::filesystem::equivalent(a, b, error);
    return error ? a == b : same;
}

// `base`, or the first of "base.001", "base.002", ... that isn't taken.
template <typename IsTaken>
static std::string MakeUniqueName(const std::string& base, IsTaken isTaken)
{
    if (!isTaken(base))
        return base;
    for (int n = 1;; ++n)
    {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), ".%03d", n);
        std::string candidate = base + suffix;
        if (!isTaken(candidate))
            return candidate;
    }
}

const Mesh* AssetLibrary::AddMesh(std::string name, Mesh mesh)
{
    auto& entry = m_Meshes.emplace_back(std::move(name), std::make_unique<Mesh>(std::move(mesh)));
    return entry.mesh.get();
}

Texture* AssetLibrary::AddTexture(std::string name, Texture texture)
{
    auto& entry = m_Textures.emplace_back(MakeUniqueTextureName(name),
                                          std::make_unique<Texture>(std::move(texture)));
    return entry.texture.get();
}

Texture* AssetLibrary::ImportTexture(const std::filesystem::path& path)
{
    for (const NamedTexture& entry : m_Textures)
    {
        if (!entry.texture->GetPath().empty() && SameFile(entry.texture->GetPath(), path))
            return entry.texture.get();
    }

    Texture texture(path);
    if (!texture.IsLoaded())
        return nullptr; // Texture already printed why
    texture.colorSpace = GuessColorSpace(path);
    return AddTexture(path.stem().string(), std::move(texture));
}

void AssetLibrary::ImportFolder(const std::filesystem::path& folder)
{
    std::error_code error;
    std::vector<std::filesystem::path> images;
    for (const auto& item : std::filesystem::directory_iterator(folder, error))
    {
        if (item.is_regular_file() && IsImageFile(item.path()))
            images.push_back(item.path());
    }
    if (error)
    {
        std::cerr << "Can't read texture folder " << folder.string() << ": " << error.message() << '\n';
        return;
    }

    // Directory order isn't guaranteed; sort so the list is stable.
    std::sort(images.begin(), images.end());
    for (const std::filesystem::path& image : images)
        ImportTexture(image);
}

void AssetLibrary::QueueImport(std::filesystem::path path)
{
    std::lock_guard lock(m_QueueMutex);
    m_ImportQueue.push_back(std::move(path));
}

void AssetLibrary::ProcessQueuedImports()
{
    std::vector<std::filesystem::path> queued;
    {
        std::lock_guard lock(m_QueueMutex);
        queued.swap(m_ImportQueue);
    }
    for (const std::filesystem::path& path : queued)
    {
        if (std::filesystem::is_directory(path))
            ImportFolder(path);
        else if (IsImageFile(path))
            ImportTexture(path);
        else
            std::cerr << "Not an image file: " << path.string() << '\n';
    }
}

void AssetLibrary::RemoveTexture(const Texture* texture)
{
    if (!texture)
        return;
    for (const std::unique_ptr<Material>& material : m_Materials)
    {
        for (const Texture** slot : { &material->baseColorMap, &material->metallicMap, &material->roughnessMap,
                                      &material->normalMap, &material->aoMap, &material->emissionMap })
        {
            if (*slot == texture)
                *slot = nullptr;
        }
    }
    std::erase_if(m_Textures, [&](const NamedTexture& entry) { return entry.texture.get() == texture; });
}

Material* AssetLibrary::CreateMaterial(const std::string& name)
{
    auto material = std::make_unique<Material>();
    material->name = MakeUniqueMaterialName(name);
    return m_Materials.emplace_back(std::move(material)).get();
}

Material* AssetLibrary::DuplicateMaterial(const Material& source)
{
    // Blender keeps the base name and bumps the number: "Metal" -> "Metal.001".
    std::string base = source.name;
    const size_t dot = base.rfind('.');
    if (dot != std::string::npos && dot + 4 == base.size()
        && std::all_of(base.begin() + dot + 1, base.end(), [](unsigned char c) { return std::isdigit(c); }))
        base.resize(dot);

    auto material = std::make_unique<Material>(source);
    material->name = MakeUniqueMaterialName(base);
    return m_Materials.emplace_back(std::move(material)).get();
}

void AssetLibrary::RemoveMaterial(const Material* material)
{
    std::erase_if(m_Materials, [&](const std::unique_ptr<Material>& m) { return m.get() == material; });
}

const char* AssetLibrary::GetName(const Mesh* mesh) const
{
    for (const NamedMesh& entry : m_Meshes)
    {
        if (entry.mesh.get() == mesh)
            return entry.name.c_str();
    }
    return "None";
}

const char* AssetLibrary::GetName(const Texture* texture) const
{
    for (const NamedTexture& entry : m_Textures)
    {
        if (entry.texture.get() == texture)
            return entry.name.c_str();
    }
    return "None";
}

const char* AssetLibrary::GetName(const Material* material) const
{
    return material ? material->name.c_str() : "None";
}

void AssetLibrary::RenameTexture(const Texture* texture, const std::string& name)
{
    for (NamedTexture& entry : m_Textures)
    {
        if (entry.texture.get() == texture)
        {
            // Clear the old name first so renaming to the same name is a no-op.
            entry.name.clear();
            entry.name = MakeUniqueTextureName(name.empty() ? "Texture" : name);
            return;
        }
    }
}

std::string AssetLibrary::MakeUniqueTextureName(const std::string& base) const
{
    return MakeUniqueName(base, [&](const std::string& name) {
        return std::any_of(m_Textures.begin(), m_Textures.end(),
                           [&](const NamedTexture& entry) { return entry.name == name; });
    });
}

std::string AssetLibrary::MakeUniqueMaterialName(const std::string& base) const
{
    return MakeUniqueName(base, [&](const std::string& name) {
        return std::any_of(m_Materials.begin(), m_Materials.end(),
                           [&](const std::unique_ptr<Material>& m) { return m->name == name; });
    });
}
