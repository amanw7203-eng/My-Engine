#pragma once

#include <glm/glm.hpp>

#include <string>

class Texture;

// A surface description shared by any number of entities, like a Blender
// material. The inputs follow Blender's Principled BSDF (and glTF): each one
// is a value, optionally multiplied by a texture. Colors are stored as seen
// in the color picker (sRGB); the renderer converts them to linear light.
//
// Texture channels read:  base color RGB, emission RGB, normal RGB,
// AO = R, roughness = G, metallic = B. A greyscale map works in any slot,
// and a packed glTF "ORM" image can go in AO, roughness and metallic at once.
struct Material
{
    std::string name;

    glm::vec3 baseColor{ 0.8f };
    const Texture* baseColorMap = nullptr;

    float metallic = 0.0f;  // 0 = plastic, wood, stone...; 1 = bare metal
    const Texture* metallicMap = nullptr;

    float roughness = 0.5f; // 0 = mirror-smooth, 1 = fully matte
    const Texture* roughnessMap = nullptr;

    const Texture* normalMap = nullptr; // tangent space, OpenGL (Y+) style
    float normalStrength = 1.0f;

    const Texture* aoMap = nullptr;     // ambient occlusion: darkens crevices
    float aoStrength = 1.0f;

    glm::vec3 emissionColor{ 1.0f };
    float emissionStrength = 0.0f;      // 0 = no glow
    const Texture* emissionMap = nullptr;

    // Texture coordinate mapping, applied to every map: uv * tiling + offset.
    glm::vec2 tiling{ 1.0f };
    glm::vec2 offset{ 0.0f };

    bool doubleSided = false; // draw back faces too (for flat quads/planes)

    bool operator==(const Material&) const = default;
};
