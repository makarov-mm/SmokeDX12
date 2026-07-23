// Fluid.hlsl
// Compute passes of a semi Lagrangian Eulerian smoke solver running on a 3D grid.
// Every pass reads through SRVs (so trilinear sampling is available) and writes through UAVs.
// The root signature binds four SRVs (t0..t3) and four UAVs (u0..u3) for every pass,
// unused slots are filled with dummy textures on the CPU side.

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
    float Padding0;
};

SamplerState LinearClamp : register(s0);

Texture3D<float4> SourceVelocity : register(t0);
Texture3D<float> SourceScalarA : register(t1);
Texture3D<float> SourceScalarB : register(t2);
Texture3D<float> SourceScalarC : register(t3);

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

bool IsBoundary(uint3 cell)
{
    return cell.x == 0u || cell.y == 0u || cell.z == 0u ||
           cell.x + 1u == GridSize.x || cell.y + 1u == GridSize.y || cell.z + 1u == GridSize.z;
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

float LoadScalarB(int3 cell)
{
    return SourceScalarB[uint3(ClampCell(cell))];
}

float Hash(float3 p)
{
    return frac(sin(dot(p, float3(12.9898f, 78.233f, 37.719f))) * 43758.5453f);
}

// Value noise, good enough to break the symmetry of the emitter.
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

    // The source wanders slowly, which is enough to stop the plume from growing
    // as a perfectly symmetric mushroom.
    float3 aspect = BoxAspect();
    float3 center = EmitterPosition;
    center.x += (Noise(float3(Time * 0.37f, 0.0f, 0.0f)) - 0.5f) * EmitterRadius * 0.8f / aspect.x;
    center.z += (Noise(float3(0.0f, 0.0f, Time * 0.31f + 17.0f)) - 0.5f) * EmitterRadius * 0.8f / aspect.z;

    float3 offset = (uvw - center) * BoxAspect();
    float radialDistance = length(offset);
    float falloff = 1.0f - smoothstep(EmitterRadius * 0.25f, EmitterRadius, radialDistance);

    // Break the shell of the source up so the plume gets structure straight away.
    float shellNoise = Noise(uvw * 24.0f + float3(0.0f, -Time * 2.6f, Time * 0.9f));
    float shaped = falloff * (0.55f + 0.9f * shellNoise);

    float density = SourceScalarA[id] + EmitDensity * shaped * Dt;
    float temperature = SourceScalarB[id] + EmitTemperature * shaped * Dt;

    float3 velocity = SourceVelocity[id].xyz;

    float3 swirl = cross(float3(0.0f, 1.0f, 0.0f), offset);
    float3 turbulence = float3(
        Noise(uvw * 11.0f + float3(Time * 1.4f, 0.0f, 0.0f)) - 0.5f,
        Noise(uvw * 11.0f + float3(0.0f, Time * 1.1f + 7.0f, 0.0f)) - 0.5f,
        Noise(uvw * 11.0f + float3(0.0f, 0.0f, Time * 1.2f + 23.0f)) - 0.5f) * 2.0f;

    float3 injected = EmitterVelocity + swirl * 55.0f * (shellNoise - 0.5f) + turbulence * 22.0f;
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

    if (IsBoundary(id))
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
    velocity += VorticityStrength * Dt * cross(normal, curl);

    TargetVelocity[id] = float4(velocity, 0.0f);
}

// ---------------------------------------------------------------------------
// Semi Lagrangian advection of the velocity field
// t0 velocity, u0 velocity
// ---------------------------------------------------------------------------

[numthreads(GROUP_SIZE, GROUP_SIZE, GROUP_SIZE)]
void CSAdvectVelocity(uint3 id : SV_DispatchThreadID)
{
    if (IsOutside(id))
    {
        return;
    }

    if (IsBoundary(id))
    {
        TargetVelocity[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 uvw = CellToUvw(id);
    float3 velocity = SourceVelocity[id].xyz;
    float3 previous = uvw - velocity * Dt / float3(GridSize);

    float3 advected = SourceVelocity.SampleLevel(LinearClamp, previous, 0.0f).xyz;
    TargetVelocity[id] = float4(advected * VelocityDissipation, 0.0f);
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
        LoadScalarA(cell + int3(1, 0, 0)) + LoadScalarA(cell - int3(1, 0, 0)) +
        LoadScalarA(cell + int3(0, 1, 0)) + LoadScalarA(cell - int3(0, 1, 0)) +
        LoadScalarA(cell + int3(0, 0, 1)) + LoadScalarA(cell - int3(0, 0, 1));

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

    if (IsBoundary(id))
    {
        TargetVelocity[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    int3 cell = int3(id);
    float3 gradient = 0.5f * float3(
        LoadScalarA(cell + int3(1, 0, 0)) - LoadScalarA(cell - int3(1, 0, 0)),
        LoadScalarA(cell + int3(0, 1, 0)) - LoadScalarA(cell - int3(0, 1, 0)),
        LoadScalarA(cell + int3(0, 0, 1)) - LoadScalarA(cell - int3(0, 0, 1)));

    float3 velocity = SourceVelocity[id].xyz - gradient;
    TargetVelocity[id] = float4(velocity, 0.0f);
}

// ---------------------------------------------------------------------------
// Advection of density and temperature with the projected velocity field
// t0 velocity, t1 density, t2 temperature
// u1 density, u2 temperature
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

    float density = SourceScalarA.SampleLevel(LinearClamp, previous, 0.0f) * DensityDissipation;
    float temperature = SourceScalarB.SampleLevel(LinearClamp, previous, 0.0f);
    temperature = lerp(AmbientTemperature, temperature, TemperatureDissipation);

    TargetScalarA[id] = max(density, 0.0f);
    TargetScalarB[id] = temperature;
}
