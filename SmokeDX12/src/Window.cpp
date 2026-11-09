#include "Window.h"

#include "Common.h"

#include <algorithm>
#include <utility>

namespace
{
    constexpr wchar_t ClassName[] = L"SmokeDX12WindowClass";
    constexpr DWORD WindowStyle = WS_OVERLAPPEDWINDOW;

    // GET_X_LPARAM and GET_Y_LPARAM. windowsx.h is not included because its
    // macros clash with ordinary names such as IsMinimized.
    POINT CursorPosition(LPARAM lParam) noexcept
    {
        return { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
    }

    // Outer size of a window with the requested client area, scaled down as a
    // whole when it would not fit the work area of the primary monitor.
    SIZE FitWindowSize(int clientWidth, int clientHeight)
    {
        RECT frame = {};
        AdjustWindowRectExForDpi(&frame, WindowStyle, FALSE, 0, GetDpiForSystem());
        const int frameWidth = frame.right - frame.left;
        const int frameHeight = frame.bottom - frame.top;

        float scale = 1.0f;
        RECT workArea = {};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0))
        {
            const float availableWidth = static_cast<float>(workArea.right - workArea.left - frameWidth);
            const float availableHeight = static_cast<float>(workArea.bottom - workArea.top - frameHeight);
            scale = std::clamp(
                std::min(availableWidth / static_cast<float>(clientWidth), availableHeight / static_cast<float>(clientHeight)),
                0.25f,
                1.0f);
        }

        return {
            static_cast<LONG>(static_cast<float>(clientWidth) * scale) + frameWidth,
            static_cast<LONG>(static_cast<float>(clientHeight) * scale) + frameHeight,
        };
    }
}

Window::Window(const wchar_t* title, int clientWidth, int clientHeight)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    const WNDCLASSEXW windowClass = {
        .cbSize = sizeof(WNDCLASSEXW),
        .lpfnWndProc = &Window::WindowProcedure,
        .hInstance = instance,
        .hCursor = LoadCursorW(nullptr, IDC_ARROW),
        .lpszClassName = ClassName,
    };

    if (RegisterClassExW(&windowClass) == 0)
    {
        ThrowLastError("RegisterClassExW");
    }

    const SIZE size = FitWindowSize(clientWidth, clientHeight);

    m_handle = CreateWindowExW(
        0,
        ClassName,
        title,
        WindowStyle,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        size.cx,
        size.cy,
        nullptr,
        nullptr,
        instance,
        this);

    if (m_handle == nullptr)
    {
        const DWORD error = GetLastError();
        UnregisterClassW(ClassName, instance);
        SetLastError(error);
        ThrowLastError("CreateWindowExW");
    }

    RECT client = {};
    GetClientRect(m_handle, &client);
    m_clientWidth = std::max<int>(client.right - client.left, 1);
    m_clientHeight = std::max<int>(client.bottom - client.top, 1);
    m_resized = false;
}

Window::~Window()
{
    DestroyWindow(m_handle);
    UnregisterClassW(ClassName, GetModuleHandleW(nullptr));
}

void Window::Show() const
{
    ShowWindow(m_handle, SW_SHOWDEFAULT);
}

bool Window::ProcessMessages()
{
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            m_closing = true;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return !m_closing;
}

void Window::WaitForMessages() const
{
    WaitMessage();
}

bool Window::IsMinimized() const
{
    return IsIconic(m_handle) != FALSE;
}

bool Window::ConsumeResize() noexcept
{
    return std::exchange(m_resized, false);
}

void Window::SetTitle(const std::wstring& title) const
{
    SetWindowTextW(m_handle, title.c_str());
}

bool Window::IsKeyDown(int virtualKey) const noexcept
{
    return virtualKey >= 0 && virtualKey < static_cast<int>(m_keyDown.size()) && m_keyDown.test(static_cast<size_t>(virtualKey));
}

bool Window::ConsumeKeyPress(int virtualKey) noexcept
{
    if (virtualKey < 0 || virtualKey >= static_cast<int>(m_keyPressed.size()))
    {
        return false;
    }

    const bool pressed = m_keyPressed.test(static_cast<size_t>(virtualKey));
    m_keyPressed.reset(static_cast<size_t>(virtualKey));
    return pressed;
}

MouseDelta Window::ConsumeMouseDelta() noexcept
{
    return std::exchange(m_mouseDelta, {});
}

float Window::ConsumeWheelDelta() noexcept
{
    return std::exchange(m_wheelDelta, 0.0f);
}

void Window::ReleaseInput() noexcept
{
    m_keyDown.reset();
    m_keyPressed.reset();
    m_dragging = false;
}

LRESULT CALLBACK Window::WindowProcedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCDESTROY)
    {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }

    return window != nullptr
        ? window->HandleMessage(hwnd, message, wParam, lParam)
        : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Window::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CLOSE:
        // The window is destroyed by its owner once the main loop has ended.
        m_closing = true;
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            const int width = LOWORD(lParam);
            const int height = HIWORD(lParam);
            if (width > 0 && height > 0 && (width != m_clientWidth || height != m_clientHeight))
            {
                m_clientWidth = width;
                m_clientHeight = height;
                m_resized = true;
            }
        }
        return 0;

    case WM_DPICHANGED:
    {
        // Keep the logical size of the window on a monitor with a different scale.
        const RECT& suggested = *reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(
            hwnd,
            nullptr,
            suggested.left,
            suggested.top,
            suggested.right - suggested.left,
            suggested.bottom - suggested.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_KILLFOCUS:
        // Whatever is released from now on is reported to another window, so
        // keys held at this point would otherwise stay down forever.
        ReleaseInput();
        break;

    case WM_KEYDOWN:
        if (wParam < m_keyDown.size())
        {
            // Bit 30 marks auto repeat, which is not a new press.
            if ((lParam & (1 << 30)) == 0)
            {
                m_keyPressed.set(wParam);
            }
            m_keyDown.set(wParam);
        }
        return 0;

    case WM_KEYUP:
        if (wParam < m_keyDown.size())
        {
            m_keyDown.reset(wParam);
        }
        return 0;

    case WM_SYSKEYUP:
        // A key released while Alt is held arrives here instead of WM_KEYUP.
        if (wParam < m_keyDown.size())
        {
            m_keyDown.reset(wParam);
        }
        break;

    case WM_LBUTTONDOWN:
        m_dragging = true;
        m_lastMousePosition = CursorPosition(lParam);
        SetCapture(hwnd);
        return 0;

    case WM_LBUTTONUP:
        m_dragging = false;
        ReleaseCapture();
        return 0;

    case WM_CAPTURECHANGED:
        // Capture can also be taken away in the middle of a drag, without a button up.
        m_dragging = false;
        return 0;

    case WM_MOUSEMOVE:
    {
        const POINT position = CursorPosition(lParam);
        if (m_dragging)
        {
            m_mouseDelta.x += static_cast<float>(position.x - m_lastMousePosition.x);
            m_mouseDelta.y += static_cast<float>(position.y - m_lastMousePosition.y);
        }
        m_lastMousePosition = position;
        return 0;
    }

    case WM_MOUSEWHEEL:
        m_wheelDelta += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}
