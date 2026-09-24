#pragma once

#include <glm/glm.hpp>

// A light shining equally in every direction from one point, like a bare
// bulb. Attached to an entity, which places it (only its position matters).
struct PointLight
{
    glm::vec3 color{ 1.0f }; // as picked (sRGB)
    // How bright a white surface facing the light looks from 1 unit away (as
    // the sun's color is for the sun). Closer is brighter, further dimmer:
    // with the square of the distance, as real light behaves.
    float intensity = 5.0f;
    // Beyond this distance it lights nothing; it fades out smoothly before
    // it, so there is no visible edge. Smaller is cheaper.
    float range = 10.0f;
    bool castShadows = true; // ray traced view only

    bool operator==(const PointLight&) const = default;
};

// A point light as the renderers use it: in the world, in linear light.
struct ScenePointLight
{
    glm::vec3 position{ 0.0f };
    glm::vec3 radiance{ 0.0f }; // linear color * intensity
    float range = 0.0f;
    bool castShadows = false;

    bool operator==(const ScenePointLight&) const = default;
};
