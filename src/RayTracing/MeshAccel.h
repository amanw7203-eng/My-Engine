#pragma once

#include "RayTracing/CudaBuffer.h"

#include <optix_types.h>

#include <memory>
#include <unordered_map>

class Mesh;
class OptixContext;

// The ray tracing version of a Mesh: its vertices and indices in CUDA memory,
// plus a geometry acceleration structure (GAS), the BVH the RT cores walk to
// find which triangle a ray hits. Built once; meshes never change after
// creation.
//
// The GAS is in the mesh's own (object) space. Entities place it in the
// world through instances in the scene's instance acceleration structure.
class MeshAccel
{
public:
    MeshAccel(const OptixContext& context, const Mesh& mesh);

    // False if building failed (the error has been logged).
    bool IsValid() const { return m_Handle != 0; }

    OptixTraversableHandle GetHandle() const { return m_Handle; }

    // The full Vertex array (position, color, uv, normal) and the triangle
    // indices, for shading a hit in the closest-hit program.
    CUdeviceptr GetVertices() const { return m_Vertices.Get(); }
    CUdeviceptr GetIndices() const { return m_Indices.Get(); }

    // GPU memory used by the acceleration structure, after compaction.
    size_t GetAccelSize() const { return m_Accel.GetSize(); }

private:
    CudaBuffer m_Vertices;
    CudaBuffer m_Indices;
    CudaBuffer m_Accel;
    OptixTraversableHandle m_Handle = 0;
};

// Builds a MeshAccel the first time a Mesh is asked for and keeps it.
// Meshes live as long as the AssetLibrary, so the Mesh pointer is a safe key;
// the cache must be destroyed before the OptixContext.
class MeshAccelCache
{
public:
    explicit MeshAccelCache(const OptixContext& context) : m_Context(context) {}

    // nullptr if the mesh couldn't be built.
    const MeshAccel* Get(const Mesh& mesh);

private:
    const OptixContext& m_Context;
    std::unordered_map<const Mesh*, std::unique_ptr<MeshAccel>> m_Accels;
};
