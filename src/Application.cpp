#include "Application.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace
{
    const uint32_t InitialWidth = 1600;
    const uint32_t InitialHeight = 900;

    // Grid resolution of the simulation. 128 is a good default, 192 or 256 look
    // better and are still interactive on a modern discrete card.
    const uint32_t GridResolution = 128;

    // The solver runs at a fixed step, which keeps the simulation reproducible
    // and stops it from blowing up when the frame time spikes.
    const float SimulationStep = 1.0f / 60.0f;

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

    if (!m_solver.Initialize(m_device.GetDevice(), &m_allocator, &m_shaders, GridResolution))
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

    m_camera.SetTarget(DirectX::XMFLOAT3(0.0f, 0.05f, 0.0f));
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
        m_fluidSettings.vorticityStrength = std::max(0.0f, m_fluidSettings.vorticityStrength - 12.0f * deltaTime);
    }
    if (m_window.IsKeyDown('E'))
    {
        m_fluidSettings.vorticityStrength = std::min(60.0f, m_fluidSettings.vorticityStrength + 12.0f * deltaTime);
    }

    if (m_window.IsKeyDown('Z'))
    {
        m_renderSettings.densityScale = std::max(0.05f, m_renderSettings.densityScale - 1.2f * deltaTime);
    }
    if (m_window.IsKeyDown('X'))
    {
        m_renderSettings.densityScale = std::min(12.0f, m_renderSettings.densityScale + 1.2f * deltaTime);
    }

    if (m_window.IsKeyDown('C'))
    {
        m_renderSettings.emissionScale = std::max(0.0f, m_renderSettings.emissionScale - 2.0f * deltaTime);
    }
    if (m_window.IsKeyDown('V'))
    {
        m_renderSettings.emissionScale = std::min(20.0f, m_renderSettings.emissionScale + 2.0f * deltaTime);
    }
}

void Application::Update(float deltaTime)
{
    if (!m_paused)
    {
        m_time += SimulationStep;
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

    m_renderer.Render(
        commandList,
        m_camera,
        m_solver.GetDensity(),
        m_solver.GetTemperature(),
        m_renderSettings,
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

    wchar_t buffer[256];
    swprintf_s(
        buffer,
        L"SmokeDX12   grid %u^3   %.1f fps   vorticity %.1f   density %.2f   emission %.2f%s",
        m_solver.GetResolution(),
        fps,
        m_fluidSettings.vorticityStrength,
        m_renderSettings.densityScale,
        m_renderSettings.emissionScale,
        m_paused ? L"   [paused]" : L"");

    m_window.SetTitle(buffer);
}
