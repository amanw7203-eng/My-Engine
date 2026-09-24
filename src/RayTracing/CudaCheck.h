#pragma once

#include <cuda_runtime.h>
#include <optix.h>
#include <optix_stubs.h>

#include <iostream>

// Wrap CUDA / OptiX calls: on failure they log the call, file and line and
// evaluate to false, so the caller can give up on ray tracing gracefully.
//
//   if (!CUDA_CHECK(cudaMalloc(&ptr, size))) return;

namespace RayTracing::Detail
{
    inline bool CheckCuda(cudaError_t result, const char* call, const char* file, int line)
    {
        if (result == cudaSuccess)
            return true;
        std::cerr << file << '(' << line << "): " << call << " failed: "
                  << cudaGetErrorString(result) << '\n';
        return false;
    }

    inline bool CheckOptix(OptixResult result, const char* call, const char* file, int line)
    {
        if (result == OPTIX_SUCCESS)
            return true;
        std::cerr << file << '(' << line << "): " << call << " failed: "
                  << optixGetErrorName(result) << '\n';
        return false;
    }
}

#define CUDA_CHECK(call) ::RayTracing::Detail::CheckCuda((call), #call, __FILE__, __LINE__)
#define OPTIX_CHECK(call) ::RayTracing::Detail::CheckOptix((call), #call, __FILE__, __LINE__)
