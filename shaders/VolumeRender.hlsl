// VolumeRender.hlsl
// Single pass renderer for the smoke grid.
//
// The pixel shader builds a primary ray, resolves the background first
// (an infinite ground plane or the sky), then integrates the radiative transfer
// equation front to back through the simulation box and composites the result
// over that background.
//
// Scattering uses a Henyey Greenstein phase function plus a cheap multiple
// scattering approximation: the same optical depth is reused across a few
// octaves with progressively weaker extinction, which is what stops dense smoke
// from turning into a black silhouette.

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
    float StepCount;

    float3 BoxSize;
    float Exposure;

    float EmissionScale;
    float ShadowSteps;
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

float SampleDensity(float3 position)
{
    float3 uvw = position / BoxSize + 0.5f;

    if (any(uvw < 0.0f) || any(uvw > 1.0f))
    {
        return 0.0f;
    }

    return DensityTexture.SampleLevel(LinearClamp, uvw, 0.0f) * DensityScale;
}

float SampleTemperature(float3 position)
{
    float3 uvw = position / BoxSize + 0.5f;
    return TemperatureTexture.SampleLevel(LinearClamp, uvw, 0.0f);
}

// Integrated density between a point and the outside of the box along a direction.
float OpticalDepthTowards(float3 origin, float3 direction, int steps)
{
    float tEnter;
    float tExit;
    if (!IntersectBox(origin, direction, BoxSize * 0.5f, tEnter, tExit))
    {
        return 0.0f;
    }

    float span = tExit - tEnter;
    float stepSize = span / steps;
    float t = tEnter + stepSize * 0.5f;

    float sum = 0.0f;

    [loop]
    for (int i = 0; i < steps; ++i)
    {
        sum += SampleDensity(origin + direction * t) * stepSize;
        t += stepSize;
    }

    return sum;
}

float HenyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    float denominator = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * Pi * pow(max(denominator, 1e-4f), 1.5f));
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
    float opticalDepth = OpticalDepthTowards(position + float3(0.0f, 0.003f, 0.0f), lightDirection, (int)ShadowSteps);
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
    // Filmic curve by Jim Hejl and Richard Burgess Dawson, gamma is baked in.
    float3 x = max(float3(0.0f, 0.0f, 0.0f), color - 0.004f);
    return (x * (6.2f * x + 0.5f)) / (x * (6.2f * x + 1.7f) + 0.06f);
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

    float tEnter;
    float tExit;
    if (!IntersectBox(CameraPosition, direction, BoxSize * 0.5f, tEnter, tExit))
    {
        return float4(ToneMap(background * Exposure), 1.0f);
    }

    tExit = min(tExit, backgroundDistance);
    if (tExit <= tEnter)
    {
        return float4(ToneMap(background * Exposure), 1.0f);
    }

    int steps = max(1, (int)StepCount);
    int shadowSteps = max(1, (int)ShadowSteps);

    float stepSize = (tExit - tEnter) / steps;
    float jitter = InterleavedGradientNoise(input.position.xy + frac(Time) * 137.0f) * stepSize;
    float t = tEnter + jitter;

    float cosTheta = dot(direction, lightDirection);

    float3 accumulated = float3(0.0f, 0.0f, 0.0f);
    float transmittance = 1.0f;

    [loop]
    for (int i = 0; i < steps; ++i)
    {
        float3 position = CameraPosition + direction * t;
        float density = SampleDensity(position);

        if (density > 0.0015f)
        {
            float extinction = density * Absorption;
            float opticalDepth = OpticalDepthTowards(position, lightDirection, shadowSteps) * Absorption;

            // Multiple scattering octaves. Each one sees a thinner medium and a
            // flatter phase function, which is what gives smoke its soft interior.
            float3 scattering = float3(0.0f, 0.0f, 0.0f);
            float attenuation = 1.0f;
            float contribution = 1.0f;
            float anisotropy = Anisotropy;

            [unroll]
            for (int octave = 0; octave < 3; ++octave)
            {
                float shadow = exp(-opticalDepth * attenuation);
                float phase = HenyeyGreenstein(cosTheta, anisotropy);
                scattering += contribution * shadow * phase;

                attenuation *= 0.45f;
                contribution *= 0.55f;
                anisotropy *= 0.5f;
            }

            float3 inScattered = LightColor * LightIntensity * scattering + AmbientColor;

            float temperature = SampleTemperature(position);
            float3 emitted = EmissionColor(temperature / max(TemperatureReference, 1e-3f)) * EmissionScale;

            float alpha = 1.0f - exp(-extinction * stepSize);
            accumulated += transmittance * alpha * (inScattered + emitted);
            transmittance *= 1.0f - alpha;

            if (transmittance < 0.005f)
            {
                break;
            }
        }

        t += stepSize;
    }

    float3 color = accumulated + transmittance * background;
    return float4(ToneMap(color * Exposure), 1.0f);
}
