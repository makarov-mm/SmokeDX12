#include "Window.h"

#include <windowsx.h>

namespace
{
    const wchar_t* WindowClassName = L"SmokeDX12WindowClass";
}

bool Window::Create(const std::wstring& title, int width, int height)
{
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(WNDCLASSEXW);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &Window::WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = WindowClassName;

    if (RegisterClassExW(&windowClass) == 0)
    {
        return false;
    }

    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    m_handle = CreateWindowExW(
        0,
        WindowClassName,
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        this);

    if (m_handle == nullptr)
    {
        return false;
    }

    m_width = width;
    m_height = height;

    ShowWindow(m_handle, SW_SHOW);
    UpdateWindow(m_handle);
    return true;
}

void Window::Destroy()
{
    if (m_handle != nullptr)
    {
        DestroyWindow(m_handle);
        m_handle = nullptr;
    }

    UnregisterClassW(WindowClassName, GetModuleHandleW(nullptr));
}

bool Window::ProcessMessages()
{
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);

        if (message.message == WM_QUIT)
        {
            m_closing = true;
        }
    }

    return !m_closing;
}

bool Window::ConsumeResizeFlag()
{
    bool resized = m_resized;
    m_resized = false;
    return resized;
}

void Window::SetTitle(const std::wstring& title)
{
    if (m_handle != nullptr)
    {
        SetWindowTextW(m_handle, title.c_str());
    }
}

bool Window::ConsumeKeyPress(int virtualKey)
{
    if (virtualKey < 0 || virtualKey > 255)
    {
        return false;
    }

    bool pressed = m_keyPressed[virtualKey];
    m_keyPressed[virtualKey] = false;
    return pressed;
}

bool Window::IsKeyDown(int virtualKey) const
{
    if (virtualKey < 0 || virtualKey > 255)
    {
        return false;
    }

    return m_keyDown[virtualKey];
}

void Window::ConsumeMouseDelta(float& deltaX, float& deltaY)
{
    deltaX = m_mouseDeltaX;
    deltaY = m_mouseDeltaY;
    m_mouseDeltaX = 0.0f;
    m_mouseDeltaY = 0.0f;
}

float Window::ConsumeWheelDelta()
{
    float delta = m_wheelDelta;
    m_wheelDelta = 0.0f;
    return delta;
}

LRESULT CALLBACK Window::WindowProcedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    Window* window = nullptr;

    if (message == WM_NCCREATE)
    {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<Window*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    }
    else
    {
        window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window != nullptr)
    {
        return window->HandleMessage(hwnd, message, wParam, lParam);
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Window::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CLOSE:
        m_closing = true;
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_SIZE:
    {
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);
        if (width > 0 && height > 0 && (width != m_width || height != m_height))
        {
            m_width = width;
            m_height = height;
            m_resized = true;
        }
        return 0;
    }

    case WM_KEYDOWN:
    {
        if (wParam < 256)
        {
            if (!m_keyDown[wParam])
            {
                m_keyPressed[wParam] = true;
            }
            m_keyDown[wParam] = true;
        }
        return 0;
    }

    case WM_KEYUP:
    {
        if (wParam < 256)
        {
            m_keyDown[wParam] = false;
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        m_leftMouseDown = true;
        m_lastMousePosition.x = GET_X_LPARAM(lParam);
        m_lastMousePosition.y = GET_Y_LPARAM(lParam);
        SetCapture(hwnd);
        return 0;
    }

    case WM_LBUTTONUP:
    {
        m_leftMouseDown = false;
        ReleaseCapture();
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        if (m_leftMouseDown)
        {
            m_mouseDeltaX += static_cast<float>(x - m_lastMousePosition.x);
            m_mouseDeltaY += static_cast<float>(y - m_lastMousePosition.y);
        }

        m_lastMousePosition.x = x;
        m_lastMousePosition.y = y;
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        m_wheelDelta += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}
