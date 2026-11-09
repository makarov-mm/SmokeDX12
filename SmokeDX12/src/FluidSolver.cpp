#include "FluidSolver.h"

#include "DescriptorAllocator.h"
#include "ShaderLibrary.h"

#include <algorithm>
#include <utility>

namespace
{
    using Field = FluidSolver::Field;
    using Pass = FluidSolver::Pass;

    // Root constants, laid out like the FluidConstants cbuffer in Fluid.hlsl.
    struct FluidConstants
    {
        UInt3 gridSize;
        float dt;

        Float3 emitterPosition;
        float emitterRadius;

        Float3 emitterVelocity;
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

        Float3 windDirection;
        float macCormackStrength;

        float turbulenceStrength;
        float turbulenceScale;
        float boundaryFade;
        float padding = 0.0f;
    };

    static_assert(sizeof(FluidConstants) == 32 * sizeof(uint32_t), "FluidConstants must fill exactly 32 root constants");

    // Matches [numthreads(8, 8, 8)] in Fluid.hlsl.
    constexpr uint32_t GroupSize = 8;

    constexpr DXGI_FORMAT VectorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT ScalarFormat = DXGI_FORMAT_R16_FLOAT;

    constexpr uint32_t Bit(Field field) noexcept
    {
        return 1u << std::to_underlying(field);
    }

    template <typename... Fields>
    constexpr uint32_t Mask(Fields... fields) noexcept
    {
        return (Bit(fields) | ... | 0u);
    }

    constexpr uint32_t VectorFields = Mask(Field::Velocity, Field::VelocityHat);

    // Fields with a single texture, the forward halves of the MacCormack steps
    // and the divergence. Everything else ping pongs between two textures.
    constexpr uint32_t SingleFields = Mask(Field::VelocityHat, Field::DensityHat, Field::TemperatureHat, Field::Divergence);

    // What each pass reads and writes. A read binds the current texture of the
    // field to its SRV slot. A write binds the other texture of the pair to its
    // UAV slot, and the pair swaps once the pass is recorded.
    struct PassDesc
    {
        Pass pass;
        const char* entryPoint;
        uint32_t reads;
        uint32_t writes;
    };

    constexpr std::array Passes = {
        PassDesc{ Pass::Inject, "CSInject",
            Mask(Field::Velocity, Field::Density, Field::Temperature),
            Mask(Field::Velocity, Field::Density, Field::Temperature) },
        PassDesc{ Pass::Forces, "CSForces",
            Mask(Field::Velocity, Field::Density, Field::Temperature),
            Mask(Field::Velocity) },
        PassDesc{ Pass::AdvectVelocity, "CSAdvectVelocity",
            Mask(Field::Velocity),
            Mask(Field::VelocityHat) },
        PassDesc{ Pass::CorrectVelocity, "CSCorrectVelocity",
            Mask(Field::Velocity, Field::VelocityHat),
            Mask(Field::Velocity) },
        PassDesc{ Pass::Divergence, "CSDivergence",
            Mask(Field::Velocity),
            Mask(Field::Divergence) },
        PassDesc{ Pass::Jacobi, "CSJacobi",
            Mask(Field::Pressure, Field::Divergence),
            Mask(Field::Pressure) },
        PassDesc{ Pass::Project, "CSProject",
            Mask(Field::Velocity, Field::Pressure),
            Mask(Field::Velocity) },
        PassDesc{ Pass::AdvectScalars, "CSAdvectScalars",
            Mask(Field::Velocity, Field::Density, Field::Temperature),
            Mask(Field::DensityHat, Field::TemperatureHat) },
        PassDesc{ Pass::CorrectScalars, "CSCorrectScalars",
            Mask(Field::Velocity, Field::Density, Field::DensityHat, Field::Temperature, Field::TemperatureHat),
            Mask(Field::Density, Field::Temperature) },
    };

    // Every pass in enum order, and none reading and writing a single texture at once.
    consteval bool PassesAreConsistent()
    {
        for (size_t index = 0; index < Passes.size(); ++index)
        {
            if (std::to_underlying(Passes[index].pass) != index || (Passes[index].reads & Passes[index].writes & SingleFields) != 0)
            {
                return false;
            }
        }

        return true;
    }

    static_assert(Passes.size() == std::to_underlying(Pass::Count));
    static_assert(PassesAreConsistent());

    constexpr std::array<const wchar_t*, std::to_underlying(Field::Count)> FieldNames = {
        L"Velocity", L"VelocityHat", L"Density", L"DensityHat",
        L"Temperature", L"TemperatureHat", L"Pressure", L"Divergence",
    };

    constexpr uint32_t GroupCount(uint32_t cells) noexcept
    {
        return (cells + GroupSize - 1) / GroupSize;
    }
}

FluidSolver::FluidSolver(ID3D12Device* device, DescriptorAllocator& descriptors, const ShaderLibrary& shaders, UInt3 resolution)
    : m_device(device)
    , m_descriptors(descriptors)
    , m_shaders(shaders)
    , m_resolution(resolution)
    , m_groupCount{ GroupCount(resolution.x), GroupCount(resolution.y), GroupCount(resolution.z) }
{
    CreateRootSignature();
    ReloadShaders();
    CreateFields();
}

Float3 FluidSolver::BoxSize() const noexcept
{
    const float smallest = static_cast<float>(std::min({ m_resolution.x, m_resolution.y, m_resolution.z }));

    return {
        static_cast<float>(m_resolution.x) / smallest,
        static_cast<float>(m_resolution.y) / smallest,
        static_cast<float>(m_resolution.z) / smallest,
    };
}

void FluidSolver::CreateRootSignature()
{
    // One layout for every pass: the constants, then a single table holding an
    // SRV and a UAV slot for every field.
    const D3D12_DESCRIPTOR_RANGE ranges[] = {
        {
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = FieldCount,
            .BaseShaderRegister = 0,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = 0,
        },
        {
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
            .NumDescriptors = FieldCount,
            .BaseShaderRegister = 0,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = FieldCount,
        },
    };

    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = { .ShaderRegister = 0, .RegisterSpace = 0, .Num32BitValues = sizeof(FluidConstants) / sizeof(uint32_t) };
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = { .NumDescriptorRanges = static_cast<UINT>(std::size(ranges)), .pDescriptorRanges = ranges };
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    const D3D12_STATIC_SAMPLER_DESC linearClamp = {
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .MaxLOD = D3D12_FLOAT32_MAX,
        .ShaderRegister = 0,
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
    };

    const D3D12_ROOT_SIGNATURE_DESC desc = {
        .NumParameters = static_cast<UINT>(std::size(parameters)),
        .pParameters = parameters,
        .NumStaticSamplers = 1,
        .pStaticSamplers = &linearClamp,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
    };

    m_rootSignature = ::CreateRootSignature(m_device, desc, L"Fluid root signature");
}

void FluidSolver::ReloadShaders()
{
    std::array<ShaderEntryPoint, PassCount> entryPoints;
    for (size_t index = 0; index < PassCount; ++index)
    {
        entryPoints[index] = { Passes[index].entryPoint, "cs_5_1" };
    }

    const std::vector<ComPtr<ID3DBlob>> blobs = m_shaders.Compile(L"Fluid.hlsl", entryPoints);

    std::array<ComPtr<ID3D12PipelineState>, PassCount> pipelines;
    for (size_t index = 0; index < PassCount; ++index)
    {
        const D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {
            .pRootSignature = m_rootSignature.Get(),
            .CS = { blobs[index]->GetBufferPointer(), blobs[index]->GetBufferSize() },
        };
        ThrowIfFailed(m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipelines[index])));
    }

    // Only swapped in once every stage has compiled, so a typo during hot
    // reloading leaves the running simulation untouched.
    m_pipelines = std::move(pipelines);
}

void FluidSolver::CreateFields()
{
    for (size_t index = 0; index < FieldCount; ++index)
    {
        const uint32_t bit = 1u << index;
        const DXGI_FORMAT format = (VectorFields & bit) != 0 ? VectorFormat : ScalarFormat;
        const size_t count = (SingleFields & bit) != 0 ? 1 : 2;

        std::vector<GpuTexture3D>& textures = m_fields[index].textures;
        textures.reserve(count);

        for (size_t copy = 0; copy < count; ++copy)
        {
            const std::wstring name = count == 1 ? FieldNames[index] : std::wstring(FieldNames[index]) + (copy == 0 ? L" A" : L" B");
            textures.emplace_back(m_device, m_descriptors, m_resolution, format, name.c_str());
        }
    }

    m_nullVectorViews = GpuTexture3D::CreateNullViews(m_device, m_descriptors, VectorFormat);
    m_nullScalarViews = GpuTexture3D::CreateNullViews(m_device, m_descriptors, ScalarFormat);
}

void FluidSolver::ResetIfRequested(ID3D12GraphicsCommandList* commandList)
{
    if (!std::exchange(m_resetPending, false))
    {
        return;
    }

    BarrierBatch barriers(commandList);
    for (FieldTextures& field : m_fields)
    {
        for (GpuTexture3D& texture : field.textures)
        {
            texture.Transition(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    barriers.Flush();

    constexpr float Zero[4] = {};

    for (FieldTextures& field : m_fields)
    {
        for (GpuTexture3D& texture : field.textures)
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE uav = texture.Uav();
            const DescriptorTable table = m_descriptors.Upload({ &uav, 1 });
            commandList->ClearUnorderedAccessViewFloat(table.gpu, uav, texture.Resource(), Zero, 0, nullptr);
        }
        field.current = 0;
    }
}

void FluidSolver::Step(ID3D12GraphicsCommandList* commandList, const FluidSettings& settings, float dt, float time)
{
    const FluidConstants constants = {
        .gridSize = m_resolution,
        .dt = dt,
        .emitterPosition = settings.emitterPosition,
        .emitterRadius = settings.emitterRadius,
        .emitterVelocity = settings.emitterVelocity,
        .buoyancy = settings.buoyancy,
        .ambientTemperature = settings.ambientTemperature,
        .vorticityStrength = settings.vorticityStrength,
        .densityDissipation = settings.densityDissipation,
        .temperatureDissipation = settings.temperatureDissipation,
        .emitDensity = settings.emitDensity,
        .emitTemperature = settings.emitTemperature,
        .time = time,
        .gravity = settings.gravity,
        .velocityDissipation = settings.velocityDissipation,
        .windStrength = settings.windStrength,
        .windGustFrequency = settings.windGustFrequency,
        .windDrag = settings.windDrag,
        .windDirection = settings.windDirection,
        .macCormackStrength = settings.macCormackStrength,
        .turbulenceStrength = settings.turbulenceStrength,
        .turbulenceScale = settings.turbulenceScale,
        .boundaryFade = settings.boundaryFade,
    };

    commandList->SetComputeRootSignature(m_rootSignature.Get());
    commandList->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(uint32_t), &constants, 0);

    Run(commandList, Pass::Inject);
    Run(commandList, Pass::Forces);
    Run(commandList, Pass::AdvectVelocity);
    Run(commandList, Pass::CorrectVelocity);
    Run(commandList, Pass::Divergence);

    // The pressure field is deliberately not cleared. The solution changes very
    // little from one step to the next, so starting from the previous one means
    // the same iteration count refines an already good guess instead of walking
    // out from zero every time. The open top pins the constant, which is what
    // makes warm starting well posed here.
    for (int iteration = 0; iteration < settings.pressureIterations; ++iteration)
    {
        Run(commandList, Pass::Jacobi);
    }

    Run(commandList, Pass::Project);
    Run(commandList, Pass::AdvectScalars);
    Run(commandList, Pass::CorrectScalars);
}

void FluidSolver::Run(ID3D12GraphicsCommandList* commandList, Pass pass)
{
    const PassDesc& desc = Passes[std::to_underlying(pass)];

    // SRVs of all fields first, then their UAVs, in the order of the root signature.
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2 * FieldCount> table;
    BarrierBatch barriers(commandList);

    for (size_t index = 0; index < FieldCount; ++index)
    {
        const uint32_t bit = 1u << index;
        FieldTextures& field = m_fields[index];
        const TextureViews& null = (VectorFields & bit) != 0 ? m_nullVectorViews : m_nullScalarViews;

        table[index] = null.srv;
        if ((desc.reads & bit) != 0)
        {
            field.Read().Transition(barriers, ShaderReadState);
            table[index] = field.Read().Srv();
        }

        table[FieldCount + index] = null.uav;
        if ((desc.writes & bit) != 0)
        {
            field.Write().Transition(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            table[FieldCount + index] = field.Write().Uav();
        }
    }

    barriers.Flush();

    commandList->SetPipelineState(m_pipelines[std::to_underlying(pass)].Get());
    commandList->SetComputeRootDescriptorTable(1, m_descriptors.Upload(table).gpu);
    commandList->Dispatch(m_groupCount.x, m_groupCount.y, m_groupCount.z);

    for (size_t index = 0; index < FieldCount; ++index)
    {
        if ((desc.writes & (1u << index)) != 0)
        {
            m_fields[index].Swap();
        }
    }
}
