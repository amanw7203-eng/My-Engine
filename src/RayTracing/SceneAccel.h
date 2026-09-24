#pragma once

#include "RayTracing/CudaBuffer.h"
#include "RayTracing/DeviceTypes.h"

#include <optix_types.h>

#include <array>
#include <unordered_map>
#include <vector>

class Entity;
class MeshAccelCache;
class OptixContext;
class Scene;
struct Material;

// The whole scene as an instance acceleration structure (IAS): one instance
// per drawn entity, each pointing at its mesh's GAS and placed in the world
// by the entity's world matrix. Rays are traced against GetHandle().
//
// Rebuilt from scratch every frame. That is cheap (it only sorts instance
// bounding boxes; the meshes' GASes are reused) and means any edit to the
// scene is picked up without tracking what changed.
class SceneAccel
{
public:
    explicit SceneAccel(const OptixContext& context) : m_Context(context) {}

    // Rebuilds from the entities Scene::Draw would draw. The build is queued
    // on the context's stream, so work queued after it sees the result.
    void Build(const Scene& scene, MeshAccelCache& meshAccels, const Material& defaultMaterial);

    // 0 for an empty scene: rays traced against it just miss.
    OptixTraversableHandle GetHandle() const { return m_Handle; }

    // InstanceData array on the GPU, indexed by optixGetInstanceId().
    const InstanceData* GetInstanceData() const
    {
        return reinterpret_cast<const InstanceData*>(m_InstanceData.Get());
    }
    size_t GetInstanceCount() const { return m_HostInstances.size(); }

    // Changes whenever anything the ray tracer sees changes: an entity
    // added, removed, moved or hidden, or a material edited. Tells the ray
    // tracer the scene is in motion, so it keeps only a short history.
    unsigned long long GetContentHash() const { return m_ContentHash; }

private:
    const OptixContext& m_Context;

    // CPU staging, kept between frames to avoid reallocating.
    std::vector<OptixInstance> m_HostInstances;
    std::vector<InstanceData> m_HostInstanceData;

    // GPU buffers only ever grow, so a steady scene allocates nothing per frame.
    CudaBuffer m_Instances;
    CudaBuffer m_InstanceData;
    CudaBuffer m_Temp;
    CudaBuffer m_Accel;
    OptixTraversableHandle m_Handle = 0;
    unsigned long long m_ContentHash = 0;

    // Each drawn entity's transform (OptixInstance layout) this frame and
    // last, for motion. Keyed by address: entities live at a stable one.
    using Transform3x4 = std::array<float, 12>;
    std::unordered_map<const Entity*, Transform3x4> m_CurrentTransforms;
    std::unordered_map<const Entity*, Transform3x4> m_PreviousTransforms;
};
