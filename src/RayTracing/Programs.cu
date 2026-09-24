// The OptiX programs: what runs on the GPU for each ray. Compiled by nvcc to
// OptiX-IR (see CMakeLists.txt) and loaded at runtime by RayTracer.
//
//   __raygen__trace        one thread per pixel: follows light paths from the
//                          camera into the scene, shading each surface they
//                          hit, and blends the light with earlier frames'
//   __raygen__downsample   one thread per 2x2 pixels: shrinks the denoiser's
//                          inputs to half resolution
//   __raygen__present      one thread per pixel: recombines direct and
//                          (denoised) indirect light -> sRGB bytes on screen
//   __closesthit__surface  a path ray hit a triangle: report the surface
//                          (position, normals, material) back to raygen
//   __miss__surface        a path ray hit nothing
//   __miss__shadow         a shadow ray hit nothing: the point is lit
//
// Lighting is metallic-roughness PBR: direct light from the sun (with a
// shadow ray), plus ambient light that bounces between surfaces. The bounce
// directions are random, so one frame's bounced light is noisy: each pixel
// reuses what earlier frames found for the same surface (following it as
// the camera and objects move), and the OptiX denoiser cleans up the noise
// that is left.

#include <optix.h>

#include "RayTracing/DeviceTypes.h"

// Filled in by optixLaunch from the buffer RayTracer uploads. The name must
// match pipelineLaunchParamsVariableName in RayTracer.cpp.
extern "C" {
__constant__ LaunchParams params;
}

// --- float3 math (CUDA provides the types but no operators) ----------------

static __forceinline__ __device__ float3 operator+(float3 a, float3 b) { return make_float3(a.x + b.x, a.y + b.y, a.z + b.z); }
static __forceinline__ __device__ float3 operator-(float3 a, float3 b) { return make_float3(a.x - b.x, a.y - b.y, a.z - b.z); }
static __forceinline__ __device__ float3 operator-(float3 a) { return make_float3(-a.x, -a.y, -a.z); }
static __forceinline__ __device__ float3 operator*(float3 a, float3 b) { return make_float3(a.x * b.x, a.y * b.y, a.z * b.z); }
static __forceinline__ __device__ float3 operator*(float3 a, float s) { return make_float3(a.x * s, a.y * s, a.z * s); }
static __forceinline__ __device__ float3 operator/(float3 a, float s) { return a * (1.0f / s); }
static __forceinline__ __device__ float3& operator+=(float3& a, float3 b) { return a = a + b; }
static __forceinline__ __device__ float3& operator*=(float3& a, float3 b) { return a = a * b; }

static __forceinline__ __device__ float Dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static __forceinline__ __device__ float Length(float3 v) { return sqrtf(Dot(v, v)); }
static __forceinline__ __device__ float3 Normalize(float3 v) { return v * rsqrtf(Dot(v, v)); }

static __forceinline__ __device__ float3 Cross(float3 a, float3 b)
{
    return make_float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static __forceinline__ __device__ float3 Mix(float3 a, float3 b, float t) { return a + (b - a) * t; }
static __forceinline__ __device__ float3 Splat(float s) { return make_float3(s, s, s); }

// --- PBR: metallic-roughness Cook-Torrance (as in glTF, Unreal, Blender's
// Principled BSDF). A surface is modelled as tiny mirror facets; roughness
// controls how scattered their directions are. ------------------------------

constexpr float kPi = 3.14159265f;

// How fast a ray cone widens after a diffuse bounce (radians): a rough
// stand-in for the whole hemisphere a diffuse surface scatters into.
constexpr float kDiffuseConeSpread = 0.3f;

// GGX / Trowbridge-Reitz normal distribution: how many facets are angled
// so that they reflect the light straight at the camera (face H).
static __forceinline__ __device__ float DistributionGgx(float NdotH, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * d * d);
}

// Height-correlated Smith visibility: the share of those facets that are not
// hidden from the light or the camera by other facets. Already divided by
// the Cook-Torrance 4 * NdotL * NdotV.
static __forceinline__ __device__ float VisibilitySmithGgx(float NdotL, float NdotV, float alpha)
{
    const float a2 = alpha * alpha;
    const float viewTerm = NdotL * sqrtf(NdotV * NdotV * (1.0f - a2) + a2);
    const float lightTerm = NdotV * sqrtf(NdotL * NdotL * (1.0f - a2) + a2);
    return 0.5f / (viewTerm + lightTerm);
}

// Schlick's Fresnel: surfaces reflect more at grazing angles, rising to
// white at 90 degrees whatever their color head-on (F0).
static __forceinline__ __device__ float3 FresnelSchlick(float cosTheta, float3 F0)
{
    const float f = powf(1.0f - cosTheta, 5.0f);
    return F0 + (Splat(1.0f) - F0) * f;
}

// How much of a uniform surrounding light a surface reflects specularly,
// averaged over all directions: F0 * scale + bias. A curve fit of the
// split-sum lookup table (Karis, "Physically Based Shading on Mobile").
static __forceinline__ __device__ float3 EnvironmentBrdf(float3 F0, float roughness, float NdotV)
{
    const float4 c0 = make_float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = make_float4(1.0f, 0.0425f, 1.04f, -0.04f);
    const float4 r = make_float4(roughness * c0.x + c1.x, roughness * c0.y + c1.y,
                                 roughness * c0.z + c1.z, roughness * c0.w + c1.w);
    const float a004 = fminf(r.x * r.x, exp2f(-9.28f * NdotV)) * r.x + r.y;
    const float scale = -1.04f * a004 + r.z;
    const float bias = 1.04f * a004 + r.w;
    return F0 * scale + Splat(bias);
}

// Light arriving from direction L (surface -> light) and reflected towards
// V, per unit of light: the Cook-Torrance BRDF times the cosine of the
// angle the light comes in at.
//
// A light's color is how bright a white surface facing it looks (for a
// point light, from 1 unit away), as in lit.frag, so both renderers match
// under the same settings. With the 1/pi in the diffuse BRDF, that means an
// irradiance of pi * color, hence the pi here.
static __forceinline__ __device__ float3 ReflectedLight(float3 N, float3 V, float NdotV, float3 L, float3 diffuseColor,
                                                        float3 F0, float alpha)
{
    const float NdotL = fmaxf(Dot(N, L), 0.0f);
    const float3 H = Normalize(L + V);
    const float NdotH = fmaxf(Dot(N, H), 0.0f);
    const float VdotH = fmaxf(Dot(V, H), 0.0f);

    const float3 F = FresnelSchlick(VdotH, F0);
    const float3 specular = F * (DistributionGgx(NdotH, alpha) * VisibilitySmithGgx(NdotL, NdotV, alpha));
    // Light reflected at the surface (F) never gets in to scatter diffusely.
    const float3 diffuse = (Splat(1.0f) - F) * diffuseColor / kPi;
    return (diffuse + specular) * (kPi * NdotL);
}

// 1 at a point light, falling smoothly to 0 at its range, so light doesn't
// stop at a visible edge (as in lit.frag and Unreal: (1 - (d / range)^4)^2).
static __forceinline__ __device__ float RangeFade(float distance, float range)
{
    const float x = distance / range;
    const float fade = fminf(fmaxf(1.0f - x * x * x * x, 0.0f), 1.0f);
    return fade * fade;
}

// --- Random numbers ---------------------------------------------------------

// PCG hash (Jarzynski & Olano, "Hash Functions for GPU Rendering"): a
// well-mixed 32-bit value from any input, so neighbouring pixels and frames
// get unrelated random sequences.
static __forceinline__ __device__ unsigned int PcgHash(unsigned int v)
{
    const unsigned int state = v * 747796405u + 2891336453u;
    const unsigned int word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Uniform in [0, 1), advancing `seed`.
static __forceinline__ __device__ float Random(unsigned int& seed)
{
    seed = PcgHash(seed);
    return (seed >> 8) * (1.0f / 16777216.0f); // top 24 bits: exact in a float
}

// A random direction on the hemisphere around `n`, more likely near `n`
// in proportion to cos(angle) (cosine-weighted, Malley's method). That is
// exactly how a diffuse surface scatters light, so each bounce ray carries
// equal weight and no cosine / pdf factors need applying.
// Two axes perpendicular to unit vector n, and to each other (Duff et al.,
// "Building an Orthonormal Basis, Revisited").
static __forceinline__ __device__ void PerpendicularAxes(float3 n, float3& tangent, float3& bitangent)
{
    const float sign = copysignf(1.0f, n.z);
    const float a = -1.0f / (sign + n.z);
    const float b = n.x * n.y * a;
    tangent = make_float3(1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x);
    bitangent = make_float3(b, sign + n.y * n.y * a, -n.y);
}

static __forceinline__ __device__ float3 CosineSampleHemisphere(float3 n, unsigned int& seed)
{
    // Uniform point on a disk, projected up onto the hemisphere.
    const float r = sqrtf(Random(seed));
    const float phi = 2.0f * kPi * Random(seed);
    const float x = r * cosf(phi);
    const float y = r * sinf(phi);
    const float z = sqrtf(fmaxf(1.0f - x * x - y * y, 0.0f));

    float3 tangent, bitangent;
    PerpendicularAxes(n, tangent, bitangent);
    return tangent * x + bitangent * y + n * z;
}

// A random microfacet normal for a glossy reflection off surface normal N,
// seen from direction V, picked the way GGX spreads them but only among the
// facets V can actually see (Heitz, "Sampling the GGX Distribution of
// Visible Normals", 2018). Reflecting V about it gives a direction spread
// as the material scatters light: tight when smooth, wide when rough.
static __forceinline__ __device__ float3 SampleGgxVisibleNormal(float3 N, float3 V, float alpha, unsigned int& seed)
{
    // V in the surface's frame (N = +Z).
    float3 tangent, bitangent;
    PerpendicularAxes(N, tangent, bitangent);
    const float3 v = make_float3(Dot(V, tangent), Dot(V, bitangent), Dot(V, N));

    // Stretch the view into the space where the facets form a hemisphere...
    const float3 vh = Normalize(make_float3(alpha * v.x, alpha * v.y, v.z));
    const float lengthSquared = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lengthSquared > 0.0f ? make_float3(-vh.y, vh.x, 0.0f) * rsqrtf(lengthSquared)
                                           : make_float3(1.0f, 0.0f, 0.0f);
    const float3 t2 = Cross(vh, t1);
    // ...pick a point on the part of it facing the view...
    const float r = sqrtf(Random(seed));
    const float phi = 2.0f * kPi * Random(seed);
    const float p1 = r * cosf(phi);
    float p2 = r * sinf(phi);
    const float s = 0.5f * (1.0f + vh.z);
    p2 = (1.0f - s) * sqrtf(fmaxf(1.0f - p1 * p1, 0.0f)) + s * p2;
    const float3 nh = t1 * p1 + t2 * p2 + vh * sqrtf(fmaxf(1.0f - p1 * p1 - p2 * p2, 0.0f));
    // ...and un-stretch it back into a facet normal.
    const float3 m = Normalize(make_float3(alpha * nh.x, alpha * nh.y, fmaxf(nh.z, 0.0f)));

    return tangent * m.x + bitangent * m.y + N * m.z;
}

// How bright a color looks (Rec. 709 weights).
static __forceinline__ __device__ float Luminance(float3 c)
{
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

// --- Helpers ----------------------------------------------------------------

// The exact sRGB curve, as in lit.frag.
static __forceinline__ __device__ float LinearToSrgb(float c)
{
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static __forceinline__ __device__ unsigned char ToByte(float c)
{
    return static_cast<unsigned char>(fminf(fmaxf(c, 0.0f), 1.0f) * 255.0f + 0.5f);
}

// Clip-space point -> world space, through the inverse view-projection.
static __forceinline__ __device__ float3 Unproject(float x, float y, float z)
{
    const float* m = params.inverseViewProjection;
    float out[4];
    for (int row = 0; row < 4; ++row)
        out[row] = m[row * 4 + 0] * x + m[row * 4 + 1] * y + m[row * 4 + 2] * z + m[row * 4 + 3];
    return make_float3(out[0], out[1], out[2]) / out[3];
}

// What a path ray found. Raygen passes a pointer to one of these through
// the ray's two payload registers; the closest-hit program fills it in.
struct SurfaceHit
{
    // Filled in by the caller: the ray as a cone, `coneWidth` wide at its
    // origin and widening by `coneSpread` per unit of distance. How wide it
    // is where it hits decides how blurry a texture mip level to read.
    float coneWidth;
    float coneSpread;

    // Filled in by the closest-hit program.
    bool hit;
    float distance;          // along the ray
    float3 position;
    float3 previousPosition; // where that point of the surface was last frame
    float3 geometricNormal;  // the flat triangle's, facing the side the ray came from
    float3 normal;           // for shading (interpolated, normal-mapped); same side as geometricNormal
    float3 baseColor;        // linear: material color * vertex color * base color map
    float3 emission;
    float metallic;
    float roughness;
    float ao;                // 1 = unoccluded, from the AO map
};

static __forceinline__ __device__ SurfaceHit* GetSurfaceHit()
{
    const unsigned long long pointer =
        static_cast<unsigned long long>(optixGetPayload_0()) << 32 | optixGetPayload_1();
    return reinterpret_cast<SurfaceHit*>(pointer);
}

// Traces a path ray, returning what it hit (hit == false if nothing).
static __forceinline__ __device__ SurfaceHit TraceSurface(float3 origin, float3 direction, float tMax,
                                                          unsigned int rayFlags, float coneWidth, float coneSpread)
{
    SurfaceHit surface;
    surface.hit = false;
    surface.coneWidth = coneWidth;
    surface.coneSpread = coneSpread;
    const unsigned long long pointer = reinterpret_cast<unsigned long long>(&surface);
    unsigned int high = static_cast<unsigned int>(pointer >> 32);
    unsigned int low = static_cast<unsigned int>(pointer);
    optixTrace(params.scene, origin, direction, 0.0f, tMax, 0.0f,
               OptixVisibilityMask(0xFF), rayFlags,
               0, 1, RAY_TYPE_SURFACE, // SBT offset, SBT stride, miss program
               high, low);
    return surface;
}

// Is there a clear line from `origin` towards `direction`, for `distance`
// (to infinity by default)? Any hit at all answers the question, so the
// trace stops at the first one and skips the closest-hit program.
static __forceinline__ __device__ bool IsUnblocked(float3 origin, float3 direction, float distance = 1e16f)
{
    unsigned int visible = 0;
    optixTrace(params.scene, origin, direction, 0.0f, distance, 0.0f,
               OptixVisibilityMask(0xFF),
               OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT | OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT,
               0, 1, RAY_TYPE_SHADOW,
               visible);
    return visible != 0;
}

// Surface point -> slightly off it, so rays leaving it can't hit the
// triangle they start on.
static __forceinline__ __device__ float3 OffsetFromSurface(const SurfaceHit& s)
{
    return s.position + s.geometricNormal * 1e-3f;
}

// The light arriving along one path, in the parts LaunchParams describes.
// The indirect parts are demodulated: stored as if the first surface
// reflected everything, then multiplied by its albedo after denoising.
struct PathLight
{
    float3 direct;
    float3 diffuse;        // light scattered in diffusely at the first surface
    float3 specular;       // light reflected (mirror-like to glossy) off it
    float3 diffuseAlbedo;  // the first surface's: multiply `diffuse` by it
    float3 specularAlbedo; // and `specular` by this
    float roughness;       // the first surface's
};

// One light path through the pixel point (x, y) (in normalized device
// coordinates): the linear light seen along it.
//
// At each surface the path gathers the direct light (sun and point lights,
// by shadow rays), then carries on in one new direction, picked at random
// in proportion to where the surface sends light: a reflection (sampled
// from the GGX lobe, so as sharp or blurred as the roughness makes it) or a
// diffuse bounce. Averaged over many paths that is exactly the surface's
// full PBR response to everything around it, for one ray per bounce.
static __device__ PathLight TracePath(float x, float y, unsigned int& seed)
{
    // The camera ray goes from the near plane to the far plane, so it sees
    // exactly what the raster camera would, clipping included.
    const float3 nearPoint = Unproject(x, y, -1.0f);
    const float3 farPoint = Unproject(x, y, 1.0f);
    const float3 toFar = farPoint - nearPoint;
    const float rayLength = Length(toFar);

    float3 origin = nearPoint;
    float3 direction = toFar / rayLength;
    float tMax = rayLength;
    // Back-face culling is a camera setting: bounce rays see every face.
    unsigned int rayFlags = params.cullBackFaces ? OPTIX_RAY_FLAG_CULL_BACK_FACING_TRIANGLES : OPTIX_RAY_FLAG_NONE;
    // The camera ray starts as a point and widens by one pixel's angle.
    float coneWidth = 0.0f;
    float coneSpread = params.pixelSpreadAngle;

    const float3 L = Normalize(-params.lightDir); // surface -> sun

    PathLight light;
    light.direct = Splat(0.0f);
    light.diffuse = Splat(0.0f);
    light.specular = Splat(0.0f);
    light.diffuseAlbedo = Splat(0.0f);
    light.specularAlbedo = Splat(0.0f);
    light.roughness = 1.0f;

    // Light at the first surface counts as direct; what comes back along
    // the first bounce goes to `diffuse` or `specular`, by which lobe it took.
    float3* out = &light.direct;
    // How much of the light arriving at the current surface reaches the
    // camera: each bounce multiplies in how much the surface it left sends
    // on, except the first, whose albedo is divided out (demodulation).
    float3 throughput = Splat(1.0f);

    for (unsigned int bounce = 0;; ++bounce)
    {
        const SurfaceHit s = TraceSurface(origin, direction, tMax, rayFlags, coneWidth, coneSpread);
        if (!s.hit)
        {
            // The camera sees the background; a bounce ray escaping into
            // the open receives light from the sky.
            *out += throughput * (bounce == 0 ? params.background : params.ambientColor);
            break;
        }

        // --- Material inputs ---
        const float metallic = fminf(fmaxf(s.metallic, 0.0f), 1.0f);
        const float roughness = fminf(fmaxf(s.roughness, 0.0f), 1.0f);
        // Metals have no diffuse color; their reflections are tinted by the
        // base color instead. Non-metals reflect about 4% at head-on angles.
        const float3 diffuseColor = s.baseColor * (1.0f - metallic);
        const float3 F0 = Mix(Splat(0.04f), s.baseColor, metallic);
        // Perceptually linear roughness -> GGX alpha. Kept above 0 so a
        // perfectly smooth surface still has a (tiny) highlight from the sun.
        const float alpha = fmaxf(roughness * roughness, 0.002f);

        const float3 N = s.normal;
        const float3 V = -direction; // surface -> previous point on the path
        // Interpolated normals can tilt slightly away from the viewer near a
        // silhouette; keep it just above 0 so the visibility term stays finite.
        const float NdotV = fmaxf(Dot(N, V), 1e-4f);

        *out += throughput * s.emission;

        // --- Direct light from the sun ---
        const float NdotL = Dot(N, L);
        if (NdotL > 0.0f && (!params.shadowsEnabled ||
                             (Dot(s.geometricNormal, L) > 0.0f && IsUnblocked(OffsetFromSurface(s), L))))
        {
            *out += throughput * ReflectedLight(N, V, NdotV, L, diffuseColor, F0, alpha) * params.lightColor;
        }

        // --- Direct light from point lights ---
        for (unsigned int i = 0; i < params.pointLightCount; ++i)
        {
            const DevicePointLight& pointLight = params.pointLights[i];
            const float3 toLight = pointLight.position - s.position;
            const float distanceSquared = Dot(toLight, toLight);
            if (distanceSquared >= pointLight.range * pointLight.range)
                continue;
            const float distance = sqrtf(distanceSquared);
            const float3 toLightDir = toLight / distance;
            if (Dot(N, toLightDir) <= 0.0f)
                continue;
            // The light is behind the triangle itself, or something is in
            // the way (the shadow ray stops just short of the light).
            if (params.shadowsEnabled && pointLight.castShadows &&
                (Dot(s.geometricNormal, toLightDir) <= 0.0f ||
                 !IsUnblocked(OffsetFromSurface(s), toLightDir, distance - 1e-3f)))
                continue;

            // Weaker with the square of the distance (kept a little away
            // from 0 so a surface touching the light isn't infinitely
            // bright), fading to nothing at the range, as in lit.frag.
            const float3 irradiance =
                pointLight.radiance * (RangeFade(distance, pointLight.range) / fmaxf(distanceSquared, 0.01f));
            *out += throughput * ReflectedLight(N, V, NdotV, toLightDir, diffuseColor, F0, alpha) * irradiance;
        }

        // --- Light from everything else ---
        // How much of the light around it the surface reflects specularly
        // (the environment BRDF), and how much gets in and scatters
        // diffusely. The AO map darkens both, as in lit.frag: it stands for
        // crevices too small to be in the geometry, which rays can't find.
        const float3 environmentBrdf = EnvironmentBrdf(F0, roughness, NdotV);
        const float3 specularWeight = environmentBrdf * s.ao;
        const float3 diffuseWeight = diffuseColor * (Splat(1.0f) - environmentBrdf) * s.ao;

        if (bounce == 0)
        {
            light.diffuseAlbedo = diffuseWeight;
            light.specularAlbedo = specularWeight;
            light.roughness = roughness;
        }

        // Out of bounces: assume open sky all round, like the raster path.
        // Without reflections, specular always does, as it did before.
        if (!params.reflections)
            *out += throughput * params.ambientColor * specularWeight;
        if (bounce >= params.maxBounces)
        {
            if (params.reflections)
                *out += throughput * params.ambientColor * specularWeight;
            *out += throughput * params.ambientColor * diffuseWeight;
            break;
        }

        // Otherwise follow the light back one more step, along a direction
        // picked from one of the two lobes, chosen in proportion to how much
        // light each sends on (so a metal always reflects, a matte surface
        // mostly scatters). Dividing by the chance of the choice keeps the
        // average right.
        const float specularShare = Luminance(specularWeight);
        const float diffuseShare = Luminance(diffuseWeight);
        float specularChance = 0.0f;
        if (params.reflections && specularShare > 0.0f)
            specularChance = diffuseShare > 0.0f ? fminf(fmaxf(specularShare / (specularShare + diffuseShare), 0.1f), 0.9f)
                                                 : 1.0f;
        const bool reflect = Random(seed) < specularChance;

        float3 nextDirection;
        float3 weight; // light arriving along nextDirection -> light leaving towards V
        if (reflect)
        {
            // Mirror V about a facet normal picked from the visible GGX
            // facets. For that sampling the BRDF * cosine / probability comes
            // out as Fresnel times the share of facets the light isn't
            // blocked from: F * G2 / G1.
            const float3 H = SampleGgxVisibleNormal(N, V, alpha, seed);
            nextDirection = H * (2.0f * Dot(V, H)) - V;
            const float NdotNext = Dot(N, nextDirection);
            if (NdotNext <= 0.0f || Dot(nextDirection, s.geometricNormal) <= 0.0f)
                break; // reflected into the surface: no light comes from there
            const float a2 = alpha * alpha;
            const float g1View = 2.0f * NdotV / (NdotV + sqrtf(a2 + (1.0f - a2) * NdotV * NdotV));
            const float g2 = VisibilitySmithGgx(NdotNext, NdotV, alpha) * 4.0f * NdotNext * NdotV;
            weight = FresnelSchlick(fmaxf(Dot(V, H), 0.0f), F0) * (g2 / g1View) * s.ao / specularChance;
        }
        else
        {
            // Pick a direction the way a diffuse surface scatters light.
            nextDirection = CosineSampleHemisphere(N, seed);
            // With a tilted shading normal, a direction can point into the
            // surface itself; that light can't arrive, so the path ends.
            if (Dot(nextDirection, s.geometricNormal) <= 0.0f)
                break;
            weight = diffuseWeight / (1.0f - specularChance);
        }

        if (bounce == 0)
        {
            // Demodulate: divide the first surface's albedo back out.
            out = reflect ? &light.specular : &light.diffuse;
            const float3 albedo = reflect ? specularWeight : diffuseWeight;
            throughput = make_float3(albedo.x > 1e-4f ? weight.x / albedo.x : 0.0f,
                                     albedo.y > 1e-4f ? weight.y / albedo.y : 0.0f,
                                     albedo.z > 1e-4f ? weight.z / albedo.z : 0.0f);
        }
        else
        {
            throughput *= weight;
        }

        origin = OffsetFromSurface(s);
        direction = nextDirection;
        tMax = 1e16f;
        rayFlags = OPTIX_RAY_FLAG_NONE;
        // The cone widens with the lobe it took: a diffuse bounce scatters
        // widely, a reflection about as much as the surface is rough. Later
        // hits then read texture mip levels matching how blurry they appear.
        coneWidth += s.distance * coneSpread;
        coneSpread = reflect ? coneSpread + alpha : kDiffuseConeSpread;
    }

    return light;
}

// --- Programs ---------------------------------------------------------------

static __forceinline__ __device__ float3 Xyz(float4 v) { return make_float3(v.x, v.y, v.z); }
static __forceinline__ __device__ float4 WithW(float3 v, float w) { return make_float4(v.x, v.y, v.z, w); }

static __forceinline__ __device__ bool IsFinite(float3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

// The most a single frame's bounced or reflected light (demodulated, so as
// seen on a white surface) may be: a few times as bright as the sun lights
// a white surface.
constexpr float kMaxIndirectBrightness = 8.0f;

// `c` scaled down, keeping its hue, if it is brighter than `maximum`.
static __forceinline__ __device__ float3 ClampBrightness(float3 c, float maximum)
{
    const float brightness = Luminance(c);
    return brightness > maximum ? c * (maximum / brightness) : c;
}

// Where world point `p` lands on screen through `viewProjection`, in pixels
// (continuous: pixel (i, j) covers i..i+1, j..j+1). False if it is behind
// the camera.
static __forceinline__ __device__ bool ToPixel(const float* viewProjection, float3 p, float2& pixel)
{
    const float* m = viewProjection;
    const float x = m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3];
    const float y = m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7];
    const float w = m[12] * p.x + m[13] * p.y + m[14] * p.z + m[15];
    if (w <= 1e-6f)
        return false;
    pixel = make_float2((x / w * 0.5f + 0.5f) * params.width, (y / w * 0.5f + 0.5f) * params.height);
    return true;
}

// What the ray through a pixel's centre hits: the same every frame while
// nothing moves, so it decides reliably whether history can be reused.
struct PixelSurface
{
    bool hit;
    float3 position;
    float3 previousPosition; // where that point of the surface was last frame
    float3 normal;
    float3 farPoint;         // along the ray, at the far plane (for the background)
};

static __device__ PixelSurface TracePixelCentre(const uint3& pixel)
{
    const float x = (pixel.x + 0.5f) / params.width * 2.0f - 1.0f;
    const float y = (pixel.y + 0.5f) / params.height * 2.0f - 1.0f;
    const float3 nearPoint = Unproject(x, y, -1.0f);
    const float3 farPoint = Unproject(x, y, 1.0f);
    const float3 toFar = farPoint - nearPoint;
    const float rayLength = Length(toFar);

    PixelSurface p;
    p.hit = false;
    p.farPoint = farPoint;
    if (!params.scene)
        return p;

    const SurfaceHit s = TraceSurface(nearPoint, toFar / rayLength, rayLength,
                                      params.cullBackFaces ? OPTIX_RAY_FLAG_CULL_BACK_FACING_TRIANGLES
                                                           : OPTIX_RAY_FLAG_NONE,
                                      0.0f, params.pixelSpreadAngle);
    p.hit = s.hit;
    p.position = s.position;
    p.previousPosition = s.previousPosition;
    p.normal = s.normal;
    return p;
}

// What earlier frames found for a pixel: the running averages it continues.
struct History
{
    float3 direct;
    float frames; // how many frames the averages hold
    float3 indirect;
    float3 albedo;
    float3 specular;
    float3 specularAlbedo;
    float roughness;
};

static __forceinline__ __device__ History ReadHistory(unsigned int index)
{
    const float4 direct = params.directIn[index];
    const float4 specularAlbedo = params.specularAlbedoIn[index];
    History h;
    h.direct = Xyz(direct);
    h.frames = direct.w;
    h.indirect = Xyz(params.indirectIn[index]);
    h.albedo = Xyz(params.albedoIn[index]);
    h.specular = Xyz(params.specularIn[index]);
    h.specularAlbedo = Xyz(specularAlbedo);
    h.roughness = specularAlbedo.w;
    return h;
}

// Last frame's history for the surface this pixel now shows, found by
// reprojection when something moved. `previousPixel` is where the pixel's
// centre was on screen last frame; the four stored pixels around it are
// blended (bilinear), but only those that saw the same thing: for a surface,
// a matching normal and a position on the same plane as where this surface
// point was; for the background, the background. A pixel whose surface just
// came into view (from behind an object, or the screen edge) finds none and
// starts over. Returns false if no history is usable.
static __device__ bool FetchReprojectedHistory(float2 previousPixel, const PixelSurface& now, History& history)
{
    // Offset by half a pixel: stored values sit at pixel centres.
    const float fx = previousPixel.x - 0.5f;
    const float fy = previousPixel.y - 0.5f;
    const int x0 = static_cast<int>(floorf(fx));
    const int y0 = static_cast<int>(floorf(fy));
    const float tx = fx - x0;
    const float ty = fy - y0;

    // How far off the plane a match may be: 2% of the distance from the
    // camera, since depth precision (and the size of a pixel) grows with it.
    const float tolerance = now.hit ? 0.02f * Length(now.position - params.cameraPosition) : 0.0f;

    History sum = {};
    float weightSum = 0.0f;
    for (int j = 0; j < 2; ++j)
    {
        for (int i = 0; i < 2; ++i)
        {
            const int x = x0 + i;
            const int y = y0 + j;
            const float weight = (i ? tx : 1.0f - tx) * (j ? ty : 1.0f - ty);
            if (weight <= 0.0f || x < 0 || y < 0 || x >= static_cast<int>(params.width) ||
                y >= static_cast<int>(params.height))
                continue;

            const unsigned int index = y * params.width + x;
            const float4 position = params.positionIn[index];
            const bool storedHit = position.w != 0.0f;
            if (storedHit != now.hit)
                continue;
            if (now.hit)
            {
                // An object turning a few degrees a frame still passes.
                if (Dot(Xyz(params.normalIn[index]), now.normal) < 0.9f)
                    continue;
                if (fabsf(Dot(Xyz(position) - now.previousPosition, now.normal)) > tolerance)
                    continue;
            }

            const History h = ReadHistory(index);
            sum.direct += h.direct * weight;
            sum.frames += h.frames * weight;
            sum.indirect += h.indirect * weight;
            sum.albedo += h.albedo * weight;
            sum.specular += h.specular * weight;
            sum.specularAlbedo += h.specularAlbedo * weight;
            sum.roughness += h.roughness * weight;
            weightSum += weight;
        }
    }

    // Too little of the neighbourhood matched to trust (an edge).
    if (weightSum < 0.1f)
        return false;
    history.direct = sum.direct / weightSum;
    history.frames = sum.frames / weightSum;
    history.indirect = sum.indirect / weightSum;
    history.albedo = sum.albedo / weightSum;
    history.specular = sum.specular / weightSum;
    history.specularAlbedo = sum.specularAlbedo / weightSum;
    history.roughness = sum.roughness / weightSum;
    return true;
}

// Renders this frame: traces the light paths for each pixel, then blends
// the result into what earlier frames found for the same surface. A still
// view averages ever more frames and converges; while anything moves, a
// shorter history (params.historyLimit) adapts quickly.
extern "C" __global__ void __raygen__trace()
{
    const uint3 pixel = optixGetLaunchIndex();
    const unsigned int pixelIndex = pixel.y * params.width + pixel.x;

    // A different random sequence for every pixel, frame and sample.
    unsigned int seed = PcgHash(pixelIndex ^ PcgHash(params.randomSeed));

    PathLight sum = {};
    for (unsigned int sample = 0; sample < params.samplesPerPixel; ++sample)
    {
        // A random point inside the pixel rather than its centre: averaged
        // over frames, that smooths jagged edges for free (anti-aliasing).
        // Launch row 0 is the bottom row, like the GL texture it ends up in.
        const float x = (pixel.x + Random(seed)) / params.width * 2.0f - 1.0f;
        const float y = (pixel.y + Random(seed)) / params.height * 2.0f - 1.0f;

        PathLight light = {};
        if (params.scene)
            light = TracePath(x, y, seed);
        else
            light.direct = params.background;
        sum.direct += light.direct;
        sum.diffuse += light.diffuse;
        sum.specular += light.specular;
        sum.diffuseAlbedo += light.diffuseAlbedo;
        sum.specularAlbedo += light.specularAlbedo;
        sum.roughness += light.roughness;
    }
    const float samples = static_cast<float>(params.samplesPerPixel);
    float3 direct = sum.direct / samples;
    float3 indirect = sum.diffuse / samples;
    float3 specular = sum.specular / samples;
    const float3 albedo = sum.diffuseAlbedo / samples;
    const float3 specularAlbedo = sum.specularAlbedo / samples;
    const float roughness = sum.roughness / samples;

    // A NaN or infinity (from degenerate geometry) would poison every
    // later frame's average, so drop that sample.
    if (!IsFinite(direct))
        direct = Splat(0.0f);
    if (!IsFinite(indirect))
        indirect = Splat(0.0f);
    if (!IsFinite(specular))
        specular = Splat(0.0f);
    // Fireflies: once in a while a random path finds a very bright route
    // (e.g. a glossy reflection of a sunlit spot) and one pixel flares for a
    // frame. Capping the bounced light's brightness trades a little energy
    // in those rare paths for a steady image.
    indirect = ClampBrightness(indirect, kMaxIndirectBrightness);
    specular = ClampBrightness(specular, kMaxIndirectBrightness);

    const PixelSurface centre = TracePixelCentre(pixel);

    // --- Temporal reuse ---
    // Continue earlier frames' running averages for what this pixel shows.
    History history;
    bool reuse = false;
    float2 motion = make_float2(0.0f, 0.0f); // pixels moved since last frame
    if (params.historyValid && params.nothingMoved)
    {
        // Same view of the same scene as last frame: the pixel's history is
        // its own.
        history = ReadHistory(pixelIndex);
        reuse = true;
    }
    else if (params.historyValid)
    {
        // Follow the surface (or, for the background, the view direction)
        // to where it was on screen last frame.
        float2 now, before;
        if (ToPixel(params.viewProjection, centre.hit ? centre.position : centre.farPoint, now) &&
            ToPixel(params.previousViewProjection, centre.hit ? centre.previousPosition : centre.farPoint, before))
        {
            motion = make_float2(now.x - before.x, now.y - before.y);
            const float2 previousPixel = make_float2(pixel.x + 0.5f - motion.x, pixel.y + 0.5f - motion.y);
            reuse = FetchReprojectedHistory(previousPixel, centre, history);
        }
    }

    // A running average over `frames` frames: with nothing moving every
    // frame counts equally; params.historyLimit caps how far back it goes.
    float frames = 1.0f;
    float3 blendedDirect = direct;
    float3 blendedIndirect = indirect;
    float3 blendedAlbedo = albedo;
    float3 blendedSpecular = specular;
    float3 blendedSpecularAlbedo = specularAlbedo;
    float blendedRoughness = roughness;
    if (reuse)
    {
        frames = fminf(history.frames, static_cast<float>(params.historyLimit)) + 1.0f;
        const float t = 1.0f / frames;
        blendedDirect = Mix(history.direct, direct, t);
        blendedIndirect = Mix(history.indirect, indirect, t);
        blendedAlbedo = Mix(history.albedo, albedo, t);
        blendedSpecularAlbedo = Mix(history.specularAlbedo, specularAlbedo, t);
        blendedRoughness = history.roughness + (roughness - history.roughness) * t;

        // Reflections move differently from the surface they are on (they
        // show what is around it, from a changing angle), so following the
        // surface smears a sharp one. While things move, smoother surfaces
        // keep less history: a mirror none, a rough surface (whose blurry
        // reflection hardly changes) as much as the rest.
        float specularFrames = frames;
        if (!params.nothingMoved)
        {
            // At least a quarter of the history even for a mirror: a single
            // frame's reflection is too grainy, a short smear far less
            // noticeable.
            const float blurriness = fminf(fmaxf((blendedRoughness - 0.1f) / 0.4f, 0.25f), 1.0f);
            specularFrames = fminf(frames, 1.0f + (frames - 1.0f) * blurriness);
        }
        blendedSpecular = Mix(history.specular, specular, 1.0f / specularFrames);
    }

    params.directOut[pixelIndex] = WithW(blendedDirect, frames);
    params.indirectOut[pixelIndex] = WithW(blendedIndirect, 1.0f);
    params.albedoOut[pixelIndex] = WithW(blendedAlbedo, 1.0f);
    params.specularOut[pixelIndex] = WithW(blendedSpecular, 1.0f);
    params.specularAlbedoOut[pixelIndex] = WithW(blendedSpecularAlbedo, blendedRoughness);
    params.positionOut[pixelIndex] = centre.hit ? WithW(centre.position, 1.0f) : make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    params.normalOut[pixelIndex] = centre.hit ? WithW(centre.normal, 0.0f) : make_float4(0.0f, 0.0f, 0.0f, 0.0f);

    // --- Denoiser inputs ---
    // Surfaces still on screen: our average. Moving ones: this frame's
    // light (see LaunchParams::denoiserInput).
    const bool stillOnScreen = reuse && fabsf(motion.x) < 0.01f && fabsf(motion.y) < 0.01f;
    params.denoiserInput[pixelIndex] = WithW(stillOnScreen ? blendedIndirect : indirect, 1.0f);
    params.specularDenoiserInput[pixelIndex] = WithW(stillOnScreen ? blendedSpecular : specular, 1.0f);

    // The temporal denoiser wants normals in camera space.
    const float3 n = centre.hit ? centre.normal : Splat(0.0f);
    const float* r = params.viewRotation;
    params.guideNormal[pixelIndex] = make_float4(r[0] * n.x + r[1] * n.y + r[2] * n.z,
                                                 r[3] * n.x + r[4] * n.y + r[5] * n.z,
                                                 r[6] * n.x + r[7] * n.y + r[8] * n.z, 0.0f);
    params.flow[pixelIndex] = motion;
    params.flowTrust[pixelIndex] = reuse ? 1.0f : 0.0f;
}

// Distance from the camera to what the pixel's centre ray hit (0 for nothing).
static __forceinline__ __device__ float PixelDistance(unsigned int index)
{
    const float4 p = params.positionOut[index];
    return p.w != 0.0f ? Length(Xyz(p) - params.cameraPosition) : 0.0f;
}

// Averages each 2x2 block of the denoiser's full-resolution inputs into
// one half-resolution pixel (see LaunchParams, half resolution denoising).
// Four samples averaged also makes the input four times less noisy.
extern "C" __global__ void __raygen__downsample()
{
    const uint3 half = optixGetLaunchIndex();
    const unsigned int halfIndex = half.y * params.halfWidth + half.x;

    float3 input = Splat(0.0f);
    float3 specular = Splat(0.0f);
    float3 albedo = Splat(0.0f);
    float3 normal = Splat(0.0f);
    float distance = 0.0f;
    float2 flow = make_float2(0.0f, 0.0f);
    float trust = 1.0f;
    float count = 0.0f;
    float hits = 0.0f;
    for (unsigned int dy = 0; dy < 2; ++dy)
    {
        for (unsigned int dx = 0; dx < 2; ++dx)
        {
            const unsigned int x = half.x * 2 + dx;
            const unsigned int y = half.y * 2 + dy;
            if (x >= params.width || y >= params.height) // odd-sized image
                continue;
            const unsigned int index = y * params.width + x;

            input += Xyz(params.denoiserInput[index]);
            specular += Xyz(params.specularDenoiserInput[index]);
            albedo += Xyz(params.albedoOut[index]);
            const float2 f = params.flow[index];
            flow = make_float2(flow.x + f.x, flow.y + f.y);
            // Only as trustworthy as the least trustworthy of the four.
            trust = fminf(trust, params.flowTrust[index]);
            count += 1.0f;

            if (params.positionOut[index].w != 0.0f)
            {
                normal += Xyz(params.guideNormal[index]);
                distance += PixelDistance(index);
                hits += 1.0f;
            }
        }
    }

    params.halfInput[halfIndex] = WithW(input / count, 1.0f);
    params.halfSpecularInput[halfIndex] = WithW(specular / count, 1.0f);
    params.halfAlbedo[halfIndex] = WithW(albedo / count, 1.0f);
    params.halfNormal[halfIndex] = hits > 0.0f ? WithW(Normalize(normal), distance / hits)
                                               : make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    // Half-resolution pixels are twice the size, so the motion is half as many.
    params.halfFlow[halfIndex] = make_float2(flow.x / count * 0.5f, flow.y / count * 0.5f);
    params.halfFlowTrust[halfIndex] = trust;
}

// A denoised half-resolution image at a full-resolution pixel: a blend of
// the four nearest half-resolution pixels (bilinear), each weighted down the
// more its surface differs from this pixel's in facing and distance. That
// keeps light from one surface bleeding onto another across an edge, e.g.
// from a lit wall onto the floor in front of it.
static __device__ float3 UpsampleHalf(const float4* source, const uint3& pixel, unsigned int pixelIndex)
{
    const float4 here = params.guideNormal[pixelIndex];
    const float3 normal = Xyz(here);
    const float distance = PixelDistance(pixelIndex);

    // Half-resolution pixel centres sit at full-resolution (2i + 1, 2j + 1).
    const float fx = (pixel.x + 0.5f) * 0.5f - 0.5f;
    const float fy = (pixel.y + 0.5f) * 0.5f - 0.5f;
    const int x0 = static_cast<int>(floorf(fx));
    const int y0 = static_cast<int>(floorf(fy));
    const float tx = fx - x0;
    const float ty = fy - y0;

    float3 sum = Splat(0.0f);
    float weightSum = 0.0f;
    float3 plainSum = Splat(0.0f); // without the edge weights, as a fallback
    float plainWeightSum = 0.0f;
    for (int j = 0; j < 2; ++j)
    {
        for (int i = 0; i < 2; ++i)
        {
            const int x = min(max(x0 + i, 0), static_cast<int>(params.halfWidth) - 1);
            const int y = min(max(y0 + j, 0), static_cast<int>(params.halfHeight) - 1);
            const unsigned int index = y * params.halfWidth + x;
            const float bilinear = (i ? tx : 1.0f - tx) * (j ? ty : 1.0f - ty);
            const float3 light = Xyz(source[index]);
            plainSum += light * bilinear;
            plainWeightSum += bilinear;

            const float4 other = params.halfNormal[index];
            if (distance == 0.0f || other.w == 0.0f)
                continue;
            // Facing: cosine between the normals, sharpened.
            const float facing = powf(fmaxf(Dot(normal, Xyz(other)), 0.0f), 8.0f);
            // Distance: falls off over 5% of the distance from the camera.
            const float depth = __expf(-fabsf(distance - other.w) / (0.05f * distance));
            const float weight = bilinear * facing * depth;
            sum += light * weight;
            weightSum += weight;
        }
    }

    // The background (its albedo is 0, so this doesn't show), or a thin
    // sliver no half-resolution pixel matches: plain bilinear.
    if (weightSum < 1e-4f)
        return plainWeightSum > 0.0f ? plainSum / plainWeightSum : Splat(0.0f);
    return sum / weightSum;
}

// Puts the finished image together and turns it into the sRGB bytes shown
// on screen: the direct light, plus the (denoised) diffuse and reflected
// light multiplied back by the surface's diffuse and specular albedo.
extern "C" __global__ void __raygen__present()
{
    const uint3 pixel = optixGetLaunchIndex();
    const unsigned int pixelIndex = pixel.y * params.width + pixel.x;

    const float3 indirect = params.upsampleIndirect ? UpsampleHalf(params.presentIndirect, pixel, pixelIndex)
                                                    : Xyz(params.presentIndirect[pixelIndex]);

    // Reflections: a sharp one comes out of the path tracer nearly clean,
    // and the denoiser (tuned by the soft diffuse light, perhaps at half
    // resolution) would blur it, so it is used as traced. A blurry one is
    // noisy, and denoised. Between the two, a blend by roughness.
    const float4 specularAlbedo = params.presentSpecularAlbedo[pixelIndex];
    float3 specular = Xyz(params.presentSpecular[pixelIndex]);
    if (params.presentSpecularDenoised)
    {
        const float3 denoised = params.upsampleIndirect ? UpsampleHalf(params.presentSpecularDenoised, pixel, pixelIndex)
                                                        : Xyz(params.presentSpecularDenoised[pixelIndex]);
        const float t = fminf(fmaxf((specularAlbedo.w - 0.08f) / 0.22f, 0.0f), 1.0f);
        specular = Mix(specular, denoised, t * t * (3.0f - 2.0f * t)); // smoothstep
    }

    const float3 c = Xyz(params.presentDirect[pixelIndex]) + Xyz(params.presentAlbedo[pixelIndex]) * indirect +
                     Xyz(specularAlbedo) * specular;

    // Lighting is computed in linear light; the screen expects sRGB.
    params.output[pixelIndex] = make_uchar4(ToByte(LinearToSrgb(c.x)), ToByte(LinearToSrgb(c.y)),
                                            ToByte(LinearToSrgb(c.z)), 255);
}

// One of the material's texture maps at `uv`, blurred to mip level
// `lodBase` + the map's own size (see the closest-hit program). White for an
// empty slot, which leaves the material's value as it is, like lit.frag's
// white fallback texture.
static __forceinline__ __device__ float3 SampleMap(const InstanceData& instance, MaterialMap map, float u, float v,
                                                   float lodBase)
{
    const cudaTextureObject_t texture = instance.maps[map];
    if (!texture)
        return Splat(1.0f);
    const float4 c = tex2DLod<float4>(texture, u, v, lodBase + instance.mapLog2Size[map]);
    return make_float3(c.x, c.y, c.z);
}

extern "C" __global__ void __closesthit__surface()
{
    const InstanceData& instance = params.instances[optixGetInstanceId()];
    const uint3 triangle = instance.indices[optixGetPrimitiveIndex()];
    const DeviceVertex& v0 = instance.vertices[triangle.x];
    const DeviceVertex& v1 = instance.vertices[triangle.y];
    const DeviceVertex& v2 = instance.vertices[triangle.z];

    // Blend the three corners' attributes by where on the triangle the hit is.
    const float2 bary = optixGetTriangleBarycentrics();
    const float w0 = 1.0f - bary.x - bary.y;
    const float3 vertexColor = v0.color * w0 + v1.color * bary.x + v2.color * bary.y;
    const float3 objectNormal = v0.normal * w0 + v1.normal * bary.x + v2.normal * bary.y;
    const float3 objectPosition = v0.position * w0 + v1.position * bary.x + v2.position * bary.y;

    // Texture coordinate, mapped as in lit.frag: uv * tiling + offset.
    const float2 tiling = make_float2(instance.uvTiling[0], instance.uvTiling[1]);
    const float u = (v0.u * w0 + v1.u * bary.x + v2.u * bary.y) * tiling.x + instance.uvOffset[0];
    const float v = (v0.v * w0 + v1.v * bary.x + v2.v * bary.y) * tiling.y + instance.uvOffset[1];

    const float3 rayDir = optixGetWorldRayDirection();
    const float distance = optixGetRayTmax();

    // The flat triangle's normal, turned to face the side the ray came from.
    // Used to push rays off the surface, and to flip the shading normal on
    // back faces (the far side of a double-sided quad) like lit.frag does
    // with gl_FrontFacing.
    float3 Ng = Normalize(optixTransformNormalFromObjectToWorldSpace(
        Cross(v1.position - v0.position, v2.position - v0.position)));
    if (Dot(Ng, rayDir) > 0.0f)
        Ng = -Ng;

    float3 N = Normalize(optixTransformNormalFromObjectToWorldSpace(objectNormal));
    if (Dot(N, Ng) < 0.0f)
        N = -N;

    // --- Texture detail level (ray cones) ---
    // Rasterizers pick a mip level from how fast UVs change between
    // neighbouring pixels; a ray has no neighbours, so instead it is treated
    // as a cone and compared with how much texture the triangle stretches
    // over the area the cone covers where it lands (Akenine-Moller et al.,
    // "Texture Level of Detail Strategies for Real-Time Ray Tracing").
    const float3 edge1 = v1.position - v0.position;
    const float3 edge2 = v2.position - v0.position;
    const float du1 = v1.u - v0.u, dv1 = v1.v - v0.v;
    const float du2 = v2.u - v0.u, dv2 = v2.v - v0.v;
    const float worldArea = Length(Cross(optixTransformVectorFromObjectToWorldSpace(edge1),
                                         optixTransformVectorFromObjectToWorldSpace(edge2)));
    const float uvArea = fabsf((du1 * dv2 - du2 * dv1) * tiling.x * tiling.y);
    float lodBase = -16.0f; // sharpest level, if the triangle has no UV area
    if (worldArea > 0.0f && uvArea > 0.0f)
    {
        const SurfaceHit& caller = *GetSurfaceHit();
        const float coneWidth = caller.coneWidth + distance * caller.coneSpread;
        // Seen at a grazing angle, the cone smears over more of the surface.
        const float cosine = fmaxf(fabsf(Dot(rayDir, Ng)), 0.05f);
        // + log2(map size) per map, in SampleMap.
        lodBase = 0.5f * log2f(uvArea / worldArea) + log2f(fmaxf(coneWidth, 1e-8f) / cosine);
    }

    // --- Material, with its texture maps (as in lit.frag) ---
    const float3 baseColor = instance.baseColor * vertexColor * SampleMap(instance, MAP_BASE_COLOR, u, v, lodBase);
    const float metallic = instance.metallic * SampleMap(instance, MAP_METALLIC, u, v, lodBase).z;
    const float roughness = instance.roughness * SampleMap(instance, MAP_ROUGHNESS, u, v, lodBase).y;
    const float ao = 1.0f + (SampleMap(instance, MAP_AO, u, v, lodBase).x - 1.0f) * instance.aoStrength;
    const float3 emission = instance.emission * SampleMap(instance, MAP_EMISSION, u, v, lodBase);

    // Normal map: stored as 0..1 per channel, a -1..1 direction in the
    // surface's tangent frame (along its U and V directions), with strength
    // scaling the sideways tilt.
    if (instance.maps[MAP_NORMAL])
    {
        const float determinant = du1 * dv2 - du2 * dv1;
        if (fabsf(determinant) > 1e-12f)
        {
            // Which way U and V run across this triangle, in the world.
            const float3 tangentU = optixTransformVectorFromObjectToWorldSpace((edge1 * dv2 - edge2 * dv1) / determinant);
            const float3 tangentV = optixTransformVectorFromObjectToWorldSpace((edge2 * du1 - edge1 * du2) / determinant);
            // Made perpendicular to the shading normal; the bitangent keeps
            // V's direction, so mirrored UVs still read the map correctly.
            const float3 T = Normalize(tangentU - N * Dot(N, tangentU));
            float3 B = Cross(N, T);
            if (Dot(B, tangentV) < 0.0f)
                B = -B;

            float3 mapped = SampleMap(instance, MAP_NORMAL, u, v, lodBase) * 2.0f - Splat(1.0f);
            mapped.x *= instance.normalStrength;
            mapped.y *= instance.normalStrength;
            const float3 bent = T * mapped.x + B * mapped.y + N * mapped.z;
            if (Dot(bent, bent) > 1e-12f)
                N = Normalize(bent);
        }
    }

    // The same point on the object, placed with last frame's transform.
    const float* m = instance.previousTransform;
    const float3 previousPosition = make_float3(
        m[0] * objectPosition.x + m[1] * objectPosition.y + m[2] * objectPosition.z + m[3],
        m[4] * objectPosition.x + m[5] * objectPosition.y + m[6] * objectPosition.z + m[7],
        m[8] * objectPosition.x + m[9] * objectPosition.y + m[10] * objectPosition.z + m[11]);

    SurfaceHit& s = *GetSurfaceHit();
    s.hit = true;
    s.distance = distance;
    s.position = optixGetWorldRayOrigin() + rayDir * distance;
    s.previousPosition = previousPosition;
    s.geometricNormal = Ng;
    s.normal = N;
    s.baseColor = baseColor;
    s.emission = emission;
    s.metallic = metallic;
    s.roughness = roughness;
    s.ao = ao;
}

extern "C" __global__ void __miss__surface()
{
    // SurfaceHit::hit stays false.
}

extern "C" __global__ void __miss__shadow()
{
    optixSetPayload_0(1);
}
