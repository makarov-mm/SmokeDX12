#include "Application.h"

#include "Common.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <utility>

namespace
{
    constexpr int InitialWidth = 1600;
    constexpr int InitialHeight = 900;

    // Grid resolution of the simulation. Cells are cubic, so the ratio between
    // the axes is also the world aspect of the box: this domain is twice as
    // tall and twice as long along the wind as it is deep, which gives the
    // plume room to rise and bend before it meets a wall.
    constexpr UInt3 GridResolution = { 192, 192, 96 };

    // Length of one simulation step, see SimulationClock.
    constexpr float SimulationStep = 1.0f / 60.0f;

    // Longer frames, after a stall or a window drag, are clipped so that held
    // keys and the turntable do not jump.
    constexpr float MaxFrameTime = 0.25f;

    constexpr uint32_t StagingDescriptors = 64;

    // A dispatch of the solver uses 16 descriptors, which leaves room for
    // several hundred pressure iterations per frame.
    constexpr uint32_t ShaderVisibleDescriptorsPerFrame = 16384;
}

Application::Application()
    : m_window(L"SmokeDX12", InitialWidth, InitialHeight)
    , m_device(m_window.Handle(), static_cast<uint32_t>(m_window.ClientWidth()), static_cast<uint32_t>(m_window.ClientHeight()))
    , m_descriptors(m_device.Device(), StagingDescriptors, ShaderVisibleDescriptorsPerFrame, GraphicsDevice::FrameCount)
    , m_solver(m_device.Device(), m_descriptors, m_shaders, GridResolution)
    , m_renderer(m_device.Device(), m_descriptors, m_shaders, GraphicsDevice::BackBufferFormat)
    , m_clock(SimulationStep)
    , m_averageFrameTime(SimulationStep)
{
    m_camera.SetTarget({ 0.0f, -0.10f, 0.0f });
}

Application::~Application()
{
    // The members release their GPU objects as they are destroyed, which must
    // not happen while the GPU is still using them.
    m_device.WaitForIdle();
}

int Application::Run()
{
    using Clock = std::chrono::steady_clock;

    m_window.Show();
    Clock::time_point previous = Clock::now();

    while (m_window.ProcessMessages())
    {
        if (m_window.IsMinimized())
        {
            // Nothing is visible, so there is nothing to render until a message
            // brings the window back.
            m_window.WaitForMessages();
            previous = Clock::now();
            m_redraw = true;
            continue;
        }

        if (m_window.ConsumeResize())
        {
            m_device.Resize(static_cast<uint32_t>(m_window.ClientWidth()), static_cast<uint32_t>(m_window.ClientHeight()));
            m_redraw = true;
        }

        const Clock::time_point now = Clock::now();
        const float frameTime = std::min(std::chrono::duration<float>(now - previous).count(), MaxFrameTime);
        previous = now;

        HandleInput(frameTime);

        if (m_paused && !m_turntable && !m_redraw)
        {
            // A paused simulation seen from a still camera keeps the last frame,
            // so there is no point in rendering the same image at full GPU load.
            m_window.WaitForMessages();
            previous = Clock::now();
            continue;
        }

        if (m_turntable)
        {
            m_camera.Orbit(0.18f * frameTime, 0.0f);
        }

        RenderFrame(!m_paused && m_clock.Advance(frameTime));
        UpdateTitle(frameTime);
        m_redraw = false;
    }

    return 0;
}

int Application::KeyAxis(int negativeKey, int positiveKey) const noexcept
{
    return static_cast<int>(m_window.IsKeyDown(positiveKey)) - static_cast<int>(m_window.IsKeyDown(negativeKey));
}

void Application::HandleInput(float frameTime)
{
    if (m_window.ConsumeKeyPress(VK_ESCAPE))
    {
        m_window.Close();
    }

    if (m_window.ConsumeKeyPress(VK_SPACE))
    {
        m_paused = !m_paused;
        m_redraw = true;
    }

    if (m_window.ConsumeKeyPress('T'))
    {
        m_turntable = !m_turntable;
        m_redraw = true;
    }

    if (m_window.ConsumeKeyPress('R'))
    {
        ReloadShaders();
        m_redraw = true;
    }

    if (m_window.ConsumeKeyPress(VK_BACK))
    {
        m_solver.RequestReset();
        m_redraw = true;
    }

    if (const MouseDelta drag = m_window.ConsumeMouseDelta(); drag.x != 0.0f || drag.y != 0.0f)
    {
        m_camera.Orbit(-0.006f * drag.x, 0.006f * drag.y);
        m_redraw = true;
    }

    if (const float wheel = m_window.ConsumeWheelDelta(); wheel != 0.0f)
    {
        m_camera.Zoom(wheel);
        m_redraw = true;
    }

    // Held keys move a setting at a fixed rate per second, within its range.
    struct Tweak
    {
        int decreaseKey;
        int increaseKey;
        float* value;
        float ratePerSecond;
        float minimum;
        float maximum;
    };

    const Tweak tweaks[] = {
        { 'Q', 'E', &m_fluidSettings.vorticityStrength, 12.0f, 0.0f, 60.0f },
        { VK_OEM_COMMA, VK_OEM_PERIOD, &m_fluidSettings.turbulenceStrength, 4.0f, 0.0f, 30.0f },
        { VK_LEFT, VK_RIGHT, &m_fluidSettings.windStrength, 10.0f, 0.0f, 60.0f },
        { 'N', 'M', &m_renderSettings.detailStrength, 0.5f, 0.0f, 1.5f },
        { 'Z', 'X', &m_renderSettings.densityScale, 1.2f, 0.05f, 12.0f },
        { 'C', 'V', &m_renderSettings.emissionScale, 2.0f, 0.0f, 20.0f },
        { 'F', 'G', &m_renderSettings.lightIntensity, 12.0f, 0.0f, 80.0f },
        { '1', '2', &m_renderSettings.exposure, 0.8f, 0.15f, 6.0f },
    };

    for (const Tweak& tweak : tweaks)
    {
        if (const int direction = KeyAxis(tweak.decreaseKey, tweak.increaseKey); direction != 0)
        {
            const float change = static_cast<float>(direction) * tweak.ratePerSecond * frameTime;
            *tweak.value = std::clamp(*tweak.value + change, tweak.minimum, tweak.maximum);
            m_redraw = true;
        }
    }

    // Up and Down turn the wind around the vertical axis.
    if (const int direction = KeyAxis(VK_DOWN, VK_UP); direction != 0)
    {
        const float angle = static_cast<float>(direction) * 0.9f * frameTime;
        const float sine = std::sin(angle);
        const float cosine = std::cos(angle);

        Float3& wind = m_fluidSettings.windDirection;
        wind = { wind.x * cosine - wind.z * sine, wind.y, wind.x * sine + wind.z * cosine };
        m_redraw = true;
    }
}

void Application::ReloadShaders()
{
    // Pipeline states may only be replaced once the GPU is done with them.
    m_device.WaitForIdle();

    std::string errors;

    try
    {
        m_solver.ReloadShaders();
    }
    catch (const ShaderCompileError& error)
    {
        errors += error.what();
    }

    try
    {
        m_renderer.ReloadShaders();
    }
    catch (const ShaderCompileError& error)
    {
        errors += error.what();
    }

    if (!errors.empty())
    {
        ShowErrorMessage(m_window.Handle(), "The shaders did not compile, the previous ones are still running.\n\n" + errors);
    }
}

void Application::RenderFrame(bool step)
{
    ID3D12GraphicsCommandList* commandList = m_device.BeginFrame();
    m_descriptors.BeginFrame(m_device.FrameIndex());

    ID3D12DescriptorHeap* const heaps[] = { m_descriptors.ShaderVisibleHeap() };
    commandList->SetDescriptorHeaps(1, heaps);

    m_solver.ResetIfRequested(commandList);
    if (step)
    {
        m_solver.Step(commandList, m_fluidSettings, m_clock.Step(), m_clock.Time());
    }

    m_renderer.Render(
        commandList,
        {
            .camera = m_camera,
            .density = m_solver.Density(),
            .temperature = m_solver.Temperature(),
            .boxSize = m_solver.BoxSize(),
            .width = m_device.Width(),
            .height = m_device.Height(),
            .time = m_clock.RenderTime(),
        },
        m_renderSettings);

    m_device.EndFrame();
}

void Application::UpdateTitle(float frameTime)
{
    // The frame rate is averaged and refreshed four times a second so that it
    // stays readable. Everything else shows up as soon as it changes.
    m_averageFrameTime += (frameTime - m_averageFrameTime) * 0.06f;
    m_fpsRefreshTimer -= frameTime;
    if (m_fpsRefreshTimer <= 0.0f)
    {
        m_fpsRefreshTimer = 0.25f;
        m_shownFps = 1.0f / m_averageFrameTime;
    }

    const UInt3 grid = m_solver.Resolution();

    std::wstring title = std::format(
        L"SmokeDX12   grid {}x{}x{}   {:.1f} fps   vorticity {:.1f}   turbulence {:.1f}   detail {:.2f}   "
        L"density {:.2f}   emission {:.2f}   light {:.1f}   exposure {:.2f}   wind {:.1f}{}",
        grid.x,
        grid.y,
        grid.z,
        m_shownFps,
        m_fluidSettings.vorticityStrength,
        m_fluidSettings.turbulenceStrength,
        m_renderSettings.detailStrength,
        m_renderSettings.densityScale,
        m_renderSettings.emissionScale,
        m_renderSettings.lightIntensity,
        m_renderSettings.exposure,
        m_fluidSettings.windStrength,
        m_paused ? L"   [paused]" : L"");

    if (title != m_title)
    {
        m_title = std::move(title);
        m_window.SetTitle(m_title);
    }
}
