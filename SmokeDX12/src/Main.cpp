#include "Application.h"
#include "Common.h"

#include <exception>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // Sharp output on scaled displays, with the window and the swap chain sized
    // in real pixels.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    try
    {
        Application application;
        return application.Run();
    }
    catch (const std::exception& error)
    {
        ShowErrorMessage(nullptr, error.what());
        return 1;
    }
}
