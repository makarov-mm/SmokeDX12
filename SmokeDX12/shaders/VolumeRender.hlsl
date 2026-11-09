// VolumeRender.hlsl
// Single pass renderer for the smoke grid.
//
// The pixel shader builds a primary ray, resolves the background first
// (an infinite ground plane or the sky), then integrates the radiative transfer
// equation front to back through the simulation box and composites the result
// over that background.
//
// Three things do most of the work for the look:
//   * sub grid detail, noise that erodes the field where it is already thin, so
//     the plume has structure finer than a cell instead of the blobby silhouette
//     the grid resolution would otherwise impose;
//   * an exponentially expanding march towards the light, which spends its
//     samples near the shading point where the shadow gradient actually is;
//   * ambient light occluded by a short march towards the sky, so the inside of
//     the column is darker than its rim.

#include "Noise.hlsli"

cbuffer RenderConstants : register(b0)
{
    float3 CameraPosition;
    float TanHalfFov;

    float3 CameraRight;
    float AspectRatio;

    float3 CameraUp;
    float DensityScale;

    float3 CameraForward;
    float Absorption;

    float3 LightDirection;
    float LightIntensity;

    float3 LightColor;
    uint StepCount;

    float3 BoxSize;
    float Exposure;

    float EmissionScale;
    uint ShadowSteps;
    float Time;
    float TemperatureReference;

    float3 SkyTopColor;
    float FloorHeight;

    float3 SkyBottomColor;
    float FogDensity;

    float3 FloorColor;
    float Anisotropy;

    float3 AmbientColor;
    float GridScale;

    float DetailStrength;
    float DetailScale;
    float DetailDrift;
    float PowderStrength;
};

SamplerState LinearClamp : register(s0);

Texture3D<float> DensityTexture : register(t0);
Texture3D<float> TemperatureTexture : register(t1);

static const float Pi = 3.14159265359f;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);

    VertexOutput output;
    output.ndc = float2(corner.x * 2.0f - 1.0f, 1.0f - corner.y * 2.0f);
    output.position = float4(output.ndc, 0.0f, 1.0f);
    return output;
}

// Interleaved gradient noise, cheap and much better than a sine hash for
// dithering the ray march start offset.
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

float Fbm3(float3 p)
{
    return 0.5714f * ValueNoise(p) +
           0.2857f * ValueNoise(p * 2.03f + 11.3f) +
           0.1429f * ValueNoise(p * 4.11f + 27.9f);
}

bool IntersectBox(float3 origin, float3 direction, float3 halfSize, out float tEnter, out float tExit)
{
    float3 inverseDirection = 1.0f / direction;
    float3 t0 = (-halfSize - origin) * inverseDirection;
    float3 t1 = (halfSize - origin) * inverseDirection;

    float3 tSmall = min(t0, t1);
    float3 tLarge = max(t0, t1);

    tEnter = max(max(tSmall.x, tSmall.y), tSmall.z);
    tExit = min(min(tLarge.x, tLarge.y), tLarge.z);
    tEnter = max(tEnter, 0.0f);

    return tExit > tEnter;
}

// Zero outside the box. One exit per function keeps FXC from warning about
// uninitialised return values once these are inlined into the march loops.
float SampleDensityRaw(float3 position)
{
    float3 uvw = position / BoxSize + 0.5f;
    float density = 0.0f;

    if (all(uvw >= 0.0f) && all(uvw <= 1.0f))
    {
        density = DensityTexture.SampleLevel(LinearClamp, uvw, 0.0f);
    }

    return density;
}

// The detail field drifts upwards and slowly sideways, so it travels roughly
// with the plume instead of sitting still in world space while the smoke slides
// through it, which is what makes a static noise texture read as a fixed
// pattern painted over moving gas.
float3 DetailPosition(float3 position)
{
    float3 p = position * DetailScale;
    p.y -= Time * DetailDrift;
    p.x -= Time * DetailDrift * 0.35f;
    return p;
}

// Erode the field where it is already thin. The dense core is left alone,
// otherwise the whole column breaks into disconnected speckles.
float ApplyDetail(float raw, float noise)
{
    float edge = 1.0f - saturate(raw * 3.0f);
    return max(raw - DetailStrength * edge * noise, 0.0f);
}

// The secondary rays deliberately skip the detail noise and read the raw field.
// Almost every sample in the frame is a shadow sample, so this is where the
// noise would cost the most, and it is also where it matters least: the shadow
// term is an exponential of an integral and the multiple scattering octaves
// blur it further, so eroded wisps are invisible in it. Erosion only removes
// thin material anyway, which contributes very little optical depth.
float SampleDensityShadow(float3 position)
{
    return SampleDensityRaw(position) * DensityScale;
}

float SampleTemperature(float3 position)
{
    float3 uvw = position / BoxSize + 0.5f;
    float temperature = 0.0f;

    if (all(uvw >= 0.0f) && all(uvw <= 1.0f))
    {
        temperature = TemperatureTexture.SampleLevel(LinearClamp, uvw, 0.0f);
    }

    return temperature;
}

// Optical depth along a direction, marched with exponentially growing steps.
// A uniform march across the whole box spends most of its samples far away
// where the medium is smooth, and resolves the contact region, which is the
// part the eye reads as shape, with two or three samples. Growing the step
// inverts that budget for the same cost.
float OpticalDepthTowards(float3 origin, float3 direction, uint steps)
{
    float longest = max(max(BoxSize.x, BoxSize.y), BoxSize.z);
    float stepSize = longest / 48.0f;
    float t = 0.0f;
    float sum = 0.0f;

    [loop]
    for (uint i = 0; i < steps; ++i)
    {
        sum += SampleDensityShadow(origin + direction * (t + stepSize * 0.5f)) * stepSize;
        t += stepSize;
        stepSize *= 1.6f;
    }

    return sum;
}

float HenyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    float denominator = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * Pi * pow(max(denominator, 1e-4f), 1.5f));
}

// Smoke scatters strongly forward but also has a weak backward lobe. A single
// lobe makes the plume look flat from every angle except directly against the
// light.
float DualLobePhase(float cosTheta, float g)
{
    return lerp(HenyeyGreenstein(cosTheta, -g * 0.35f), HenyeyGreenstein(cosTheta, g), 0.7f);
}

// Rough blackbody style ramp. The input is already normalised by the reference
// temperature, so a value of one is the hottest part of the source.
float3 EmissionColor(float t)
{
    float clamped = saturate(t);

    float3 deepRed = float3(0.90f, 0.09f, 0.01f);
    float3 orange = float3(1.00f, 0.38f, 0.05f);
    float3 yellow = float3(1.00f, 0.80f, 0.28f);
    float3 white = float3(1.00f, 0.96f, 0.88f);

    float3 color = lerp(deepRed, orange, smoothstep(0.00f, 0.40f, clamped));
    color = lerp(color, yellow, smoothstep(0.40f, 0.75f, clamped));
    color = lerp(color, white, smoothstep(0.75f, 1.00f, clamped));

    // Only genuinely hot gas glows, and the falloff is steep so the plume cools
    // into grey smoke instead of staying a blown out white ball.
    float intensity = pow(saturate(t - 0.10f), 4.0f);
    return color * intensity;
}

float3 ShadeSky(float3 direction, float3 lightDirection)
{
    float height = saturate(direction.y * 0.5f + 0.5f);
    float3 sky = lerp(SkyBottomColor, SkyTopColor, pow(height, 1.5f));

    float cosAngle = saturate(dot(direction, lightDirection));
    sky += LightColor * pow(cosAngle, 900.0f) * 12.0f;
    sky += LightColor * pow(cosAngle, 6.0f) * 0.05f;

    return sky;
}

float3 ShadeFloor(float3 position, float3 lightDirection)
{
    float2 coordinate = position.xz * GridScale;

    float2 derivative = max(fwidth(coordinate), 1e-5f);
    float2 fine = abs(frac(coordinate) - 0.5f) / derivative;
    float fineLine = 1.0f - saturate(min(fine.x, fine.y) - 0.6f);

    float2 coarseCoordinate = coordinate * 0.25f;
    float2 coarseDerivative = max(fwidth(coarseCoordinate), 1e-5f);
    float2 coarse = abs(frac(coarseCoordinate) - 0.5f) / coarseDerivative;
    float coarseLine = 1.0f - saturate(min(coarse.x, coarse.y) - 0.6f);

    float3 base = FloorColor;
    base = lerp(base, FloorColor * 1.8f, fineLine * 0.30f);
    base = lerp(base, FloorColor * 3.2f, coarseLine * 0.40f);

    // The volume casts a real shadow on the ground.
    float opticalDepth = OpticalDepthTowards(position + float3(0.0f, 0.003f, 0.0f), lightDirection, ShadowSteps);
    float shadow = exp(-opticalDepth * Absorption);

    float3 lit = base * (AmbientColor * 2.2f + LightColor * LightIntensity * 0.055f * shadow * saturate(lightDirection.y));

    // Warm bounce light pooling under the source.
    float radialSquared = dot(position.xz, position.xz);
    lit += float3(1.0f, 0.42f, 0.14f) * EmissionScale * 0.020f / (1.0f + 26.0f * radialSquared);

    float distanceToCamera = length(position - CameraPosition);
    float fog = 1.0f - exp(-distanceToCamera * FogDensity);
    float3 horizon = lerp(SkyBottomColor, SkyTopColor, 0.2f);

    return lerp(lit, horizon, saturate(fog));
}

float3 ToneMap(float3 color)
{
    // Narkowicz fit of the ACES filmic curve. It holds the saturation of the hot
    // core together instead of shifting it towards white the way the older
    // Hejl curve does.
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;

    float3 mapped = saturate((color * (a * color + b)) / (color * (c * color + d) + e));
    return pow(mapped, 1.0f / 2.2f);
}

float4 PSMain(VertexOutput input) : SV_Target
{
    float3 direction = normalize(
        CameraForward +
        CameraRight * input.ndc.x * TanHalfFov * AspectRatio +
        CameraUp * input.ndc.y * TanHalfFov);

    float3 lightDirection = normalize(LightDirection);

    // Resolve whatever sits behind the volume first.
    float3 background;
    float backgroundDistance = 1e6f;

    float floorT = -1.0f;
    if (direction.y < -1e-4f)
    {
        floorT = (FloorHeight - CameraPosition.y) / direction.y;
    }

    if (floorT > 0.0f)
    {
        float3 hit = CameraPosition + direction * floorT;
        background = ShadeFloor(hit, lightDirection);
        backgroundDistance = floorT;
    }
    else
    {
        background = ShadeSky(direction, lightDirection);
    }

    float dither = InterleavedGradientNoise(input.position.xy + frac(Time) * 137.0f);

    float tEnter;
    float tExit;
    if (!IntersectBox(CameraPosition, direction, BoxSize * 0.5f, tEnter, tExit))
    {
        return float4(ToneMap(background * Exposure) + (dither - 0.5f) / 255.0f, 1.0f);
    }

    tExit = min(tExit, backgroundDistance);
    if (tExit <= tEnter)
    {
        return float4(ToneMap(background * Exposure) + (dither - 0.5f) / 255.0f, 1.0f);
    }

    uint steps = max(1u, StepCount);
    uint shadowSteps = max(1u, ShadowSteps);

    float stepSize = (tExit - tEnter) / steps;
    float t = tEnter + dither * stepSize;

    // Multiple scattering octaves. Each one sees a thinner medium and a flatter
    // phase function, which is what gives smoke its soft interior. The phase
    // function only depends on the angle between the ray and the light, so the
    // weight of every octave is the same for all samples of the ray.
    float cosTheta = dot(direction, lightDirection);
    float3 octaveWeight = float3(
        DualLobePhase(cosTheta, Anisotropy),
        0.55f * DualLobePhase(cosTheta, Anisotropy * 0.5f),
        (0.55f * 0.55f) * DualLobePhase(cosTheta, Anisotropy * 0.25f));
    float3 octaveAttenuation = float3(1.0f, 0.45f, 0.45f * 0.45f);

    float3 accumulated = float3(0.0f, 0.0f, 0.0f);
    float transmittance = 1.0f;

    [loop]
    for (uint i = 0; i < steps && t < tExit; ++i)
    {
        float3 position = CameraPosition + direction * t;

        // Most of a ray through this box is empty. Testing the raw field first
        // is one texture fetch, and it lets empty space be crossed in strides
        // instead of one uniform step at a time.
        float raw = SampleDensityRaw(position);
        if (raw < 0.0015f)
        {
            t += stepSize * 2.5f;
            continue;
        }

        float density = ApplyDetail(raw, Fbm3(DetailPosition(position))) * DensityScale;
        if (density > 0.0f)
        {
            float extinction = density * Absorption;
            float opticalDepth = OpticalDepthTowards(position, lightDirection, shadowSteps) * Absorption;

            float scattering = dot(octaveWeight, exp(-opticalDepth * octaveAttenuation));

            // Single scattering overestimates the brightness of thin gas facing
            // the light. The powder term darkens it back down and is what makes
            // the lit side of a plume read as volume rather than as fog.
            float powder = 1.0f - exp(-density * 3.0f);
            float3 direct = LightColor * LightIntensity * scattering * lerp(1.0f, powder, PowderStrength);

            // Ambient light comes from the sky, so it should be occluded by
            // whatever is stacked above this point. A flat ambient term added
            // everywhere is exactly what flattens a volume out.
            float skyDepth = OpticalDepthTowards(position, float3(0.0f, 1.0f, 0.0f), 4) * Absorption;
            float3 ambient = AmbientColor * (0.25f + 0.75f * exp(-skyDepth * 0.6f));

            float temperature = SampleTemperature(position);
            float3 emitted = EmissionColor(temperature / max(TemperatureReference, 1e-3f)) * EmissionScale;

            float alpha = 1.0f - exp(-extinction * stepSize);
            accumulated += transmittance * alpha * (direct + ambient + emitted);
            transmittance *= 1.0f - alpha;

            if (transmittance < 0.004f)
            {
                break;
            }
        }

        t += stepSize;
    }

    float3 color = accumulated + transmittance * background;

    // A little dither on the way out, the sky gradient bands badly without it.
    return float4(ToneMap(color * Exposure) + (dither - 0.5f) / 255.0f, 1.0f);
}
