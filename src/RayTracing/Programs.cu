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
static __forceinline__ __device__ float3 CosineSampleHemisphere(float3 n, unsigned int& seed)
{
    // Uniform point on a disk, projected up onto the hemisphere.
    const float r = sqrtf(Random(seed));
    const float phi = 2.0f * kPi * Random(seed);
    const float x = r * cosf(phi);
    const float y = r * sinf(phi);
    const float z = sqrtf(fmaxf(1.0f - x * x - y * y, 0.0f));

    // Two axes perpendicular to n (Duff et al., "Building an Orthonormal
    // Basis, Revisited").
    const float sign = copysignf(1.0f, n.z);
    const float a = -1.0f / (sign + n.z);
    const float b = n.x * n.y * a;
    const float3 tangent = make_float3(1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x);
    const float3 bitangent = make_float3(b, sign + n.y * n.y * a, -n.y);

    return tangent * x + bitangent * y + n * z;
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
    bool hit;
    float3 position;
    float3 previousPosition; // where that point of the surface was last frame
    float3 geometricNormal;  // the flat triangle's, facing the side the ray came from
    float3 normal;           // interpolated, for shading; same side as geometricNormal
    float3 baseColor;        // linear, material color times vertex color
    float3 emission;
    float metallic;
    float roughness;
};

static __forceinline__ __device__ SurfaceHit* GetSurfaceHit()
{
    const unsigned long long pointer =
        static_cast<unsigned long long>(optixGetPayload_0()) << 32 | optixGetPayload_1();
    return reinterpret_cast<SurfaceHit*>(pointer);
}

// Traces a path ray, returning what it hit (hit == false if nothing).
static __forceinline__ __device__ SurfaceHit TraceSurface(float3 origin, float3 direction, float tMax,
                                                          unsigned int rayFlags)
{
    SurfaceHit surface;
    surface.hit = false;
    const unsigned long long pointer = reinterpret_cast<unsigned long long>(&surface);
    unsigned int high = static_cast<unsigned int>(pointer >> 32);
    unsigned int low = static_cast<unsigned int>(pointer);
    optixTrace(params.scene, origin, direction, 0.0f, tMax, 0.0f,
               OptixVisibilityMask(0xFF), rayFlags,
               0, 1, RAY_TYPE_SURFACE, // SBT offset, SBT stride, miss program
               high, low);
    return surface;
}

// Is there a clear line from `origin` towards `direction`, to infinity?
// Any hit at all answers the question, so the trace stops at the first one
// and skips the closest-hit program.
static __forceinline__ __device__ bool IsUnblocked(float3 origin, float3 direction)
{
    unsigned int visible = 0;
    optixTrace(params.scene, origin, direction, 0.0f, 1e16f, 0.0f,
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

// The light arriving along one path, in the two parts LaunchParams
// describes.
struct PathLight
{
    float3 direct;
    float3 indirect;      // demodulated: as if the first surface were white
    float3 diffuseAlbedo; // the first surface's: multiply `indirect` by it
};

// One light path through the pixel point (x, y) (in normalized device
// coordinates): the linear light seen along it.
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

    const float3 L = Normalize(-params.lightDir); // surface -> sun

    PathLight light;
    light.direct = Splat(0.0f);
    light.indirect = Splat(0.0f);
    light.diffuseAlbedo = Splat(0.0f);

    // Light at the first surface counts as direct, everything from later
    // bounces as indirect.
    float3* out = &light.direct;
    // How much of the light arriving at the current surface reaches the
    // camera: each bounce multiplies in the color of the surface it left,
    // except the first surface's, which is left out of the indirect light
    // (demodulation) and multiplied back in after denoising.
    float3 throughput = Splat(1.0f);

    for (unsigned int bounce = 0;; ++bounce)
    {
        const SurfaceHit s = TraceSurface(origin, direction, tMax, rayFlags);
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
            const float3 H = Normalize(L + V);
            const float NdotH = fmaxf(Dot(N, H), 0.0f);
            const float VdotH = fmaxf(Dot(V, H), 0.0f);

            const float3 F = FresnelSchlick(VdotH, F0);
            const float3 specular = F * (DistributionGgx(NdotH, alpha) * VisibilitySmithGgx(NdotL, NdotV, alpha));
            // Light reflected at the surface (F) never gets in to scatter diffusely.
            const float3 diffuse = (Splat(1.0f) - F) * diffuseColor / kPi;

            // The sun color is how bright a white surface facing it looks,
            // as in lit.frag, so both renderers match under the same
            // settings. With the 1/pi in the diffuse BRDF, that means an
            // irradiance of pi * color.
            *out += throughput * (diffuse + specular) * params.lightColor * (kPi * NdotL);
        }

        // --- Ambient light ---
        // Split by the environment BRDF into what the surface reflects
        // specularly and what gets in and scatters diffusely.
        const float3 ambientSpecular = EnvironmentBrdf(F0, roughness, NdotV);
        const float3 diffuseWeight = diffuseColor * (Splat(1.0f) - ambientSpecular);

        if (bounce == 0)
            light.diffuseAlbedo = diffuseWeight;

        // Specular: the sky as if nothing blocked it. (Reflection rays
        // would replace this with what is really there.)
        *out += throughput * params.ambientColor * ambientSpecular;

        // Diffuse: out of bounces, assume open sky, like the raster path.
        // Otherwise follow the light back one more step: pick a direction
        // the way a diffuse surface scatters, and see what light arrives
        // from there, whether sky or another lit surface.
        if (bounce >= params.maxBounces)
        {
            *out += throughput * params.ambientColor * diffuseWeight;
            break;
        }

        const float3 bounceDir = CosineSampleHemisphere(N, seed);
        // With a tilted shading normal, a direction can point into the
        // surface itself; that light can't arrive, so the path ends.
        if (Dot(bounceDir, s.geometricNormal) <= 0.0f)
            break;

        if (bounce == 0)
            out = &light.indirect; // demodulated: throughput stays 1 here
        else
            throughput *= diffuseWeight;
        origin = OffsetFromSurface(s);
        direction = bounceDir;
        tMax = 1e16f;
        rayFlags = OPTIX_RAY_FLAG_NONE;
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
                                                           : OPTIX_RAY_FLAG_NONE);
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
};

static __forceinline__ __device__ History ReadHistory(unsigned int index)
{
    const float4 direct = params.directIn[index];
    History h;
    h.direct = Xyz(direct);
    h.frames = direct.w;
    h.indirect = Xyz(params.indirectIn[index]);
    h.albedo = Xyz(params.albedoIn[index]);
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

    float3 direct = Splat(0.0f);
    float3 indirect = Splat(0.0f);
    float3 albedo = Splat(0.0f);
    for (unsigned int sample = 0; sample < params.samplesPerPixel; ++sample)
    {
        // A random point inside the pixel rather than its centre: averaged
        // over frames, that smooths jagged edges for free (anti-aliasing).
        // Launch row 0 is the bottom row, like the GL texture it ends up in.
        const float x = (pixel.x + Random(seed)) / params.width * 2.0f - 1.0f;
        const float y = (pixel.y + Random(seed)) / params.height * 2.0f - 1.0f;

        PathLight light;
        if (params.scene)
        {
            light = TracePath(x, y, seed);
        }
        else
        {
            light.direct = params.background;
            light.indirect = Splat(0.0f);
            light.diffuseAlbedo = Splat(0.0f);
        }
        direct += light.direct;
        indirect += light.indirect;
        albedo += light.diffuseAlbedo;
    }
    const float samples = static_cast<float>(params.samplesPerPixel);
    direct = direct / samples;
    indirect = indirect / samples;
    albedo = albedo / samples;

    // A NaN or infinity (from degenerate geometry) would poison every
    // later frame's average, so drop that sample.
    if (!IsFinite(direct))
        direct = Splat(0.0f);
    if (!IsFinite(indirect))
        indirect = Splat(0.0f);

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
    if (reuse)
    {
        frames = fminf(history.frames, static_cast<float>(params.historyLimit)) + 1.0f;
        const float t = 1.0f / frames;
        blendedDirect = Mix(history.direct, direct, t);
        blendedIndirect = Mix(history.indirect, indirect, t);
        blendedAlbedo = Mix(history.albedo, albedo, t);
    }

    params.directOut[pixelIndex] = WithW(blendedDirect, frames);
    params.indirectOut[pixelIndex] = WithW(blendedIndirect, 1.0f);
    params.albedoOut[pixelIndex] = WithW(blendedAlbedo, 1.0f);
    params.positionOut[pixelIndex] = centre.hit ? WithW(centre.position, 1.0f) : make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    params.normalOut[pixelIndex] = centre.hit ? WithW(centre.normal, 0.0f) : make_float4(0.0f, 0.0f, 0.0f, 0.0f);

    // --- Denoiser inputs ---
    // Surfaces still on screen: our average. Moving ones: this frame's
    // light (see LaunchParams::denoiserInput).
    const bool stillOnScreen = reuse && fabsf(motion.x) < 0.01f && fabsf(motion.y) < 0.01f;
    params.denoiserInput[pixelIndex] = WithW(stillOnScreen ? blendedIndirect : indirect, 1.0f);

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
    params.halfAlbedo[halfIndex] = WithW(albedo / count, 1.0f);
    params.halfNormal[halfIndex] = hits > 0.0f ? WithW(Normalize(normal), distance / hits)
                                               : make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    // Half-resolution pixels are twice the size, so the motion is half as many.
    params.halfFlow[halfIndex] = make_float2(flow.x / count * 0.5f, flow.y / count * 0.5f);
    params.halfFlowTrust[halfIndex] = trust;
}

// The denoised half-resolution indirect light at a full-resolution pixel:
// a blend of the four nearest half-resolution pixels (bilinear), each
// weighted down the more its surface differs from this pixel's in facing
// and distance. That keeps light from one surface bleeding onto another
// across an edge, e.g. from a lit wall onto the floor in front of it.
static __device__ float3 UpsampleIndirect(const uint3& pixel, unsigned int pixelIndex)
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
            const float3 light = Xyz(params.presentIndirect[index]);
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
// on screen: the direct light, plus the (denoised) indirect light
// multiplied back by the surface's diffuse albedo.
extern "C" __global__ void __raygen__present()
{
    const uint3 pixel = optixGetLaunchIndex();
    const unsigned int pixelIndex = pixel.y * params.width + pixel.x;
    const float3 indirect = params.upsampleIndirect ? UpsampleIndirect(pixel, pixelIndex)
                                                    : Xyz(params.presentIndirect[pixelIndex]);
    const float3 c = Xyz(params.presentDirect[pixelIndex]) + Xyz(params.presentAlbedo[pixelIndex]) * indirect;

    // Lighting is computed in linear light; the screen expects sRGB.
    params.output[pixelIndex] = make_uchar4(ToByte(LinearToSrgb(c.x)), ToByte(LinearToSrgb(c.y)),
                                            ToByte(LinearToSrgb(c.z)), 255);
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

    const float3 rayDir = optixGetWorldRayDirection();

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

    // The same point on the object, placed with last frame's transform.
    const float* m = instance.previousTransform;
    const float3 previousPosition = make_float3(
        m[0] * objectPosition.x + m[1] * objectPosition.y + m[2] * objectPosition.z + m[3],
        m[4] * objectPosition.x + m[5] * objectPosition.y + m[6] * objectPosition.z + m[7],
        m[8] * objectPosition.x + m[9] * objectPosition.y + m[10] * objectPosition.z + m[11]);

    SurfaceHit& s = *GetSurfaceHit();
    s.hit = true;
    s.position = optixGetWorldRayOrigin() + rayDir * optixGetRayTmax();
    s.previousPosition = previousPosition;
    s.geometricNormal = Ng;
    s.normal = N;
    s.baseColor = instance.baseColor * vertexColor;
    s.emission = instance.emission;
    s.metallic = instance.metallic;
    s.roughness = instance.roughness;
}

extern "C" __global__ void __miss__surface()
{
    // SurfaceHit::hit stays false.
}

extern "C" __global__ void __miss__shadow()
{
    optixSetPayload_0(1);
}
