#pragma once

#include "RayTracing/CudaBuffer.h"
#include "RayTracing/Denoiser.h"
#include "RayTracing/InteropTexture.h"
#include "Scene/Light.h"

#include <cuda_runtime.h>
#include <glm/glm.hpp>
#include <optix_types.h>

#include <filesystem>
#include <vector>

class OptixContext;
class SceneAccel;

// Renders the scene with hardware ray tracing: light paths from the camera,
// shaded with metallic-roughness PBR, a shadow ray to the sun instead of a
// shadow map, and ambient light bouncing between surfaces.
//
// Built for real time: each frame traces few paths per pixel, reuses what
// earlier frames found for the same surfaces (following them as the camera
// moves), and cleans up the remaining noise with the OptiX denoiser.
//
// Owns the OptiX pipeline (the compiled Programs.cu, linked into one GPU
// program), the shader binding tables that tell OptiX which programs to run,
// the per-pixel frame buffers, and the texture the image is shown through.
// Like the other GL owners, it must be destroyed before the GL context.
class RayTracer
{
public:
    // `programsFile` is the OptiX-IR compiled from Programs.cu.
    RayTracer(const OptixContext& context, const std::filesystem::path& programsFile);
    ~RayTracer();

    // One RayTracer owns its OptiX objects: copying would double-destroy them.
    RayTracer(const RayTracer&) = delete;
    RayTracer& operator=(const RayTracer&) = delete;

    // False if OptiX is unavailable or the pipeline couldn't be created
    // (the reason has been logged).
    bool IsValid() const { return m_Pipeline != nullptr; }
    bool IsDenoiserAvailable() const { return m_Denoiser.IsValid(); }

    // Per-frame inputs, matching the raster pass's uniforms where they
    // overlap. Colors are linear.
    struct FrameSettings
    {
        glm::mat4 view;
        glm::mat4 projection;
        glm::vec3 lightDir;      // from the light towards the scene
        glm::vec3 lightColor;
        glm::vec3 ambientColor;  // sky light, arriving from every open direction
        glm::vec3 background;
        bool shadowsEnabled;
        bool reflections;        // trace reflections (else a plain sky is reflected)
        bool cullBackFaces;
        int maxBounces;          // ambient light bounces; 0 = flat ambient as in raster
        int samplesPerPixel;     // light paths per pixel per frame
        bool denoise;            // run the OptiX denoiser on the result
        // Denoise at half resolution: about a quarter of the cost, slightly
        // softer bounced light. The denoiser is by far the most expensive step.
        bool denoiseHalfResolution;
        // While the camera moves, average at most this many past frames:
        // fewer adapts faster (less smearing), more is less noisy.
        int movingHistoryFrames;
        // Changing these counts as scene motion (a short history), not as a
        // lighting change that starts over: lights get moved around a lot.
        std::vector<ScenePointLight> pointLights;

        bool operator==(const FrameSettings&) const = default;
    };

    // Traces `scene` at width x height and copies the image onto the
    // window's framebuffer (bound as the draw framebuffer afterwards).
    //
    // With a still camera and scene, frames are averaged ever longer, so the
    // image converges; after kMaxAccumulatedFrames it is final and the GPU
    // stops tracing until something changes. While the camera or anything in
    // the scene moves, each pixel follows its surface and keeps a short
    // history. Changing the lighting or render settings starts over.
    void Render(const SceneAccel& scene, const FrameSettings& settings, int width, int height);

    // Forget earlier frames: call when Render wasn't called for a while
    // (ray tracing was switched off), since the history no longer matches.
    void ResetHistory();

    static constexpr unsigned int kMaxAccumulatedFrames = 1024;

    // Frames rendered since the view last changed (for display in the UI).
    unsigned int GetAccumulatedFrames() const { return m_StillFrames; }

    // How long the GPU spent on each stage of a recent frame, in milliseconds.
    struct GpuTimings
    {
        float trace = 0.0f;
        float denoise = 0.0f;
        float present = 0.0f;
    };
    const GpuTimings& GetTimings() const { return m_Timings; }

private:
    bool CreatePipeline(const std::filesystem::path& programsFile);
    void CreateShaderBindingTables();
    bool ResizeFrameBuffers(int width, int height);
    void ReadTimings();
    void Release();

    const OptixContext& m_Context;

    OptixModule m_Module = nullptr;
    OptixProgramGroup m_RaygenTrace = nullptr;
    OptixProgramGroup m_RaygenPresent = nullptr;
    OptixProgramGroup m_RaygenDownsample = nullptr;
    OptixProgramGroup m_MissSurface = nullptr;
    OptixProgramGroup m_MissShadow = nullptr;
    OptixProgramGroup m_HitGroup = nullptr;
    OptixPipeline m_Pipeline = nullptr;

    // The launches share miss and hit records and differ in raygen.
    CudaBuffer m_RaygenRecords; // trace, present, downsample
    CudaBuffer m_MissRecords;
    CudaBuffer m_HitGroupRecords;
    OptixShaderBindingTable m_TraceSbt{};
    OptixShaderBindingTable m_PresentSbt{};
    OptixShaderBindingTable m_DownsampleSbt{};

    CudaBuffer m_LaunchParams;
    CudaBuffer m_PointLights; // DevicePointLight array, grown as needed
    InteropTexture m_Output;
    Denoiser m_Denoiser;

    // Per-pixel images (see LaunchParams), float4 unless noted. The history
    // buffers come in pairs: last frame's is read while this frame's is
    // written, then they swap.
    CudaBuffer m_Direct[2];
    CudaBuffer m_Indirect[2];
    CudaBuffer m_Specular[2];
    CudaBuffer m_SpecularAlbedo[2];
    CudaBuffer m_Albedo[2];
    CudaBuffer m_Normal[2];
    CudaBuffer m_Position[2];
    CudaBuffer m_DenoiserInput;
    CudaBuffer m_SpecularDenoiserInput;
    CudaBuffer m_GuideNormal;
    CudaBuffer m_Flow;      // float2
    CudaBuffer m_FlowTrust; // float
    // The denoised indirect light. The temporal denoiser also reads it
    // back next frame as its previous result.
    CudaBuffer m_Denoised;
    CudaBuffer m_SpecularDenoised;
    // Half resolution denoising (see LaunchParams): the downsampled inputs
    // (float4 unless noted) and the denoised result.
    CudaBuffer m_HalfInput;
    CudaBuffer m_HalfSpecularInput;
    CudaBuffer m_HalfAlbedo;
    CudaBuffer m_HalfNormal;
    CudaBuffer m_HalfFlow;      // float2
    CudaBuffer m_HalfFlowTrust; // float
    CudaBuffer m_HalfDenoised;
    CudaBuffer m_HalfSpecularDenoised;
    int m_Current = 0;          // which of each pair holds last frame's
    bool m_HistoryValid = false;
    // The denoiser ran recently in the current mode, so m_Denoised or
    // m_HalfDenoised holds a result it can continue from.
    bool m_DenoiserRunning = false;
    bool m_DenoiserWasHalfResolution = false;

    // Last frame's inputs, to tell what changed.
    FrameSettings m_PreviousSettings{};
    unsigned long long m_PreviousSceneHash = 0;
    glm::mat4 m_PreviousViewProjection{ 1.0f };
    unsigned int m_StillFrames = 0;
    unsigned int m_RandomSeed = 0;

    // GPU timestamps around each stage: before trace, after trace, after
    // denoise, after present. Read back a frame later, when they are done.
    cudaEvent_t m_Events[4] = {};
    bool m_TimingPending = false;
    GpuTimings m_Timings;
};
