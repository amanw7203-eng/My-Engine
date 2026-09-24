#version 460 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec3 vColor;
in vec2 vTexCoord;

// --- Material (see Material.h) ---
// Each input is a value times a texture; empty slots get a white texture,
// which leaves the value unchanged. The *Srgb flags say whether that texture
// is marked as a color image (decode from sRGB) or as Non-Color data.
uniform vec3 uBaseColor;        // linear
uniform sampler2D uBaseColorMap;
uniform bool uBaseColorMapSrgb;

uniform float uMetallic;
uniform sampler2D uMetallicMap; // blue channel
uniform bool uMetallicMapSrgb;

uniform float uRoughness;
uniform sampler2D uRoughnessMap; // green channel
uniform bool uRoughnessMapSrgb;

uniform bool uHasNormalMap;
uniform sampler2D uNormalMap;
uniform bool uNormalMapSrgb;
uniform float uNormalStrength;

uniform sampler2D uAoMap;       // red channel
uniform bool uAoMapSrgb;
uniform float uAoStrength;

uniform vec3 uEmission;         // linear color * strength
uniform sampler2D uEmissionMap;
uniform bool uEmissionMapSrgb;

uniform vec2 uUvTiling;
uniform vec2 uUvOffset;

// --- Lights ---
// One directional light (like the sun): every surface sees it coming from
// the same direction. uLightDir points from the light towards the scene.
// Colors are linear.
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform vec3 uAmbientColor;

// Point lights (like bare bulbs): light spreading out from a position,
// dimming with the square of the distance and fading to nothing at its
// range. No shadows in this view. `radiance` is linear color * intensity:
// how bright a white surface facing the light looks from 1 unit away.
struct PointLight
{
    vec3 position;
    vec3 radiance;
    float range;
};
const int MAX_POINT_LIGHTS = 16;
uniform PointLight uPointLights[MAX_POINT_LIGHTS];
uniform int uPointLightCount;

// Camera position in world space, for the view direction.
uniform vec3 uViewPos;

// Shadows. uShadowMap holds, for each texel, the depth of the closest
// surface the light sees there; uLightSpace maps world space onto it.
uniform bool uShadowsEnabled;
uniform sampler2D uShadowMap;
uniform mat4 uLightSpace;
// How far to push the lookup point out along the normal (world units, about
// one shadow texel). Without it a surface can shadow itself in a striped
// pattern ("shadow acne"), because its own depth is stored with limited
// precision.
uniform float uShadowNormalOffset;
// Shadows fade out as they approach this distance from the camera, so
// there is no hard line where the shadow map stops.
uniform float uShadowDistance;

out vec4 FragColor;

// The exact sRGB curve, both ways.
vec3 SrgbToLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 LinearToSrgb(vec3 c)
{
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec3 SampleMap(sampler2D map, bool srgb, vec2 uv)
{
    vec3 c = texture(map, uv).rgb;
    return srgb ? SrgbToLinear(c) : c;
}

// Builds a tangent frame (tangent, bitangent, normal) for normal mapping
// from how the position and UVs change across neighbouring pixels, so the
// meshes don't need to store tangents. (Christian Schüler, "Normal Mapping
// Without Precomputed Tangents".)
mat3 CotangentFrame(vec3 N, vec3 p, vec2 uv)
{
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);

    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    // Scale-invariant: normalize by the larger of the two lengths.
    float invMax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
    return mat3(T * invMax, B * invMax, N);
}

// Light arriving from direction L (surface -> light), reflected towards V,
// per unit of light: diffuse plus a Blinn-Phong highlight, times the cosine
// of the angle the light comes in at. The (n + 8) / 8 factor keeps the total
// reflected light about the same as the highlight spreads out.
vec3 BlinnPhong(vec3 N, vec3 V, vec3 L, vec3 diffuseColor, vec3 specularColor, float shininess)
{
    float NdotL = max(dot(N, L), 0.0);
    vec3 H = normalize(L + V); // halfway vector
    float spec = pow(max(dot(N, H), 0.0), shininess) * (shininess + 8.0) / 8.0;
    return (diffuseColor + specularColor * spec) * NdotL;
}

// 1 at a point light, falling smoothly to 0 at its range, so light doesn't
// stop at a visible edge (as in Unreal: (1 - (d / range)^4)^2).
float RangeFade(float lightDistance, float range)
{
    float x = lightDistance / range;
    float fade = clamp(1.0 - x * x * x * x, 0.0, 1.0);
    return fade * fade;
}

// 1.0 = fully lit, 0.0 = fully in shadow.
float ShadowFactor(vec3 N, float NdotL)
{
    if (!uShadowsEnabled)
        return 1.0;

    // 0 near the camera, rising to 1 over the last 20% of the distance.
    float fade = smoothstep(0.8 * uShadowDistance, uShadowDistance, length(uViewPos - vWorldPos));
    if (fade >= 1.0)
        return 1.0;

    vec4 lightClip = uLightSpace * vec4(vWorldPos + N * uShadowNormalOffset, 1.0);
    // Clip space -1..1 -> texture coordinates and depth 0..1.
    vec3 p = lightClip.xyz / lightClip.w * 0.5 + 0.5;
    // Past the far end of the light's box: nothing there casts a shadow.
    if (p.z > 1.0)
        return 1.0;

    // A small extra bias, larger on surfaces at a grazing angle to the light.
    float bias = mix(0.0015, 0.0003, NdotL);

    // Percentage-closer filtering: test a 3x3 block of texels and average,
    // which softens the stair-stepped shadow edges.
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float lit = 0.0;
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            float closest = texture(uShadowMap, p.xy + vec2(x, y) * texel).r;
            lit += p.z - bias > closest ? 0.0 : 1.0;
        }
    }
    return mix(lit / 9.0, 1.0, fade);
}

void main()
{
    vec2 uv = vTexCoord * uUvTiling + uUvOffset;

    // --- Material inputs ---
    vec3 baseColor = uBaseColor * vColor * SampleMap(uBaseColorMap, uBaseColorMapSrgb, uv);
    float metallic = clamp(uMetallic * SampleMap(uMetallicMap, uMetallicMapSrgb, uv).b, 0.0, 1.0);
    float roughness = clamp(uRoughness * SampleMap(uRoughnessMap, uRoughnessMapSrgb, uv).g, 0.0, 1.0);
    float ao = mix(1.0, SampleMap(uAoMap, uAoMapSrgb, uv).r, uAoStrength);
    vec3 emission = uEmission * SampleMap(uEmissionMap, uEmissionMapSrgb, uv);

    // Interpolation shortens normals between vertices, so re-normalize.
    // Back faces (e.g. the far side of a quad) flip the normal to face us.
    vec3 geometricN = normalize(vNormal);
    if (!gl_FrontFacing)
        geometricN = -geometricN;

    vec3 N = geometricN;
    if (uHasNormalMap)
    {
        // Stored as 0..1 per channel; unpack to a -1..1 direction in the
        // surface's tangent frame. Strength scales the sideways tilt.
        vec3 tangentN = SampleMap(uNormalMap, uNormalMapSrgb, uv) * 2.0 - 1.0;
        tangentN.xy *= uNormalStrength;
        N = normalize(CotangentFrame(geometricN, vWorldPos, uv) * tangentN);
    }

    vec3 L = normalize(-uLightDir);             // surface -> light
    vec3 V = normalize(uViewPos - vWorldPos);   // surface -> camera
    float NdotL = max(dot(N, L), 0.0);

    // --- Shading (Blinn-Phong driven by the PBR inputs, until PBR lands) ---
    // Metals have no diffuse color; their reflections are tinted by the base
    // color instead. Non-metals reflect about 4% at head-on angles.
    vec3 diffuseColor = baseColor * (1.0 - metallic);
    vec3 specularColor = mix(vec3(0.04), baseColor, metallic);

    // Rougher = wider, dimmer highlight.
    float alpha = max(roughness * roughness, 0.002);
    float shininess = min(2.0 / (alpha * alpha) - 2.0, 4096.0);

    vec3 direct = BlinnPhong(N, V, L, diffuseColor, specularColor, shininess) * uLightColor;

    // Point lights: the same shading, with light that weakens with distance.
    vec3 pointDirect = vec3(0.0);
    for (int i = 0; i < min(uPointLightCount, MAX_POINT_LIGHTS); ++i)
    {
        vec3 toLight = uPointLights[i].position - vWorldPos;
        float distanceSquared = dot(toLight, toLight);
        float range = uPointLights[i].range;
        if (distanceSquared >= range * range)
            continue;
        float lightDistance = sqrt(distanceSquared);
        // Kept a little away from 0 so a surface touching the light isn't
        // infinitely bright.
        vec3 irradiance = uPointLights[i].radiance * RangeFade(lightDistance, range) / max(distanceSquared, 0.01);
        pointDirect += BlinnPhong(N, V, toLight / lightDistance, diffuseColor, specularColor, shininess) * irradiance;
    }

    // Shadow blocks the direct light only; ambient still fills it in. The
    // shadow lookup uses the real surface, not the normal-mapped one.
    float shadow = NdotL > 0.0 ? ShadowFactor(geometricN, max(dot(geometricN, L), 0.0)) : 1.0;

    // Ambient: a flat fill so faces turned away from the light aren't black,
    // darkened in crevices by the AO map.
    vec3 ambient = uAmbientColor * (diffuseColor + specularColor) * ao;

    vec3 color = ambient + shadow * direct + pointDirect + emission;

    // Lighting is computed in linear light; the screen expects sRGB.
    FragColor = vec4(LinearToSrgb(max(color, 0.0)), 1.0);
}
