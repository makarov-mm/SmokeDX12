#include "OrbitCamera.h"

#include <algorithm>
#include <cmath>

void OrbitCamera::Orbit(float deltaYaw, float deltaPitch) noexcept
{
    // Stop short of the poles, where the basis built from the world up vector degenerates.
    constexpr float PitchLimit = 1.52f;

    m_yaw += deltaYaw;
    m_pitch = std::clamp(m_pitch + deltaPitch, -PitchLimit, PitchLimit);
}

void OrbitCamera::Zoom(float wheelSteps) noexcept
{
    m_distance = std::clamp(m_distance * std::exp(-0.12f * wheelSteps), 0.6f, 12.0f);
}

CameraBasis OrbitCamera::Basis() const noexcept
{
    const float cosPitch = std::cos(m_pitch);

    CameraBasis basis;
    basis.position = {
        m_target.x + m_distance * cosPitch * std::sin(m_yaw),
        m_target.y + m_distance * std::sin(m_pitch),
        m_target.z + m_distance * cosPitch * std::cos(m_yaw),
    };

    basis.forward = Normalize(m_target - basis.position);
    basis.right = Normalize(Cross({ 0.0f, 1.0f, 0.0f }, basis.forward));
    basis.up = Normalize(Cross(basis.forward, basis.right));
    return basis;
}
