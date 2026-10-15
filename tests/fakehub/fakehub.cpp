// A stand-in for FusionFixHub.exe --upscaler <name> without a GPU, for tests/upscaler.cpp: the real connection of the
// hub's side (upscaler/bridge/upscaler_bridge_server.h) with the D3D12, DLSS and FSR work replaced by checks and echoes.
// Built as FusionFixHub.exe in a folder of its own (the plugin starts <game folder>\FusionFixHub.exe).
//
//   start      answers Ok with both backends available and "fake hub <version>" as the message
//   Configure  checks the sizes as the real bridge does, then echoes them: TextureSizes[i] = width * height * (i + 1)
//              for the inputs, OutputWidth * OutputHeight for Output; no handles
//   Evaluate   Ok when SignalValue > WaitValue; Sharpness < 0 delays the answer by -Sharpness milliseconds;
//              the message echoes the frame's values
//   Shutdown   answered, exit code 0; exit code 1 when the plugin's process ended first
//   exit codes 10 + OpenResult when the connection fails (11 missing objects, 12 version mismatch, 13 no client)

#include "../../upscaler/bridge/upscaler_bridge_server.h"
#include <cmath>
#include <cwchar>

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc < 3 || wcscmp(argv[1], UpscalerBridge::ArgumentName) != 0)
        return 2;

    using namespace UpscalerBridge;
    Server::Connection connection;
    auto result = connection.Connect(argv[2]);
    if (result != Server::OpenResult::Ok)
        return 10 + static_cast<int>(result);

    auto& shared = *connection.shared;
    shared.AvailableBackends = BackendBit(Backend::DLSS) | BackendBit(Backend::FSR);
    connection.Message("fake hub %u, %d-bit, client %s", Version, static_cast<int>(sizeof(void*) * 8), shared.ClientName);
    connection.Respond(Status::Ok);

    bool shutdown = connection.Serve([&](Command command)
    {
        switch (command)
        {
        case Command::Configure:
        {
            uint32_t width = shared.Width, height = shared.Height;
            uint32_t outputWidth = shared.OutputWidth ? shared.OutputWidth : width;
            uint32_t outputHeight = shared.OutputHeight ? shared.OutputHeight : height;
            if (width == 0 || height == 0 || outputWidth > 16384 || outputHeight > 16384 || outputWidth < width || outputHeight < height)
                return false;
            if (shared.ConfigureBackend != Backend::DLSS && shared.ConfigureBackend != Backend::FSR)
                return false;
            for (uint32_t i = 0; i < InputCount; ++i)
            {
                shared.TextureSizes[i] = uint64_t(width) * height * (i + 1);
                shared.TextureHandles[i] = 0;
            }
            shared.TextureSizes[static_cast<size_t>(Texture::Output)] = uint64_t(outputWidth) * outputHeight;
            shared.TextureHandles[static_cast<size_t>(Texture::Output)] = 0;
            shared.FenceHandle = 0;
            connection.Message("configured %u %ux%u -> %ux%u flags %u preset %u", static_cast<uint32_t>(shared.ConfigureBackend), width, height,
                outputWidth, outputHeight, shared.Flags, shared.DLSSPreset);
            return true;
        }
        case Command::Evaluate:
            if (shared.Sharpness < 0.0f)
                Sleep(static_cast<DWORD>(-shared.Sharpness));
            connection.Message("evaluate %llu %llu %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f reset %u", static_cast<unsigned long long>(shared.WaitValue),
                static_cast<unsigned long long>(shared.SignalValue), shared.JitterX, shared.JitterY, shared.MotionScaleX, shared.MotionScaleY,
                shared.CameraNear, shared.CameraFar, shared.CameraFovY, shared.FrameTimeMs, shared.Reset);
            return shared.SignalValue > shared.WaitValue;
        default:
            return false;
        }
    });
    return shutdown ? 0 : 1;
}
