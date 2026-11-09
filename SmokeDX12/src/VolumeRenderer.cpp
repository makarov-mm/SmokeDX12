#include "VolumeRenderer.h"

#include "DescriptorAllocator.h"
#include "GpuTexture3D.h"
#include "OrbitCamera.h"
#include "ShaderLibrary.h"

#include <array>
#include <climits>
#include <cmath>
#include <utility>

namespace
{
    // Root constants, laid out like the RenderConstants cbuffer in VolumeRender.hlsl.
    struct RenderConstants
    {
        Float3 cameraPosition;
        float tanHalfFov;

        Float3 cameraRight;
        float aspectRatio;

        Float3 cameraUp;
        float densityScale;

        Float3 cameraForward;
        float absorption;

        Float3 lightDirection;
        float lightIntensity;

        Float3 lightColor;
        uint32_t stepCount;

        Float3 boxSize;
        float exposure;

        float emissionScale;
        uint32_t shadowSteps;
        float time;
        float temperatureReference;

        Float3 skyTopColor;
        float floorHeight;

        Float3 skyBottomColor;
        float fogDensity;

        Float3 floorColor;
        float anisotropy;

        Float3 ambientColor;
        float gridScale;

        float detailStrength;
        float detailScale;
        float detailDrift;
        float powderStrength;
    };

    static_assert(sizeof(RenderConstants) == 52 * sizeof(uint32_t), "RenderConstants must fill exactly 52 root constants");
}

VolumeRenderer::VolumeRenderer(ID3D12Device* device, DescriptorAllocator& descriptors, const ShaderLibrary& shaders, DXGI_FORMAT renderTargetFormat)
    : m_device(device)
    , m_descriptors(descriptors)
    , m_shaders(shaders)
    , m_renderTargetFormat(renderTargetFormat)
{
    CreateRootSignature();
    ReloadShaders();
}

void VolumeRenderer::CreateRootSignature()
{
    const D3D12_DESCRIPTOR_RANGE range = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 2,
        .BaseShaderRegister = 0,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = 0,
    };

    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = { .ShaderRegister = 0, .RegisterSpace = 0, .Num32BitValues = sizeof(RenderConstants) / sizeof(uint32_t) };
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = { .NumDescriptorRanges = 1, .pDescriptorRanges = &range };
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    const D3D12_STATIC_SAMPLER_DESC linearClamp = {
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .MaxLOD = D3D12_FLOAT32_MAX,
        .ShaderRegister = 0,
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
    };

    // The vertex shader builds the triangle from SV_VertexID alone, so only the
    // pixel shader reads anything from the root signature.
    const D3D12_ROOT_SIGNATURE_DESC desc = {
        .NumParameters = static_cast<UINT>(std::size(parameters)),
        .pParameters = parameters,
        .NumStaticSamplers = 1,
        .pStaticSamplers = &linearClamp,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS
            | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
            | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
            | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS,
    };

    m_rootSignature = ::CreateRootSignature(m_device, desc, L"Volume root signature");
}

void VolumeRenderer::ReloadShaders()
{
    constexpr std::array<ShaderEntryPoint, 2> EntryPoints = { {
        { "VSMain", "vs_5_1" },
        { "PSMain", "ps_5_1" },
    } };

    const std::vector<ComPtr<ID3DBlob>> blobs = m_shaders.Compile(L"VolumeRender.hlsl", EntryPoints);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_rootSignature.Get();
    desc.VS = { blobs[0]->GetBufferPointer(), blobs[0]->GetBufferSize() };
    desc.PS = { blobs[1]->GetBufferPointer(), blobs[1]->GetBufferSize() };

    // Opaque: the fullscreen triangle writes every pixel exactly once.
    D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = static_cast<UINT8>(D3D12_COLOR_WRITE_ENABLE_ALL);

    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = m_renderTargetFormat;
    desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pipeline;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    m_pipeline = std::move(pipeline);
}

void VolumeRenderer::Render(ID3D12GraphicsCommandList* commandList, const VolumeView& view, const RenderSettings& settings)
{
    BarrierBatch barriers(commandList);
    view.density.Transition(barriers, ShaderReadState);
    view.temperature.Transition(barriers, ShaderReadState);
    barriers.Flush();

    const CameraBasis camera = view.camera.Basis();

    const RenderConstants constants = {
        .cameraPosition = camera.position,
        .tanHalfFov = std::tan(view.camera.FieldOfView() * 0.5f),
        .cameraRight = camera.right,
        .aspectRatio = static_cast<float>(view.width) / static_cast<float>(view.height),
        .cameraUp = camera.up,
        .densityScale = settings.densityScale,
        .cameraForward = camera.forward,
        .absorption = settings.absorption,
        .lightDirection = settings.lightDirection,
        .lightIntensity = settings.lightIntensity,
        .lightColor = settings.lightColor,
        .stepCount = settings.stepCount,
        .boxSize = view.boxSize,
        .exposure = settings.exposure,
        .emissionScale = settings.emissionScale,
        .shadowSteps = settings.shadowSteps,
        .time = view.time,
        .temperatureReference = settings.temperatureReference,
        .skyTopColor = settings.skyTopColor,
        // The ground is the bottom face of the box, where the source sits, for any grid size.
        .floorHeight = -0.5f * view.boxSize.y,
        .skyBottomColor = settings.skyBottomColor,
        .fogDensity = settings.fogDensity,
        .floorColor = settings.floorColor,
        .anisotropy = settings.anisotropy,
        .ambientColor = settings.ambientColor,
        .gridScale = settings.gridScale,
        .detailStrength = settings.detailStrength,
        .detailScale = settings.detailScale,
        .detailDrift = settings.detailDrift,
        .powderStrength = settings.powderStrength,
    };

    const D3D12_VIEWPORT viewport = {
        .Width = static_cast<float>(view.width),
        .Height = static_cast<float>(view.height),
        .MaxDepth = 1.0f,
    };
    const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(view.width), static_cast<LONG>(view.height) };
    const D3D12_CPU_DESCRIPTOR_HANDLE textures[] = { view.density.Srv(), view.temperature.Srv() };

    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    commandList->SetPipelineState(m_pipeline.Get());
    commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
    commandList->SetGraphicsRootDescriptorTable(1, m_descriptors.Upload(textures).gpu);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
}
