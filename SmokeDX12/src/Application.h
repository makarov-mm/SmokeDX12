#pragma once

#include "DescriptorAllocator.h"
#include "FluidSolver.h"
#include "GraphicsDevice.h"
#include "OrbitCamera.h"
#include "ShaderLibrary.h"
#include "SimulationClock.h"
#include "VolumeRenderer.h"
#include "Window.h"

#include <string>

class Application
{
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int Run();

private:
    void HandleInput(float frameTime);
    void ReloadShaders();
    void RenderFrame(bool step);
    void UpdateTitle(float frameTime);
    int KeyAxis(int negativeKey, int positiveKey) const noexcept;

    // Members are constructed in this order and each one only uses those above it.
    Window m_window;
    GraphicsDevice m_device;
    DescriptorAllocator m_descriptors;
    ShaderLibrary m_shaders;
    FluidSolver m_solver;
    VolumeRenderer m_renderer;

    OrbitCamera m_camera;
    SimulationClock m_clock;
    FluidSettings m_fluidSettings;
    RenderSettings m_renderSettings;

    bool m_paused = false;
    bool m_turntable = false;
    // Set when the image on screen is out of date, which matters while paused.
    bool m_redraw = true;

    float m_averageFrameTime;
    float m_fpsRefreshTimer = 0.0f;
    float m_shownFps = 0.0f;
    std::wstring m_title;
};
