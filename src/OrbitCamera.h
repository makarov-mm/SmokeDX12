#pragma once

#include <DirectXMath.h>

// Simple orbit camera looking at a fixed target.
class OrbitCamera
{
public:
    void Orbit(float deltaYaw, float deltaPitch);
    void Zoom(float amount);

    DirectX::XMFLOAT3 GetPosition() const;
    DirectX::XMFLOAT3 GetForward() const;
    DirectX::XMFLOAT3 GetRight() const;
    DirectX::XMFLOAT3 GetUp() const;

    float GetFieldOfView() const { return m_fieldOfView; }
    void SetTarget(const DirectX::XMFLOAT3& target) { m_target = target; }

private:
    DirectX::XMFLOAT3 m_target = { 0.0f, 0.0f, 0.0f };
    float m_yaw = 0.55f;
    float m_pitch = 0.18f;
    float m_distance = 4.2f;
    float m_fieldOfView = 0.9f;
};
