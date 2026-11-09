#pragma once

#include "Common.h"
#include "Math.h"

#include <d3d12.h>

#include <cstdint>

class DescriptorAllocator;
class GpuTexture3D;
class OrbitCamera;
class ShaderLibrary;

// Look of the scene, independent from the physics.
struct RenderSettings
{
    float densityScale = 1.4f;
    float absorption = 34.0f;
    float emissionScale = 1.1f;
    float exposure = 1.35f;
    float temperatureReference = 5.0f;

    Float3 lightDirection = { -0.42f, 0.68f, -0.58f };
    Float3 lightColor = { 1.00f, 0.93f, 0.82f };
    float lightIntensity = 18.0f;

    Float3 ambientColor = { 0.070f, 0.090f, 0.140f };
    Float3 skyTopColor = { 0.026f, 0.048f, 0.100f };
    Float3 skyBottomColor = { 0.105f, 0.118f, 0.150f };
    Float3 floorColor = { 0.040f, 0.043f, 0.052f };

    float fogDensity = 0.035f;
    float anisotropy = 0.40f;
    float gridScale = 4.0f;

    // Sub grid detail. Strength is how deep the noise can bite into the field,
    // scale is in cycles per world unit, drift is how fast the pattern travels
    // with the plume in world units per second.
    float detailStrength = 0.55f;
    float detailScale = 9.0f;
    float detailDrift = 0.22f;
    float powderStrength = 0.45f;

    // The shadow march grows its step exponentially, so a handful of samples
    // covers the same range a uniform march would need many more for.
    uint32_t stepCount = 160;
    uint32_t shadowSteps = 8;
};

// Everything one frame of the volume depends on besides the settings.
struct VolumeView
{
    const OrbitCamera& camera;
    GpuTexture3D& density;
    GpuTexture3D& temperature;
    Float3 boxSize;
    uint32_t width;
    uint32_t height;
    float time;
};

// Draws the environment and ray marches the smoke in one fullscreen pass.
class VolumeRenderer
{
public:
    VolumeRenderer(ID3D12Device* device, DescriptorAllocator& descriptors, const ShaderLibrary& shaders, DXGI_FORMAT renderTargetFormat);

    // Recompiles VolumeRender.hlsl. When that fails the current pipeline stays
    // in use and ShaderCompileError is thrown. The GPU must be idle.
    void ReloadShaders();

    void Render(ID3D12GraphicsCommandList* commandList, const VolumeView& view, const RenderSettings& settings);

private:
    void CreateRootSignature();

    ID3D12Device* m_device;
    DescriptorAllocator& m_descriptors;
    const ShaderLibrary& m_shaders;
    DXGI_FORMAT m_renderTargetFormat;

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipeline;
};
