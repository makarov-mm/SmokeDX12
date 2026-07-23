#include "OrbitCamera.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

void OrbitCamera::Orbit(float deltaYaw, float deltaPitch)
{
    m_yaw += deltaYaw;
    m_pitch += deltaPitch;

    const float limit = 1.52f;
    m_pitch = std::max(-limit, std::min(limit, m_pitch));
}

void OrbitCamera::Zoom(float amount)
{
    m_distance *= std::exp(-amount * 0.12f);
    m_distance = std::max(0.6f, std::min(12.0f, m_distance));
}

XMFLOAT3 OrbitCamera::GetPosition() const
{
    float cosPitch = std::cos(m_pitch);

    XMFLOAT3 position;
    position.x = m_target.x + m_distance * cosPitch * std::sin(m_yaw);
    position.y = m_target.y + m_distance * std::sin(m_pitch);
    position.z = m_target.z + m_distance * cosPitch * std::cos(m_yaw);
    return position;
}

XMFLOAT3 OrbitCamera::GetForward() const
{
    XMFLOAT3 position = GetPosition();

    XMVECTOR forward = XMVectorSubtract(XMLoadFloat3(&m_target), XMLoadFloat3(&position));
    forward = XMVector3Normalize(forward);

    XMFLOAT3 result;
    XMStoreFloat3(&result, forward);
    return result;
}

XMFLOAT3 OrbitCamera::GetRight() const
{
    XMFLOAT3 forward = GetForward();
    XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, XMLoadFloat3(&forward)));

    XMFLOAT3 result;
    XMStoreFloat3(&result, right);
    return result;
}

XMFLOAT3 OrbitCamera::GetUp() const
{
    XMFLOAT3 forward = GetForward();
    XMFLOAT3 right = GetRight();
    XMVECTOR up = XMVector3Normalize(XMVector3Cross(XMLoadFloat3(&forward), XMLoadFloat3(&right)));

    XMFLOAT3 result;
    XMStoreFloat3(&result, up);
    return result;
}
