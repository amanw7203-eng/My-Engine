#include "RayTracing/SceneAccel.h"

#include "Assets/Material.h"
#include "RayTracing/CudaCheck.h"
#include "RayTracing/MeshAccel.h"
#include "RayTracing/OptixContext.h"
#include "RayTracing/TextureCache.h"
#include "Renderer/ColorSpace.h"
#include "Renderer/Mesh.h"
#include "Renderer/Texture.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

// The OptiX programs read the Vertex array uploaded by MeshAccel as DeviceVertex.
static_assert(sizeof(DeviceVertex) == sizeof(Vertex));
static_assert(offsetof(DeviceVertex, position) == offsetof(Vertex, position));
static_assert(offsetof(DeviceVertex, color) == offsetof(Vertex, color));
static_assert(offsetof(DeviceVertex, u) == offsetof(Vertex, uv));
static_assert(offsetof(DeviceVertex, normal) == offsetof(Vertex, normal));

// The content hash reads InstanceData as raw bytes, so it must have no
// padding (whose bytes are undefined): 16 pointer bytes, 8 material
// floats, 12 transform floats, 6 texture handles, 12 map/UV floats.
static_assert(sizeof(InstanceData) == 16 + 8 * 4 + 12 * 4 + 6 * 8 + 12 * 4);

namespace
{
    // Makes `buffer` at least `size` bytes. Contents are not kept.
    void EnsureSize(CudaBuffer& buffer, size_t size)
    {
        if (buffer.GetSize() < size)
            buffer = CudaBuffer(size);
    }

    float3 ToFloat3(const glm::vec3& v)
    {
        return { v.x, v.y, v.z };
    }

    // FNV-1a over raw bytes. Fine for the structs hashed here, which have
    // no hidden padding (OptixInstance pads explicitly, InstanceData packs).
    unsigned long long HashBytes(const void* data, size_t size, unsigned long long hash)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * 1099511628211ull;
        return hash;
    }
}

void SceneAccel::Build(const Scene& scene, MeshAccelCache& meshAccels, TextureCache& textures,
                       const Material& defaultMaterial)
{
    m_HostInstances.clear();
    m_HostInstanceData.clear();
    // This frame's transforms become next frame's "previous" ones; entities
    // that are gone drop out.
    std::swap(m_PreviousTransforms, m_CurrentTransforms);
    m_CurrentTransforms.clear();

    scene.ForEachDrawable([&](Entity& entity, const glm::mat4& world)
    {
        const MeshAccel* accel = meshAccels.Get(*entity.mesh);
        if (!accel)
            return;

        const Material& material = entity.material ? *entity.material : defaultMaterial;

        OptixInstance instance{};
        // OptiX wants the top 3 rows of the matrix, row by row; GLM stores
        // it column by column (world[column][row]).
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column)
                instance.transform[row * 4 + column] = world[column][row];

        instance.instanceId = static_cast<unsigned int>(m_HostInstanceData.size());
        instance.sbtOffset = 0; // every instance uses the same hit programs
        instance.visibilityMask = 0xFF;
        instance.traversableHandle = accel->GetHandle();

        // Same rules as the raster path in Scene::Draw. OptiX decides which
        // side of a triangle is the front in object space, so a mirrored
        // entity must flip it or back-face culling would hide its outside.
        instance.flags = OPTIX_INSTANCE_FLAG_NONE;
        if (glm::determinant(glm::mat3(world)) < 0.0f)
            instance.flags |= OPTIX_INSTANCE_FLAG_FLIP_TRIANGLE_FACING;
        if (material.doubleSided)
            instance.flags |= OPTIX_INSTANCE_FLAG_DISABLE_TRIANGLE_FACE_CULLING;

        m_HostInstances.push_back(instance);

        InstanceData& data = m_HostInstanceData.emplace_back();
        data.vertices = reinterpret_cast<const DeviceVertex*>(accel->GetVertices());
        data.indices = reinterpret_cast<const uint3*>(accel->GetIndices());
        // The color pickers show sRGB; lighting works in linear light.
        data.baseColor = ToFloat3(SrgbToLinear(material.baseColor));
        data.emission = ToFloat3(SrgbToLinear(material.emissionColor) * material.emissionStrength);
        data.metallic = material.metallic;
        data.roughness = material.roughness;

        // Texture maps, in MaterialMap order. The color space setting picks
        // which of the two CUDA views of the image to read through.
        const Texture* const maps[MAP_COUNT] = { material.baseColorMap, material.metallicMap, material.roughnessMap,
                                                 material.normalMap, material.aoMap, material.emissionMap };
        for (unsigned int map = 0; map < MAP_COUNT; ++map)
        {
            if (!maps[map])
                continue;
            const TextureCache::Entry& texture = textures.Get(*maps[map]);
            data.maps[map] = maps[map]->colorSpace == Texture::ColorSpace::Srgb ? texture.srgb : texture.linear;
            data.mapLog2Size[map] = texture.log2Size;
        }
        data.normalStrength = material.normalStrength;
        data.aoStrength = material.aoStrength;
        data.uvTiling[0] = material.tiling.x;
        data.uvTiling[1] = material.tiling.y;
        data.uvOffset[0] = material.offset.x;
        data.uvOffset[1] = material.offset.y;

        // Where the entity was last frame; one that just appeared was here.
        Transform3x4 current;
        std::copy(std::begin(instance.transform), std::end(instance.transform), current.begin());
        const auto previous = m_PreviousTransforms.find(&entity);
        const Transform3x4& before = previous != m_PreviousTransforms.end() ? previous->second : current;
        std::copy(before.begin(), before.end(), std::begin(data.previousTransform));
        m_CurrentTransforms[&entity] = current;
    });
    // Frees the CUDA copies of textures no material in the scene uses any more.
    textures.EndFrame();

    // Instances hold the transforms and meshes, instance data the materials.
    m_ContentHash = HashBytes(m_HostInstances.data(), m_HostInstances.size() * sizeof(OptixInstance),
                              14695981039346656037ull);
    m_ContentHash = HashBytes(m_HostInstanceData.data(), m_HostInstanceData.size() * sizeof(InstanceData),
                              m_ContentHash);

    m_Handle = 0;
    if (m_HostInstances.empty())
        return;

    // cudaMemcpy waits for work already queued on the context's stream, so
    // last frame's build and trace are done with these buffers first.
    const size_t instanceBytes = m_HostInstances.size() * sizeof(OptixInstance);
    const size_t dataBytes = m_HostInstanceData.size() * sizeof(InstanceData);
    EnsureSize(m_Instances, instanceBytes);
    EnsureSize(m_InstanceData, dataBytes);
    if (!m_Instances.Get() || !m_InstanceData.Get())
        return;
    m_Instances.Upload(m_HostInstances.data(), instanceBytes);
    m_InstanceData.Upload(m_HostInstanceData.data(), dataBytes);

    OptixBuildInput input{};
    input.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
    input.instanceArray.instances = m_Instances.Get();
    input.instanceArray.numInstances = static_cast<unsigned int>(m_HostInstances.size());

    OptixAccelBuildOptions options{};
    // Rebuilt every frame, so favour build speed over trace speed.
    options.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_BUILD;
    options.operation = OPTIX_BUILD_OPERATION_BUILD;

    OptixAccelBufferSizes sizes{};
    if (!OPTIX_CHECK(optixAccelComputeMemoryUsage(m_Context.Get(), &options, &input, 1, &sizes)))
        return;

    EnsureSize(m_Temp, sizes.tempSizeInBytes);
    EnsureSize(m_Accel, sizes.outputSizeInBytes);
    if (!m_Temp.Get() || !m_Accel.Get())
        return;

    OptixTraversableHandle handle = 0;
    if (OPTIX_CHECK(optixAccelBuild(m_Context.Get(), m_Context.GetStream(), &options, &input, 1,
                                    m_Temp.Get(), m_Temp.GetSize(),
                                    m_Accel.Get(), m_Accel.GetSize(),
                                    &handle, nullptr, 0)))
        m_Handle = handle;
}
