#include "RayTracing/RayTracer.h"

#include "RayTracing/CudaCheck.h"
#include "RayTracing/DeviceTypes.h"
#include "RayTracing/OptixContext.h"
#include "RayTracing/SceneAccel.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace
{
    // A shader binding table record: which program group to run. The
    // header is filled in by optixSbtRecordPackHeader; our programs read
    // everything else from the launch params, so there is no data after it.
    struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) SbtRecord
    {
        char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    };

    // OptiX fills this with compiler / linker messages. Printed only when
    // something fails: on success it is just statistics.
    struct Log
    {
        char text[2048] = {};
        size_t size = sizeof(text);

        void Print(const char* what) const
        {
            if (size > 1 && text[0] != '\0')
                std::cerr << what << ":\n" << text << '\n';
        }
    };

    float3 ToFloat3(const glm::vec3& v)
    {
        return { v.x, v.y, v.z };
    }

    // GLM stores matrices column by column ([column][row]); the programs
    // read them row by row.
    void ToRows(const glm::mat4& m, float (&rows)[16])
    {
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                rows[row * 4 + column] = m[column][row];
    }
}

RayTracer::RayTracer(const OptixContext& context, const std::filesystem::path& programsFile)
    : m_Context(context)
    , m_Denoiser(context)
{
    if (!m_Context.IsValid())
        return;

    if (!CreatePipeline(programsFile))
    {
        Release();
        return;
    }
    CreateShaderBindingTables();
    m_LaunchParams = CudaBuffer(sizeof(LaunchParams));
    if (!m_RaygenRecords.Get() || !m_MissRecords.Get() || !m_HitGroupRecords.Get() || !m_LaunchParams.Get())
    {
        Release();
        return;
    }

    for (cudaEvent_t& event : m_Events)
        CUDA_CHECK(cudaEventCreate(&event));
}

RayTracer::~RayTracer()
{
    for (cudaEvent_t event : m_Events)
        if (event)
            cudaEventDestroy(event);
    Release();
}

bool RayTracer::CreatePipeline(const std::filesystem::path& programsFile)
{
    std::ifstream file(programsFile, std::ios::binary);
    if (!file)
    {
        std::cerr << "Can't open OptiX programs: " << programsFile.string() << '\n';
        return false;
    }
    const std::vector<char> ir((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // Must match how the programs are used: Programs.cu and SceneAccel.
    OptixPipelineCompileOptions pipelineOptions{};
    pipelineOptions.usesMotionBlur = 0;
    pipelineOptions.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
    pipelineOptions.numPayloadValues = 2;   // a pointer to a SurfaceHit, in two halves
    pipelineOptions.numAttributeValues = 2; // triangle barycentrics
    pipelineOptions.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
    pipelineOptions.pipelineLaunchParamsVariableName = "params";
    pipelineOptions.usesPrimitiveTypeFlags = static_cast<unsigned int>(OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE);

    OptixModuleCompileOptions moduleOptions{};
    moduleOptions.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    // Keeps the line info nvcc's -lineinfo adds, for Nsight, at no speed cost.
    moduleOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_MINIMAL;

    Log log;
    if (!OPTIX_CHECK(optixModuleCreate(m_Context.Get(), &moduleOptions, &pipelineOptions,
                                       ir.data(), ir.size(), log.text, &log.size, &m_Module)))
    {
        log.Print("OptiX module");
        return false;
    }

    // One program group per entry point in Programs.cu.
    constexpr unsigned int kGroupCount = 6;
    OptixProgramGroupDesc descs[kGroupCount] = {};
    descs[0].kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    descs[0].raygen.module = m_Module;
    descs[0].raygen.entryFunctionName = "__raygen__trace";
    descs[1].kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    descs[1].raygen.module = m_Module;
    descs[1].raygen.entryFunctionName = "__raygen__present";
    descs[2].kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    descs[2].raygen.module = m_Module;
    descs[2].raygen.entryFunctionName = "__raygen__downsample";
    // Miss programs, in RayType order.
    descs[3].kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    descs[3].miss.module = m_Module;
    descs[3].miss.entryFunctionName = "__miss__surface";
    descs[4].kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    descs[4].miss.module = m_Module;
    descs[4].miss.entryFunctionName = "__miss__shadow";
    descs[5].kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    descs[5].hitgroup.moduleCH = m_Module;
    descs[5].hitgroup.entryFunctionNameCH = "__closesthit__surface";

    OptixProgramGroupOptions groupOptions{};
    OptixProgramGroup groups[kGroupCount] = {};
    log = Log();
    if (!OPTIX_CHECK(optixProgramGroupCreate(m_Context.Get(), descs, kGroupCount, &groupOptions,
                                             log.text, &log.size, groups)))
    {
        log.Print("OptiX program groups");
        return false;
    }
    m_RaygenTrace = groups[0];
    m_RaygenPresent = groups[1];
    m_RaygenDownsample = groups[2];
    m_MissSurface = groups[3];
    m_MissShadow = groups[4];
    m_HitGroup = groups[5];

    OptixPipelineLinkOptions linkOptions{};
    // Every ray is traced from raygen; the closest-hit program traces none.
    linkOptions.maxTraceDepth = 1;

    log = Log();
    if (!OPTIX_CHECK(optixPipelineCreate(m_Context.Get(), &pipelineOptions, &linkOptions,
                                         groups, kGroupCount, log.text, &log.size, &m_Pipeline)))
    {
        log.Print("OptiX pipeline");
        m_Pipeline = nullptr;
        return false;
    }

    // Trace depth 1, no callables; the scene is IAS -> GAS, a graph depth of 2.
    if (!OPTIX_CHECK(optixPipelineSetStackSizeFromCallDepths(m_Pipeline, 1, 0, 0, 0, 2)))
        return false;

    return true;
}

void RayTracer::CreateShaderBindingTables()
{
    SbtRecord raygen[3]{};
    OPTIX_CHECK(optixSbtRecordPackHeader(m_RaygenTrace, &raygen[0]));
    OPTIX_CHECK(optixSbtRecordPackHeader(m_RaygenPresent, &raygen[1]));
    OPTIX_CHECK(optixSbtRecordPackHeader(m_RaygenDownsample, &raygen[2]));
    m_RaygenRecords = CudaBuffer::FromSpan(std::span<const SbtRecord>(raygen));

    // Indexed by the miss index passed to optixTrace, i.e. by RayType.
    SbtRecord miss[2]{};
    OPTIX_CHECK(optixSbtRecordPackHeader(m_MissSurface, &miss[RAY_TYPE_SURFACE]));
    OPTIX_CHECK(optixSbtRecordPackHeader(m_MissShadow, &miss[RAY_TYPE_SHADOW]));
    m_MissRecords = CudaBuffer::FromSpan(std::span<const SbtRecord>(miss));

    // A single hit group for everything: every instance has sbtOffset 0,
    // every mesh one SBT record, and every trace an SBT stride of 1.
    SbtRecord hitGroup{};
    OPTIX_CHECK(optixSbtRecordPackHeader(m_HitGroup, &hitGroup));
    m_HitGroupRecords = CudaBuffer::FromSpan(std::span<const SbtRecord>(&hitGroup, 1));

    m_TraceSbt = {};
    m_TraceSbt.raygenRecord = m_RaygenRecords.Get();
    m_TraceSbt.missRecordBase = m_MissRecords.Get();
    m_TraceSbt.missRecordStrideInBytes = sizeof(SbtRecord);
    m_TraceSbt.missRecordCount = 2;
    m_TraceSbt.hitgroupRecordBase = m_HitGroupRecords.Get();
    m_TraceSbt.hitgroupRecordStrideInBytes = sizeof(SbtRecord);
    m_TraceSbt.hitgroupRecordCount = 1;

    m_PresentSbt = m_TraceSbt;
    m_PresentSbt.raygenRecord = m_RaygenRecords.Get() + sizeof(SbtRecord);
    m_DownsampleSbt = m_TraceSbt;
    m_DownsampleSbt.raygenRecord = m_RaygenRecords.Get() + 2 * sizeof(SbtRecord);
}

bool RayTracer::ResizeFrameBuffers(int width, int height)
{
    const size_t pixels = static_cast<size_t>(width) * height;
    const size_t bytes = pixels * sizeof(float4);
    if (m_Denoised.GetSize() != bytes)
    {
        for (int i = 0; i < 2; ++i)
        {
            m_Direct[i] = CudaBuffer(bytes);
            m_Indirect[i] = CudaBuffer(bytes);
            m_Specular[i] = CudaBuffer(bytes);
            m_SpecularAlbedo[i] = CudaBuffer(bytes);
            m_Albedo[i] = CudaBuffer(bytes);
            m_Normal[i] = CudaBuffer(bytes);
            m_Position[i] = CudaBuffer(bytes);
        }
        m_DenoiserInput = CudaBuffer(bytes);
        m_SpecularDenoiserInput = CudaBuffer(bytes);
        m_GuideNormal = CudaBuffer(bytes);
        m_Flow = CudaBuffer(pixels * sizeof(float2));
        m_FlowTrust = CudaBuffer(pixels * sizeof(float));
        m_Denoised = CudaBuffer(bytes);
        m_SpecularDenoised = CudaBuffer(bytes);

        // Half resolution, rounded up so an odd row or column is covered.
        const size_t halfPixels = static_cast<size_t>((width + 1) / 2) * ((height + 1) / 2);
        m_HalfInput = CudaBuffer(halfPixels * sizeof(float4));
        m_HalfSpecularInput = CudaBuffer(halfPixels * sizeof(float4));
        m_HalfAlbedo = CudaBuffer(halfPixels * sizeof(float4));
        m_HalfNormal = CudaBuffer(halfPixels * sizeof(float4));
        m_HalfFlow = CudaBuffer(halfPixels * sizeof(float2));
        m_HalfFlowTrust = CudaBuffer(halfPixels * sizeof(float));
        m_HalfDenoised = CudaBuffer(halfPixels * sizeof(float4));
        m_HalfSpecularDenoised = CudaBuffer(halfPixels * sizeof(float4));

        // Nothing from before the resize lines up with the new pixels.
        ResetHistory();
        m_DenoiserRunning = false;
    }

    for (int i = 0; i < 2; ++i)
        if (!m_Direct[i].Get() || !m_Indirect[i].Get() || !m_Specular[i].Get() || !m_SpecularAlbedo[i].Get() ||
            !m_Albedo[i].Get() || !m_Normal[i].Get() ||
            !m_Position[i].Get())
            return false;
    for (const CudaBuffer* buffer : { &m_DenoiserInput, &m_GuideNormal, &m_Flow, &m_FlowTrust, &m_Denoised,
                                      &m_HalfInput, &m_HalfAlbedo, &m_HalfNormal, &m_HalfFlow, &m_HalfFlowTrust,
                                      &m_HalfDenoised, &m_SpecularDenoiserInput, &m_SpecularDenoised,
                                      &m_HalfSpecularInput, &m_HalfSpecularDenoised })
        if (!buffer->Get())
            return false;
    return true;
}

void RayTracer::ResetHistory()
{
    m_HistoryValid = false;
    m_StillFrames = 0;
    m_Denoiser.StartNewSequence();
}

void RayTracer::ReadTimings()
{
    // The last event is recorded after the rest, so once it has happened
    // they all have. Not finished yet: keep the older numbers.
    if (!m_TimingPending || cudaEventQuery(m_Events[3]) != cudaSuccess)
        return;
    m_TimingPending = false;

    cudaEventElapsedTime(&m_Timings.trace, m_Events[0], m_Events[1]);
    cudaEventElapsedTime(&m_Timings.denoise, m_Events[1], m_Events[2]);
    cudaEventElapsedTime(&m_Timings.present, m_Events[2], m_Events[3]);
}

void RayTracer::Render(const SceneAccel& scene, const FrameSettings& settings, int width, int height)
{
    if (!IsValid() || !m_Output.Resize(width, height) || !ResizeFrameBuffers(width, height))
        return;
    ReadTimings();

    // --- What changed since last frame decides what history to keep ---
    // Motion is followed: the trace finds where each pixel's surface was
    // last frame, whether the camera or the object moved, and keeps a short
    // history. (Scene edits count as motion: an edited material fades in
    // over that short history.) What reprojection can't follow is a change
    // of lighting or render settings, which alters every pixel: start over.
    // Denoising and history length only affect how frames are combined.
    FrameSettings sameView = settings;
    sameView.view = m_PreviousSettings.view;
    sameView.projection = m_PreviousSettings.projection;
    sameView.denoise = m_PreviousSettings.denoise;
    sameView.denoiseHalfResolution = m_PreviousSettings.denoiseHalfResolution;
    sameView.movingHistoryFrames = m_PreviousSettings.movingHistoryFrames;
    sameView.pointLights = m_PreviousSettings.pointLights;
    const bool lightingChanged = !(sameView == m_PreviousSettings);
    const bool cameraMoved = settings.view != m_PreviousSettings.view ||
                             settings.projection != m_PreviousSettings.projection;
    const bool sceneMoved = scene.GetContentHash() != m_PreviousSceneHash ||
                            settings.pointLights != m_PreviousSettings.pointLights;
    if (lightingChanged)
        ResetHistory();
    if (!(settings == m_PreviousSettings) || sceneMoved)
        m_StillFrames = 0;
    m_PreviousSettings = settings;
    m_PreviousSceneHash = scene.GetContentHash();

    // Converged: the texture already holds the final image.
    if (m_StillFrames >= kMaxAccumulatedFrames)
    {
        m_Output.BlitToScreen();
        return;
    }

    // --- Denoising ---
    const bool halfResolution = settings.denoiseHalfResolution;
    const unsigned int halfWidth = static_cast<unsigned int>(width + 1) / 2;
    const unsigned int halfHeight = static_cast<unsigned int>(height + 1) / 2;
    const bool denoise = settings.denoise && (halfResolution ? m_Denoiser.Resize(halfWidth, halfHeight)
                                                             : m_Denoiser.Resize(width, height));
    // The denoiser can only continue from its last result if that was
    // made recently, in the same mode.
    if (!denoise || halfResolution != m_DenoiserWasHalfResolution)
        m_DenoiserRunning = false;
    if (denoise && !m_DenoiserRunning)
        m_Denoiser.StartNewSequence();
    // With nothing moving, the history converges on its own and changes
    // little from frame to frame, so after a short while the denoiser only
    // runs every few frames; its last result is shown in between.
    constexpr unsigned int kStillFramesDenoisedEveryFrame = 16;
    constexpr unsigned int kStillDenoiseInterval = 8;
    const bool nothingMoved = !cameraMoved && !sceneMoved;
    const bool runDenoiser = denoise && (!m_DenoiserRunning || !nothingMoved ||
                                         m_StillFrames < kStillFramesDenoisedEveryFrame ||
                                         m_StillFrames % kStillDenoiseInterval == 0);

    const int previous = m_Current;
    const int current = 1 - m_Current;

    const cudaStream_t stream = m_Context.GetStream();
    uchar4* pixels = m_Output.Map(stream);
    if (!pixels)
        return;

    const glm::mat4 viewProjection = settings.projection * settings.view;
    auto asFloat4 = [](const CudaBuffer& buffer) { return reinterpret_cast<float4*>(buffer.Get()); };

    LaunchParams params{};
    params.output = pixels;
    params.presentDirect = asFloat4(m_Direct[current]);
    params.presentAlbedo = asFloat4(m_Albedo[current]);
    params.presentIndirect = asFloat4(!denoise ? m_Indirect[current] : halfResolution ? m_HalfDenoised : m_Denoised);
    params.upsampleIndirect = denoise && halfResolution;
    params.presentSpecular = asFloat4(m_Specular[current]);
    // Reflections only go through the denoiser when there are any.
    params.presentSpecularDenoised = denoise && settings.reflections
        ? asFloat4(halfResolution ? m_HalfSpecularDenoised : m_SpecularDenoised)
        : nullptr;
    params.presentSpecularAlbedo = asFloat4(m_SpecularAlbedo[current]);

    params.halfInput = asFloat4(m_HalfInput);
    params.halfSpecularInput = asFloat4(m_HalfSpecularInput);
    params.halfAlbedo = asFloat4(m_HalfAlbedo);
    params.halfNormal = asFloat4(m_HalfNormal);
    params.halfFlow = reinterpret_cast<float2*>(m_HalfFlow.Get());
    params.halfFlowTrust = reinterpret_cast<float*>(m_HalfFlowTrust.Get());
    params.halfWidth = halfWidth;
    params.halfHeight = halfHeight;

    params.directIn = asFloat4(m_Direct[previous]);
    params.directOut = asFloat4(m_Direct[current]);
    params.indirectIn = asFloat4(m_Indirect[previous]);
    params.indirectOut = asFloat4(m_Indirect[current]);
    params.specularIn = asFloat4(m_Specular[previous]);
    params.specularOut = asFloat4(m_Specular[current]);
    params.specularAlbedoIn = asFloat4(m_SpecularAlbedo[previous]);
    params.specularAlbedoOut = asFloat4(m_SpecularAlbedo[current]);
    params.albedoIn = asFloat4(m_Albedo[previous]);
    params.albedoOut = asFloat4(m_Albedo[current]);
    params.normalIn = asFloat4(m_Normal[previous]);
    params.normalOut = asFloat4(m_Normal[current]);
    params.positionIn = asFloat4(m_Position[previous]);
    params.positionOut = asFloat4(m_Position[current]);
    params.denoiserInput = asFloat4(m_DenoiserInput);
    params.specularDenoiserInput = asFloat4(m_SpecularDenoiserInput);
    params.guideNormal = asFloat4(m_GuideNormal);
    params.flow = reinterpret_cast<float2*>(m_Flow.Get());
    params.flowTrust = reinterpret_cast<float*>(m_FlowTrust.Get());

    params.width = static_cast<unsigned int>(width);
    params.height = static_cast<unsigned int>(height);
    params.randomSeed = m_RandomSeed++;
    params.samplesPerPixel = static_cast<unsigned int>((std::max)(settings.samplesPerPixel, 1));
    params.maxBounces = static_cast<unsigned int>((std::max)(settings.maxBounces, 0));
    params.historyValid = m_HistoryValid;
    params.nothingMoved = nothingMoved;
    params.historyLimit = params.nothingMoved
        ? kMaxAccumulatedFrames
        : static_cast<unsigned int>((std::max)(settings.movingHistoryFrames, 1));

    ToRows(glm::inverse(viewProjection), params.inverseViewProjection);
    ToRows(viewProjection, params.viewProjection);
    ToRows(m_PreviousViewProjection, params.previousViewProjection);
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            params.viewRotation[row * 3 + column] = settings.view[column][row];
    params.cameraPosition = ToFloat3(glm::vec3(glm::inverse(settings.view)[3]));
    // projection[1][1] is 1 / tan(half the vertical field of view), so the
    // view spans 2 / projection[1][1] (in tangent units) over `height` pixels.
    params.pixelSpreadAngle = 2.0f / (settings.projection[1][1] * static_cast<float>(height));

    params.lightDir = ToFloat3(settings.lightDir);
    params.lightColor = ToFloat3(settings.lightColor);
    params.ambientColor = ToFloat3(settings.ambientColor);
    params.background = ToFloat3(settings.background);

    // Point lights: uploaded every frame (a handful of bytes each).
    std::vector<DevicePointLight> pointLights;
    pointLights.reserve(settings.pointLights.size());
    for (const ScenePointLight& light : settings.pointLights)
        pointLights.push_back({ ToFloat3(light.position), ToFloat3(light.radiance), light.range, light.castShadows ? 1 : 0 });
    const size_t pointLightBytes = pointLights.size() * sizeof(DevicePointLight);
    if (m_PointLights.GetSize() < pointLightBytes)
        m_PointLights = CudaBuffer(pointLightBytes);
    if (!pointLights.empty() && m_PointLights.Get())
    {
        m_PointLights.Upload(pointLights.data(), pointLightBytes);
        params.pointLights = reinterpret_cast<const DevicePointLight*>(m_PointLights.Get());
        params.pointLightCount = static_cast<unsigned int>(pointLights.size());
    }
    params.shadowsEnabled = settings.shadowsEnabled;
    params.reflections = settings.reflections;
    params.cullBackFaces = settings.cullBackFaces;
    params.scene = scene.GetHandle();
    params.instances = scene.GetInstanceData();

    m_LaunchParams.Upload(&params, sizeof(params));

    // Trace -> (downsample ->) denoise -> present, all queued on the stream
    // in order.
    cudaEventRecord(m_Events[0], stream);
    const bool traced = OPTIX_CHECK(optixLaunch(m_Pipeline, stream, m_LaunchParams.Get(), sizeof(LaunchParams),
                                                &m_TraceSbt, params.width, params.height, 1));
    cudaEventRecord(m_Events[1], stream);
    if (traced && runDenoiser)
    {
        // Only the indirect light: the rest has no noise to remove. The
        // trace picked, per pixel, our average or this frame's alone
        // (see LaunchParams::denoiserInput).
        Denoiser::Images images{};
        if (halfResolution)
        {
            const bool downsampled =
                OPTIX_CHECK(optixLaunch(m_Pipeline, stream, m_LaunchParams.Get(), sizeof(LaunchParams),
                                        &m_DownsampleSbt, halfWidth, halfHeight, 1));
            images.input = m_HalfInput.Get();
            images.albedo = m_HalfAlbedo.Get();
            images.normal = m_HalfNormal.Get();
            images.flow = m_HalfFlow.Get();
            images.flowTrust = m_HalfFlowTrust.Get();
            images.output = m_HalfDenoised.Get();
            if (settings.reflections)
            {
                images.secondInput = m_HalfSpecularInput.Get();
                images.secondOutput = m_HalfSpecularDenoised.Get();
            }
            m_DenoiserRunning = downsampled && m_Denoiser.Run(images);
        }
        else
        {
            images.input = m_DenoiserInput.Get();
            images.albedo = m_Albedo[current].Get();
            images.normal = m_GuideNormal.Get();
            images.flow = m_Flow.Get();
            images.flowTrust = m_FlowTrust.Get();
            images.output = m_Denoised.Get();
            if (settings.reflections)
            {
                images.secondInput = m_SpecularDenoiserInput.Get();
                images.secondOutput = m_SpecularDenoised.Get();
            }
            m_DenoiserRunning = m_Denoiser.Run(images);
        }
        m_DenoiserWasHalfResolution = halfResolution;
    }
    cudaEventRecord(m_Events[2], stream);
    if (traced)
    {
        OPTIX_CHECK(optixLaunch(m_Pipeline, stream, m_LaunchParams.Get(), sizeof(LaunchParams),
                                &m_PresentSbt, params.width, params.height, 1));
    }
    cudaEventRecord(m_Events[3], stream);
    m_TimingPending = true;

    // Unmap makes GL wait for the launches before it uses the texture.
    m_Output.Unmap(stream);
    m_Output.BlitToScreen();

    if (traced)
    {
        m_Current = current;
        m_HistoryValid = true;
        m_PreviousViewProjection = viewProjection;
        ++m_StillFrames;
    }
}

void RayTracer::Release()
{
    if (m_Pipeline)
        optixPipelineDestroy(m_Pipeline);
    for (OptixProgramGroup group :
         { m_RaygenTrace, m_RaygenPresent, m_RaygenDownsample, m_MissSurface, m_MissShadow, m_HitGroup })
        if (group)
            optixProgramGroupDestroy(group);
    if (m_Module)
        optixModuleDestroy(m_Module);

    m_Pipeline = nullptr;
    m_RaygenTrace = m_RaygenPresent = m_RaygenDownsample = m_MissSurface = m_MissShadow = m_HitGroup = nullptr;
    m_Module = nullptr;
}
