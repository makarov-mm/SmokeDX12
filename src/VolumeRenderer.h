#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

#include "OrbitCamera.h"

class DescriptorAllocator;
class ShaderLibrary;
class GpuTexture3D;

// Look of the volume, independent from the physics.
struct RenderSettings
{
    float densityScale = 1.6f;
    float absorption = 26.0f;
    float emissionScale = 2.4f;
    float exposure = 1.5f;
    float lightDirection[3] = { -0.45f, 0.75f, -0.48f };
    float lightColor[3] = { 1.0f, 0.94f, 0.86f };
    float lightIntensity = 3.2f;
    int stepCount = 160;
    int shadowSteps = 12;
};

// Constant buffer layout shared with VolumeRender.hlsl. Exactly 32 root constants.
struct RenderConstants
{
    float cameraPosition[3];
    float tanHalfFov;

    float cameraRight[3];
    float aspectRatio;

    float cameraUp[3];
    float densityScale;

    float cameraForward[3];
    float absorption;

    float lightDirection[3];
    float lightIntensity;

    float lightColor[3];
    float stepCount;

    float boxSize[3];
    float exposure;

    float emissionScale;
    float shadowSteps;
    float time;
    float padding;
};

class VolumeRenderer
{
public:
    bool Initialize(ID3D12Device* device, DescriptorAllocator* allocator, ShaderLibrary* shaders, DXGI_FORMAT backBufferFormat);
    bool CreatePipeline();

    void Render(
        ID3D12GraphicsCommandList* commandList,
        const OrbitCamera& camera,
        GpuTexture3D& density,
        GpuTexture3D& temperature,
        const RenderSettings& settings,
        uint32_t width,
        uint32_t height,
        float time);

private:
    bool CreateRootSignature();

    ID3D12Device* m_device = nullptr;
    DescriptorAllocator* m_allocator = nullptr;
    ShaderLibrary* m_shaders = nullptr;
    DXGI_FORMAT m_backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
};
