#pragma once

#include <optix_types.h> // CUdeviceptr

#include <cstddef>
#include <span>

// A block of GPU memory (cudaMalloc/cudaFree). Moving hands over ownership;
// an empty buffer has Get() == 0.
class CudaBuffer
{
public:
    CudaBuffer() = default;
    explicit CudaBuffer(size_t sizeInBytes);
    ~CudaBuffer();

    // One CudaBuffer owns its memory: copying would double-free it.
    CudaBuffer(const CudaBuffer&) = delete;
    CudaBuffer& operator=(const CudaBuffer&) = delete;

    CudaBuffer(CudaBuffer&& other) noexcept;
    CudaBuffer& operator=(CudaBuffer&& other) noexcept;

    // A buffer holding a copy of `data`.
    template <typename T>
    static CudaBuffer FromSpan(std::span<const T> data)
    {
        CudaBuffer buffer(data.size_bytes());
        buffer.Upload(data.data(), data.size_bytes());
        return buffer;
    }

    // Copies `sizeInBytes` from CPU memory to the start of the buffer.
    void Upload(const void* data, size_t sizeInBytes);
    // Copies `sizeInBytes` from the start of the buffer to CPU memory.
    void Download(void* data, size_t sizeInBytes) const;

    // Frees the memory; the buffer becomes empty.
    void Release();

    CUdeviceptr Get() const { return m_Ptr; }
    size_t GetSize() const { return m_Size; }

private:
    CUdeviceptr m_Ptr = 0;
    size_t m_Size = 0;
};
