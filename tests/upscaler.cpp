// upscaler/: the bridge protocol (layout identical in x86 and x64, values through the shared memory), the plugin's
// client (upscaler_bridge_client.h) against a fake hub that uses the hub's real connection code
// (bridge/upscaler_bridge_server.h; tests/fakehub): start, Configure, Evaluate, late answers, shutdown, the job that ends
// the hub with the game, and a protocol version mismatch. The fake hub of the other bitness is used too when it was
// built (a 32-bit game talks to the 64-bit hub). The GPU side of the bridge
// (bridge/upscaler_bridge.cpp, needs the vendor SDKs) is compile-checked only.

#include "../upscaler/upscaler_bridge_client.h"
#include "../upscaler/bridge/upscaler_bridge_server.h"
#include "check.h"
#include <cstddef>
#include <fstream>
#include <thread>

namespace
{
    namespace fs = std::filesystem;
    using namespace UpscalerBridge;

    fs::path ExeFolder()
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return fs::path(path).parent_path();
    }

    // bin/<platform>/<configuration>/fakehub/FusionFixHub.exe of this platform and of the other one, when built
    std::vector<fs::path> FakeHubFolders()
    {
        std::vector<fs::path> folders;
        auto own = ExeFolder() / L"fakehub";
        if (fs::exists(own / HubFileName))
            folders.push_back(own);
        auto configuration = ExeFolder().filename();
        auto other = ExeFolder().parent_path().parent_path() / (sizeof(void*) == 8 ? L"Win32" : L"x64") / configuration / L"fakehub";
        if (fs::exists(other / HubFileName))
            folders.push_back(other);
        return folders;
    }

    bool WaitStarted(Client& client, bool& ok, DWORD timeout = 10000)
    {
        auto start = GetTickCount64();
        while (GetTickCount64() - start < timeout)
        {
            if (client.PollStarted(ok))
                return true;
            Sleep(5);
        }
        return false;
    }

    // The objects of a plugin made by hand, for what Client doesn't do
    struct HandMade
    {
        std::wstring name;
        HANDLE mapping = nullptr, request = nullptr, response = nullptr;
        Shared* shared = nullptr;

        explicit HandMade(const wchar_t* tag)
        {
            name = L"Local\\FusionFixUtilsTests." + std::wstring(tag) + L"." + std::to_wstring(GetCurrentProcessId());
            mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), (name + MappingSuffix).c_str());
            request = CreateEventW(nullptr, FALSE, FALSE, (name + RequestSuffix).c_str());
            response = CreateEventW(nullptr, FALSE, FALSE, (name + ResponseSuffix).c_str());
            shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
            std::memset(shared, 0, sizeof(Shared));
            shared->Version = Version;
            shared->ClientProcessId = GetCurrentProcessId();
        }

        ~HandMade()
        {
            UnmapViewOfFile(shared);
            for (auto handle : { mapping, request, response })
                CloseHandle(handle);
        }
    };

    // Starts the fake hub with arguments of our own; returns its process handle
    HANDLE StartFakeHub(const fs::path& folder, const std::wstring& arguments)
    {
        auto exe = folder / HubFileName;
        auto commandLine = L"\"" + exe.wstring() + L"\" " + arguments;
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, folder.c_str(), &startup, &info))
            return nullptr;
        CloseHandle(info.hThread);
        return info.hProcess;
    }

    DWORD ExitCode(HANDLE process, DWORD timeout = 10000)
    {
        if (WaitForSingleObject(process, timeout) != WAIT_OBJECT_0)
            return 0xFFFFFFFF;
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        return code;
    }
}

TEST_CASE("upscaler: the shared layout is the same in x86 and x64")
{
    CHECK(sizeof(Shared) == 2872);
    CHECK(offsetof(Shared, GameDirectory) == 16);
    CHECK(offsetof(Shared, LogPath) == 1056);
    CHECK(offsetof(Shared, ClientName) == 2096);
    CHECK(offsetof(Shared, AvailableBackends) == 2160);
    CHECK(offsetof(Shared, Message) == 2168);
    CHECK(offsetof(Shared, RequestCommand) == 2680);
    CHECK(offsetof(Shared, RequestSerial) == 2684);
    CHECK(offsetof(Shared, ConfigureBackend) == 2688);
    CHECK(offsetof(Shared, Flags) == 2712);
    CHECK(offsetof(Shared, WaitValue) == 2720);
    CHECK(offsetof(Shared, SignalValue) == 2728);
    CHECK(offsetof(Shared, JitterX) == 2736);
    CHECK(offsetof(Shared, Reset) == 2772);
    CHECK(offsetof(Shared, ResponseSerial) == 2776);
    CHECK(offsetof(Shared, ResponseStatus) == 2780);
    CHECK(offsetof(Shared, TextureHandles) == 2784);
    CHECK(offsetof(Shared, TextureSizes) == 2824);
    CHECK(offsetof(Shared, FenceHandle) == 2864);
}

TEST_CASE("upscaler: protocol constants")
{
    CHECK(Version == 3);
    CHECK(BackendBit(Backend::DLSS) == 2u);
    CHECK(BackendBit(Backend::FSR) == 4u);
    CHECK(InputCount == 4u);
    CHECK(static_cast<uint32_t>(Texture::Count) == 5u);
    CHECK(std::wstring(HubFileName) == L"FusionFixHub.exe");
    CHECK(std::wstring(ArgumentName) == L"--upscaler");
    CHECK(std::size(DLSSFiles) == 1 && std::size(FSRFiles) == 2);
}

TEST_CASE("upscaler: values written by the plugin's view are what the hub's view reads, and back")
{
    HandMade plugin(L"RoundTrip");
    plugin.shared->ConfigureBackend = Backend::FSR;
    plugin.shared->Width = 1920;
    plugin.shared->Height = 1080;
    plugin.shared->WaitValue = 0x1122334455667788ull;
    plugin.shared->JitterX = -0.375f;
    plugin.shared->Flags = ConfigureFlags::ReactiveMask | ConfigureFlags::SharedWithD3D11;
    wcsncpy_s(plugin.shared->GameDirectory, L"C:\\Games\\Some Game", _TRUNCATE);

    Server::Connection hub;
    REQUIRE(hub.Connect(plugin.name) == Server::OpenResult::Ok);
    REQUIRE(hub.shared != plugin.shared);
    CHECK(hub.shared->ConfigureBackend == Backend::FSR);
    CHECK(hub.shared->Width == 1920 && hub.shared->Height == 1080);
    CHECK(hub.shared->WaitValue == 0x1122334455667788ull);
    CHECK(hub.shared->JitterX == -0.375f);
    CHECK(hub.shared->Flags == (ConfigureFlags::ReactiveMask | ConfigureFlags::SharedWithD3D11));
    CHECK(std::wstring(hub.shared->GameDirectory) == L"C:\\Games\\Some Game");

    // The answer: serial of the request, status, message, the plugin's event
    plugin.shared->RequestSerial = 41;
    hub.shared->AvailableBackends = BackendBit(Backend::FSR);
    std::string logged;
    static std::string* sink = nullptr;
    sink = &logged;
    hub.log = [](const char* line) { *sink = line; };
    hub.Message("ready %d", 7);
    hub.Respond(Status::Ok);
    CHECK(WaitForSingleObject(plugin.response, 0) == WAIT_OBJECT_0);
    CHECK(plugin.shared->ResponseSerial == 41);
    CHECK(plugin.shared->ResponseStatus == Status::Ok);
    CHECK(std::string(plugin.shared->Message) == "ready 7");
    CHECK(logged == "ready 7");
    CHECK(plugin.shared->AvailableBackends == BackendBit(Backend::FSR));
}

TEST_CASE("upscaler: the hub's side refuses missing objects and another protocol version")
{
    Server::Connection missing;
    CHECK(missing.Connect(L"Local\\FusionFixUtilsTests.NoSuchBridge") == Server::OpenResult::MissingObjects);
    CHECK(!missing.Open(L"Local\\FusionFixUtilsTests.NoSuchBridge"));

    HandMade plugin(L"Version");
    plugin.shared->Version = Version + 1;
    Server::Connection newer;
    CHECK(newer.Connect(plugin.name) == Server::OpenResult::VersionMismatch);
    plugin.shared->Version = Version - 1;
    Server::Connection older;
    CHECK(older.Connect(plugin.name) == Server::OpenResult::VersionMismatch);
    // Nothing was answered
    CHECK(WaitForSingleObject(plugin.response, 0) == WAIT_TIMEOUT);
}

TEST_CASE("upscaler: the hub terminates the plugin's strings")
{
    HandMade plugin(L"Strings");
    std::memset(plugin.shared->ClientName, 'x', sizeof(plugin.shared->ClientName));
    std::fill(std::begin(plugin.shared->LogPath), std::end(plugin.shared->LogPath), L'y');
    std::fill(std::begin(plugin.shared->GameDirectory), std::end(plugin.shared->GameDirectory), L'z');
    Server::Connection hub;
    REQUIRE(hub.Connect(plugin.name) == Server::OpenResult::Ok);
    CHECK(std::strlen(hub.shared->ClientName) == sizeof(hub.shared->ClientName) - 1);
    CHECK(std::wcslen(hub.shared->LogPath) == PathLength - 1);
    CHECK(std::wcslen(hub.shared->GameDirectory) == PathLength - 1);
}

TEST_CASE("upscaler: the hub stops serving when the plugin's process ends")
{
    auto folders = FakeHubFolders();
    REQUIRE(!folders.empty());
    // A process that ends at once plays the plugin
    HANDLE gone = StartFakeHub(folders[0], L"");
    REQUIRE(gone != nullptr);
    CHECK(ExitCode(gone) == 2);

    HandMade plugin(L"Gone");
    plugin.shared->ClientProcessId = GetProcessId(gone);
    Server::Connection hub;
    REQUIRE(hub.Connect(plugin.name) == Server::OpenResult::Ok);
    int handled = 0;
    CHECK(!hub.Serve([&](Command) { ++handled; return true; }));
    CHECK(handled == 0);
    CloseHandle(gone);
}

TEST_CASE("upscaler: plugin and fake hub, start, Configure, Evaluate, late answers, shutdown")
{
    auto folders = FakeHubFolders();
    REQUIRE(!folders.empty());
    for (auto& folder : folders)
    {
        Client client;
        REQUIRE(client.Start(folder, LUID{ 0x1234, 5 }, L"Local\\FusionFixUtilsTests.Upscaler", "UpscalerTest", L""));
        bool ok = false;
        REQUIRE(WaitStarted(client, ok));
        CHECK(ok);
        CHECK(client.IsAvailable(Backend::DLSS));
        CHECK(client.IsAvailable(Backend::FSR));
        CHECK(!client.IsAvailable(Backend::None));
        std::string message = client.shared->Message;
        CHECK(message.starts_with("fake hub 3, "));
        CHECK(message.ends_with("client UpscalerTest"));
        std::printf("    %s\n", message.c_str());
        // What the plugin wrote before starting the hub
        CHECK(client.shared->AdapterLuidLow == 0x1234 && client.shared->AdapterLuidHigh == 5);
        CHECK(fs::path(client.shared->GameDirectory) == folder);

        auto& s = *client.shared;
        s.ConfigureBackend = Backend::DLSS;
        s.Width = 1280;
        s.Height = 720;
        s.OutputWidth = 0;
        s.OutputHeight = 0;
        s.Flags = ConfigureFlags::DepthInverted;
        s.DLSSPreset = 6;
        CHECK(client.Request(Command::Configure, 5000));
        CHECK(s.TextureSizes[0] == 1280ull * 720);
        CHECK(s.TextureSizes[3] == 1280ull * 720 * 4);
        CHECK(s.TextureSizes[static_cast<size_t>(Texture::Output)] == 1280ull * 720);
        CHECK(std::string(s.Message) == "configured 1 1280x720 -> 1280x720 flags 2 preset 6");
        client.CloseSharedHandles();

        // The output may not be smaller than the render size
        s.OutputWidth = 640;
        s.OutputHeight = 360;
        CHECK(!client.Request(Command::Configure, 5000));
        CHECK(s.ResponseStatus == Status::Failed);

        s.WaitValue = 10;
        s.SignalValue = 11;
        s.JitterX = 0.25f;
        s.JitterY = -0.125f;
        s.CameraNear = 0.5f;
        s.CameraFar = 2000.0f;
        s.Reset = 1;
        CHECK(client.Request(Command::Evaluate, 5000));
        CHECK(std::string(s.Message).starts_with("evaluate 10 11 0.250 -0.125 "));
        CHECK(std::string(s.Message).ends_with("reset 1"));
        s.SignalValue = 10;
        CHECK(!client.Request(Command::Evaluate, 5000));

        // An unknown command is answered, with Failed
        CHECK(!client.Request(Command::None, 5000));
        CHECK(s.ResponseSerial == client.serial);

        // Too late: the plugin gives up; the late answer doesn't count for the next request
        s.SignalValue = 20;
        s.Sharpness = -300.0f;
        auto serial = client.serial;
        CHECK(!client.Request(Command::Evaluate, 50));
        Sleep(400);
        CHECK(s.ResponseSerial == serial + 1);
        s.Sharpness = 0.0f;
        s.WaitValue = 30;
        s.SignalValue = 29;
        // The stale answer (Ok) is still signalled; the request skips it and waits for its own (Failed)
        CHECK(!client.Request(Command::Evaluate, 5000));
        CHECK(s.ResponseSerial == client.serial);
        s.SignalValue = 31;
        CHECK(client.Request(Command::Evaluate, 5000));

        HANDLE process = nullptr;
        DuplicateHandle(GetCurrentProcess(), client.process, GetCurrentProcess(), &process, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0);
        CHECK(!client.Exited());
        // What Stop(true) sends: the hub answers and ends on its own
        CHECK(client.Request(Command::Shutdown, 5000));
        CHECK(ExitCode(process) == 0);
        CHECK(client.Exited());
        client.Stop(true);
        CloseHandle(process);
    }
}

TEST_CASE("upscaler: the hub ends with the plugin (job), and a hub of another protocol version doesn't answer")
{
    auto folders = FakeHubFolders();
    REQUIRE(!folders.empty());
    {
        Client client;
        REQUIRE(client.Start(folders[0], LUID{}, L"Local\\FusionFixUtilsTests.Job", "JobTest", L""));
        bool ok = false;
        REQUIRE(WaitStarted(client, ok));
        HANDLE process = nullptr;
        DuplicateHandle(GetCurrentProcess(), client.process, GetCurrentProcess(), &process, SYNCHRONIZE, FALSE, 0);
        // Not graceful (the game is being unloaded): closing the job ends the hub
        client.Stop(false);
        CHECK(WaitForSingleObject(process, 10000) == WAIT_OBJECT_0);
        CloseHandle(process);
    }

    for (auto& folder : folders)
    {
        HandMade plugin(L"Mismatch");
        plugin.shared->Version = Version + 1;
        HANDLE hub = StartFakeHub(folder, std::wstring(ArgumentName) + L" " + plugin.name);
        REQUIRE(hub != nullptr);
        CHECK(ExitCode(hub) == 10 + static_cast<DWORD>(Server::OpenResult::VersionMismatch));
        CHECK(WaitForSingleObject(plugin.response, 0) == WAIT_TIMEOUT);
        CloseHandle(hub);
    }
}

TEST_CASE("upscaler: the client finds the hub and the vendor runtimes in the game's folder")
{
    CHECK(GameRoot() == ExeFolder());
    auto game = ExeFolder() / L"upscaler-test-game";
    std::error_code ec;
    fs::remove_all(game, ec);
    fs::create_directories(game);
    auto touch = [&](const wchar_t* name) { std::ofstream(game / name) << "x"; };

    CHECK(!Present(game, true));
    touch(L"nvngx_dlss.dll");
    CHECK(RuntimePresent(game, Backend::DLSS));
    CHECK(!RuntimePresent(game, Backend::FSR));
    CHECK(!Present(game, true));
    touch(HubFileName);
    CHECK(Present(game, true));
    CHECK(!Present(game, false));
    touch(L"amd_fidelityfx_loader_dx12.dll");
    CHECK(!RuntimePresent(game, Backend::FSR));
    touch(L"amd_fidelityfx_upscaler_dx12.dll");
    CHECK(RuntimePresent(game, Backend::FSR));
    CHECK(Present(game, false));
    CHECK(HubPath(game) == game / L"FusionFixHub.exe");
    CHECK(RuntimeDirectory(game) == game);
    fs::remove_all(game, ec);
}
