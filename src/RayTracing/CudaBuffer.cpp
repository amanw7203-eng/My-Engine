#include "RayTracing/CudaBuffer.h"

#include "RayTracing/CudaCheck.h"

#include <utility>

CudaBuffer::CudaBuffer(size_t sizeInBytes)
{
    if (sizeInBytes == 0)
        return;

    void* ptr = nullptr;
    if (CUDA_CHECK(cudaMalloc(&ptr, sizeInBytes)))
    {
        m_Ptr = reinterpret_cast<CUdeviceptr>(ptr);
        m_Size = sizeInBytes;
    }
}

CudaBuffer::~CudaBuffer()
{
    Release();
}

CudaBuffer::CudaBuffer(CudaBuffer&& other) noexcept
    : m_Ptr(std::exchange(other.m_Ptr, 0))
    , m_Size(std::exchange(other.m_Size, 0))
{
}

CudaBuffer& CudaBuffer::operator=(CudaBuffer&& other) noexcept
{
    if (this != &other)
    {
        Release();
        m_Ptr = std::exchange(other.m_Ptr, 0);
        m_Size = std::exchange(other.m_Size, 0);
    }
    return *this;
}

void CudaBuffer::Upload(const void* data, size_t sizeInBytes)
{
    if (m_Ptr && sizeInBytes <= m_Size)
        CUDA_CHECK(cudaMemcpy(reinterpret_cast<void*>(m_Ptr), data, sizeInBytes, cudaMemcpyHostToDevice));
}

void CudaBuffer::Download(void* data, size_t sizeInBytes) const
{
    if (m_Ptr && sizeInBytes <= m_Size)
        CUDA_CHECK(cudaMemcpy(data, reinterpret_cast<const void*>(m_Ptr), sizeInBytes, cudaMemcpyDeviceToHost));
}

void CudaBuffer::Release()
{
    if (m_Ptr)
        CUDA_CHECK(cudaFree(reinterpret_cast<void*>(m_Ptr)));
    m_Ptr = 0;
    m_Size = 0;
}
