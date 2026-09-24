#pragma once

#include "RayTracing/CudaBuffer.h"

#include <optix_types.h>

class OptixContext;

// The OptiX AI denoiser, temporal model: turns a noisy image into a clean
// one, using its own result from last frame (moved along the per-pixel
// motion) so it stays stable from frame to frame instead of shimmering.
// Guide images tell detail that belongs to the scene from noise. Runs on
// the context's stream.
class Denoiser
{
public:
    explicit Denoiser(const OptixContext& context);
    ~Denoiser();

    // One Denoiser owns its OptiX objects: copying would double-destroy them.
    Denoiser(const Denoiser&) = delete;
    Denoiser& operator=(const Denoiser&) = delete;

    // False if it couldn't be created (the reason has been logged).
    bool IsValid() const { return m_Denoiser != nullptr; }

    // Prepares for width x height images, only redoing the work if the
    // size changed (which also starts a new sequence). Returns false on failure.
    bool Resize(unsigned int width, unsigned int height);

    // The next frame doesn't follow on from the last (the view jumped, or
    // the lighting changed): don't carry the old result over.
    void StartNewSequence() { m_HasPrevious = false; }

    // The images Run reads and writes, each width x height.
    struct Images
    {
        CUdeviceptr input;       // float4, RGB: the noisy image
        CUdeviceptr albedo;      // float4, RGB: first surface's color
        CUdeviceptr normal;      // float4, XYZ: first surface's normal, camera space
        CUdeviceptr flow;        // float2: how far each pixel moved since last frame
        CUdeviceptr flowTrust;   // float: 0..1, how much to trust that
        CUdeviceptr output;      // float4, RGB; keep it for the next frame, which reads it
        // Optional: a second image denoised along with the first (with the
        // same guides, by the same run), e.g. reflections. 0 if none.
        CUdeviceptr secondInput;
        CUdeviceptr secondOutput;
    };

    // Denoises images.input into images.output. Queued on the stream;
    // returns false on failure.
    bool Run(const Images& images);

private:
    const OptixContext& m_Context;
    OptixDenoiser m_Denoiser = nullptr;
    CudaBuffer m_State;
    CudaBuffer m_Scratch;
    // The denoiser's own per-pixel memory of last frame: read from one,
    // written to the other, swapped every frame.
    CudaBuffer m_InternalGuide[2];
    size_t m_InternalGuidePixelSize = 0;
    int m_CurrentGuide = 0;
    bool m_HasPrevious = false;
    unsigned int m_Width = 0;
    unsigned int m_Height = 0;
};
