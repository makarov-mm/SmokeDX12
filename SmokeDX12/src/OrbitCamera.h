#pragma once

#include "Math.h"

struct CameraBasis
{
    Float3 position;
    Float3 forward;
    Float3 right;
    Float3 up;
};

// Orbit camera looking at a fixed target.
class OrbitCamera
{
public:
    void Orbit(float deltaYaw, float deltaPitch) noexcept;
    void Zoom(float wheelSteps) noexcept;
    void SetTarget(Float3 target) noexcept { m_target = target; }

    CameraBasis Basis() const noexcept;
    float FieldOfView() const noexcept { return m_fieldOfView; }

private:
    Float3 m_target;
    float m_yaw = 0.55f;
    float m_pitch = 0.18f;
    float m_distance = 4.2f;
    float m_fieldOfView = 0.9f;
};
