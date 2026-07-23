#include <Windows.h>

#include "Application.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previousInstance, PWSTR commandLine, int showCommand)
{
    static_cast<void>(instance);
    static_cast<void>(previousInstance);
    static_cast<void>(commandLine);
    static_cast<void>(showCommand);

    Application application;
    return application.Run();
}
