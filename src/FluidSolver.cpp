#include "FluidSolver.h"

#include <d3dcompiler.h>

#include <array>
#include <utility>

#include "DescriptorAllocator.h"
#include "ShaderLibrary.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr std::array<const char*, FluidSolver::PipelineCount> EntryPoints =
    {
        "CSClearScalar",
        "CSClearVelocity",
        "CSInject",
        "CSForces",
        "CSAdvectVelocity",
        "CSDivergence",
        "CSJacobi",
        "CSProject",
        "CSAdvectScalars"
    };
}

bool FluidSolver::Initialize(ID3D12Device* device, DescriptorAllocator* allocator, ShaderLibrary* shaders, uint32_t resolution)
{
    m_device = device;
    m_allocator = allocator;
    m_shaders = shaders;
    m_resolution = resolution;
    m_groupCount = (resolution + 7) / 8;

    if (!CreateRootSignature())
    {
        return false;
    }

    if (!CreatePipelines())
    {
        return false;
    }

    return CreateTextures();
}

bool FluidSolver::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 4;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[3] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = sizeof(FluidConstants) / sizeof(uint32_t);
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 3;
    desc.pParameters = parameters;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

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

    m_rootSignature->SetName(L"FluidRootSignature");
    return true;
}

bool FluidSolver::CreatePipelines()
{
    std::array<ComPtr<ID3D12PipelineState>, PipelineCount> compiled;

    for (size_t index = 0; index < PipelineCount; ++index)
    {
        ComPtr<ID3DBlob> code = m_shaders->Compile(L"Fluid.hlsl", EntryPoints[index], "cs_5_1");
        if (!code)
        {
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_rootSignature.Get();
        desc.CS.pShaderBytecode = code->GetBufferPointer();
        desc.CS.BytecodeLength = code->GetBufferSize();

        if (FAILED(m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&compiled[index]))))
        {
            return false;
        }
    }

    // Only swap the pipelines in once every stage has compiled, so a typo in a
    // shader during hot reloading leaves the running simulation untouched.
    m_pipelines = std::move(compiled);
    return true;
}

bool FluidSolver::CreateTextures()
{
    const uint32_t size = m_resolution;
    const DXGI_FORMAT vectorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const DXGI_FORMAT scalarFormat = DXGI_FORMAT_R16_FLOAT;
    const D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    bool ok = true;
    ok = ok && m_velocity[0].Create(m_device, *m_allocator, size, size, size, vectorFormat, initialState, L"VelocityA");
    ok = ok && m_velocity[1].Create(m_device, *m_allocator, size, size, size, vectorFormat, initialState, L"VelocityB");
    ok = ok && m_density[0].Create(m_device, *m_allocator, size, size, size, scalarFormat, initialState, L"DensityA");
    ok = ok && m_density[1].Create(m_device, *m_allocator, size, size, size, scalarFormat, initialState, L"DensityB");
    ok = ok && m_temperature[0].Create(m_device, *m_allocator, size, size, size, scalarFormat, initialState, L"TemperatureA");
    ok = ok && m_temperature[1].Create(m_device, *m_allocator, size, size, size, scalarFormat, initialState, L"TemperatureB");
    ok = ok && m_pressure[0].Create(m_device, *m_allocator, size, size, size, scalarFormat, initialState, L"PressureA");
    ok = ok && m_pressure[1].Create(m_device, *m_allocator, size, size, size, scalarFormat, initialState, L"PressureB");
    ok = ok && m_divergence.Create(m_device, *m_allocator, size, size, size, scalarFormat, initialState, L"Divergence");

    // One by one by one placeholders that stay in a fixed resource state forever.
    ok = ok && m_dummyVectorSrv.Create(m_device, *m_allocator, 1, 1, 1, vectorFormat, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"DummyVectorSrv");
    ok = ok && m_dummyScalarSrv.Create(m_device, *m_allocator, 1, 1, 1, scalarFormat, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"DummyScalarSrv");
    ok = ok && m_dummyVectorUav.Create(m_device, *m_allocator, 1, 1, 1, vectorFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"DummyVectorUav");
    ok = ok && m_dummyScalarUav.Create(m_device, *m_allocator, 1, 1, 1, scalarFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"DummyScalarUav");

    return ok;
}

void FluidSolver::Dispatch(
    ID3D12GraphicsCommandList* commandList,
    PipelineId pipeline,
    const D3D12_CPU_DESCRIPTOR_HANDLE* srvs,
    const D3D12_CPU_DESCRIPTOR_HANDLE* uavs)
{
    commandList->SetPipelineState(m_pipelines[static_cast<size_t>(pipeline)].Get());

    D3D12_GPU_DESCRIPTOR_HANDLE srvTable = m_allocator->CopyToShaderVisibleHeap(srvs, 4);
    D3D12_GPU_DESCRIPTOR_HANDLE uavTable = m_allocator->CopyToShaderVisibleHeap(uavs, 4);

    commandList->SetComputeRootDescriptorTable(1, srvTable);
    commandList->SetComputeRootDescriptorTable(2, uavTable);
    commandList->Dispatch(m_groupCount, m_groupCount, m_groupCount);
}

void FluidSolver::Reset(ID3D12GraphicsCommandList* commandList)
{
    const D3D12_CPU_DESCRIPTOR_HANDLE srvs[4] =
    {
        m_dummyVectorSrv.GetSrv(),
        m_dummyScalarSrv.GetSrv(),
        m_dummyScalarSrv.GetSrv(),
        m_dummyScalarSrv.GetSrv()
    };

    GpuTexture3D* scalars[] =
    {
        &m_density[0], &m_density[1],
        &m_temperature[0], &m_temperature[1],
        &m_pressure[0], &m_pressure[1],
        &m_divergence
    };

    for (GpuTexture3D* texture : scalars)
    {
        texture->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const D3D12_CPU_DESCRIPTOR_HANDLE uavs[4] =
        {
            m_dummyVectorUav.GetUav(),
            texture->GetUav(),
            m_dummyScalarUav.GetUav(),
            m_dummyScalarUav.GetUav()
        };

        Dispatch(commandList, PipelineId::ClearScalar, srvs, uavs);
    }

    for (int index = 0; index < 2; ++index)
    {
        m_velocity[index].Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const D3D12_CPU_DESCRIPTOR_HANDLE uavs[4] =
        {
            m_velocity[index].GetUav(),
            m_dummyScalarUav.GetUav(),
            m_dummyScalarUav.GetUav(),
            m_dummyScalarUav.GetUav()
        };

        Dispatch(commandList, PipelineId::ClearVelocity, srvs, uavs);
    }

    m_velocityIndex = 0;
    m_densityIndex = 0;
    m_temperatureIndex = 0;
    m_pressureIndex = 0;
}

void FluidSolver::Step(ID3D12GraphicsCommandList* commandList, const FluidSettings& settings, float dt, float time)
{
    commandList->SetComputeRootSignature(m_rootSignature.Get());

    FluidConstants constants = {};
    constants.gridSize[0] = m_resolution;
    constants.gridSize[1] = m_resolution;
    constants.gridSize[2] = m_resolution;
    constants.dt = dt;
    constants.emitterPosition[0] = settings.emitterPosition[0];
    constants.emitterPosition[1] = settings.emitterPosition[1];
    constants.emitterPosition[2] = settings.emitterPosition[2];
    constants.emitterRadius = settings.emitterRadius;
    constants.emitterVelocity[0] = settings.emitterVelocity[0];
    constants.emitterVelocity[1] = settings.emitterVelocity[1];
    constants.emitterVelocity[2] = settings.emitterVelocity[2];
    constants.buoyancy = settings.buoyancy;
    constants.ambientTemperature = settings.ambientTemperature;
    constants.vorticityStrength = settings.vorticityStrength;
    constants.densityDissipation = settings.densityDissipation;
    constants.temperatureDissipation = settings.temperatureDissipation;
    constants.emitDensity = settings.emitDensity;
    constants.emitTemperature = settings.emitTemperature;
    constants.time = time;
    constants.gravity = settings.gravity;
    constants.velocityDissipation = settings.velocityDissipation;
    constants.windStrength = settings.windStrength;
    constants.windGustFrequency = settings.windGustFrequency;
    constants.windDrag = settings.windDrag;
    constants.windDirection[0] = settings.windDirection[0];
    constants.windDirection[1] = settings.windDirection[1];
    constants.windDirection[2] = settings.windDirection[2];

    commandList->SetComputeRoot32BitConstants(0, sizeof(FluidConstants) / sizeof(uint32_t), &constants, 0);

    if (m_resetRequested)
    {
        Reset(commandList);
        m_resetRequested = false;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE dummySrvVector = m_dummyVectorSrv.GetSrv();
    const D3D12_CPU_DESCRIPTOR_HANDLE dummySrvScalar = m_dummyScalarSrv.GetSrv();
    const D3D12_CPU_DESCRIPTOR_HANDLE dummyUavVector = m_dummyVectorUav.GetUav();
    const D3D12_CPU_DESCRIPTOR_HANDLE dummyUavScalar = m_dummyScalarUav.GetUav();

    // Stage 1: inject density, temperature and momentum around the emitter.
    {
        GpuTexture3D& sourceVelocity = m_velocity[m_velocityIndex];
        GpuTexture3D& sourceDensity = m_density[m_densityIndex];
        GpuTexture3D& sourceTemperature = m_temperature[m_temperatureIndex];
        GpuTexture3D& targetVelocity = m_velocity[1 - m_velocityIndex];
        GpuTexture3D& targetDensity = m_density[1 - m_densityIndex];
        GpuTexture3D& targetTemperature = m_temperature[1 - m_temperatureIndex];

        sourceVelocity.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sourceDensity.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sourceTemperature.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        targetVelocity.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        targetDensity.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        targetTemperature.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const D3D12_CPU_DESCRIPTOR_HANDLE srvs[4] =
        {
            sourceVelocity.GetSrv(), sourceDensity.GetSrv(), sourceTemperature.GetSrv(), dummySrvScalar
        };
        const D3D12_CPU_DESCRIPTOR_HANDLE uavs[4] =
        {
            targetVelocity.GetUav(), targetDensity.GetUav(), targetTemperature.GetUav(), dummyUavScalar
        };

        Dispatch(commandList, PipelineId::Inject, srvs, uavs);

        m_velocityIndex = 1 - m_velocityIndex;
        m_densityIndex = 1 - m_densityIndex;
        m_temperatureIndex = 1 - m_temperatureIndex;
    }

    // Stage 2: buoyancy, weight and vorticity confinement.
    {
        GpuTexture3D& sourceVelocity = m_velocity[m_velocityIndex];
        GpuTexture3D& temperature = m_temperature[m_temperatureIndex];
        GpuTexture3D& density = m_density[m_densityIndex];
        GpuTexture3D& targetVelocity = m_velocity[1 - m_velocityIndex];

        sourceVelocity.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        temperature.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        density.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        targetVelocity.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const D3D12_CPU_DESCRIPTOR_HANDLE srvs[4] =
        {
            sourceVelocity.GetSrv(), temperature.GetSrv(), density.GetSrv(), dummySrvScalar
        };
        const D3D12_CPU_DESCRIPTOR_HANDLE uavs[4] =
        {
            targetVelocity.GetUav(), dummyUavScalar, dummyUavScalar, dummyUavScalar
        };

        Dispatch(commandList, PipelineId::Forces, srvs, uavs);
        m_velocityIndex = 1 - m_velocityIndex;
    }

    // Stage 3: self advection of the velocity field.
    {
        GpuTexture3D& sourceVelocity = m_velocity[m_velocityIndex];
        GpuTexture3D& targetVelocity = m_velocity[1 - m_velocityIndex];

        sourceVelocity.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        targetVelocity.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const D3D12_CPU_DESCRIPTOR_HANDLE srvs[4] =
        {
            sourceVelocity.GetSrv(), dummySrvScalar, dummySrvScalar, dummySrvScalar
        };
        const D3D12_CPU_DESCRIPTOR_HANDLE uavs[4] =
        {
            targetVelocity.GetUav(), dummyUavScalar, dummyUavScalar, dummyUavScalar
        };

        Dispatch(commandList, PipelineId::AdvectVelocity, srvs, uavs);
        m_velocityIndex = 1 - m_velocityIndex;
    }

    // Stage 4: divergence of the intermediate velocity field.
    {
        GpuTexture3D& sourceVelocity = m_velocity[m_velocityIndex];

        sourceVelocity.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_divergence.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const D3D12_CPU_DESCRIPTOR_HANDLE srvs[4] =
        {
            sourceVelocity.GetSrv(), dummySrvScalar, dummySrvScalar, dummySrvScalar
        };
        const D3D12_CPU_DESCRIPTOR_HANDLE uavs[4] =
        {
            dummyUavVector, m_divergence.GetUav(), dummyUavScalar, dummyUavScalar
        };

        Dispatch(commandList, PipelineId::Divergence, srvs, uavs);
    }

    // Stage 5: clear the pressure field before the iterative solve.
    for (int index = 0; index < 2; ++index)
    {
        m_pressure[index].Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const D3D12_CPU_DESCRIPTOR_HANDLE srvs[4] =
        {
            dummySrvVector, dummySrvScalar, dummySrvScalar, dummySrvScalar
        };
        const D3D12_CPU_DESCRIPTOR_HANDLE uavs[4] =
        {
            dummyUavVector, m_pressure[index].GetUav(), dummyUavScalar, dummyUavScalar
        };

        Dispatch(commandList, PipelineId::ClearScalar, srvs, uavs);
    }

    // Stage 6: Jacobi iterations of the pressure Poisson equation.
    m_divergence.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    for (int iteration = 0; iteration < settings.pressureIterations; ++iteration)
    {
        GpuTexture3D& sourcePressure = m_pressure[m_pressureIndex];
        GpuTexture3D& targetPressure = m_pressure[1 - m_pressureIndex];

        sourcePressure.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        targetPressure.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const D3D12_CPU_DESCRIPTOR_HANDLE srvs[4] =
        {
            dummySrvVector, sourcePressure.GetSrv(), m_divergence.GetSrv(), dummySrvScalar
        };
        const D3D12_CPU_DESCRIPTOR_HANDLE uavs[4] =
        {
            dummyUavVector, targetPressure.GetUav(), dummyUavScalar, dummyUavScalar
        };

        Dispatch(commandList, PipelineId::Jacobi, srvs, uavs);
        m_pressureIndex = 1 - m_pressureIndex;
    }

    // Stage 7: subtract the pressure gradient to make the field divergence free.
    {
        GpuTexture3D& sourceVelocity = m_velocity[m_velocityIndex];
        GpuTexture3D& pressure = m_pressure[m_pressureIndex];
        GpuTexture3D& targetVelocity = m_velocity[1 - m_velocityIndex];

        sourceVelocity.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        pressure.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        targetVelocity.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const D3D12_CPU_DESCRIPTOR_HANDLE srvs[4] =
        {
            sourceVelocity.GetSrv(), pressure.GetSrv(), dummySrvScalar, dummySrvScalar
        };
        const D3D12_CPU_DESCRIPTOR_HANDLE uavs[4] =
        {
            targetVelocity.GetUav(), dummyUavScalar, dummyUavScalar, dummyUavScalar
        };

        Dispatch(commandList, PipelineId::Project, srvs, uavs);
        m_velocityIndex = 1 - m_velocityIndex;
    }

    // Stage 8: transport density and temperature with the corrected velocity field.
    {
        GpuTexture3D& velocity = m_velocity[m_velocityIndex];
        GpuTexture3D& sourceDensity = m_density[m_densityIndex];
        GpuTexture3D& sourceTemperature = m_temperature[m_temperatureIndex];
        GpuTexture3D& targetDensity = m_density[1 - m_densityIndex];
        GpuTexture3D& targetTemperature = m_temperature[1 - m_temperatureIndex];

        velocity.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sourceDensity.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sourceTemperature.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        targetDensity.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        targetTemperature.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const D3D12_CPU_DESCRIPTOR_HANDLE srvs[4] =
        {
            velocity.GetSrv(), sourceDensity.GetSrv(), sourceTemperature.GetSrv(), dummySrvScalar
        };
        const D3D12_CPU_DESCRIPTOR_HANDLE uavs[4] =
        {
            dummyUavVector, targetDensity.GetUav(), targetTemperature.GetUav(), dummyUavScalar
        };

        Dispatch(commandList, PipelineId::AdvectScalars, srvs, uavs);

        m_densityIndex = 1 - m_densityIndex;
        m_temperatureIndex = 1 - m_temperatureIndex;
    }
}
