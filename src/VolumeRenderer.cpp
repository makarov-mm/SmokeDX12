#include "VolumeRenderer.h"

#include <d3dcompiler.h>

#include <climits>
#include <cmath>

#include "DescriptorAllocator.h"
#include "GpuTexture3D.h"
#include "ShaderLibrary.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

bool VolumeRenderer::Initialize(ID3D12Device* device, DescriptorAllocator* allocator, ShaderLibrary* shaders, DXGI_FORMAT backBufferFormat)
{
    m_device = device;
    m_allocator = allocator;
    m_shaders = shaders;
    m_backBufferFormat = backBufferFormat;

    if (!CreateRootSignature())
    {
        return false;
    }

    return CreatePipeline();
}

bool VolumeRenderer::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = sizeof(RenderConstants) / sizeof(uint32_t);
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = parameters;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;

    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        return false;
    }

    if (FAILED(m_device->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature))))
    {
        return false;
    }

    m_rootSignature->SetName(L"VolumeRootSignature");
    return true;
}

bool VolumeRenderer::CreatePipeline()
{
    ComPtr<ID3DBlob> vertexShader = m_shaders->Compile(L"VolumeRender.hlsl", "VSMain", "vs_5_1");
    if (!vertexShader)
    {
        return false;
    }

    ComPtr<ID3DBlob> pixelShader = m_shaders->Compile(L"VolumeRender.hlsl", "PSMain", "ps_5_1");
    if (!pixelShader)
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_rootSignature.Get();
    desc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
    desc.VS.BytecodeLength = vertexShader->GetBufferSize();
    desc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
    desc.PS.BytecodeLength = pixelShader->GetBufferSize();

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;

    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;

    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = m_backBufferFormat;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
    {
        return false;
    }

    m_pipeline = pipeline;
    return true;
}

void VolumeRenderer::Render(
    ID3D12GraphicsCommandList* commandList,
    const OrbitCamera& camera,
    GpuTexture3D& density,
    GpuTexture3D& temperature,
    const RenderSettings& settings,
    const float boxSize[3],
    uint32_t width,
    uint32_t height,
    float time)
{
    density.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    temperature.Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;

    D3D12_RECT scissor = {};
    scissor.right = static_cast<LONG>(width);
    scissor.bottom = static_cast<LONG>(height);

    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    commandList->SetPipelineState(m_pipeline.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const XMFLOAT3 position = camera.GetPosition();
    const XMFLOAT3 forward = camera.GetForward();
    const XMFLOAT3 right = camera.GetRight();
    const XMFLOAT3 up = camera.GetUp();

    RenderConstants constants = {};
    constants.cameraPosition[0] = position.x;
    constants.cameraPosition[1] = position.y;
    constants.cameraPosition[2] = position.z;
    constants.tanHalfFov = std::tan(camera.GetFieldOfView() * 0.5f);

    constants.cameraRight[0] = right.x;
    constants.cameraRight[1] = right.y;
    constants.cameraRight[2] = right.z;
    constants.aspectRatio = static_cast<float>(width) / static_cast<float>(height);

    constants.cameraUp[0] = up.x;
    constants.cameraUp[1] = up.y;
    constants.cameraUp[2] = up.z;
    constants.densityScale = settings.densityScale;

    constants.cameraForward[0] = forward.x;
    constants.cameraForward[1] = forward.y;
    constants.cameraForward[2] = forward.z;
    constants.absorption = settings.absorption;

    constants.lightDirection[0] = settings.lightDirection[0];
    constants.lightDirection[1] = settings.lightDirection[1];
    constants.lightDirection[2] = settings.lightDirection[2];
    constants.lightIntensity = settings.lightIntensity;

    constants.lightColor[0] = settings.lightColor[0];
    constants.lightColor[1] = settings.lightColor[1];
    constants.lightColor[2] = settings.lightColor[2];
    constants.stepCount = static_cast<float>(settings.stepCount);

    constants.boxSize[0] = boxSize[0];
    constants.boxSize[1] = boxSize[1];
    constants.boxSize[2] = boxSize[2];
    constants.exposure = settings.exposure;

    constants.emissionScale = settings.emissionScale;
    constants.shadowSteps = static_cast<float>(settings.shadowSteps);
    constants.time = time;
    constants.temperatureReference = settings.temperatureReference;

    constants.skyTopColor[0] = settings.skyTopColor[0];
    constants.skyTopColor[1] = settings.skyTopColor[1];
    constants.skyTopColor[2] = settings.skyTopColor[2];
    constants.floorHeight = settings.floorHeight;

    constants.skyBottomColor[0] = settings.skyBottomColor[0];
    constants.skyBottomColor[1] = settings.skyBottomColor[1];
    constants.skyBottomColor[2] = settings.skyBottomColor[2];
    constants.fogDensity = settings.fogDensity;

    constants.floorColor[0] = settings.floorColor[0];
    constants.floorColor[1] = settings.floorColor[1];
    constants.floorColor[2] = settings.floorColor[2];
    constants.anisotropy = settings.anisotropy;

    constants.ambientColor[0] = settings.ambientColor[0];
    constants.ambientColor[1] = settings.ambientColor[1];
    constants.ambientColor[2] = settings.ambientColor[2];
    constants.gridScale = settings.gridScale;

    commandList->SetGraphicsRoot32BitConstants(0, sizeof(RenderConstants) / sizeof(uint32_t), &constants, 0);

    const D3D12_CPU_DESCRIPTOR_HANDLE srvs[2] = { density.GetSrv(), temperature.GetSrv() };
    D3D12_GPU_DESCRIPTOR_HANDLE table = m_allocator->CopyToShaderVisibleHeap(srvs, 2);
    commandList->SetGraphicsRootDescriptorTable(1, table);

    commandList->DrawInstanced(3, 1, 0, 0);
}
