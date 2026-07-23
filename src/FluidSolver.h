#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>

#include "GpuTexture3D.h"

class DescriptorAllocator;
class ShaderLibrary;

// Tunable parameters of the simulation, all of them are safe to change at run time.
struct FluidSettings
{
    // Position is in normalised grid coordinates, the radius is in box units.
    float emitterPosition[3] = { 0.30f, 0.045f, 0.5f };
    float emitterRadius = 0.10f;
    float emitterVelocity[3] = { 0.0f, 26.0f, 0.0f };
    float emitDensity = 0.85f;
    float emitTemperature = 2.6f;
    float buoyancy = 20.0f;
    float gravity = 1.0f;
    float ambientTemperature = 0.0f;
    float vorticityStrength = 24.0f;
    float densityDissipation = 0.9990f;
    float temperatureDissipation = 0.9930f;
    float velocityDissipation = 0.9995f;

    float windDirection[3] = { 1.0f, 0.0f, 0.15f };
    float windStrength = 10.0f;
    float windGustFrequency = 0.35f;
    float windDrag = 1.6f;

    int pressureIterations = 40;
};

// Constant buffer layout shared with Fluid.hlsl. Exactly 28 root constants.
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
    float windStrength;
    float windGustFrequency;
    float windDrag;

    float windDirection[3];
    float padding;
};

// Eulerian smoke solver. Every stage is a compute dispatch over the whole grid.
class FluidSolver
{
public:
    bool Initialize(
        ID3D12Device* device,
        DescriptorAllocator* allocator,
        ShaderLibrary* shaders,
        uint32_t resolutionX,
        uint32_t resolutionY,
        uint32_t resolutionZ);
    bool CreatePipelines();

    void RequestReset() { m_resetRequested = true; }
    void Step(ID3D12GraphicsCommandList* commandList, const FluidSettings& settings, float dt, float time);

    GpuTexture3D& GetDensity() { return m_density[m_densityIndex]; }
    GpuTexture3D& GetTemperature() { return m_temperature[m_temperatureIndex]; }

    uint32_t GetResolutionX() const { return m_resolutionX; }
    uint32_t GetResolutionY() const { return m_resolutionY; }
    uint32_t GetResolutionZ() const { return m_resolutionZ; }

    // World size of the box: cells are cubic, so the aspect is the grid size
    // normalised by its smallest axis.
    float GetBoxSizeX() const { return static_cast<float>(m_resolutionX) / SmallestAxis(); }
    float GetBoxSizeY() const { return static_cast<float>(m_resolutionY) / SmallestAxis(); }
    float GetBoxSizeZ() const { return static_cast<float>(m_resolutionZ) / SmallestAxis(); }

    enum class PipelineId : size_t
    {
        ClearScalar = 0,
        ClearVelocity,
        Inject,
        Forces,
        AdvectVelocity,
        Divergence,
        Jacobi,
        Project,
        AdvectScalars,
        Count
    };

    static constexpr size_t PipelineCount = static_cast<size_t>(PipelineId::Count);

private:
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
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, PipelineCount> m_pipelines;

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

    float SmallestAxis() const
    {
        return static_cast<float>(std::min({ m_resolutionX, m_resolutionY, m_resolutionZ }));
    }

    uint32_t m_resolutionX = 128;
    uint32_t m_resolutionY = 128;
    uint32_t m_resolutionZ = 128;
    uint32_t m_groupCountX = 16;
    uint32_t m_groupCountY = 16;
    uint32_t m_groupCountZ = 16;

    uint32_t m_velocityIndex = 0;
    uint32_t m_densityIndex = 0;
    uint32_t m_temperatureIndex = 0;
    uint32_t m_pressureIndex = 0;

    bool m_resetRequested = true;
};
