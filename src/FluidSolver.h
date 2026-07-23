#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

#include "GpuTexture3D.h"

class DescriptorAllocator;
class ShaderLibrary;

// Tunable parameters of the simulation, all of them are safe to change at run time.
struct FluidSettings
{
    float emitterPosition[3] = { 0.5f, 0.10f, 0.5f };
    float emitterRadius = 0.06f;
    float emitterVelocity[3] = { 0.0f, 30.0f, 0.0f };
    float emitDensity = 1.2f;
    float emitTemperature = 3.5f;
    float buoyancy = 14.0f;
    float gravity = 0.9f;
    float ambientTemperature = 0.0f;
    float vorticityStrength = 14.0f;
    float densityDissipation = 0.990f;
    float temperatureDissipation = 0.965f;
    float velocityDissipation = 0.999f;
    int pressureIterations = 40;
};

// Constant buffer layout shared with Fluid.hlsl. Exactly 24 root constants.
struct FluidConstants
{
    uint32_t gridSize[3];
    float dt;

    float emitterPosition[3];
    float emitterRadius;

    float emitterVelocity[3];
    float buoyancy;

    float ambientTemperature;
    float vorticityStrength;
    float densityDissipation;
    float temperatureDissipation;

    float emitDensity;
    float emitTemperature;
    float time;
    float gravity;

    float velocityDissipation;
    float padding[3];
};

// Eulerian smoke solver. Every stage is a compute dispatch over the whole grid.
class FluidSolver
{
public:
    bool Initialize(ID3D12Device* device, DescriptorAllocator* allocator, ShaderLibrary* shaders, uint32_t resolution);
    bool CreatePipelines();

    void RequestReset() { m_resetRequested = true; }
    void Step(ID3D12GraphicsCommandList* commandList, const FluidSettings& settings, float dt, float time);

    GpuTexture3D& GetDensity() { return m_density[m_densityIndex]; }
    GpuTexture3D& GetTemperature() { return m_temperature[m_temperatureIndex]; }
    uint32_t GetResolution() const { return m_resolution; }

private:
    enum PipelineId
    {
        PipelineClearScalar = 0,
        PipelineClearVelocity,
        PipelineInject,
        PipelineForces,
        PipelineAdvectVelocity,
        PipelineDivergence,
        PipelineJacobi,
        PipelineProject,
        PipelineAdvectScalars,
        PipelineCount
    };

    bool CreateRootSignature();
    bool CreateTextures();

    void Reset(ID3D12GraphicsCommandList* commandList);
    void Dispatch(
        ID3D12GraphicsCommandList* commandList,
        PipelineId pipeline,
        const D3D12_CPU_DESCRIPTOR_HANDLE* srvs,
        const D3D12_CPU_DESCRIPTOR_HANDLE* uavs);

    ID3D12Device* m_device = nullptr;
    DescriptorAllocator* m_allocator = nullptr;
    ShaderLibrary* m_shaders = nullptr;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelines[PipelineCount];

    GpuTexture3D m_velocity[2];
    GpuTexture3D m_density[2];
    GpuTexture3D m_temperature[2];
    GpuTexture3D m_pressure[2];
    GpuTexture3D m_divergence;

    // Placeholders bound to descriptor slots a given pass does not use.
    GpuTexture3D m_dummyVectorSrv;
    GpuTexture3D m_dummyVectorUav;
    GpuTexture3D m_dummyScalarSrv;
    GpuTexture3D m_dummyScalarUav;

    uint32_t m_resolution = 128;
    uint32_t m_groupCount = 16;

    uint32_t m_velocityIndex = 0;
    uint32_t m_densityIndex = 0;
    uint32_t m_temperatureIndex = 0;
    uint32_t m_pressureIndex = 0;

    bool m_resetRequested = true;
};
