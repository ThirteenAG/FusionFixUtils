// CallbackProbe.dll: a module nothing loads until a test does (tests/callbacks.cpp, tests/modulelist.cpp)
#include <windows.h>

extern "C" __declspec(dllexport) int CallbackProbeValue()
{
    return 42;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
