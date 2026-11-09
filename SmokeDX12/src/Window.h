#pragma once

#include <Windows.h>

#include <bitset>
#include <string>

struct MouseDelta
{
    float x = 0.0f;
    float y = 0.0f;
};

// Win32 window that also keeps track of keyboard and mouse state.
//
// The window is created hidden, so that nothing unresponsive is on screen while
// the application compiles its shaders, and is shown with Show() once it is
// ready to render.
class Window
{
public:
    // The client size is in physical pixels and is shrunk to fit the work area
    // of the primary monitor.
    Window(const wchar_t* title, int clientWidth, int clientHeight);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void Show() const;
    void Close() noexcept { m_closing = true; }

    // Dispatches pending messages. Returns false once the window is closing.
    bool ProcessMessages();
    // Sleeps until the next message arrives.
    void WaitForMessages() const;

    HWND Handle() const noexcept { return m_handle; }
    int ClientWidth() const noexcept { return m_clientWidth; }
    int ClientHeight() const noexcept { return m_clientHeight; }
    bool IsMinimized() const;
    bool ConsumeResize() noexcept;

    void SetTitle(const std::wstring& title) const;

    bool IsKeyDown(int virtualKey) const noexcept;
    bool ConsumeKeyPress(int virtualKey) noexcept;
    MouseDelta ConsumeMouseDelta() noexcept;
    float ConsumeWheelDelta() noexcept;

private:
    static LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void ReleaseInput() noexcept;

    HWND m_handle = nullptr;
    int m_clientWidth = 0;
    int m_clientHeight = 0;
    bool m_closing = false;
    bool m_resized = false;

    std::bitset<256> m_keyDown;
    std::bitset<256> m_keyPressed;

    bool m_dragging = false;
    POINT m_lastMousePosition = {};
    MouseDelta m_mouseDelta;
    float m_wheelDelta = 0.0f;
};
