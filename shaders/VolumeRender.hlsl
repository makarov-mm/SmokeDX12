// VolumeRender.hlsl
// Single pass volumetric ray marcher for the smoke grid.
// A fullscreen triangle is generated in the vertex shader, the pixel shader
// intersects the view ray with the simulation box and integrates the
// radiative transfer equation front to back with a secondary shadow march.

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
    float Padding0;
};

SamplerState LinearClamp : register(s0);

Texture3D<float> DensityTexture : register(t0);
Texture3D<float> TemperatureTexture : register(t1);

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

float Hash12(float2 p)
{
    return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f);
}

// Slab test against the axis aligned simulation box centred on the origin.
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

// Rough blackbody style ramp driven by the temperature field.
float3 EmissionColor(float temperature)
{
    float t = saturate(temperature);
    float3 deepRed = float3(0.85f, 0.10f, 0.02f);
    float3 orange = float3(1.00f, 0.42f, 0.08f);
    float3 yellow = float3(1.00f, 0.85f, 0.35f);
    float3 white = float3(1.00f, 0.97f, 0.90f);

    float3 color = lerp(deepRed, orange, smoothstep(0.00f, 0.35f, t));
    color = lerp(color, yellow, smoothstep(0.35f, 0.70f, t));
    color = lerp(color, white, smoothstep(0.70f, 1.00f, t));

    // Emission only kicks in once the gas is actually hot.
    float intensity = pow(saturate(temperature - 0.12f), 3.0f);
    return color * intensity;
}

float3 BackgroundColor(float3 direction)
{
    float height = saturate(direction.y * 0.5f + 0.5f);
    float3 bottom = float3(0.020f, 0.024f, 0.033f);
    float3 top = float3(0.055f, 0.070f, 0.105f);
    return lerp(bottom, top, height * height);
}

float3 ToneMap(float3 color)
{
    // Filmic curve by Jim Hejl and Richard Burgess Dawson, already includes gamma.
    float3 x = max(float3(0.0f, 0.0f, 0.0f), color - 0.004f);
    return (x * (6.2f * x + 0.5f)) / (x * (6.2f * x + 1.7f) + 0.06f);
}

float4 PSMain(VertexOutput input) : SV_Target
{
    float3 direction = normalize(
        CameraForward +
        CameraRight * input.ndc.x * TanHalfFov * AspectRatio +
        CameraUp * input.ndc.y * TanHalfFov);

    float3 background = BackgroundColor(direction);

    float3 halfSize = BoxSize * 0.5f;
    float tEnter;
    float tExit;
    if (!IntersectBox(CameraPosition, direction, halfSize, tEnter, tExit))
    {
        return float4(ToneMap(background * Exposure), 1.0f);
    }

    int steps = max(1, (int)StepCount);
    int shadowSteps = max(1, (int)ShadowSteps);

    float stepSize = (tExit - tEnter) / steps;
    float jitter = Hash12(input.position.xy + Time) * stepSize;
    float t = tEnter + jitter;

    float shadowStepSize = length(BoxSize) / shadowSteps;
    float3 lightDirection = normalize(LightDirection);

    float3 accumulated = float3(0.0f, 0.0f, 0.0f);
    float transmittance = 1.0f;

    [loop]
    for (int i = 0; i < steps; ++i)
    {
        float3 position = CameraPosition + direction * t;
        float3 uvw = position / BoxSize + 0.5f;

        float density = DensityTexture.SampleLevel(LinearClamp, uvw, 0.0f) * DensityScale;

        if (density > 0.001f)
        {
            float temperature = TemperatureTexture.SampleLevel(LinearClamp, uvw, 0.0f);

            // Secondary march towards the light for self shadowing.
            float opticalDepth = 0.0f;
            float3 shadowPosition = position;

            [loop]
            for (int j = 0; j < shadowSteps; ++j)
            {
                shadowPosition += lightDirection * shadowStepSize;
                float3 shadowUvw = shadowPosition / BoxSize + 0.5f;

                if (any(shadowUvw < 0.0f) || any(shadowUvw > 1.0f))
                {
                    break;
                }

                opticalDepth += DensityTexture.SampleLevel(LinearClamp, shadowUvw, 0.0f) * DensityScale * shadowStepSize;
            }

            float shadow = exp(-opticalDepth * Absorption);
            float3 scattered = LightColor * LightIntensity * shadow;
            float3 emitted = EmissionColor(temperature) * EmissionScale;

            float alpha = 1.0f - exp(-density * Absorption * stepSize);
            accumulated += transmittance * alpha * (scattered + emitted);
            transmittance *= 1.0f - alpha;

            if (transmittance < 0.008f)
            {
                break;
            }
        }

        t += stepSize;
    }

    float3 color = accumulated + transmittance * background;
    return float4(ToneMap(color * Exposure), 1.0f);
}
