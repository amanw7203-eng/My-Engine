#include "RayTracing/Denoiser.h"

#include "RayTracing/CudaCheck.h"
#include "RayTracing/OptixContext.h"

namespace
{
    // A width x height image with pixels `pixelSize` bytes apart, of
    // which `format` is read.
    OptixImage2D Image(CUdeviceptr data, unsigned int width, unsigned int height, size_t pixelSize,
                       OptixPixelFormat format)
    {
        OptixImage2D image{};
        image.data = data;
        image.width = width;
        image.height = height;
        image.pixelStrideInBytes = static_cast<unsigned int>(pixelSize);
        image.rowStrideInBytes = static_cast<unsigned int>(width * pixelSize);
        image.format = format;
        return image;
    }

    // float4 pixels, of which the RGB is read.
    OptixImage2D Rgb(CUdeviceptr data, unsigned int width, unsigned int height)
    {
        return Image(data, width, height, 4 * sizeof(float), OPTIX_PIXEL_FORMAT_FLOAT3);
    }
}

Denoiser::Denoiser(const OptixContext& context)
    : m_Context(context)
{
    if (!m_Context.IsValid())
        return;

    OptixDenoiserOptions options{};
    options.guideAlbedo = 1;
    options.guideNormal = 1;
    options.denoiseAlpha = OPTIX_DENOISER_ALPHA_MODE_COPY;

    // For image sequences: uses last frame's result, so it doesn't shimmer.
    if (!OPTIX_CHECK(optixDenoiserCreate(m_Context.Get(), OPTIX_DENOISER_MODEL_KIND_TEMPORAL_AOV, &options,
                                         &m_Denoiser)))
        m_Denoiser = nullptr;
}

Denoiser::~Denoiser()
{
    if (m_Denoiser)
        optixDenoiserDestroy(m_Denoiser);
}

bool Denoiser::Resize(unsigned int width, unsigned int height)
{
    if (!m_Denoiser)
        return false;
    if (width == m_Width && height == m_Height)
        return true;
    m_Width = m_Height = 0;
    m_HasPrevious = false;

    OptixDenoiserSizes sizes{};
    if (!OPTIX_CHECK(optixDenoiserComputeMemoryResources(m_Denoiser, width, height, &sizes)))
        return false;

    // The whole image in one go (no tiles), so no overlap is needed.
    m_State = CudaBuffer(sizes.stateSizeInBytes);
    m_Scratch = CudaBuffer(sizes.withoutOverlapScratchSizeInBytes);
    m_InternalGuidePixelSize = sizes.internalGuideLayerPixelSizeInBytes;
    const size_t guideBytes = static_cast<size_t>(width) * height * m_InternalGuidePixelSize;
    for (CudaBuffer& guide : m_InternalGuide)
    {
        guide = CudaBuffer(guideBytes);
        // The first frame must find either a previous run's data or zeros.
        if (guide.Get())
            CUDA_CHECK(cudaMemset(reinterpret_cast<void*>(guide.Get()), 0, guideBytes));
    }
    if (!m_State.Get() || !m_Scratch.Get() || !m_InternalGuide[0].Get() || !m_InternalGuide[1].Get())
        return false;

    if (!OPTIX_CHECK(optixDenoiserSetup(m_Denoiser, m_Context.GetStream(), width, height,
                                        m_State.Get(), m_State.GetSize(), m_Scratch.Get(), m_Scratch.GetSize())))
        return false;

    m_Width = width;
    m_Height = height;
    return true;
}

bool Denoiser::Run(const Images& images)
{
    if (!m_Denoiser || m_Width == 0)
        return false;

    const int previousGuide = m_CurrentGuide;
    const int currentGuide = 1 - m_CurrentGuide;

    OptixDenoiserGuideLayer guides{};
    guides.albedo = Rgb(images.albedo, m_Width, m_Height);
    guides.normal = Rgb(images.normal, m_Width, m_Height);
    guides.flow = Image(images.flow, m_Width, m_Height, 2 * sizeof(float), OPTIX_PIXEL_FORMAT_FLOAT2);
    guides.flowTrustworthiness = Image(images.flowTrust, m_Width, m_Height, sizeof(float), OPTIX_PIXEL_FORMAT_FLOAT1);
    guides.previousOutputInternalGuideLayer =
        Image(m_InternalGuide[previousGuide].Get(), m_Width, m_Height, m_InternalGuidePixelSize,
              OPTIX_PIXEL_FORMAT_INTERNAL_GUIDE_LAYER);
    guides.outputInternalGuideLayer =
        Image(m_InternalGuide[currentGuide].Get(), m_Width, m_Height, m_InternalGuidePixelSize,
              OPTIX_PIXEL_FORMAT_INTERNAL_GUIDE_LAYER);

    // The main image, and optionally a second one (an AOV, in OptiX's
    // terms) denoised with it.
    OptixDenoiserLayer layers[2] = {};
    unsigned int layerCount = 0;
    auto addLayer = [&](CUdeviceptr input, CUdeviceptr output, OptixDenoiserAOVType type) {
        OptixDenoiserLayer& layer = layers[layerCount++];
        layer.input = Rgb(input, m_Width, m_Height);
        layer.output = Rgb(output, m_Width, m_Height);
        // Last frame's result, which the output buffer still holds (reading
        // it before overwriting is allowed). At the start of a sequence
        // there is none: the noisy input stands in, as the OptiX docs say.
        layer.previousOutput = m_HasPrevious ? layer.output : layer.input;
        layer.type = type;
    };
    addLayer(images.input, images.output, OPTIX_DENOISER_AOV_TYPE_BEAUTY);
    if (images.secondInput && images.secondOutput)
        addLayer(images.secondInput, images.secondOutput, OPTIX_DENOISER_AOV_TYPE_SPECULAR);

    // Exposure and average color left null: the denoiser measures them
    // from the input each frame.
    OptixDenoiserParams params{};
    params.blendFactor = 0.0f; // fully denoised
    params.temporalModeUsePreviousLayers = m_HasPrevious ? 1u : 0u;

    if (!OPTIX_CHECK(optixDenoiserInvoke(m_Denoiser, m_Context.GetStream(), &params,
                                         m_State.Get(), m_State.GetSize(), &guides, layers, layerCount, 0, 0,
                                         m_Scratch.Get(), m_Scratch.GetSize())))
    {
        m_HasPrevious = false;
        return false;
    }

    m_CurrentGuide = currentGuide;
    m_HasPrevious = true;
    return true;
}
