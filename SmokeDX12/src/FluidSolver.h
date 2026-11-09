#pragma once

#include "Common.h"
#include "GpuTexture3D.h"
#include "Math.h"

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <vector>

class DescriptorAllocator;
class ShaderLibrary;

// Tunable parameters of the simulation, all of them safe to change at run time.
struct FluidSettings
{
    // Position is in normalised grid coordinates, the radius is in box units.
    Float3 emitterPosition = { 0.30f, 0.045f, 0.5f };
    float emitterRadius = 0.10f;
    Float3 emitterVelocity = { 0.0f, 26.0f, 0.0f };
    float emitDensity = 0.85f;
    float emitTemperature = 2.6f;
    float buoyancy = 20.0f;
    float gravity = 1.0f;
    float ambientTemperature = 0.0f;

    // MacCormack advection keeps far more of the small scale structure alive by
    // itself, so the confinement that used to compensate for the diffusion of
    // the first order scheme can come down. Too much of it on top of a sharp
    // advection scheme makes the plume boil.
    float vorticityStrength = 15.0f;
    float macCormackStrength = 1.0f;

    // The MacCormack limiter is not conservative: reverting cells to first order
    // biases the field upwards by about 0.15 percent of its mass per step. The
    // dissipation has to be stronger than that or the domain fills up, which the
    // numerical diffusion of the old first order scheme used to hide.
    float densityDissipation = 0.9980f;
    float temperatureDissipation = 0.9930f;
    float velocityDissipation = 0.9995f;

    // Divergence free turbulence added to the gas itself, in cells per second.
    // The scale is in cycles across the shortest axis of the box.
    float turbulenceStrength = 5.0f;
    float turbulenceScale = 3.0f;

    // Width of the shell along the open top and the side walls in which density
    // is faded out, as a fraction of the domain.
    float boundaryFade = 0.07f;

    Float3 windDirection = { 1.0f, 0.0f, 0.15f };
    float windStrength = 10.0f;
    float windGustFrequency = 0.35f;
    float windDrag = 1.6f;

    int pressureIterations = 40;
};

// Eulerian smoke solver. Every stage is a compute dispatch over the whole grid.
class FluidSolver
{
public:
    // The grids the solver keeps. The value of a field is also its slot in the
    // descriptor table every pass sees: SRV t<value> and UAV u<value> in Fluid.hlsl.
    enum class Field : uint32_t
    {
        Velocity,
        VelocityHat,
        Density,
        DensityHat,
        Temperature,
        TemperatureHat,
        Pressure,
        Divergence,
        Count
    };

    // The compute passes, one entry point of Fluid.hlsl each.
    enum class Pass : uint32_t
    {
        Inject,
        Forces,
        AdvectVelocity,
        CorrectVelocity,
        Divergence,
        Jacobi,
        Project,
        AdvectScalars,
        CorrectScalars,
        Count
    };

    FluidSolver(ID3D12Device* device, DescriptorAllocator& descriptors, const ShaderLibrary& shaders, UInt3 resolution);

    // Recompiles Fluid.hlsl. When that fails the current pipelines stay in use
    // and ShaderCompileError is thrown. The GPU must be idle.
    void ReloadShaders();

    void RequestReset() noexcept { m_resetPending = true; }
    // Records clears of every field if a reset was requested.
    void ResetIfRequested(ID3D12GraphicsCommandList* commandList);
    // Records one simulation step of length `dt` that ends at `time`.
    void Step(ID3D12GraphicsCommandList* commandList, const FluidSettings& settings, float dt, float time);

    GpuTexture3D& Density() noexcept { return Textures(Field::Density).Read(); }
    GpuTexture3D& Temperature() noexcept { return Textures(Field::Temperature).Read(); }

    UInt3 Resolution() const noexcept { return m_resolution; }
    // World size of the box. Cells are cubic, so the aspect of the box is the
    // grid size normalised by its smallest axis.
    Float3 BoxSize() const noexcept;

private:
    static constexpr size_t FieldCount = static_cast<size_t>(Field::Count);
    static constexpr size_t PassCount = static_cast<size_t>(Pass::Count);

    // The textures of one field: a pair the passes ping pong between, or a
    // single one for a field that one pass writes and the next one reads.
    struct FieldTextures
    {
        std::vector<GpuTexture3D> textures;
        size_t current = 0;

        GpuTexture3D& Read() noexcept { return textures[current]; }
        GpuTexture3D& Write() noexcept { return textures[textures.size() - 1 - current]; }
        void Swap() noexcept { current = textures.size() - 1 - current; }
    };

    void CreateRootSignature();
    void CreateFields();
    void Run(ID3D12GraphicsCommandList* commandList, Pass pass);

    FieldTextures& Textures(Field field) noexcept { return m_fields[static_cast<size_t>(field)]; }

    ID3D12Device* m_device;
    DescriptorAllocator& m_descriptors;
    const ShaderLibrary& m_shaders;
    UInt3 m_resolution;
    UInt3 m_groupCount;

    ComPtr<ID3D12RootSignature> m_rootSignature;
    std::array<ComPtr<ID3D12PipelineState>, PassCount> m_pipelines;
    std::array<FieldTextures, FieldCount> m_fields;

    // Bound to the slots of the fields a pass does not touch.
    TextureViews m_nullVectorViews = {};
    TextureViews m_nullScalarViews = {};

    bool m_resetPending = true;
};
