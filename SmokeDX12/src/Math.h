#pragma once

#include <cmath>
#include <cstdint>

// The few vector types the project needs. Their layout matches HLSL float3 and
// uint3, so they can sit directly in root constant structs.
struct Float3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    friend constexpr Float3 operator+(Float3 a, Float3 b) noexcept { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    friend constexpr Float3 operator-(Float3 a, Float3 b) noexcept { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    friend constexpr Float3 operator*(Float3 v, float s) noexcept { return { v.x * s, v.y * s, v.z * s }; }
    friend constexpr Float3 operator*(float s, Float3 v) noexcept { return v * s; }
};

struct UInt3
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
};

static_assert(sizeof(Float3) == 12 && sizeof(UInt3) == 12);

constexpr float Dot(Float3 a, Float3 b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

constexpr Float3 Cross(Float3 a, Float3 b) noexcept
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

inline float Length(Float3 v) noexcept
{
    return std::sqrt(Dot(v, v));
}

inline Float3 Normalize(Float3 v) noexcept
{
    const float length = Length(v);
    return { v.x / length, v.y / length, v.z / length };
}
