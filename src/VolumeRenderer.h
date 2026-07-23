#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

#include "OrbitCamera.h"

class DescriptorAllocator;
class ShaderLibrary;
class GpuTexture3D;

// Look of the scene, independent from the physics.
struct RenderSettings
{
    float densityScale = 1.4f;
    float absorption = 34.0f;
    float emissionScale = 1.1f;
    float exposure = 1.35f;
    float temperatureReference = 5.0f;

    float lightDirection[3] = { -0.42f, 0.68f, -0.58f };
    float lightColor[3] = { 1.00f, 0.93f, 0.82f };
    float lightIntensity = 18.0f;

    float ambientColor[3] = { 0.070f, 0.090f, 0.140f };
    float skyTopColor[3] = { 0.026f, 0.048f, 0.100f };
    float skyBottomColor[3] = { 0.105f, 0.118f, 0.150f };
    float floorColor[3] = { 0.040f, 0.043f, 0.052f };

    float floorHeight = -1.0f;
    float fogDensity = 0.035f;
    float anisotropy = 0.40f;
    float gridScale = 4.0f;

    int stepCount = 128;
    int shadowSteps = 10;
};

// Constant buffer layout shared with VolumeRender.hlsl. Exactly 48 root constants.
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
    float temperatureReference;

    float skyTopColor[3];
    float floorHeight;

    float skyBottomColor[3];
    float fogDensity;

    float floorColor[3];
    float anisotropy;

    float ambientColor[3];
    float gridScale;
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
        const float boxSize[3],
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
