#pragma once

class AssetLibrary;
class Texture;
struct Material;

// Editor widgets shared by the Inspector and the Assets panel.

// Drag-and-drop payload types. The payload is the asset's pointer.
inline constexpr const char* kTexturePayload = "TEXTURE"; // const Texture*
inline constexpr const char* kMaterialPayload = "MATERIAL"; // Material*

// A texture's image, `size` pixels square (stretched to fit).
void TextureThumbnail(const Texture& texture, float size);

// A dropdown choosing a texture for one material slot, showing thumbnails;
// a texture dragged from the Assets panel can also be dropped onto it.
// Returns true if the slot changed.
bool TextureSlotPicker(const char* id, const Texture*& slot, const AssetLibrary& assets);

// All of a material's settings, laid out like Blender's Principled BSDF panel.
void DrawMaterialEditor(Material& material, const AssetLibrary& assets);
