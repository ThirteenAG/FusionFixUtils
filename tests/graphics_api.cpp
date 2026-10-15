// upscaler/graphics_api.h: what can run without a game and a GPU backend. The SHA-256 check of DXVK, the export check
// that tells d3d9.dll files apart (system, Fusion Fix's pass-through placeholder, an old Fusion Fix proxy, a
// third-party wrapper), the crash guard's marker and the decision at the start (the setting set back to DirectX 9
// through the plugin's settings, here a fake sink), availability, the notice, the device methods' hooks on a real
// Direct3D 9 device (they survive the runtime writing the device's vtable again). The redirection itself (imports,
// the system's objects on another API) is tested in game.

#include "../upscaler/graphics_api.h"
#include "check.h"
#include <fstream>

namespace
{
    namespace fs = std::filesystem;
    using namespace FusionFixGraphicsApi;

    fs::path TestFolder(const wchar_t* name)
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        auto folder = fs::path(path).parent_path() / L"graphics-api-test" / name;
        std::error_code ec;
        fs::remove_all(folder, ec);
        fs::create_directories(folder, ec);
        return folder;
    }

    fs::path SystemD3D9Path()
    {
        wchar_t folder[MAX_PATH] = {};
        GetSystemDirectoryW(folder, MAX_PATH);
        return fs::path(folder) / L"d3d9.dll";
    }
}

TEST_CASE("graphics_api: SHA-256 and the known DXVK builds")
{
    CHECK(detail::Sha256("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(detail::Sha256("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(detail::KnownDxvkHash("265888c31ca78dffa290c39cb7e50bfb02762590e41927906e46fb32f01497fa"));
    // The older DXVK that GTA IV's Fusion Fix 5.0.1 shipped in its zip isn't one of upscalers.json
    CHECK(!detail::KnownDxvkHash("00bc142e98ddad1e334a4d88fa630038e092b0758b3d79e295172b181a4c772d"));
    CHECK(!detail::KnownDxvkHash(""));

    auto folder = TestFolder(L"hash");
    {
        std::ofstream out(folder / L"file.bin", std::ios::binary);
        out << "abc";
    }
    CHECK(detail::Sha256(folder / L"file.bin") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(detail::Sha256(folder / L"missing.bin").empty());
}

TEST_CASE("graphics_api: exports of loaded modules and of files")
{
    auto system = LoadLibraryExW(L"d3d9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    REQUIRE(system != nullptr);
    CHECK(detail::ImageExports(system, "Direct3DCreate9"));
    CHECK(detail::ImageExports(system, "Direct3DCreate9Ex"));
    CHECK(!detail::ImageExports(system, OldProxyExport));
    CHECK(!detail::ImageExports(system, PassThroughExport));
    CHECK(!detail::ImageExports(nullptr, "Direct3DCreate9"));

    CHECK(detail::FileExports(SystemD3D9Path(), "Direct3DCreate9"));
    CHECK(!detail::FileExports(SystemD3D9Path(), D3D9ON12_CAPABILITIES_EXPORT));

    auto folder = TestFolder(L"exports");
    {
        std::ofstream out(folder / L"d3d9on12.dll", std::ios::binary);
        out << "MZ not a dll";
    }
    CHECK(!detail::FileExports(folder / L"d3d9on12.dll", D3D9ON12_CAPABILITIES_EXPORT));
    CHECK(!detail::FileExports(folder / L"missing.dll", D3D9ON12_CAPABILITIES_EXPORT));
}

TEST_CASE("graphics_api: d3d9.dll files are told apart")
{
    CHECK(detail::Classify(true, false, false) == D3D9Module::System);
    CHECK(detail::Classify(true, true, true) == D3D9Module::System);
    CHECK(detail::Classify(false, true, false) == D3D9Module::PassThrough);
    CHECK(detail::Classify(false, false, true) == D3D9Module::OldProxy);
    CHECK(detail::Classify(false, false, false) == D3D9Module::Foreign);

    auto system = LoadLibraryExW(L"d3d9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    REQUIRE(system != nullptr);
    CHECK(detail::InSystemDirectory(detail::ModulePath(system)));
    CHECK(detail::Classify(system) == D3D9Module::System);
    // This exe: not in System32; it holds the header's code
    CHECK(!detail::InSystemDirectory(detail::ModulePath(nullptr)));
    CHECK(detail::Classify(GetModuleHandleW(nullptr)) == D3D9Module::Own);     // the header's code is in this exe
    CHECK(detail::ModuleOf(reinterpret_cast<const void*>(&detail::Direct3DCreate9)) == GetModuleHandleW(nullptr));
    CHECK(detail::Classify(detail::ModuleOf(reinterpret_cast<const void*>(GetProcAddress(system, "Direct3DCreate9")))) == D3D9Module::System);
}

TEST_CASE("graphics_api: the marker")
{
    auto folder = TestFolder(L"marker");
    CHECK(detail::MarkerPath({}, L"Game.Fix").empty());
    auto marker = detail::MarkerPath(folder, L"Game.Fix");
    CHECK(marker.filename() == L"Game.Fix.graphics-api-start");
    CHECK(!detail::ReadMarker(marker).found);

    detail::WriteMarker(marker, Api::D3D9On12, "the device couldn't be created (0x8876086C)");
    auto read = detail::ReadMarker(marker);
    CHECK(read.found);
    CHECK(read.api == Api::D3D9On12);
    CHECK(read.reason == "the device couldn't be created (0x8876086C)");

    detail::WriteMarker(marker, Api::Vulkan, "");
    read = detail::ReadMarker(marker);
    CHECK(read.api == Api::Vulkan);
    CHECK(read.reason.empty());

    detail::RemoveMarker(marker);
    CHECK(!fs::exists(marker));
    detail::WriteMarker({}, Api::Vulkan, "nowhere");     // no folder: no guard
}

TEST_CASE("graphics_api: the decision at the start")
{
    detail::Availability all{ true, true, true };
    detail::Availability none{ false, false, true };
    detail::Marker noMarker;
    detail::Marker vulkanCrashed{ true, Api::Vulkan, "the game closed or crashed before its first frames on Vulkan" };
    detail::Marker dx12Crashed{ true, Api::D3D9On12, "the device couldn't be created (0x80004005)" };

    // Nothing wrong
    auto d = detail::Decide(Api::Vulkan, noMarker, all);
    CHECK(d.effective == Api::Vulkan);
    CHECK(d.why.empty());
    CHECK(!d.setBack);
    CHECK(!d.notice);
    CHECK(!d.removeMarker);
    CHECK(detail::Decide(Api::D3D9On12, noMarker, all).effective == Api::D3D9On12);
    CHECK(detail::Decide(Api::Direct3D9, noMarker, none).effective == Api::Direct3D9);

    // The last start of this API crashed: DirectX 9, the setting set back, a notice
    d = detail::Decide(Api::Vulkan, vulkanCrashed, all);
    CHECK(d.effective == Api::Direct3D9);
    CHECK(d.setBack);
    CHECK(d.notice);
    CHECK(d.removeMarker);
    CHECK(d.failure == vulkanCrashed.reason);
    CHECK(d.why.find("Vulkan didn't start last time") == 0);
    d = detail::Decide(Api::D3D9On12, dx12Crashed, all);
    CHECK(d.effective == Api::Direct3D9);
    CHECK(d.setBack);

    // Another API was chosen since: the marker is stale, removed, and the choice runs
    d = detail::Decide(Api::D3D9On12, vulkanCrashed, all);
    CHECK(d.effective == Api::D3D9On12);
    CHECK(d.removeMarker);
    CHECK(!d.setBack);
    d = detail::Decide(Api::Direct3D9, vulkanCrashed, all);
    CHECK(d.effective == Api::Direct3D9);
    CHECK(d.removeMarker);
    CHECK(!d.setBack);
    CHECK(!d.notice);

    // Missing files: DirectX 9 this start, the choice kept (ASI Settings availability shows DirectX 9 in effect)
    d = detail::Decide(Api::Vulkan, noMarker, none);
    CHECK(d.effective == Api::Direct3D9);
    CHECK(!d.setBack);
    CHECK(!d.notice);
    CHECK(d.why.find("vulkan.dll") != std::string::npos);
    d = detail::Decide(Api::D3D9On12, noMarker, none);
    CHECK(d.effective == Api::Direct3D9);
    CHECK(!d.setBack);
    CHECK(d.why.find("d3d9on12.dll") != std::string::npos);
    d = detail::Decide(Api::D3D9On12, noMarker, detail::Availability{ true, true, false });
    CHECK(d.effective == Api::Direct3D9);
    CHECK(d.why.find("Windows") != std::string::npos);
}

TEST_CASE("graphics_api: the start sets the setting back through the plugin's settings")
{
    auto folder = TestFolder(L"start");
    std::vector<Api> stored;
    Options options;
    options.markerFolder = folder;
    options.markerName = L"Game.Fix";
    options.callbacks.setApiSetting = [&](Api api) { stored.push_back(api); };

    // No marker: nothing stored, no marker written by the decision itself
    options.api = Api::Vulkan;
    fs::path marker;
    auto d = detail::Start(options, { true, true, true }, marker);
    CHECK(d.effective == Api::Vulkan);
    CHECK(stored.empty());
    CHECK(marker == folder / L"Game.Fix.graphics-api-start");
    CHECK(!fs::exists(marker));

    // A start that didn't present: DirectX 9 stored, the marker used up
    detail::WriteMarker(marker, Api::Vulkan, "the game closed or crashed before its first frames on Vulkan");
    d = detail::Start(options, { true, true, true }, marker);
    CHECK(d.effective == Api::Direct3D9);
    REQUIRE(stored.size() == 1);
    CHECK(stored[0] == Api::Direct3D9);
    CHECK(!fs::exists(marker));

    // The next start (the setting now says DirectX 9): nothing to do
    options.api = Api::Direct3D9;
    d = detail::Start(options, { true, true, true }, marker);
    CHECK(d.effective == Api::Direct3D9);
    CHECK(stored.size() == 1);

    // A stale marker of another API: removed, nothing stored
    detail::WriteMarker(marker, Api::D3D9On12, "old");
    options.api = Api::Vulkan;
    d = detail::Start(options, { true, true, true }, marker);
    CHECK(d.effective == Api::Vulkan);
    CHECK(stored.size() == 1);
    CHECK(!fs::exists(marker));

    // Without a settings sink the start still falls back
    options.callbacks.setApiSetting = nullptr;
    detail::WriteMarker(marker, Api::Vulkan, "x");
    CHECK(detail::Start(options, { true, true, true }, marker).effective == Api::Direct3D9);
}

TEST_CASE("graphics_api: availability and the notice")
{
    CHECK(Available(Api::Direct3D9));
    // The test's folder has no vulkan.dll and no driver
    CHECK(!VulkanAvailable());
    CHECK(!Available(Api::Vulkan));
    CHECK(!DriverAvailable());
    CHECK(!Available(Api::D3D9On12));
    CHECK(Running() == Api::Direct3D9);
    CHECK(DriverCaps() == 0);
    CHECK(QueuedInterop() == nullptr);
    CHECK(ExclusiveFullscreenAllowed());

    auto text = detail::NoticeText(Api::Vulkan, "vulkan.dll has no Direct3DCreate9", L"");
    CHECK(text.find(L"Vulkan (DXVK) couldn't start (vulkan.dll has no Direct3DCreate9)") == 0);
    CHECK(text.find(L"third-party") == std::wstring::npos);
    text = detail::NoticeText(Api::D3D9On12, "", L"C:\\Game\\d3d9.dll");
    CHECK(text.find(L"DirectX 12 couldn't start.") == 0);
    CHECK(text.find(L"C:\\Game\\d3d9.dll") != std::wstring::npos);
    CHECK(std::string(Name(Api::D3D9On12)) == "DirectX 12");
}

TEST_CASE("graphics_api: the device's Reset and Present hooks survive the runtime writing its vtable again")
{
    auto& s = detail::Get();
    if (!s.system)
        s.system = detail::SystemD3D9();
    REQUIRE(s.system != nullptr);
    std::vector<std::string> lines;
    s.options.callbacks.log = [&](const char* line) { lines.push_back(line); };
    auto resets = [&]
    {
        return std::count_if(lines.begin(), lines.end(), [](const std::string& line) { return line.rfind("Reset: ", 0) == 0; });
    };

    auto create = reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(s.system, "Direct3DCreate9"));
    REQUIRE(create != nullptr);
    auto d3d = create(D3D_SDK_VERSION);
    REQUIRE(d3d != nullptr);
    HWND window = CreateWindowW(L"STATIC", L"graphics_api test", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.hDeviceWindow = window;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    IDirect3DDevice9* device = nullptr;
    // No device to test with (no D3D9 device on the machine)
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device)) || !device)
    {
        d3d->Release();
        DestroyWindow(window);
        s.options.callbacks.log = nullptr;
        return;
    }
    auto vtable = *reinterpret_cast<void***>(device);
    void* runtimeReset = vtable[16];
    void* runtimePresent = vtable[17];
    // Basic Render (a machine without a GPU, CI) gives a device whose methods aren't in System32's d3d9.dll: those
    // are hooked in the vtable, which a rewrite of the vtable undoes
    bool inRuntime = detail::InImage(runtimeReset, s.system) && detail::InImage(runtimePresent, s.system);
    if (!inRuntime)
        std::printf("note [graphics_api]: the device's methods aren't in System32's d3d9.dll, the vtable rewrite isn't tested\n");

    detail::AfterCreateDevice(S_OK, device, false, &pp);
    CHECK(detail::OwnDevice(device));
    CHECK(s.reset.Original(device) != nullptr);
    // Hooked in the runtime's code: the vtable is left as it is
    if (inRuntime)
        CHECK(vtable[16] == runtimeReset);

    CHECK(SUCCEEDED(device->Reset(&pp)));
    CHECK(resets() == 1);
    CHECK(SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr)));

    // The runtime writes the device's vtable again (what it did to the vtable hook in GTA IV): still hooked
    if (inRuntime)
    {
        DWORD protection = 0;
        VirtualProtect(&vtable[16], 2 * sizeof(void*), PAGE_READWRITE, &protection);
        vtable[16] = runtimeReset;
        vtable[17] = runtimePresent;
        VirtualProtect(&vtable[16], 2 * sizeof(void*), protection, &protection);
        CHECK(SUCCEEDED(device->Reset(&pp)));
        CHECK(resets() == 2);
        CHECK(SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr)));
    }

    // A second device of the process (not the game's on the chosen API) runs through the hook unchanged
    IDirect3DDevice9* other = nullptr;
    if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &other)) && other)
    {
        CHECK(!detail::OwnDevice(other));
        CHECK(SUCCEEDED(other->Reset(&pp)));
        other->Release();
    }

    {
        std::lock_guard lock(s.mutex);
        s.devices.clear();
    }
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    s.options.callbacks.log = nullptr;
}