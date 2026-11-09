// Noise.hlsli
// Hashed value noise, shared by the solver and the renderer.

#ifndef NOISE_HLSLI
#define NOISE_HLSLI

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

// Lattice values blended with a smoothstep fade, in [0, 1).
float ValueNoise(float3 p)
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

#endif
