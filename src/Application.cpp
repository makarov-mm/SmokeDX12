#include "Application.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace
{
    constexpr uint32_t InitialWidth = 1600;
    constexpr uint32_t InitialHeight = 900;

    // Grid resolution of the simulation. Cells are cubic, so the ratio between
    // the axes is also the world aspect of the box: this domain is twice as
    // tall and twice as long along the wind as it is deep, which gives the
    // plume room to rise and bend before it meets a wall.
    constexpr uint32_t GridResolutionX = 192;
    constexpr uint32_t GridResolutionY = 192;
    constexpr uint32_t GridResolutionZ = 96;

    // The solver runs at a fixed step, which keeps the simulation reproducible
    // and stops it from blowing up when the frame time spikes.
    constexpr float SimulationStep = 1.0f / 60.0f;

    void ReportError(const wchar_t* message)
    {
        MessageBoxW(nullptr, message, L"SmokeDX12", MB_OK | MB_ICONERROR);
    }
}

int Application::Run()
{
    if (!Initialize())
    {
        Shutdown();
        return 1;
    }

    LARGE_INTEGER frequency = {};
    LARGE_INTEGER previous = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&previous);

    while (m_window.ProcessMessages())
    {
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);

        float deltaTime = static_cast<float>(now.QuadPart - previous.QuadPart) / static_cast<float>(frequency.QuadPart);
        previous = now;
        deltaTime = std::min(deltaTime, 0.1f);

        if (m_window.ConsumeResizeFlag())
        {
            m_device.Resize(static_cast<uint32_t>(m_window.GetWidth()), static_cast<uint32_t>(m_window.GetHeight()));
        }

        HandleInput(deltaTime);
        Update(deltaTime);
        Render(deltaTime);
        UpdateTitle(deltaTime);
    }

    Shutdown();
    return 0;
}

bool Application::Initialize()
{
    if (!m_window.Create(L"SmokeDX12", static_cast<int>(InitialWidth), static_cast<int>(InitialHeight)))
    {
        ReportError(L"Failed to create the window.");
        return false;
    }

    bool enableDebugLayer = false;
#if defined(_DEBUG)
    enableDebugLayer = true;
#endif

    if (!m_device.Initialize(m_window.GetHandle(), InitialWidth, InitialHeight, enableDebugLayer))
    {
        ReportError(L"Failed to create a Direct3D 12 device. A GPU with feature level 11_0 is required.");
        return false;
    }

    if (!m_allocator.Initialize(m_device.GetDevice(), 256, 4096, GraphicsDevice::FrameCount))
    {
        ReportError(L"Failed to create the descriptor heaps.");
        return false;
    }

    if (!m_shaders.Initialize())
    {
        ReportError(L"Could not find the shaders directory next to the executable.");
        return false;
    }

    if (!m_solver.Initialize(m_device.GetDevice(), &m_allocator, &m_shaders, GridResolutionX, GridResolutionY, GridResolutionZ))
    {
        const std::string& error = m_shaders.GetLastError();
        std::wstring message(error.begin(), error.end());
        ReportError(message.empty() ? L"Failed to initialise the fluid solver." : message.c_str());
        return false;
    }

    if (!m_renderer.Initialize(m_device.GetDevice(), &m_allocator, &m_shaders, DXGI_FORMAT_R8G8B8A8_UNORM))
    {
        const std::string& error = m_shaders.GetLastError();
        std::wstring message(error.begin(), error.end());
        ReportError(message.empty() ? L"Failed to initialise the volume renderer." : message.c_str());
        return false;
    }

    m_camera.SetTarget(DirectX::XMFLOAT3(0.0f, -0.10f, 0.0f));
    return true;
}

void Application::Shutdown()
{
    m_device.Shutdown();
    m_allocator.Shutdown();
    m_window.Destroy();
}

void Application::HandleInput(float deltaTime)
{
    if (m_window.ConsumeKeyPress(VK_ESCAPE))
    {
        PostQuitMessage(0);
    }

    if (m_window.ConsumeKeyPress(VK_SPACE))
    {
        m_paused = !m_paused;
    }

    if (m_window.ConsumeKeyPress('R'))
    {
        // Hot reload. The GPU has to be idle before pipeline states are replaced.
        m_device.WaitForGpu();
        m_solver.CreatePipelines();
        m_renderer.CreatePipeline();
    }

    if (m_window.ConsumeKeyPress(VK_BACK))
    {
        m_solver.RequestReset();
    }

    if (m_window.ConsumeKeyPress('T'))
    {
        m_turntable = !m_turntable;
    }

    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    m_window.ConsumeMouseDelta(mouseDeltaX, mouseDeltaY);

    if (m_window.IsLeftMouseDown())
    {
        m_camera.Orbit(-mouseDeltaX * 0.006f, mouseDeltaY * 0.006f);
    }

    m_camera.Zoom(m_window.ConsumeWheelDelta());

    if (m_window.IsKeyDown('Q'))
    {
        m_fluidSettings.vorticityStrength = std::clamp(m_fluidSettings.vorticityStrength - 12.0f * deltaTime, 0.0f, 60.0f);
    }
    if (m_window.IsKeyDown('E'))
    {
        m_fluidSettings.vorticityStrength = std::clamp(m_fluidSettings.vorticityStrength + 12.0f * deltaTime, 0.0f, 60.0f);
    }

    if (m_window.IsKeyDown('Z'))
    {
        m_renderSettings.densityScale = std::clamp(m_renderSettings.densityScale - 1.2f * deltaTime, 0.05f, 12.0f);
    }
    if (m_window.IsKeyDown('X'))
    {
        m_renderSettings.densityScale = std::clamp(m_renderSettings.densityScale + 1.2f * deltaTime, 0.05f, 12.0f);
    }

    if (m_window.IsKeyDown('C'))
    {
        m_renderSettings.emissionScale = std::clamp(m_renderSettings.emissionScale - 2.0f * deltaTime, 0.0f, 20.0f);
    }
    if (m_window.IsKeyDown('V'))
    {
        m_renderSettings.emissionScale = std::clamp(m_renderSettings.emissionScale + 2.0f * deltaTime, 0.0f, 20.0f);
    }

    if (m_window.IsKeyDown('F'))
    {
        m_renderSettings.lightIntensity = std::clamp(m_renderSettings.lightIntensity - 12.0f * deltaTime, 0.0f, 80.0f);
    }
    if (m_window.IsKeyDown('G'))
    {
        m_renderSettings.lightIntensity = std::clamp(m_renderSettings.lightIntensity + 12.0f * deltaTime, 0.0f, 80.0f);
    }

    if (m_window.IsKeyDown('1'))
    {
        m_renderSettings.exposure = std::clamp(m_renderSettings.exposure - 0.8f * deltaTime, 0.15f, 6.0f);
    }
    if (m_window.IsKeyDown('2'))
    {
        m_renderSettings.exposure = std::clamp(m_renderSettings.exposure + 0.8f * deltaTime, 0.15f, 6.0f);
    }

    if (m_window.IsKeyDown('N'))
    {
        m_renderSettings.detailStrength = std::clamp(m_renderSettings.detailStrength - 0.5f * deltaTime, 0.0f, 1.5f);
    }
    if (m_window.IsKeyDown('M'))
    {
        m_renderSettings.detailStrength = std::clamp(m_renderSettings.detailStrength + 0.5f * deltaTime, 0.0f, 1.5f);
    }

    if (m_window.IsKeyDown(VK_OEM_COMMA))
    {
        m_fluidSettings.turbulenceStrength = std::clamp(m_fluidSettings.turbulenceStrength - 4.0f * deltaTime, 0.0f, 30.0f);
    }
    if (m_window.IsKeyDown(VK_OEM_PERIOD))
    {
        m_fluidSettings.turbulenceStrength = std::clamp(m_fluidSettings.turbulenceStrength + 4.0f * deltaTime, 0.0f, 30.0f);
    }

    if (m_window.IsKeyDown(VK_LEFT))
    {
        m_fluidSettings.windStrength = std::clamp(m_fluidSettings.windStrength - 10.0f * deltaTime, 0.0f, 60.0f);
    }
    if (m_window.IsKeyDown(VK_RIGHT))
    {
        m_fluidSettings.windStrength = std::clamp(m_fluidSettings.windStrength + 10.0f * deltaTime, 0.0f, 60.0f);
    }

    if (m_window.IsKeyDown(VK_UP) || m_window.IsKeyDown(VK_DOWN))
    {
        // Rotate the wind direction around the vertical axis.
        const float sign = m_window.IsKeyDown(VK_UP) ? 1.0f : -1.0f;
        const float angle = sign * 0.9f * deltaTime;
        const float sine = std::sin(angle);
        const float cosine = std::cos(angle);
        const float x = m_fluidSettings.windDirection[0];
        const float z = m_fluidSettings.windDirection[2];
        m_fluidSettings.windDirection[0] = x * cosine - z * sine;
        m_fluidSettings.windDirection[2] = x * sine + z * cosine;
    }
}

void Application::Update(float deltaTime)
{
    if (!m_paused)
    {
        m_time += SimulationStep;
    }

    if (m_turntable)
    {
        m_camera.Orbit(deltaTime * 0.18f, 0.0f);
    }

    m_smoothedFrameTime = m_smoothedFrameTime * 0.94f + deltaTime * 0.06f;
}

void Application::Render(float deltaTime)
{
    static_cast<void>(deltaTime);

    m_device.BeginFrame();
    m_allocator.BeginFrame(m_device.GetFrameIndex());

    ID3D12GraphicsCommandList* commandList = m_device.GetCommandList();

    ID3D12DescriptorHeap* heaps[] = { m_allocator.GetShaderVisibleHeap() };
    commandList->SetDescriptorHeaps(1, heaps);

    if (!m_paused)
    {
        m_solver.Step(commandList, m_fluidSettings, SimulationStep, m_time);
    }

    m_device.TransitionBackBuffer(D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView = m_device.GetBackBufferView();
    commandList->OMSetRenderTargets(1, &renderTargetView, FALSE, nullptr);

    const float clearColor[4] = { 0.02f, 0.024f, 0.033f, 1.0f };
    commandList->ClearRenderTargetView(renderTargetView, clearColor, 0, nullptr);

    const float boxSize[3] = { m_solver.GetBoxSizeX(), m_solver.GetBoxSizeY(), m_solver.GetBoxSizeZ() };

    m_renderer.Render(
        commandList,
        m_camera,
        m_solver.GetDensity(),
        m_solver.GetTemperature(),
        m_renderSettings,
        boxSize,
        m_device.GetWidth(),
        m_device.GetHeight(),
        m_time);

    m_device.TransitionBackBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    m_device.EndFrame();
}

void Application::UpdateTitle(float deltaTime)
{
    m_titleTimer += deltaTime;
    if (m_titleTimer < 0.25f)
    {
        return;
    }

    m_titleTimer = 0.0f;

    const float fps = m_smoothedFrameTime > 0.0f ? 1.0f / m_smoothedFrameTime : 0.0f;

    const std::wstring title = std::format(
        L"SmokeDX12   grid {}x{}x{}   {:.1f} fps   vorticity {:.1f}   turbulence {:.1f}   detail {:.2f}   density {:.2f}   emission {:.2f}   light {:.1f}   exposure {:.2f}   wind {:.1f}{}",
        m_solver.GetResolutionX(),
        m_solver.GetResolutionY(),
        m_solver.GetResolutionZ(),
        fps,
        m_fluidSettings.vorticityStrength,
        m_fluidSettings.turbulenceStrength,
        m_renderSettings.detailStrength,
        m_renderSettings.densityScale,
        m_renderSettings.emissionScale,
        m_renderSettings.lightIntensity,
        m_renderSettings.exposure,
        m_fluidSettings.windStrength,
        m_paused ? L"   [paused]" : L"");

    m_window.SetTitle(title);
}
