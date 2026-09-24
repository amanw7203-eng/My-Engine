#pragma once

#include <cuda_runtime.h>
#include <optix.h>

// The connection to OptiX, NVIDIA's hardware ray tracing API. Owns the OptiX
// device context and a CUDA stream that all ray tracing work is queued on.
// Everything else in RayTracing/ is created from one of these.
//
// Ray tracing is optional: if there is no RTX-capable GPU or driver,
// IsValid() is false and the engine keeps rendering with OpenGL only.
class OptixContext
{
public:
    OptixContext();
    ~OptixContext();

    // One OptixContext owns its OptiX/CUDA objects: copying would double-destroy them.
    OptixContext(const OptixContext&) = delete;
    OptixContext& operator=(const OptixContext&) = delete;

    bool IsValid() const { return m_Context != nullptr; }

    OptixDeviceContext Get() const { return m_Context; }
    cudaStream_t GetStream() const { return m_Stream; }

private:
    OptixDeviceContext m_Context = nullptr;
    cudaStream_t m_Stream = nullptr;
};
