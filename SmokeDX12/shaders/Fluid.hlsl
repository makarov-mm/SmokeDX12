// Fluid.hlsl
// Compute passes of an Eulerian smoke solver running on a 3D grid.
//
// Every pass sees the same descriptor table: field i of FluidSolver::Field is
// bound as SRV t<i> and as UAV u<i>. A pass reads the current texture of a field
// through its SRV, so trilinear sampling is available, and writes the next one
// through its UAV. Slots a pass does not use hold null descriptors.
//
// Advection is MacCormack: a forward semi Lagrangian step, a reverse step to
// measure the error of that step, and a corrected result that falls back to the
// forward step wherever it leaves the range of the trilinear neighbourhood the
// value was fetched from. Plain semi Lagrangian advection is first order and its
// numerical diffusion is what turns a plume into a featureless grey blob after a
// few seconds.

#include "Noise.hlsli"

cbuffer FluidConstants : register(b0)
{
    uint3 GridSize;
    float Dt;

    float3 EmitterPosition;
    float EmitterRadius;

    float3 EmitterVelocity;
    float Buoyancy;

    float AmbientTemperature;
    float VorticityStrength;
    float DensityDissipation;
    float TemperatureDissipation;

    float EmitDensity;
    float EmitTemperature;
    float Time;
    float Gravity;

    float VelocityDissipation;
    float WindStrength;
    float WindGustFrequency;
    float WindDrag;

    float3 WindDirection;
    float MacCormackStrength;

    float TurbulenceStrength;
    float TurbulenceScale;
    float BoundaryFade;
    float Padding0;
};

SamplerState LinearClamp : register(s0);

Texture3D<float4> Velocity       : register(t0);
Texture3D<float4> VelocityHat    : register(t1);
Texture3D<float>  Density        : register(t2);
Texture3D<float>  DensityHat     : register(t3);
Texture3D<float>  Temperature    : register(t4);
Texture3D<float>  TemperatureHat : register(t5);
Texture3D<float>  Pressure       : register(t6);
Texture3D<float>  Divergence     : register(t7);

RWTexture3D<float4> VelocityOut       : register(u0);
RWTexture3D<float4> VelocityHatOut    : register(u1);
RWTexture3D<float>  DensityOut        : register(u2);
RWTexture3D<float>  DensityHatOut     : register(u3);
RWTexture3D<float>  TemperatureOut    : register(u4);
RWTexture3D<float>  TemperatureHatOut : register(u5);
RWTexture3D<float>  PressureOut       : register(u6);
RWTexture3D<float>  DivergenceOut     : register(u7);

#define GROUP_SIZE 8

// The domain is not a cube. Cells are cubic, so the world aspect of the box is
// the grid size normalised by its smallest axis.
float3 BoxAspect()
{
    float smallest = min(min(GridSize.x, GridSize.y), GridSize.z);
    return float3(GridSize) / smallest;
}

bool IsOutside(uint3 cell)
{
    return cell.x >= GridSize.x || cell.y >= GridSize.y || cell.z >= GridSize.z;
}

// Only the floor is solid. The box is a piece of open air, not a room: closing
// the side walls forces the wind driven part of the plume to turn round and come
// back down, and that return flow mixes the whole domain into a uniform fog
// instead of leaving a plume behind.
bool IsSolidBoundary(uint3 cell)
{
    return cell.y == 0u;
}

float3 CellToUvw(uint3 cell)
{
    return (float3(cell) + 0.5f) / float3(GridSize);
}

int3 ClampCell(int3 cell)
{
    return clamp(cell, int3(0, 0, 0), int3(GridSize) - int3(1, 1, 1));
}

float3 LoadVelocity(int3 cell)
{
    return Velocity[uint3(ClampCell(cell))].xyz;
}

// Pressure boundaries. Below the floor the ghost value is a copy of the cell,
// which is the zero gradient Neumann condition of a solid wall. On the other
// five faces it is zero, the Dirichlet condition of an open outlet, and that is
// what lets gas leave the domain instead of recirculating inside it. Pinning the
// pressure at an outlet also fixes the constant that a pure Neumann problem
// leaves undetermined.
float LoadPressure(int3 cell)
{
    bool outlet = cell.y >= 0 && (
        cell.x < 0 || cell.z < 0 ||
        cell.x >= int(GridSize.x) || cell.y >= int(GridSize.y) || cell.z >= int(GridSize.z));

    return outlet ? 0.0f : Pressure[uint3(ClampCell(cell))];
}

// Density and temperature are faded out in a thin shell along the five open
// faces, so nothing accumulates against a boundary as a flat sheet. The floor is
// solid and is deliberately not faded, the source sits on it.
float BoundaryFalloff(float3 uvw)
{
    float fade = 1.0f - smoothstep(1.0f - BoundaryFade, 1.0f, uvw.y);
    fade *= smoothstep(0.0f, BoundaryFade, uvw.x);
    fade *= 1.0f - smoothstep(1.0f - BoundaryFade, 1.0f, uvw.x);
    fade *= smoothstep(0.0f, BoundaryFade, uvw.z);
    fade *= 1.0f - smoothstep(1.0f - BoundaryFade, 1.0f, uvw.z);
    return fade;
}

// Dissipation constants are authored per 60 Hz step, this keeps them meaningful
// if the fixed step ever changes.
float DissipationOverStep(float perFrame)
{
    return pow(max(perFrame, 1e-4f), Dt * 60.0f);
}

// ---------------------------------------------------------------------------
// Noise
// ---------------------------------------------------------------------------

float Fbm2(float3 p)
{
    return 0.6667f * ValueNoise(p) + 0.3333f * ValueNoise(p * 2.03f + 19.7f);
}

float3 NoisePotential(float3 p)
{
    return float3(
        Fbm2(p),
        Fbm2(p + float3(31.416f, 47.853f, 12.793f)),
        Fbm2(p + float3(73.156f, 11.937f, 91.421f)));
}

// Curl of a vector potential built from noise. The result is divergence free by
// construction, which matters a lot here: a plain random velocity field is
// almost entirely gradient, so the pressure projection deletes it on the same
// frame and all it leaves behind is a flicker.
float3 CurlNoise(float3 p)
{
    const float e = 0.4f;

    float3 xm = NoisePotential(p - float3(e, 0.0f, 0.0f));
    float3 xp = NoisePotential(p + float3(e, 0.0f, 0.0f));
    float3 ym = NoisePotential(p - float3(0.0f, e, 0.0f));
    float3 yp = NoisePotential(p + float3(0.0f, e, 0.0f));
    float3 zm = NoisePotential(p - float3(0.0f, 0.0f, e));
    float3 zp = NoisePotential(p + float3(0.0f, 0.0f, e));

    float3 curl;
    curl.x = (yp.z - ym.z) - (zp.y - zm.y);
    curl.y = (zp.x - zm.x) - (xp.z - xm.z);
    curl.z = (xp.y - xm.y) - (yp.x - ym.x);

    return curl / (2.0f * e);
}

// Curl of the velocity field, computed with central differences in cell units.
float3 CurlAt(int3 cell)
{
    float3 vxp = LoadVelocity(cell + int3(1, 0, 0));
    float3 vxm = LoadVelocity(cell - int3(1, 0, 0));
    float3 vyp = LoadVelocity(cell + int3(0, 1, 0));
    float3 vym = LoadVelocity(cell - int3(0, 1, 0));
    float3 vzp = LoadVelocity(cell + int3(0, 0, 1));
    float3 vzm = LoadVelocity(cell - int3(0, 0, 1));

    float3 curl;
    curl.x = (vyp.z - vym.z) - (vzp.y - vzm.y);
    curl.y = (vzp.x - vzm.x) - (vxp.z - vxm.z);
    curl.z = (vxp.y - vxm.y) - (vyp.x - vym.x);
    return 0.5f * curl;
}

// ---------------------------------------------------------------------------
// MacCormack
// ---------------------------------------------------------------------------

// The base cell of the trilinear footprint a sample at uvw reads from.
int3 FootprintBase(float3 uvw)
{
    return int3(floor(uvw * float3(GridSize) - 0.5f));
}

// The MacCormack correction of a scalar field. `field` holds the values at step
// n and `forward` the result of the forward step, which `baseCell` points into.
//
// The correction is only trustworthy inside the range of the values the forward
// step interpolated between. Outside it the scheme overshoots and rings, so
// those cells fall back to the plain first order result. Clamping to the edge of
// the range instead of reverting looks equivalent and is not: it keeps a biased
// extreme value, which turns advection into a max filter and pumps mass into the
// domain every step.
float CorrectScalar(Texture3D<float> field, Texture3D<float> forward, uint3 id, float3 uvw, float3 offset, int3 baseCell)
{
    float current = field[id];
    float forwardValue = forward[id];
    float reverseValue = forward.SampleLevel(LinearClamp, uvw + offset, 0.0f);

    // Half of the round trip error is the second order correction term.
    float corrected = forwardValue + 0.5f * MacCormackStrength * (current - reverseValue);

    float minimum = 1e30f;
    float maximum = -1e30f;

    [unroll]
    for (int k = 0; k < 2; ++k)
    {
        [unroll]
        for (int j = 0; j < 2; ++j)
        {
            [unroll]
            for (int i = 0; i < 2; ++i)
            {
                float sampled = field[uint3(ClampCell(baseCell + int3(i, j, k)))];
                minimum = min(minimum, sampled);
                maximum = max(maximum, sampled);
            }
        }
    }

    return (corrected < minimum || corrected > maximum) ? forwardValue : corrected;
}

// ---------------------------------------------------------------------------
// Source injection
// reads velocity, density, temperature
// writes velocity, density, temperature
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSInject(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    float3 uvw = CellToUvw(id);
    float3 aspect = BoxAspect();

    // The source wanders slowly, which is enough to stop the plume from growing
    // as a perfectly symmetric mushroom.
    float3 center = EmitterPosition;
    center.x += (ValueNoise(float3(Time * 0.37f, 0.0f, 0.0f)) - 0.5f) * EmitterRadius * 0.8f / aspect.x;
    center.z += (ValueNoise(float3(0.0f, 0.0f, Time * 0.31f + 17.0f)) - 0.5f) * EmitterRadius * 0.8f / aspect.z;

    float3 offset = (uvw - center) * aspect;
    float radialDistance = length(offset);
    float falloff = 1.0f - smoothstep(EmitterRadius * 0.25f, EmitterRadius, radialDistance);

    // Everything below is expensive and only meaningful inside the source, so
    // the rest of the grid just copies straight through the ping pong.
    if (falloff <= 0.0f)
    {
        VelocityOut[id] = float4(Velocity[id].xyz, 0.0f);
        DensityOut[id] = Density[id];
        TemperatureOut[id] = Temperature[id];
        return;
    }

    // Two octaves rather than one, so the source already carries structure at
    // more than a single scale and the plume is not a smooth cone near the base.
    float shell = Fbm2(uvw * aspect * 14.0f + float3(0.0f, -Time * 2.2f, Time * 0.8f));
    float shaped = falloff * (0.45f + 1.1f * shell);

    float density = Density[id] + EmitDensity * shaped * Dt;
    float temperature = Temperature[id] + EmitTemperature * shaped * Dt;

    float3 velocity = Velocity[id].xyz;

    // Swirl around the vertical axis, weighted by the distance from the centre
    // so the core rises straight and only the rim rotates, with a sign that
    // varies over the source rather than a uniform spin.
    float3 swirl = cross(float3(0.0f, 1.0f, 0.0f), float3(offset.x, 0.0f, offset.z));

    // Divergence free turbulence. Per cell white noise would be almost entirely
    // gradient, so the pressure projection would delete it on the same frame it
    // was added and all that survived would be a flicker at the source.
    float3 turbulence = CurlNoise(uvw * aspect * TurbulenceScale * 3.0f + float3(0.0f, -Time * 1.3f, 0.0f));

    float3 injected = EmitterVelocity + swirl * 40.0f * (shell - 0.35f) + turbulence * TurbulenceStrength * 4.0f;
    velocity += injected * falloff * Dt;

    VelocityOut[id] = float4(velocity, 0.0f);
    DensityOut[id] = density;
    TemperatureOut[id] = temperature;
}

// ---------------------------------------------------------------------------
// External forces: thermal buoyancy, weight of the smoke, wind, vorticity
// confinement and turbulence
// reads velocity, density, temperature
// writes velocity
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSForces(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    if (IsSolidBoundary(id))
    {
        VelocityOut[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 velocity = Velocity[id].xyz;
    float temperature = Temperature[id];
    float density = Density[id];

    float lift = Buoyancy * (temperature - AmbientTemperature) - Gravity * density;
    velocity += Dt * lift * float3(0.0f, 1.0f, 0.0f);

    // Wind is modelled as drag towards a target field instead of a constant
    // force, so it cannot accelerate the gas beyond the wind speed itself.
    // Gusts modulate the strength and slowly steer the direction over time,
    // and the whole thing fades in with height so the source stays anchored.
    float3 uvw = CellToUvw(id);
    float gust = 0.55f + 0.65f * ValueNoise(float3(Time * WindGustFrequency, uvw.y * 2.0f, 7.0f));
    float steer = (ValueNoise(float3(3.0f, Time * WindGustFrequency * 0.35f, 11.0f)) - 0.5f) * 1.2f;

    float3 windDirection = normalize(WindDirection + float3(-steer * WindDirection.z, 0.0f, steer * WindDirection.x));
    float heightFade = smoothstep(0.08f, 0.45f, uvw.y);

    float3 windTarget = windDirection * WindStrength * gust * heightFade;

    // Only the horizontal components relax towards the wind. Dragging the
    // vertical component too would fight buoyancy and cap the rise height.
    float relaxation = saturate(WindDrag * Dt);
    velocity.xz += (windTarget.xz - velocity.xz) * relaxation;

    // Everything from here on only shapes gas that is actually there. Stirring
    // the empty part of the domain costs time and produces nothing visible.
    float smokeMask = saturate(density * 6.0f);

    if (smokeMask > 0.001f)
    {
        // Vorticity confinement puts back the small scale rotation that advection smears out.
        int3 cell = int3(id);
        float3 curl = CurlAt(cell);

        float mxp = length(CurlAt(cell + int3(1, 0, 0)));
        float mxm = length(CurlAt(cell - int3(1, 0, 0)));
        float myp = length(CurlAt(cell + int3(0, 1, 0)));
        float mym = length(CurlAt(cell - int3(0, 1, 0)));
        float mzp = length(CurlAt(cell + int3(0, 0, 1)));
        float mzm = length(CurlAt(cell - int3(0, 0, 1)));

        float3 gradient = 0.5f * float3(mxp - mxm, myp - mym, mzp - mzm);
        float3 normal = gradient / (length(gradient) + 1e-5f);
        velocity += VorticityStrength * Dt * smokeMask * cross(normal, curl);

        // A slow divergence free turbulence field on top, which gives the plume
        // detail at a scale the grid can still resolve but the solver would
        // never generate on its own at this resolution.
        float3 turbulence = CurlNoise(uvw * BoxAspect() * TurbulenceScale + float3(0.0f, -Time * 0.6f, Time * 0.15f));
        float altitude = smoothstep(0.05f, 0.5f, uvw.y);
        velocity += turbulence * TurbulenceStrength * smokeMask * altitude * Dt;
    }

    VelocityOut[id] = float4(velocity, 0.0f);
}

// ---------------------------------------------------------------------------
// Forward semi Lagrangian step of the velocity field, the first half of MacCormack
// reads velocity
// writes velocity hat
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSAdvectVelocity(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    if (IsSolidBoundary(id))
    {
        VelocityHatOut[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 uvw = CellToUvw(id);
    float3 velocity = Velocity[id].xyz;
    float3 previous = uvw - velocity * Dt / float3(GridSize);

    VelocityHatOut[id] = float4(Velocity.SampleLevel(LinearClamp, previous, 0.0f).xyz, 0.0f);
}

// ---------------------------------------------------------------------------
// MacCormack correction of the velocity field, plus dissipation
// reads velocity at step n, velocity hat
// writes velocity at step n+1
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSCorrectVelocity(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    if (IsSolidBoundary(id))
    {
        VelocityOut[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 uvw = CellToUvw(id);
    float3 velocity = Velocity[id].xyz;
    float3 offset = velocity * Dt / float3(GridSize);

    float3 forwardValue = VelocityHat[id].xyz;
    float3 reverseValue = VelocityHat.SampleLevel(LinearClamp, uvw + offset, 0.0f).xyz;

    // Half of the round trip error is the second order correction term.
    float3 corrected = forwardValue + 0.5f * MacCormackStrength * (velocity - reverseValue);

    // The same limiter as CorrectScalar, per component.
    int3 baseCell = FootprintBase(uvw - offset);
    float3 minimum = float3(1e30f, 1e30f, 1e30f);
    float3 maximum = float3(-1e30f, -1e30f, -1e30f);

    [unroll]
    for (int k = 0; k < 2; ++k)
    {
        [unroll]
        for (int j = 0; j < 2; ++j)
        {
            [unroll]
            for (int i = 0; i < 2; ++i)
            {
                float3 sampled = LoadVelocity(baseCell + int3(i, j, k));
                minimum = min(minimum, sampled);
                maximum = max(maximum, sampled);
            }
        }
    }

    if (any(corrected < minimum) || any(corrected > maximum))
    {
        corrected = forwardValue;
    }

    VelocityOut[id] = float4(corrected * DissipationOverStep(VelocityDissipation), 0.0f);
}

// ---------------------------------------------------------------------------
// Divergence of the velocity field
// reads velocity
// writes divergence
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSDivergence(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    int3 cell = int3(id);
    float3 vxp = LoadVelocity(cell + int3(1, 0, 0));
    float3 vxm = LoadVelocity(cell - int3(1, 0, 0));
    float3 vyp = LoadVelocity(cell + int3(0, 1, 0));
    float3 vym = LoadVelocity(cell - int3(0, 1, 0));
    float3 vzp = LoadVelocity(cell + int3(0, 0, 1));
    float3 vzm = LoadVelocity(cell - int3(0, 0, 1));

    DivergenceOut[id] = 0.5f * ((vxp.x - vxm.x) + (vyp.y - vym.y) + (vzp.z - vzm.z));
}

// ---------------------------------------------------------------------------
// One Jacobi iteration of the pressure Poisson equation
// reads pressure, divergence
// writes pressure
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSJacobi(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    int3 cell = int3(id);
    float sum =
        LoadPressure(cell + int3(1, 0, 0)) + LoadPressure(cell - int3(1, 0, 0)) +
        LoadPressure(cell + int3(0, 1, 0)) + LoadPressure(cell - int3(0, 1, 0)) +
        LoadPressure(cell + int3(0, 0, 1)) + LoadPressure(cell - int3(0, 0, 1));

    PressureOut[id] = (sum - Divergence[id]) / 6.0f;
}

// ---------------------------------------------------------------------------
// Subtract the pressure gradient, the result is a divergence free field
// reads velocity, pressure
// writes velocity
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSProject(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    if (IsSolidBoundary(id))
    {
        VelocityOut[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    int3 cell = int3(id);
    float3 gradient = 0.5f * float3(
        LoadPressure(cell + int3(1, 0, 0)) - LoadPressure(cell - int3(1, 0, 0)),
        LoadPressure(cell + int3(0, 1, 0)) - LoadPressure(cell - int3(0, 1, 0)),
        LoadPressure(cell + int3(0, 0, 1)) - LoadPressure(cell - int3(0, 0, 1)));

    VelocityOut[id] = float4(Velocity[id].xyz - gradient, 0.0f);
}

// ---------------------------------------------------------------------------
// Forward semi Lagrangian step of density and temperature
// reads velocity, density, temperature
// writes density hat, temperature hat
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSAdvectScalars(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    float3 uvw = CellToUvw(id);
    float3 velocity = Velocity[id].xyz;
    float3 previous = uvw - velocity * Dt / float3(GridSize);

    DensityHatOut[id] = Density.SampleLevel(LinearClamp, previous, 0.0f);
    TemperatureHatOut[id] = Temperature.SampleLevel(LinearClamp, previous, 0.0f);
}

// ---------------------------------------------------------------------------
// MacCormack correction of density and temperature, plus dissipation, cooling
// and the boundary fade
// reads velocity, density, density hat, temperature, temperature hat
// writes density, temperature
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSCorrectScalars(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    float3 uvw = CellToUvw(id);
    float3 offset = Velocity[id].xyz * Dt / float3(GridSize);
    int3 baseCell = FootprintBase(uvw - offset);
    float fade = BoundaryFalloff(uvw);

    float density = CorrectScalar(Density, DensityHat, id, uvw, offset, baseCell);
    density = max(density, 0.0f) * DissipationOverStep(DensityDissipation);
    DensityOut[id] = density * fade;

    // Temperature cools towards the ambient value rather than towards zero.
    float temperature = CorrectScalar(Temperature, TemperatureHat, id, uvw, offset, baseCell);
    temperature = lerp(AmbientTemperature, temperature, DissipationOverStep(TemperatureDissipation));
    TemperatureOut[id] = lerp(AmbientTemperature, temperature, fade);
}
