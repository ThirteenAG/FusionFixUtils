#pragma once
#include <filesystem>
#include <string>

// FusionFixHub.exe --upscaler <name>: DLSS and FSR for the plugins of the fixes, see ../upscaler_bridge_protocol.h.
// Compiled into the hub.
namespace fh::bridge
{
    // Runs until the plugin's process ends or asks to stop; the exit code of the hub
    int Run(const std::wstring& name);

    // Loads what a start loads (D3D12, NGX, FidelityFX from the game folder gameRoot), for the hub's DLL safety test;
    // returns the line for the hub's log
    std::string SelfTest(const std::filesystem::path& gameRoot);

    // A vendor DLL of the game folder opened so that nothing can change, replace or delete it while the handle is open
    // (no link followed), then its Authenticode signature checked through that handle: the file loaded is the file
    // checked. organization: the signing certificate's. Returns the handle (closed once the DLL is no longer used) or
    // nullptr.
    void* OpenSigned(const std::filesystem::path& file, const wchar_t* organization);
}
