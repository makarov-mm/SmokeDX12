#pragma once

#include <Windows.h>
#include <string>

// Thin Win32 window wrapper that also keeps track of keyboard and mouse state.
class Window
{
public:
    bool Create(const std::wstring& title, int width, int height);
    void Destroy();

    bool ProcessMessages();

    HWND GetHandle() const { return m_handle; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

    bool IsClosing() const { return m_closing; }
    bool ConsumeResizeFlag();

    void SetTitle(const std::wstring& title);

    bool ConsumeKeyPress(int virtualKey);
    bool IsKeyDown(int virtualKey) const;
    bool IsLeftMouseDown() const { return m_leftMouseDown; }
    void ConsumeMouseDelta(float& deltaX, float& deltaY);
    float ConsumeWheelDelta();

private:
    static LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND m_handle = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_closing = false;
    bool m_resized = false;

    bool m_keyDown[256] = {};
    bool m_keyPressed[256] = {};

    bool m_leftMouseDown = false;
    POINT m_lastMousePosition = {};
    float m_mouseDeltaX = 0.0f;
    float m_mouseDeltaY = 0.0f;
    float m_wheelDelta = 0.0f;
};
