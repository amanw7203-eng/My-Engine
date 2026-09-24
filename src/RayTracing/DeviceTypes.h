#pragma once

// Data shared between the engine (C++) and the OptiX programs (CUDA), so
// only plain types from CUDA's vector_types.h are used here: no GLM.

#include <optix_types.h>  // OptixTraversableHandle
#include <vector_types.h> // float2, float3, uint3, uchar4

// Same memory layout as the engine's Vertex (checked in SceneAccel.cpp).
// uv is two floats rather than a float2, whose 8-byte alignment would add
// padding and shift `normal`.
struct DeviceVertex
{
    float3 position;
    float3 color;
    float u, v;
    float3 normal;
};

// One per instance in the scene's acceleration structure, looked up in the
// closest-hit program with optixGetInstanceId(). Holds what shading a hit
// needs: the mesh's geometry and the entity's material.
struct InstanceData
{
    const DeviceVertex* vertices;
    const uint3* indices;    // one uint3 per triangle (optixGetPrimitiveIndex())

    float3 baseColor;        // linear
    float3 emission;         // linear, already multiplied by strength
    float metallic;
    float roughness;

    // Object -> world as it was last frame (top 3 rows, row by row), to
    // find where a hit point was then: how far it moved on screen.
    float previousTransform[12];
};

// Ray types. Each has its own miss program; both share the one hit group
// (shadow rays skip its closest-hit program).
enum RayType : unsigned int
{
    RAY_TYPE_SURFACE = 0, // "which surface does this ray hit first?"
    RAY_TYPE_SHADOW = 1,  // "does anything block this ray?"
};

// Everything one launch needs, readable by every OptiX program as the
// constant `params`. Filled in by RayTracer::Render.
//
// Each frame is two launches with the denoiser between them:
//   __raygen__trace    traces the light paths and blends them into the
//                      history buffers below
//   (denoiser)         cleans up the indirect light
//   __raygen__present  direct + diffuse albedo * indirect -> sRGB output
// All images are width * height pixels, row by row from the bottom (the
// order GL textures use).
//
// The light is kept in two parts, because only one of them is noisy:
//   direct    everything seen at the first surface: sun (with its shadow),
//             emission, sky reflection. Noise-free apart from edges.
//   indirect  light bounced off other surfaces onto the first one: the
//             noisy part, the only part denoised. Stored "demodulated", as
//             if the first surface were white; multiplying by its diffuse
//             albedo afterwards keeps surface detail out of the denoiser.
struct LaunchParams
{
    // --- Present ---
    uchar4* output;                // sRGB RGBA8, shown on screen
    const float4* presentDirect;   // RGB
    const float4* presentAlbedo;   // RGB
    // RGB: denoised, or straight from the trace. Full resolution, unless
    // upsampleIndirect: then half resolution (halfWidth x halfHeight),
    // scaled up guided by halfNormal.
    const float4* presentIndirect;
    int upsampleIndirect;

    // --- Half resolution denoising ---
    // The indirect light is soft (demodulated, it has no surface detail),
    // so it can be denoised at half resolution for a quarter of the cost.
    // __raygen__downsample averages each 2x2 block of the full-resolution
    // denoiser inputs into these, which the denoiser then works on.
    float4* halfInput;
    float4* halfAlbedo;
    // Camera-space normal of the block's surface (XYZ) and its distance
    // from the camera (A), 0 if the block shows only background. Also
    // what the upsampling compares against, to keep edges sharp.
    float4* halfNormal;
    float2* halfFlow;
    float* halfFlowTrust;
    unsigned int halfWidth;
    unsigned int halfHeight;

    // --- Trace ---
    // Temporal reuse: last frame's results are read from the *In buffers
    // and this frame's written to the *Out ones (they swap every frame).
    // Direct light averaged over frames (RGB), and how many frames the
    // averages hold (A).
    const float4* directIn;
    float4* directOut;
    // Demodulated indirect light averaged over frames (RGB).
    const float4* indirectIn;
    float4* indirectOut;
    // The first surface the camera rays hit, averaged over frames like the
    // light: its diffuse albedo, i.e. how much of the indirect light it
    // scatters back (RGB).
    const float4* albedoIn;
    float4* albedoOut;
    // What the ray through the pixel's centre hit: world position (XYZ,
    // A = 1), or all 0 for nothing, and world normal (XYZ). Unlike the
    // light's randomly placed samples it is the same every frame while
    // nothing moves, so it reliably checks that reused history shows the
    // same surface.
    const float4* positionIn;
    float4* positionOut;
    const float4* normalIn;
    float4* normalOut;

    // The indirect light for the denoiser (RGB). Where the pixel's surface
    // is still on screen, the average over frames. Where it moves, this
    // frame's alone: the denoiser follows the motion and averages over time
    // itself, while our average, resampled as it follows, would have its
    // noise smeared into streaks the denoiser can't recognise as noise.
    float4* denoiserInput;

    // Denoiser guides for this frame, written by the trace.
    float4* guideNormal; // the centre ray's normal in camera space (XYZ)
    float2* flow;        // how far each pixel's surface moved on screen since last frame
    float* flowTrust;    // 1 where the pixel's history was found, 0 where it is new

    unsigned int width;
    unsigned int height;
    unsigned int randomSeed;      // different every frame
    unsigned int samplesPerPixel; // light paths per pixel per frame
    unsigned int maxBounces;      // diffuse bounces per path; 0 = flat ambient
    // Reuse last frame's history (0 = start over), averaging at most this
    // many frames: more is smoother, fewer adapts faster.
    int historyValid;
    unsigned int historyLimit;
    // Neither the camera nor anything in the scene moved since last frame:
    // each pixel's history is its own, no reprojection needed.
    int nothingMoved;

    // Camera matrices, row by row. Plain floats, not float4: those need
    // 16-byte alignment, which `params` doesn't promise.
    // inverseViewProjection turns a pixel back into a world-space point;
    // the other two find where a world point is on screen now and was last
    // frame, i.e. how far it moved. viewRotation turns world directions
    // into camera space.
    float inverseViewProjection[16];
    float viewProjection[16];
    float previousViewProjection[16];
    float viewRotation[9];
    float3 cameraPosition;

    // Lighting: the same inputs lit.frag gets. Colors are linear.
    float3 lightDir;     // from the light towards the scene
    float3 lightColor;
    float3 ambientColor; // the sky: light from every direction not blocked
    float3 background;   // seen where a camera ray hits nothing
    int shadowsEnabled;
    int cullBackFaces;

    OptixTraversableHandle scene;
    const InstanceData* instances;
};
