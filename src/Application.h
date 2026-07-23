#pragma once

#include <cstdint>

#include "DescriptorAllocator.h"
#include "FluidSolver.h"
#include "GraphicsDevice.h"
#include "OrbitCamera.h"
#include "ShaderLibrary.h"
#include "VolumeRenderer.h"
#include "Window.h"

class Application
{
public:
    int Run();

private:
    bool Initialize();
    void Shutdown();
    void Update(float deltaTime);
    void Render(float deltaTime);
    void HandleInput(float deltaTime);
    void UpdateTitle(float deltaTime);

    Window m_window;
    GraphicsDevice m_device;
    DescriptorAllocator m_allocator;
    ShaderLibrary m_shaders;
    FluidSolver m_solver;
    VolumeRenderer m_renderer;
    OrbitCamera m_camera;

    FluidSettings m_fluidSettings;
    RenderSettings m_renderSettings;

    bool m_paused = false;
    float m_time = 0.0f;
    float m_titleTimer = 0.0f;
    float m_smoothedFrameTime = 0.016f;
};
