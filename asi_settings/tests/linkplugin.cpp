// A plugin for the live link tests (tests.cpp, LiveLink): LinkPluginA.dll and LinkPluginB.dll are built from this file,
// each with its own description. The test exe plays the game: it loads both, calls LinkTest_Init, and changes values
// the way the game's own menu would (LinkTest_Set) to see the link report them. LinkPluginC.dll (tests.cpp, HandEdits)
// starts everything from DllMain.
#include "../asi_settings.hpp"
#include <atomic>

asi_settings::Plugin settings;
ASI_SETTINGS_EXPORT(settings)

namespace
{
    std::atomic<bool> modeTwoUsable = false;
}

// Loads the settings and starts the link; true when this plugin serves it
extern "C" __declspec(dllexport) bool __cdecl LinkTest_Init()
{
    if (!settings.Load())
        return false;
#ifdef LINK_PLUGIN_B
    // Mode 2 needs something that isn't there yet (as DLAA needs its DLLs)
    settings.Availability("MAIN.Mode", [](std::string_view value) { return value != "2" || modeTwoUsable; },
        [] { return std::string("Mode 2 needs the test's runtime"); });
#endif
    return asi_settings::LiveLink::Start();
}

extern "C" __declspec(dllexport) bool __cdecl LinkTest_Start() { return asi_settings::LiveLink::Start(); }
extern "C" __declspec(dllexport) void __cdecl LinkTest_Stop() { asi_settings::LiveLink::Stop(); }
extern "C" __declspec(dllexport) bool __cdecl LinkTest_Serving() { return asi_settings::LiveLink::Serving(); }

// The game's own menu changes a value
extern "C" __declspec(dllexport) int32_t __cdecl LinkTest_Set(const char* id, const char* value) { return settings.Set(id, value); }

// What the setting depends on appears or goes away
extern "C" __declspec(dllexport) void __cdecl LinkTest_Usable(bool usable)
{
    modeTwoUsable = usable;
    settings.Refresh("MAIN.Mode");
}

#ifdef LINK_PLUGIN_C
namespace
{
    std::atomic<int> levelApplied = 0;
}

// Loads its settings, watches its file and starts the link under the loader lock, as plugins started from DllMain by
// some ASI loaders do: none of it may wait for a thread
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH && settings.Load(module))
    {
        settings.OnChange("MAIN.Level", [] { ++levelApplied; });
        settings.WatchFile();
        asi_settings::LiveLink::Start();
    }
    return TRUE;
}

extern "C" __declspec(dllexport) int __cdecl WatchTest_Applied() { return levelApplied; }
extern "C" __declspec(dllexport) int __cdecl WatchTest_Level() { return settings.Int("MAIN.Level"); }
extern "C" __declspec(dllexport) uint32_t __cdecl WatchTest_Reloads() { return settings.FileReloads(); }
extern "C" __declspec(dllexport) bool __cdecl WatchTest_Watching() { return settings.Watching(); }
#endif
