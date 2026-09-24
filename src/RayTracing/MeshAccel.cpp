#include "RayTracing/MeshAccel.h"

#include "RayTracing/CudaCheck.h"
#include "RayTracing/OptixContext.h"
#include "Renderer/Mesh.h"

#include <cstddef>

// OptiX reads positions straight out of the Vertex array, skipping the
// other attributes with the stride, so position must come first.
static_assert(offsetof(Vertex, position) == 0);

MeshAccel::MeshAccel(const OptixContext& context, const Mesh& mesh)
    : m_Vertices(CudaBuffer::FromSpan(mesh.GetVertices()))
    , m_Indices(CudaBuffer::FromSpan(mesh.GetIndices()))
{
    if (!m_Vertices.Get() || !m_Indices.Get())
        return;

    const CUdeviceptr vertexBuffers[] = { m_Vertices.Get() };
    // One SBT record for the whole mesh. Any-hit is disabled because
    // everything is opaque: the RT cores can stop at the closest hit without
    // calling back into our code.
    const unsigned int geometryFlags[] = { OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT };

    OptixBuildInput input{};
    input.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    OptixBuildInputTriangleArray& triangles = input.triangleArray;
    triangles.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
    triangles.vertexStrideInBytes = sizeof(Vertex);
    triangles.numVertices = static_cast<unsigned int>(mesh.GetVertices().size());
    triangles.vertexBuffers = vertexBuffers;
    triangles.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
    triangles.indexStrideInBytes = 3 * sizeof(unsigned int);
    triangles.numIndexTriplets = static_cast<unsigned int>(mesh.GetIndices().size() / 3);
    triangles.indexBuffer = m_Indices.Get();
    triangles.flags = geometryFlags;
    triangles.numSbtRecords = 1;

    OptixAccelBuildOptions options{};
    // Compaction: build into a worst-case-sized buffer, then copy into one
    // that fits exactly (typically about half the size).
    options.buildFlags = OPTIX_BUILD_FLAG_ALLOW_COMPACTION | OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
    options.operation = OPTIX_BUILD_OPERATION_BUILD;

    OptixAccelBufferSizes sizes{};
    if (!OPTIX_CHECK(optixAccelComputeMemoryUsage(context.Get(), &options, &input, 1, &sizes)))
        return;

    CudaBuffer temp(sizes.tempSizeInBytes);
    CudaBuffer uncompacted(sizes.outputSizeInBytes);
    CudaBuffer compactedSize(sizeof(size_t));
    if (!temp.Get() || !uncompacted.Get() || !compactedSize.Get())
        return;

    // Have the build write the size the compacted GAS will need.
    OptixAccelEmitDesc emit{};
    emit.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
    emit.result = compactedSize.Get();

    OptixTraversableHandle handle = 0;
    if (!OPTIX_CHECK(optixAccelBuild(context.Get(), context.GetStream(), &options, &input, 1,
                                     temp.Get(), temp.GetSize(),
                                     uncompacted.Get(), uncompacted.GetSize(),
                                     &handle, &emit, 1)))
        return;
    if (!CUDA_CHECK(cudaStreamSynchronize(context.GetStream())))
        return;

    size_t compactedBytes = 0;
    compactedSize.Download(&compactedBytes, sizeof(compactedBytes));

    if (compactedBytes > 0 && compactedBytes < uncompacted.GetSize())
    {
        CudaBuffer compacted(compactedBytes);
        OptixTraversableHandle compactedHandle = 0;
        if (compacted.Get() &&
            OPTIX_CHECK(optixAccelCompact(context.Get(), context.GetStream(), handle,
                                          compacted.Get(), compacted.GetSize(), &compactedHandle)) &&
            CUDA_CHECK(cudaStreamSynchronize(context.GetStream())))
        {
            m_Accel = std::move(compacted);
            m_Handle = compactedHandle;
            return;
        }
        // Compaction failed: the uncompacted GAS below still works.
    }

    m_Accel = std::move(uncompacted);
    m_Handle = handle;
}

const MeshAccel* MeshAccelCache::Get(const Mesh& mesh)
{
    auto it = m_Accels.find(&mesh);
    if (it == m_Accels.end())
        it = m_Accels.emplace(&mesh, std::make_unique<MeshAccel>(m_Context, mesh)).first;

    // A failed build stays in the cache so it isn't retried every frame.
    return it->second->IsValid() ? it->second.get() : nullptr;
}
