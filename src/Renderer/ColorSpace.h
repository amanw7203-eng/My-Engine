#pragma once

#include <glm/glm.hpp>

#include <cmath>

// Color pickers show sRGB (gamma-encoded) values, but lighting math must be
// done on linear light. This converts one to the other (exact sRGB curve).
inline float SrgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline glm::vec3 SrgbToLinear(const glm::vec3& c)
{
    // Values above 1 (HDR colors) follow the same curve.
    return glm::vec3(SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b));
}
