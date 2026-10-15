#pragma once

// Upscaler bridge protocol between the plugins (x86 or x64) and FusionFixHub.exe (x64), which runs NVIDIA DLSS (NGX)
// and AMD FSR (FidelityFX API), both 64-bit D3D12 only, on its own D3D12 device on the game's adapter when started with
// `--upscaler <name>` (no window, no network). The plugin creates a file mapping holding Shared and two auto-reset
// events (<name>.Request, <name>.Response); the hub answers each request exactly once, and the start (with the
// available backends) without one. GPU work is ordered by a D3D12 fence of the hub that the plugin opens on its own
// queue (a Vulkan timeline semaphore with DXVK, or on the D3D12 device of D3D9On12): the plugin signals WaitValue once
// the inputs are copied, the hub signals SignalValue when the output is ready and answers Evaluate only after its GPU
// work is submitted. The vendor runtimes are not shipped with the fixes; the hub loads them only by full path from the
// folder of the game's exe, after checking their vendor's signature. Version changes with every incompatible change.

#include <cstdint>
#include <cstddef>

namespace UpscalerBridge
{
    constexpr uint32_t Version = 3;
    constexpr uint32_t PathLength = 520;

    constexpr const wchar_t* HubFileName = L"FusionFixHub.exe";
    constexpr const wchar_t* ArgumentName = L"--upscaler";

    // The files of each backend's runtime, in the game's folder; a backend is offered when all of them are there
    constexpr const wchar_t* DLSSFiles[] = { L"nvngx_dlss.dll" };
    constexpr const wchar_t* FSRFiles[] = { L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_upscaler_dx12.dll" };

    enum class Command : uint32_t
    {
        None,
        Configure,      // (re)create the shared textures and the upscaler for ConfigureBackend
        Evaluate,       // upscale one frame
        Shutdown,
    };

    enum class Backend : uint32_t
    {
        None,
        DLSS,
        FSR,
    };

    constexpr uint32_t BackendBit(Backend backend)
    {
        return 1u << static_cast<uint32_t>(backend);
    }

    enum class Status : uint32_t
    {
        Pending,
        Ok,
        Failed,
    };

    // Shared textures, the inputs at the render size, Output at the output size:
    // Color     DXGI_FORMAT_R16G16B16A16_FLOAT  scene, HDR unless ConfigureFlags::LowDynamicRange
    // Depth     DXGI_FORMAT_R32_FLOAT           [0, 1] depth, 1 is far unless ConfigureFlags::DepthInverted
    // Motion    DXGI_FORMAT_R16G16_FLOAT        previous - current position in texture coordinates, no jitter
    // Reactive  DXGI_FORMAT_R16_FLOAT           0 to 1, how much a pixel should follow the current frame
    // Output    DXGI_FORMAT_R16G16B16A16_FLOAT  written by the upscaler
    enum class Texture : uint32_t
    {
        Color, Depth, Motion, Reactive, Output, Count
    };

    // The inputs the plugin copies every frame
    constexpr uint32_t InputCount = static_cast<uint32_t>(Texture::Output);

    namespace ConfigureFlags
    {
        constexpr uint32_t ReactiveMask = 1 << 0;       // Reactive is written every frame and used by FSR
        constexpr uint32_t DepthInverted = 1 << 1;      // 1 is near, 0 is far
        constexpr uint32_t LowDynamicRange = 1 << 2;    // Color is in [0, 1]
        constexpr uint32_t SharedWithD3D11 = 1 << 3;    // the plugin opens the textures on a Direct3D 11 device, which can
                                                        // only open render targets with simultaneous access
    }

#pragma pack(push, 8)
    struct Shared
    {
        uint32_t Version;
        uint32_t ClientProcessId;

        // Written by the plugin before starting the hub
        uint32_t AdapterLuidLow;
        int32_t AdapterLuidHigh;
        wchar_t GameDirectory[PathLength];  // the folder of the game's exe, where FusionFixHub.exe and the vendor runtimes are
        wchar_t LogPath[PathLength];        // written by the hub, empty for no log
        char ClientName[64];                // for the log, e.g. the plugin's name

        // Written by the hub, answering the implicit start request
        uint32_t AvailableBackends;         // BackendBit() of each usable backend
        uint32_t Reserved0;
        char Message[512];

        // Request
        Command RequestCommand;
        uint32_t RequestSerial;

        // Configure
        Backend ConfigureBackend;
        uint32_t Width;                     // render size
        uint32_t Height;
        uint32_t OutputWidth;               // output size, at least the render size
        uint32_t OutputHeight;
        uint32_t DLSSPreset;                // NVSDK_NGX_DLSS_Hint_Render_Preset, 0 is the default
        uint32_t Flags;                     // ConfigureFlags
        uint32_t Reserved1;

        // Evaluate
        uint64_t WaitValue;                 // signalled by the plugin when the inputs are ready
        uint64_t SignalValue;               // signalled by the hub when the output is ready
        float JitterX;                      // pixels of the render size: how far the rendered content moved, x right,
                                            // y down (DLSS and FSR take the same value)
        float JitterY;
        float MotionScaleX;                 // converts motion vectors to pixels of the render size
        float MotionScaleY;
        float CameraNear;
        float CameraFar;
        float CameraFovY;                   // radians
        float FrameTimeMs;
        float Sharpness;                    // FSR, 0 is off
        uint32_t Reset;                     // the history does not belong to this frame (camera cut)

        // Response
        uint32_t ResponseSerial;
        Status ResponseStatus;

        // Configure results, handles already duplicated into the plugin's process
        uint64_t TextureHandles[static_cast<size_t>(Texture::Count)];
        uint64_t TextureSizes[static_cast<size_t>(Texture::Count)];
        uint64_t FenceHandle;
    };
#pragma pack(pop)

    static_assert(sizeof(wchar_t) == 2);
    static_assert(offsetof(Shared, WaitValue) % 8 == 0);
    static_assert(offsetof(Shared, TextureHandles) % 8 == 0);
    static_assert(sizeof(Shared) == 2872, "The layout must be identical in the x86 and x64 builds");

    inline const wchar_t* MappingSuffix = L".Mapping";
    inline const wchar_t* RequestSuffix = L".Request";
    inline const wchar_t* ResponseSuffix = L".Response";
}
