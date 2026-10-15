// Upscaler bridge: FusionFixHub.exe --upscaler <name>. Started by a plugin (see upscaler_bridge_protocol.h), the hub
// runs NVIDIA DLSS (NGX) and AMD FSR (FidelityFX API), 64-bit only, on its own D3D12 device on the game's adapter and
// exchanges the frames through shared textures and a shared fence; the connection is upscaler_bridge_server.h. The
// hub supplies the vendor SDKs on its include and library paths (nvsdk_ngx*.h, nvsdk_ngx_s.lib, ffx_api*.h,
// ffx_upscale.h). DLL safety: every load is limited to System32 by the hub's entry point; D3D12, DXGI and what the NGX
// loader imports are delay-loaded from there. The vendor runtimes are loaded only by full path from the game's folder
// after their vendor's signature was checked; that folder is never a DLL search location (NGX gets it as its feature
// path, for nvngx_dlss.dll only).
//
// This software contains source code provided by NVIDIA Corporation.

#include <windows.h>
#include <softpub.h>
#include <wintrust.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>

#include "upscaler_bridge.h"
#include "upscaler_bridge_server.h"
#include "../upscaler_bridge_client.h"

using Microsoft::WRL::ComPtr;
namespace Protocol = UpscalerBridge;

namespace
{
    // -----------------------------------------------------------------------------------------------
    // Log

    FILE* gLog = nullptr;

    void Log(const char* format, ...)
    {
        if (!gLog)
            return;

        SYSTEMTIME time;
        GetLocalTime(&time);
        fprintf(gLog, "%02d:%02d:%02d.%03d ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);

        va_list args;
        va_start(args, format);
        vfprintf(gLog, format, args);
        va_end(args);

        fputc('\n', gLog);
        fflush(gLog);
    }

    // %LOCALAPPDATA%\FusionFixHub, where NGX keeps its own logs
    std::filesystem::path HubDataFolder()
    {
        wchar_t localAppData[MAX_PATH * 4] = {};
        auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, DWORD(std::size(localAppData)));
        if (length == 0 || length >= std::size(localAppData))
            return {};
        std::filesystem::path folder = std::filesystem::path(localAppData) / L"FusionFixHub";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        return folder;
    }

    // The vendor runtimes come from the game's folder, which anything can write to: only files with a valid
    // Authenticode signature of their vendor are loaded (organization of the signing certificate). No network:
    // revocation is not checked.
    bool SignedBy(HANDLE handle, const std::filesystem::path& file, const wchar_t* organization)
    {
        WINTRUST_FILE_INFO fileInfo{ sizeof(fileInfo) };
        fileInfo.pcwszFilePath = file.c_str();
        fileInfo.hFile = handle;
        WINTRUST_DATA data{ sizeof(data) };
        data.dwUIChoice = WTD_UI_NONE;
        data.fdwRevocationChecks = WTD_REVOKE_NONE;
        data.dwUnionChoice = WTD_CHOICE_FILE;
        data.pFile = &fileInfo;
        data.dwStateAction = WTD_STATEACTION_VERIFY;
        data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        bool trusted = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data) == ERROR_SUCCESS;

        bool matches = false;
        if (trusted)
        {
            auto provider = WTHelperProvDataFromStateData(data.hWVTStateData);
            auto signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
            if (signer && signer->csCertChain > 0 && signer->pasCertChain && signer->pasCertChain[0].pCert)
            {
                wchar_t name[256] = {};
                CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_ATTR_TYPE, 0, const_cast<char*>(szOID_ORGANIZATION_NAME), name, DWORD(std::size(name)));
                matches = wcscmp(name, organization) == 0;
            }
        }
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);

        Log("%ls: %s", file.c_str(), !trusted ? "no valid signature, not loaded" : matches ? "signed by its vendor" : "signed by someone else, not loaded");
        return trusted && matches;
    }

    // The file locked (others may read it, the loader too, but not write, rename or delete it) and checked through that
    // handle
    HANDLE LockSigned(const std::filesystem::path& file, const wchar_t* organization)
    {
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return nullptr;
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) || (tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            !SignedBy(handle, file, organization))
        {
            CloseHandle(handle);
            return nullptr;
        }
        return handle;
    }

    // Handles of the vendor DLLs checked, open while the bridge runs
    struct Locks
    {
        std::vector<HANDLE> handles;
        ~Locks() { Close(); }
        void Close()
        {
            for (auto handle : handles)
                CloseHandle(handle);
            handles.clear();
        }
    };

    constexpr const wchar_t* NVIDIA = L"NVIDIA Corporation";
    constexpr const wchar_t* AMD = L"Advanced Micro Devices";

    // -----------------------------------------------------------------------------------------------
    // Connection to the plugin: upscaler_bridge_server.h, its messages go to the log

    struct Connection : Protocol::Server::Connection
    {
        Connection()
        {
            log = [](const char* line) { Log("%s", line); };
        }
    };

    // -----------------------------------------------------------------------------------------------
    // D3D12

    struct SharedTexture
    {
        ComPtr<ID3D12Resource> resource;
        HANDLE handle = nullptr;
    };

    class Device
    {
    public:
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> sharedFence;
        ComPtr<ID3D12Fence> localFence;
        uint64_t localValue = 0;
        HANDLE fenceEvent = nullptr;

        static constexpr uint32_t Frames = 3;
        std::array<ComPtr<ID3D12CommandAllocator>, Frames> allocators;
        std::array<uint64_t, Frames> allocatorValues{};
        ComPtr<ID3D12GraphicsCommandList> list;
        uint32_t frame = 0;

        std::array<SharedTexture, static_cast<size_t>(Protocol::Texture::Count)> textures;
        HANDLE sharedFenceHandle = nullptr;

        // luid: the game's adapter, or zero for the first hardware adapter
        bool Create(LUID luid)
        {
            ComPtr<IDXGIFactory4> factory;
            if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
                return false;
            if (luid.LowPart || luid.HighPart)
            {
                if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
                    return false;
            }
            else if (FAILED(factory->EnumAdapters1(0, &adapter)))
            {
                return false;
            }
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
                return false;

            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
                return false;

            for (auto& allocator : allocators)
                if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
                    return false;

            if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list))))
                return false;
            list->Close();

            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&localFence))))
                return false;

            fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            return fenceEvent != nullptr;
        }

        bool WaitFence(ID3D12Fence* fence, uint64_t value, DWORD timeout)
        {
            if (fence->GetCompletedValue() >= value)
                return true;
            fence->SetEventOnCompletion(value, fenceEvent);
            return WaitForSingleObject(fenceEvent, timeout) == WAIT_OBJECT_0;
        }

        // Command list for immediate work, e.g. feature creation, waited for on the CPU
        ID3D12GraphicsCommandList* BeginImmediate()
        {
            WaitFence(localFence.Get(), localValue, 5000);
            allocators[0]->Reset();
            list->Reset(allocators[0].Get(), nullptr);
            return list.Get();
        }

        bool SubmitImmediate()
        {
            if (FAILED(list->Close()))
                return false;
            ID3D12CommandList* lists[] = { list.Get() };
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(localFence.Get(), ++localValue);
            return WaitFence(localFence.Get(), localValue, 10000);
        }

        // Command list of one frame; allocators are reused once the GPU finished with them
        ID3D12GraphicsCommandList* BeginFrame()
        {
            frame = (frame + 1) % Frames;
            if (sharedFence)
                WaitFence(sharedFence.Get(), allocatorValues[frame], 1000);
            allocators[frame]->Reset();
            list->Reset(allocators[frame].Get(), nullptr);
            return list.Get();
        }

        void SubmitFrame(uint64_t waitValue, uint64_t signalValue, bool execute)
        {
            list->Close();
            queue->Wait(sharedFence.Get(), waitValue);
            if (execute)
            {
                ID3D12CommandList* lists[] = { list.Get() };
                queue->ExecuteCommandLists(1, lists);
            }
            queue->Signal(sharedFence.Get(), signalValue);
            allocatorValues[frame] = signalValue;
        }

        void ReleaseTextures()
        {
            // The plugin has already imported or dropped its duplicates
            if (sharedFence)
                WaitFence(sharedFence.Get(), *std::max_element(allocatorValues.begin(), allocatorValues.end()), 1000);
            for (auto& texture : textures)
            {
                if (texture.handle)
                    CloseHandle(texture.handle);
                texture = {};
            }
            if (sharedFenceHandle)
                CloseHandle(sharedFenceHandle);
            sharedFenceHandle = nullptr;
            sharedFence.Reset();
            allocatorValues.fill(0);
        }

        bool CreateTexture(Protocol::Texture index, uint32_t width, uint32_t height, DXGI_FORMAT format, bool unorderedAccess, bool d3d11)
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = unorderedAccess ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
            // OpenSharedResource1 of Direct3D 11 refuses anything else (E_INVALIDARG). Not for DXVK: its Vulkan import
            // expects the layout of the plain texture.
            if (d3d11)
                desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

            auto& texture = textures[static_cast<size_t>(index)];
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture.resource))))
                return false;
            return SUCCEEDED(device->CreateSharedHandle(texture.resource.Get(), nullptr, GENERIC_ALL, nullptr, &texture.handle));
        }

        uint64_t AllocationSize(Protocol::Texture index)
        {
            auto desc = textures[static_cast<size_t>(index)].resource->GetDesc();
            return device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        }

        bool CreateSharedFence()
        {
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sharedFence))))
                return false;
            return SUCCEEDED(device->CreateSharedHandle(sharedFence.Get(), nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle));
        }

        ID3D12Resource* Texture(Protocol::Texture index)
        {
            return textures[static_cast<size_t>(index)].resource.Get();
        }

        void Transition(ID3D12GraphicsCommandList* cmd, bool toUse)
        {
            D3D12_RESOURCE_BARRIER barriers[static_cast<size_t>(Protocol::Texture::Count)]{};
            for (size_t i = 0; i < std::size(barriers); ++i)
            {
                auto output = i == static_cast<size_t>(Protocol::Texture::Output);
                auto use = output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[i].Transition.pResource = textures[i].resource.Get();
                barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barriers[i].Transition.StateBefore = toUse ? D3D12_RESOURCE_STATE_COMMON : use;
                barriers[i].Transition.StateAfter = toUse ? use : D3D12_RESOURCE_STATE_COMMON;
            }
            cmd->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
        }
    };

    // -----------------------------------------------------------------------------------------------
    // Frame parameters

    struct Size
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        uint32_t flags = 0;
    };

    struct FrameParams
    {
        Size size;
        float jitterX = 0.0f;
        float jitterY = 0.0f;
        float motionScaleX = 1.0f;
        float motionScaleY = 1.0f;
        float cameraNear = 0.1f;
        float cameraFar = 1000.0f;
        float cameraFovY = 1.0f;
        float frameTimeMs = 16.6f;
        float sharpness = 0.0f;
        bool reset = false;
    };

    // -----------------------------------------------------------------------------------------------
    // NVIDIA DLSS

    class DLSS
    {
        ID3D12Device* device = nullptr;
        NVSDK_NGX_Parameter* capabilities = nullptr;
        NVSDK_NGX_Parameter* parameters = nullptr;
        NVSDK_NGX_Handle* feature = nullptr;
        bool initialized = false;

        // NGX keeps using the feature search paths after initialization
        std::vector<std::wstring> paths;
        Locks locks;
        std::vector<const wchar_t*> pathPointers;

    public:
        bool available = false;

        // searchPaths: the only folders NGX loads nvngx_dlss.dll from
        bool Init(ID3D12Device* d3d, const std::vector<std::filesystem::path>& searchPaths, const std::filesystem::path& dataPath, std::string& message)
        {
            device = d3d;

            // Only folders whose nvngx_dlss.dll is NVIDIA's
            std::error_code ec;
            paths.clear();
            locks.Close();
            for (auto& path : searchPaths)
                if (std::filesystem::is_regular_file(path / Protocol::DLSSFiles[0], ec))
                    if (auto lock = LockSigned(path / Protocol::DLSSFiles[0], NVIDIA))
                    {
                        locks.handles.push_back(lock);
                        paths.push_back(path.wstring());
                    }
            if (paths.empty())
            {
                message = "DLSS is not available: nvngx_dlss.dll not found in the game's folder or not signed by NVIDIA";
                return false;
            }
            pathPointers.clear();
            for (auto& path : paths)
                pathPointers.push_back(path.c_str());

            NVSDK_NGX_FeatureCommonInfo info{};
            info.PathListInfo.Path = pathPointers.data();
            info.PathListInfo.Length = static_cast<unsigned int>(pathPointers.size());

            // Identifies this integration to NGX, a project id is only needed for NVIDIA-registered titles
            auto result = NVSDK_NGX_D3D12_Init_with_ProjectID("1b4f6c2e-9a3d-4e57-8c21-6f0d3b7a9e14", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                dataPath.c_str(), device, &info, NVSDK_NGX_Version_API);
            if (NVSDK_NGX_FAILED(result))
            {
                message = "NGX initialization failed: " + std::to_string(static_cast<uint32_t>(result));
                return false;
            }
            initialized = true;

            if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities)) || !capabilities)
            {
                message = "NGX capability parameters are not available";
                return false;
            }

            int supported = 0;
            int needsUpdatedDriver = 0;
            NVSDK_NGX_Parameter_GetI(capabilities, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdatedDriver);
            auto queried = NVSDK_NGX_Parameter_GetI(capabilities, NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
            if (NVSDK_NGX_FAILED(queried) || !supported)
            {
                message = needsUpdatedDriver ? "DLSS needs a newer NVIDIA driver" : "DLSS is not supported: no RTX GPU or nvngx_dlss.dll not found";
                return false;
            }

            if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&parameters)) || !parameters)
            {
                message = "NGX parameters could not be allocated";
                return false;
            }

            available = true;
            message = "DLSS available";
            return true;
        }

        bool Create(ID3D12GraphicsCommandList* cmd, const Size& size, uint32_t preset)
        {
            Release();

            for (auto hint : { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality })
                NVSDK_NGX_Parameter_SetUI(parameters, hint, preset);

            bool dlaa = size.outputWidth == size.width && size.outputHeight == size.height;
            NVSDK_NGX_DLSS_Create_Params create{};
            create.Feature.InWidth = size.width;
            create.Feature.InHeight = size.height;
            create.Feature.InTargetWidth = size.outputWidth;
            create.Feature.InTargetHeight = size.outputHeight;
            create.Feature.InPerfQualityValue = dlaa ? NVSDK_NGX_PerfQuality_Value_DLAA : NVSDK_NGX_PerfQuality_Value_MaxQuality;
            // Motion vectors at render resolution without jitter
            int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
            if (!(size.flags & Protocol::ConfigureFlags::LowDynamicRange))
                flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
            if (size.flags & Protocol::ConfigureFlags::DepthInverted)
                flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
            create.InFeatureCreateFlags = flags;
            create.InEnableOutputSubrects = false;

            auto result = NGX_D3D12_CREATE_DLSS_EXT(cmd, 1, 1, &feature, parameters, &create);
            if (NVSDK_NGX_FAILED(result))
            {
                Log("DLSS feature creation failed: 0x%08X", static_cast<uint32_t>(result));
                feature = nullptr;
                return false;
            }
            return true;
        }

        bool Evaluate(ID3D12GraphicsCommandList* cmd, Device& d, const FrameParams& frame)
        {
            if (!feature)
                return false;

            NVSDK_NGX_D3D12_DLSS_Eval_Params eval{};
            eval.Feature.pInColor = d.Texture(Protocol::Texture::Color);
            eval.Feature.pInOutput = d.Texture(Protocol::Texture::Output);
            eval.pInDepth = d.Texture(Protocol::Texture::Depth);
            eval.pInMotionVectors = d.Texture(Protocol::Texture::Motion);
            eval.InJitterOffsetX = frame.jitterX;
            eval.InJitterOffsetY = frame.jitterY;
            eval.InRenderSubrectDimensions = { frame.size.width, frame.size.height };
            eval.InReset = frame.reset ? 1 : 0;
            eval.InMVScaleX = frame.motionScaleX;
            eval.InMVScaleY = frame.motionScaleY;
            eval.InPreExposure = 1.0f;
            eval.InExposureScale = 1.0f;
            eval.InFrameTimeDeltaInMsec = frame.frameTimeMs;

            auto result = NGX_D3D12_EVALUATE_DLSS_EXT(cmd, feature, parameters, &eval);
            if (NVSDK_NGX_FAILED(result))
            {
                Log("DLSS evaluation failed: 0x%08X", static_cast<uint32_t>(result));
                return false;
            }
            return true;
        }

        void Release()
        {
            if (feature)
                NVSDK_NGX_D3D12_ReleaseFeature(feature);
            feature = nullptr;
        }

        void Shutdown()
        {
            Release();
            if (parameters)
                NVSDK_NGX_D3D12_DestroyParameters(parameters);
            if (capabilities)
                NVSDK_NGX_D3D12_DestroyParameters(capabilities);
            parameters = nullptr;
            capabilities = nullptr;
            if (initialized)
                NVSDK_NGX_D3D12_Shutdown1(device);
            initialized = false;
        }
    };

    // -----------------------------------------------------------------------------------------------
    // AMD FSR, through the FidelityFX API runtime

    class FSR
    {
        HMODULE module = nullptr;
        ffxFunctions functions{};
        ffxContext context = nullptr;
        ID3D12Device* device = nullptr;
        Locks locks;

    public:
        bool available = false;

        bool Init(ID3D12Device* d3d, const std::vector<std::filesystem::path>& searchPaths, std::string& message)
        {
            device = d3d;

            std::error_code ec;
            for (auto& dir : searchPaths)
            {
                // All files of the runtime from one folder
                bool complete = true;
                locks.Close();
                for (auto name : Protocol::FSRFiles)
                {
                    HANDLE lock = std::filesystem::is_regular_file(dir / name, ec) ? LockSigned(dir / name, AMD) : nullptr;
                    complete = complete && lock;
                    if (lock)
                        locks.handles.push_back(lock);
                }
                if (!complete)
                {
                    locks.Close();
                    continue;
                }

                // The runtime is in the game's folder, next to proxy DLLs, which never becomes a search location.
                // The loader loads amd_fidelityfx_upscaler_dx12.dll by name (LoadLibraryA): it is loaded first, by
                // full path, and the loader's load by name gets the module already loaded. Their own imports come
                // from System32.
                for (size_t i = std::size(Protocol::FSRFiles); i-- > 1;)
                {
                    auto dependency = dir / Protocol::FSRFiles[i];
                    if (!LoadLibraryExW(dependency.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
                        Log("FidelityFX runtime could not be loaded: %ls (%lu)", dependency.c_str(), GetLastError());
                }
                auto path = dir / Protocol::FSRFiles[0];
                module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (module)
                {
                    Log("FidelityFX runtime: %ls", path.c_str());
                    break;
                }
                Log("FidelityFX runtime could not be loaded: %ls (%lu)", path.c_str(), GetLastError());
            }

            if (!module)
            {
                message = "FSR is not available: the FidelityFX runtime is not in the game's folder, incomplete or not signed by AMD";
                return false;
            }

            ffxLoadFunctions(&functions, module);
            if (!functions.CreateContext || !functions.DestroyContext || !functions.Query || !functions.Dispatch)
            {
                message = "FSR is not available: the FidelityFX runtime has no FidelityFX API exports";
                return false;
            }

            uint64_t count = 0;
            ffxQueryDescGetVersions versions{};
            versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
            versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            versions.device = device;
            versions.outputCount = &count;
            if (functions.Query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0)
            {
                message = "FSR is not available: the FidelityFX runtime has no upscaler for this GPU";
                return false;
            }

            std::vector<uint64_t> ids(count);
            std::vector<const char*> names(count);
            versions.versionIds = ids.data();
            versions.versionNames = names.data();
            if (functions.Query(nullptr, &versions.header) == FFX_API_RETURN_OK)
                for (uint64_t i = 0; i < count; ++i)
                    Log("FSR version available: %s", names[i] ? names[i] : "?");

            available = true;
            message = "FSR available";
            return true;
        }

        bool Create(const Size& size)
        {
            Release();

            ffxCreateBackendDX12Desc backend{};
            backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
            backend.device = device;

            ffxCreateContextDescUpscale create{};
            create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            create.header.pNext = &backend.header;
            create.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
            if (!(size.flags & Protocol::ConfigureFlags::LowDynamicRange))
                create.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
            if (size.flags & Protocol::ConfigureFlags::DepthInverted)
                create.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
            create.maxRenderSize = { size.width, size.height };
            create.maxUpscaleSize = { size.outputWidth, size.outputHeight };

            auto result = functions.CreateContext(&context, &create.header, nullptr);
            if (result != FFX_API_RETURN_OK)
            {
                Log("FSR context creation failed: %u", result);
                context = nullptr;
                return false;
            }

            ffxQueryGetProviderVersion provider{};
            provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
            if (functions.Query(&context, &provider.header) == FFX_API_RETURN_OK && provider.versionName)
                Log("FSR provider: %s", provider.versionName);
            return true;
        }

        bool Evaluate(ID3D12GraphicsCommandList* cmd, Device& d, const FrameParams& frame)
        {
            if (!context)
                return false;

            ffxDispatchDescUpscale dispatch{};
            dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            dispatch.commandList = cmd;
            dispatch.color = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::Color), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            dispatch.depth = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::Depth), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            dispatch.motionVectors = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::Motion), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            if (frame.size.flags & Protocol::ConfigureFlags::ReactiveMask)
                dispatch.reactive = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::Reactive), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            dispatch.output = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::Output), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            dispatch.jitterOffset = { frame.jitterX, frame.jitterY };
            dispatch.motionVectorScale = { frame.motionScaleX, frame.motionScaleY };
            dispatch.renderSize = { frame.size.width, frame.size.height };
            dispatch.upscaleSize = { frame.size.outputWidth, frame.size.outputHeight };
            dispatch.enableSharpening = frame.sharpness > 0.0f;
            dispatch.sharpness = frame.sharpness;
            dispatch.frameTimeDelta = frame.frameTimeMs;
            dispatch.preExposure = 1.0f;
            dispatch.reset = frame.reset;
            dispatch.cameraNear = frame.cameraNear;
            dispatch.cameraFar = frame.cameraFar;
            dispatch.cameraFovAngleVertical = frame.cameraFovY;
            dispatch.viewSpaceToMetersFactor = 1.0f;

            auto result = functions.Dispatch(&context, &dispatch.header);
            if (result != FFX_API_RETURN_OK)
            {
                Log("FSR dispatch failed: %u", result);
                return false;
            }
            return true;
        }

        void Release()
        {
            if (context)
                functions.DestroyContext(&context, nullptr);
            context = nullptr;
        }

        void Shutdown()
        {
            Release();
            if (module)
                FreeLibrary(module);
            module = nullptr;
        }
    };

    // -----------------------------------------------------------------------------------------------

    class Bridge
    {
        Connection connection;
        Device device;
        DLSS dlss;
        FSR fsr;
        Protocol::Backend backend = Protocol::Backend::None;
        Size size;

        bool Duplicate(HANDLE source, uint64_t& target)
        {
            HANDLE duplicate = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), source, connection.client, &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return false;
            target = reinterpret_cast<uint64_t>(duplicate);
            return true;
        }

        bool Configure()
        {
            auto& shared = *connection.shared;

            dlss.Release();
            fsr.Release();
            device.ReleaseTextures();
            backend = Protocol::Backend::None;

            size.width = shared.Width;
            size.height = shared.Height;
            size.outputWidth = shared.OutputWidth ? shared.OutputWidth : shared.Width;
            size.outputHeight = shared.OutputHeight ? shared.OutputHeight : shared.Height;
            size.flags = shared.Flags;
            if (size.width == 0 || size.height == 0 || size.outputWidth > 16384 || size.outputHeight > 16384 ||
                size.outputWidth < size.width || size.outputHeight < size.height)
                return false;

            using T = Protocol::Texture;
            bool d3d11 = (size.flags & Protocol::ConfigureFlags::SharedWithD3D11) != 0;
            if (!device.CreateTexture(T::Color, size.width, size.height, DXGI_FORMAT_R16G16B16A16_FLOAT, false, d3d11) ||
                !device.CreateTexture(T::Depth, size.width, size.height, DXGI_FORMAT_R32_FLOAT, false, d3d11) ||
                !device.CreateTexture(T::Motion, size.width, size.height, DXGI_FORMAT_R16G16_FLOAT, false, d3d11) ||
                !device.CreateTexture(T::Reactive, size.width, size.height, DXGI_FORMAT_R16_FLOAT, false, d3d11) ||
                !device.CreateTexture(T::Output, size.outputWidth, size.outputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, true, d3d11) ||
                !device.CreateSharedFence())
            {
                connection.Message("Shared textures could not be created");
                return false;
            }

            bool created = false;
            if (shared.ConfigureBackend == Protocol::Backend::DLSS && dlss.available)
            {
                auto cmd = device.BeginImmediate();
                created = dlss.Create(cmd, size, shared.DLSSPreset);
                created = device.SubmitImmediate() && created;
            }
            else if (shared.ConfigureBackend == Protocol::Backend::FSR && fsr.available)
            {
                created = fsr.Create(size);
            }

            if (!created)
            {
                connection.Message("The upscaler could not be created at %ux%u -> %ux%u", size.width, size.height, size.outputWidth, size.outputHeight);
                return false;
            }

            for (size_t i = 0; i < static_cast<size_t>(T::Count); ++i)
            {
                shared.TextureSizes[i] = device.AllocationSize(static_cast<T>(i));
                if (!Duplicate(device.textures[i].handle, shared.TextureHandles[i]))
                    return false;
            }
            if (!Duplicate(device.sharedFenceHandle, shared.FenceHandle))
                return false;

            backend = shared.ConfigureBackend;
            connection.Message("%s ready at %ux%u -> %ux%u", backend == Protocol::Backend::DLSS ? "DLSS" : "FSR", size.width, size.height,
                size.outputWidth, size.outputHeight);
            return true;
        }

        bool Evaluate()
        {
            auto& shared = *connection.shared;
            if (backend == Protocol::Backend::None || !device.sharedFence)
                return false;

            FrameParams frame;
            frame.size = size;
            frame.jitterX = shared.JitterX;
            frame.jitterY = shared.JitterY;
            frame.motionScaleX = shared.MotionScaleX;
            frame.motionScaleY = shared.MotionScaleY;
            frame.cameraNear = shared.CameraNear;
            frame.cameraFar = shared.CameraFar;
            frame.cameraFovY = shared.CameraFovY;
            frame.frameTimeMs = shared.FrameTimeMs;
            frame.sharpness = shared.Sharpness;
            frame.reset = shared.Reset != 0;

            auto cmd = device.BeginFrame();
            device.Transition(cmd, true);
            bool evaluated = backend == Protocol::Backend::DLSS ? dlss.Evaluate(cmd, device, frame) : fsr.Evaluate(cmd, device, frame);
            device.Transition(cmd, false);

            // The fence always advances, so the plugin can rely on the values it waits for
            device.SubmitFrame(shared.WaitValue, shared.SignalValue, evaluated);
            return evaluated;
        }

    public:
        int Run(const std::wstring& name)
        {
            if (!connection.Open(name))
                return 1;

            auto& shared = *connection.shared;
            if (shared.LogPath[0])
                gLog = _wfopen(shared.LogPath, L"w");
            Log("FusionFixHub upscaler bridge %u started for %s, process %u", Protocol::Version, shared.ClientName, shared.ClientProcessId);

            LUID luid{ shared.AdapterLuidLow, shared.AdapterLuidHigh };
            if (!device.Create(luid))
            {
                connection.Message("D3D12 device could not be created on the game's adapter");
                connection.Respond(Protocol::Status::Failed);
                return 2;
            }

            DXGI_ADAPTER_DESC1 adapterDesc{};
            device.adapter->GetDesc1(&adapterDesc);
            Log("Adapter: %ls", adapterDesc.Description);

            auto searchPaths = std::vector<std::filesystem::path>{ UpscalerBridge::RuntimeDirectory(shared.GameDirectory) };
            Log("Runtime folder: %ls", searchPaths[0].c_str());

            std::string dlssMessage;
            std::string fsrMessage;
            if (adapterDesc.VendorId == 0x10DE)
                dlss.Init(device.device.Get(), searchPaths, HubDataFolder(), dlssMessage);
            else
                dlssMessage = "DLSS is not available: not an NVIDIA GPU";
            fsr.Init(device.device.Get(), searchPaths, fsrMessage);

            Log("%s", dlssMessage.c_str());
            Log("%s", fsrMessage.c_str());
            shared.AvailableBackends = (dlss.available ? Protocol::BackendBit(Protocol::Backend::DLSS) : 0) |
                (fsr.available ? Protocol::BackendBit(Protocol::Backend::FSR) : 0);
            connection.Message("%s; %s", dlssMessage.c_str(), fsrMessage.c_str());
            connection.Respond(Protocol::Status::Ok);

            connection.Serve([this](Protocol::Command command)
            {
                switch (command)
                {
                case Protocol::Command::Configure: return Configure();
                case Protocol::Command::Evaluate: return Evaluate();
                default: return false;
                }
            });

            if (device.queue && device.localFence)
            {
                device.queue->Signal(device.localFence.Get(), ++device.localValue);
                device.WaitFence(device.localFence.Get(), device.localValue, 2000);
            }
            dlss.Shutdown();
            fsr.Shutdown();
            device.ReleaseTextures();
            Log("Bridge stopped");
            if (gLog)
                fclose(gLog);
            return 0;
        }

        // The work of a start without a plugin: device, NGX and FidelityFX from the game folder gameRoot
        std::string SelfTest(const std::filesystem::path& gameRoot)
        {
            if (!device.Create(LUID{}))
                return "Upscaler bridge self test: no D3D12 device";
            auto searchPaths = std::vector<std::filesystem::path>{ UpscalerBridge::RuntimeDirectory(gameRoot) };
            std::string dlssMessage = "DLSS: not an NVIDIA GPU";
            std::string fsrMessage;
            DXGI_ADAPTER_DESC1 adapterDesc{};
            device.adapter->GetDesc1(&adapterDesc);
            if (adapterDesc.VendorId == 0x10DE)
                dlss.Init(device.device.Get(), searchPaths, HubDataFolder(), dlssMessage);
            fsr.Init(device.device.Get(), searchPaths, fsrMessage);
            dlss.Shutdown();
            fsr.Shutdown();
            return "Upscaler bridge self test: " + dlssMessage + "; " + fsrMessage;
        }
    };
}

namespace fh::bridge
{
    int Run(const std::wstring& name)
    {
        Bridge bridge;
        return bridge.Run(name);
    }

    void* OpenSigned(const std::filesystem::path& file, const wchar_t* organization)
    {
        return LockSigned(file, organization);
    }

    std::string SelfTest(const std::filesystem::path& gameRoot)
    {
        Bridge bridge;
        return bridge.SelfTest(gameRoot);
    }
}
