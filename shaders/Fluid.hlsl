// Fluid.hlsl
// Compute passes of an Eulerian smoke solver running on a 3D grid.
// Every pass reads through SRVs (so trilinear sampling is available) and writes through UAVs.
// The root signature binds five SRVs (t0..t4) and four UAVs (u0..u3) for every pass,
// unused slots are filled with dummy textures on the CPU side.
//
// Advection is MacCormack: a forward semi Lagrangian step, a reverse step to
// measure the error of that step, and a corrected result clamped to the extrema
// of the trilinear neighbourhood the value was fetched from. Plain semi
// Lagrangian advection is first order and its numerical diffusion is what turns
// a plume into a featureless grey blob after a few seconds.

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

Texture3D<float4> SourceVelocity : register(t0);
Texture3D<float> SourceScalarA : register(t1);
Texture3D<float> SourceScalarB : register(t2);
Texture3D<float> SourceScalarC : register(t3);
Texture3D<float4> SourceVelocityHat : register(t4);

RWTexture3D<float4> TargetVelocity : register(u0);
RWTexture3D<float> TargetScalarA : register(u1);
RWTexture3D<float> TargetScalarB : register(u2);
RWTexture3D<float> TargetScalarC : register(u3);

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
    return SourceVelocity[uint3(ClampCell(cell))].xyz;
}

float LoadScalarA(int3 cell)
{
    return SourceScalarA[uint3(ClampCell(cell))];
}

// Pressure boundaries. Below the floor the ghost value is a copy of the cell,
// which is the zero gradient Neumann condition of a solid wall. On the other
// five faces it is zero, the Dirichlet condition of an open outlet, and that is
// what lets gas leave the domain instead of recirculating inside it. Pinning the
// pressure at an outlet also fixes the constant that a pure Neumann problem
// leaves undetermined.
float LoadPressure(int3 cell)
{
    if (cell.y < 0)
    {
        return SourceScalarA[uint3(ClampCell(cell))];
    }

    bool outside =
        cell.x < 0 || cell.z < 0 ||
        cell.x >= int(GridSize.x) || cell.y >= int(GridSize.y) || cell.z >= int(GridSize.z);

    return outside ? 0.0f : SourceScalarA[uint3(ClampCell(cell))];
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

uint HashUint(uint x)
{
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

// An integer bit mixer instead of the usual sine hash. The sine version repeats
// on an axis aligned lattice at grid scale and that pattern is clearly visible
// once the advection is sharp enough to preserve it.
float Hash(float3 p)
{
    int3 cell = int3(round(p));
    uint mixed = uint(cell.x) * 0x8da6b343u + uint(cell.y) * 0xd8163841u + uint(cell.z) * 0xcb1ab31fu;
    return float(HashUint(mixed)) * (1.0f / 4294967296.0f);
}

float Noise(float3 p)
{
    float3 cell = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);

    float n000 = Hash(cell + float3(0.0f, 0.0f, 0.0f));
    float n100 = Hash(cell + float3(1.0f, 0.0f, 0.0f));
    float n010 = Hash(cell + float3(0.0f, 1.0f, 0.0f));
    float n110 = Hash(cell + float3(1.0f, 1.0f, 0.0f));
    float n001 = Hash(cell + float3(0.0f, 0.0f, 1.0f));
    float n101 = Hash(cell + float3(1.0f, 0.0f, 1.0f));
    float n011 = Hash(cell + float3(0.0f, 1.0f, 1.0f));
    float n111 = Hash(cell + float3(1.0f, 1.0f, 1.0f));

    float x00 = lerp(n000, n100, f.x);
    float x10 = lerp(n010, n110, f.x);
    float x01 = lerp(n001, n101, f.x);
    float x11 = lerp(n011, n111, f.x);

    float y0 = lerp(x00, x10, f.y);
    float y1 = lerp(x01, x11, f.y);

    return lerp(y0, y1, f.z);
}

float Fbm2(float3 p)
{
    return 0.6667f * Noise(p) + 0.3333f * Noise(p * 2.03f + 19.7f);
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

// The base cell of the trilinear footprint a sample at uvw reads from.
int3 FootprintBase(float3 uvw)
{
    return int3(floor(uvw * float3(GridSize) - 0.5f));
}

// ---------------------------------------------------------------------------
// Clearing
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSClearScalar(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    TargetScalarA[id] = 0.0f;
}

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSClearVelocity(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    TargetVelocity[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
}

// ---------------------------------------------------------------------------
// Source injection
// t0 velocity, t1 density, t2 temperature
// u0 velocity, u1 density, u2 temperature
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
    center.x += (Noise(float3(Time * 0.37f, 0.0f, 0.0f)) - 0.5f) * EmitterRadius * 0.8f / aspect.x;
    center.z += (Noise(float3(0.0f, 0.0f, Time * 0.31f + 17.0f)) - 0.5f) * EmitterRadius * 0.8f / aspect.z;

    float3 offset = (uvw - center) * aspect;
    float radialDistance = length(offset);
    float falloff = 1.0f - smoothstep(EmitterRadius * 0.25f, EmitterRadius, radialDistance);

    // Everything below is expensive and only meaningful inside the source, so
    // the rest of the grid just copies straight through the ping pong.
    if (falloff <= 0.0f)
    {
        TargetVelocity[id] = float4(SourceVelocity[id].xyz, 0.0f);
        TargetScalarA[id] = SourceScalarA[id];
        TargetScalarB[id] = SourceScalarB[id];
        return;
    }

    // Two octaves rather than one, so the source already carries structure at
    // more than a single scale and the plume is not a smooth cone near the base.
    float shell = Fbm2(uvw * aspect * 14.0f + float3(0.0f, -Time * 2.2f, Time * 0.8f));
    float shaped = falloff * (0.45f + 1.1f * shell);

    float density = SourceScalarA[id] + EmitDensity * shaped * Dt;
    float temperature = SourceScalarB[id] + EmitTemperature * shaped * Dt;

    float3 velocity = SourceVelocity[id].xyz;

    // Swirl around the vertical axis, weighted by the distance from the centre
    // so the core rises straight and only the rim rotates, with a sign that
    // varies over the source rather than a uniform spin.
    float3 swirl = cross(float3(0.0f, 1.0f, 0.0f), float3(offset.x, 0.0f, offset.z));

    // Divergence free turbulence, instead of the per cell white noise the
    // previous version injected. White noise is almost entirely gradient, so the
    // pressure projection deletes it on the same frame it was added and all that
    // survives is a flicker at the source.
    float3 turbulence = CurlNoise(uvw * aspect * TurbulenceScale * 3.0f + float3(0.0f, -Time * 1.3f, 0.0f));

    float3 injected = EmitterVelocity + swirl * 40.0f * (shell - 0.35f) + turbulence * TurbulenceStrength * 4.0f;
    velocity += injected * falloff * Dt;

    TargetVelocity[id] = float4(velocity, 0.0f);
    TargetScalarA[id] = density;
    TargetScalarB[id] = temperature;
}

// ---------------------------------------------------------------------------
// External forces: thermal buoyancy, weight of the smoke, vorticity confinement
// t0 velocity, t1 temperature, t2 density
// u0 velocity
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
        TargetVelocity[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 velocity = SourceVelocity[id].xyz;
    float temperature = SourceScalarA[id];
    float density = SourceScalarB[id];

    float lift = Buoyancy * (temperature - AmbientTemperature) - Gravity * density;
    velocity += Dt * lift * float3(0.0f, 1.0f, 0.0f);

    // Wind is modelled as drag towards a target field instead of a constant
    // force, so it cannot accelerate the gas beyond the wind speed itself.
    // Gusts modulate the strength and slowly steer the direction over time,
    // and the whole thing fades in with height so the source stays anchored.
    float3 uvw = CellToUvw(id);
    float gust = 0.55f + 0.65f * Noise(float3(Time * WindGustFrequency, uvw.y * 2.0f, 7.0f));
    float steer = (Noise(float3(3.0f, Time * WindGustFrequency * 0.35f, 11.0f)) - 0.5f) * 1.2f;

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

    TargetVelocity[id] = float4(velocity, 0.0f);
}

// ---------------------------------------------------------------------------
// Forward semi Lagrangian step of the velocity field, the first half of MacCormack
// t0 velocity, u0 intermediate velocity
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
        TargetVelocity[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 uvw = CellToUvw(id);
    float3 velocity = SourceVelocity[id].xyz;
    float3 previous = uvw - velocity * Dt / float3(GridSize);

    TargetVelocity[id] = float4(SourceVelocity.SampleLevel(LinearClamp, previous, 0.0f).xyz, 0.0f);
}

// ---------------------------------------------------------------------------
// MacCormack correction of the velocity field
// t0 velocity at step n, t4 forward advected velocity
// u0 velocity at step n+1
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSAdvectVelocityCorrect(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    if (IsSolidBoundary(id))
    {
        TargetVelocity[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 uvw = CellToUvw(id);
    float3 velocity = SourceVelocity[id].xyz;
    float3 offset = velocity * Dt / float3(GridSize);

    float3 forwardValue = SourceVelocityHat[id].xyz;
    float3 reverseValue = SourceVelocityHat.SampleLevel(LinearClamp, uvw + offset, 0.0f).xyz;

    // Half of the round trip error is the second order correction term.
    float3 corrected = forwardValue + 0.5f * MacCormackStrength * (velocity - reverseValue);

    // The correction is only trustworthy inside the range of the values the
    // forward step interpolated between. Outside it the scheme overshoots and
    // rings, so those cells fall back to the plain first order result. Clamping
    // to the edge of the range instead of reverting looks equivalent and is not:
    // it keeps a biased extreme value, which turns advection into a max filter
    // and pumps mass into the domain every step.
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

    TargetVelocity[id] = float4(corrected * DissipationOverStep(VelocityDissipation), 0.0f);
}

// ---------------------------------------------------------------------------
// Divergence of the velocity field
// t0 velocity, u1 divergence
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

    float divergence = 0.5f * ((vxp.x - vxm.x) + (vyp.y - vym.y) + (vzp.z - vzm.z));
    TargetScalarA[id] = divergence;
}

// ---------------------------------------------------------------------------
// One Jacobi iteration of the pressure Poisson equation
// t1 pressure, t2 divergence, u1 pressure
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

    float divergence = SourceScalarB[id];
    TargetScalarA[id] = (sum - divergence) / 6.0f;
}

// ---------------------------------------------------------------------------
// Subtract the pressure gradient, the result is a divergence free field
// t0 velocity, t1 pressure, u0 velocity
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
        TargetVelocity[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    int3 cell = int3(id);
    float3 gradient = 0.5f * float3(
        LoadPressure(cell + int3(1, 0, 0)) - LoadPressure(cell - int3(1, 0, 0)),
        LoadPressure(cell + int3(0, 1, 0)) - LoadPressure(cell - int3(0, 1, 0)),
        LoadPressure(cell + int3(0, 0, 1)) - LoadPressure(cell - int3(0, 0, 1)));

    float3 velocity = SourceVelocity[id].xyz - gradient;
    TargetVelocity[id] = float4(velocity, 0.0f);
}

// ---------------------------------------------------------------------------
// Forward semi Lagrangian step of density and temperature
// t0 velocity, t1 density, t2 temperature
// u1 intermediate density, u2 intermediate temperature
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSAdvectScalars(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    float3 uvw = CellToUvw(id);
    float3 velocity = SourceVelocity[id].xyz;
    float3 previous = uvw - velocity * Dt / float3(GridSize);

    TargetScalarA[id] = SourceScalarA.SampleLevel(LinearClamp, previous, 0.0f);
    TargetScalarB[id] = SourceScalarB.SampleLevel(LinearClamp, previous, 0.0f);
}

// ---------------------------------------------------------------------------
// MacCormack correction of the density field, plus dissipation
// t0 velocity, t1 density at step n, t2 forward advected density
// u1 density at step n+1
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSAdvectDensityCorrect(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    float3 uvw = CellToUvw(id);
    float3 velocity = SourceVelocity[id].xyz;
    float3 offset = velocity * Dt / float3(GridSize);

    float current = SourceScalarA[id];
    float forwardValue = SourceScalarB[id];
    float reverseValue = SourceScalarB.SampleLevel(LinearClamp, uvw + offset, 0.0f);

    float corrected = forwardValue + 0.5f * MacCormackStrength * (current - reverseValue);

    int3 baseCell = FootprintBase(uvw - offset);
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
                float sampled = LoadScalarA(baseCell + int3(i, j, k));
                minimum = min(minimum, sampled);
                maximum = max(maximum, sampled);
            }
        }
    }

    if (corrected < minimum || corrected > maximum)
    {
        corrected = forwardValue;
    }

    float density = max(corrected, 0.0f) * DissipationOverStep(DensityDissipation);
    TargetScalarA[id] = density * BoundaryFalloff(uvw);
}

// ---------------------------------------------------------------------------
// MacCormack correction of the temperature field, plus cooling
// t0 velocity, t1 temperature at step n, t2 forward advected temperature
// u1 temperature at step n+1
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSAdvectTemperatureCorrect(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    float3 uvw = CellToUvw(id);
    float3 velocity = SourceVelocity[id].xyz;
    float3 offset = velocity * Dt / float3(GridSize);

    float current = SourceScalarA[id];
    float forwardValue = SourceScalarB[id];
    float reverseValue = SourceScalarB.SampleLevel(LinearClamp, uvw + offset, 0.0f);

    float corrected = forwardValue + 0.5f * MacCormackStrength * (current - reverseValue);

    int3 baseCell = FootprintBase(uvw - offset);
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
                float sampled = LoadScalarA(baseCell + int3(i, j, k));
                minimum = min(minimum, sampled);
                maximum = max(maximum, sampled);
            }
        }
    }

    if (corrected < minimum || corrected > maximum)
    {
        corrected = forwardValue;
    }

    float temperature = lerp(AmbientTemperature, corrected, DissipationOverStep(TemperatureDissipation));
    TargetScalarA[id] = lerp(AmbientTemperature, temperature, BoundaryFalloff(uvw));
}
