#include "RayTracing/OptixContext.h"

#include <optix_function_table_definition.h> // exactly one .cpp must include this
#include <optix_stubs.h>

#include <iostream>

namespace
{
    // OptiX reports errors, warnings and build info through this callback.
    void LogCallback(unsigned int level, const char* tag, const char* message, void* /*userData*/)
    {
        std::cerr << "[OptiX " << level << "][" << tag << "] " << message << '\n';
    }
}

OptixContext::OptixContext()
{
    // Creates the CUDA context on the default GPU (cudaFree(0) is the usual
    // way to force that without doing anything else).
    cudaError_t cudaResult = cudaFree(nullptr);
    if (cudaResult != cudaSuccess)
    {
        std::cerr << "CUDA init failed: " << cudaGetErrorString(cudaResult) << '\n';
        return;
    }

    cudaDeviceProp props{};
    cudaGetDeviceProperties(&props, 0);

    // Loads the OptiX functions from the driver.
    OptixResult result = optixInit();
    if (result != OPTIX_SUCCESS)
    {
        std::cerr << "optixInit failed: " << optixGetErrorName(result) << '\n';
        return;
    }

    cudaResult = cudaStreamCreate(&m_Stream);
    if (cudaResult != cudaSuccess)
    {
        std::cerr << "cudaStreamCreate failed: " << cudaGetErrorString(cudaResult) << '\n';
        return;
    }

    OptixDeviceContextOptions options{};
    options.logCallbackFunction = &LogCallback;
    options.logCallbackLevel = 3; // 1 = fatal, 2 = error, 3 = warning, 4 = print
#ifndef NDEBUG
    // Extra checks on every OptiX call; slow, so debug builds only.
    options.validationMode = OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_ALL;
#endif

    // 0 = use the CUDA context that is current on this thread.
    result = optixDeviceContextCreate(nullptr, &options, &m_Context);
    if (result != OPTIX_SUCCESS)
    {
        std::cerr << "optixDeviceContextCreate failed: " << optixGetErrorName(result) << '\n';
        m_Context = nullptr;
        return;
    }

    std::cout << "OptiX " << OPTIX_VERSION / 10000 << '.' << (OPTIX_VERSION % 10000) / 100
              << " on " << props.name << '\n';
}

OptixContext::~OptixContext()
{
    if (m_Context)
        optixDeviceContextDestroy(m_Context);
    if (m_Stream)
        cudaStreamDestroy(m_Stream);
}
