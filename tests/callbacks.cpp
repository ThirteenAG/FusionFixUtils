// include/callbacks.h: module load and unload callbacks (with the one-callback-per-module rule), deferred callbacks at
// GetSystemTimeAsFileTime (a nested call inside a callback passes through instead of locking up) and patterns.

#include "../include/callbacks.h"
#include "check.h"
#include <array>
#include <atomic>
#include <cstring>
#include <thread>

namespace
{
    // CallbackProbe.dll: a DLL next to the test exe that nothing loads until a test does
    constexpr const wchar_t* Probe = L"CallbackProbe.dll";

    FILETIME Now()
    {
        FILETIME time{};
        GetSystemTimeAsFileTime(&time);
        return time;
    }

    bool NonZero(const FILETIME& time)
    {
        return time.dwLowDateTime != 0 || time.dwHighDateTime != 0;
    }

    // The first bytes of GetSystemTimeAsFileTime as this module calls it: the hook patches them while it is installed
    std::array<uint8_t, 16> Prologue()
    {
        std::array<uint8_t, 16> bytes{};
        std::memcpy(bytes.data(), reinterpret_cast<const void*>(&GetSystemTimeAsFileTime), bytes.size());
        return bytes;
    }
}

TEST_CASE("callbacks: a callback without a module runs at once")
{
    int calls = 0;
    CallbackHandler::RegisterCallback([&] { ++calls; });
    CHECK(calls == 1);
    CallbackHandler::RegisterCallback(L"", [&] { ++calls; });
    CHECK(calls == 2);
}

TEST_CASE("callbacks: a callback for a loaded module runs at once")
{
    int calls = 0;
    CallbackHandler::RegisterCallback(L"kernel32.dll", [&] { ++calls; });
    CHECK(calls == 1);
    CallbackHandler::RegisterModuleLoadCallback(L"KERNEL32.DLL", [&] { ++calls; });
    CHECK(calls == 2);
}

TEST_CASE("callbacks: load and unload callbacks of a module, one per module, on every load")
{
    REQUIRE(GetModuleHandleW(Probe) == nullptr);

    static int loads = 0, secondLoads = 0, unloads = 0, anyLoads = 0, anyUnloads = 0;
    static HMODULE anyLoaded = nullptr;
    CallbackHandler::RegisterCallback(Probe, [] { ++loads; });
    // The one-callback-per-module rule: a second registration (any case) for a module not loaded yet is dropped
    CallbackHandler::RegisterCallback(L"CALLBACKPROBE.DLL", [] { ++secondLoads; });
    CallbackHandler::RegisterModuleUnloadCallback(L"callbackprobe.dll", [] { ++unloads; });
    CallbackHandler::RegisterAnyModuleLoadCallback([](HMODULE module)
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        if (std::wstring_view(path).ends_with(L"CallbackProbe.dll"))
        {
            ++anyLoads;
            anyLoaded = module;
        }
    });
    CallbackHandler::RegisterAnyModuleUnloadCallback([](std::wstring_view name)
    {
        if (_wcsicmp(std::wstring(name).c_str(), Probe) == 0)
            ++anyUnloads;
    });
    CHECK(loads == 0);

    HMODULE probe = LoadLibraryW(Probe);
    REQUIRE(probe != nullptr);
    CHECK(loads == 1);
    CHECK(secondLoads == 0);
    CHECK(anyLoads == 1);
    CHECK(anyLoaded == probe);

    FreeLibrary(probe);
    CHECK(GetModuleHandleW(Probe) == nullptr);
    CHECK(unloads == 1);
    CHECK(anyUnloads == 1);

    // Fires again when the module is loaded again (Max Payne's driver DLL is reloaded on video mode changes)
    probe = LoadLibraryW(Probe);
    REQUIRE(probe != nullptr);
    CHECK(loads == 2);
    CHECK(secondLoads == 0);
    FreeLibrary(probe);
    CHECK(unloads == 2);
}

TEST_CASE("callbacks: module names outside ASCII don't break the lookup")
{
    // Wide characters above 255 in module names (the narrow tolower asserts on them in debug CRTs)
    static int calls = 0;
    CallbackHandler::RegisterCallback(L"ÄÖÜЖ.dll", [] { ++calls; });
    CallbackHandler::RegisterCallback(L"äöüж.dll", [] { ++calls; });
    HMODULE probe = LoadLibraryW(Probe);
    CHECK(probe != nullptr);
    if (probe)
        FreeLibrary(probe);
    CHECK(calls == 0);
}

TEST_CASE("callbacks: deferred callbacks run at the next GetSystemTimeAsFileTime, in order, then the hook goes")
{
    auto original = Prologue();
    std::vector<int> order;
    CallbackHandler::RegisterCallbackAtGetSystemTimeAsFileTime([&] { order.push_back(1); });
    CallbackHandler::RegisterCallbackAtGetSystemTimeAsFileTime([&] { order.push_back(2); });
    CallbackHandler::RegisterCallbackAtGetSystemTimeAsFileTime([&] { order.push_back(3); });
    CHECK(order.empty());
    CHECK(Prologue() != original);

    auto time = Now();
    CHECK(NonZero(time));
    CHECK((order == std::vector<int>{ 1, 2, 3 }));

    // All ran: the hook is removed, the function is as it was
    CHECK(Prologue() == original);
    Now();
    CHECK(order.size() == 3);
}

TEST_CASE("callbacks: GetSystemTimeAsFileTime inside a deferred callback passes through (RE fix)")
{
    // Initialization (and the CRT's mutex implementation) query the time. The nested call must not lock the callback
    // mutex again on the same thread or run the hook's dispatch inside itself.
    int calls = 0;
    bool nestedOk = false;
    CallbackHandler::RegisterCallbackAtGetSystemTimeAsFileTime([&]
    {
        ++calls;
        nestedOk = NonZero(Now());
        Now();
    });
    Now();
    CHECK(calls == 1);
    CHECK(nestedOk);
    Now();
    CHECK(calls == 1);
}

TEST_CASE("callbacks: a deferred callback can register another one, which runs at a later call")
{
    auto original = Prologue();
    int first = 0, second = 0;
    CallbackHandler::RegisterCallbackAtGetSystemTimeAsFileTime([&]
    {
        ++first;
        CallbackHandler::RegisterCallbackAtGetSystemTimeAsFileTime([&] { ++second; });
    });
    Now();
    CHECK(first == 1);
    CHECK(second == 0);
    // The second one isn't done: the hook stays
    CHECK(Prologue() != original);
    Now();
    CHECK(first == 1);
    CHECK(second == 1);
    CHECK(Prologue() == original);
}

TEST_CASE("callbacks: a pattern callback runs at once when found, else at the first call that finds it")
{
    static volatile uint8_t buffer[64] = {};
    auto begin = reinterpret_cast<uintptr_t>(&buffer[0]);
    auto end = begin + sizeof(buffer);

    int calls = 0;
    // Nothing matches yet
    CallbackHandler::RegisterCallbackAtGetSystemTimeAsFileTime([&] { ++calls; }, hook::range_pattern(begin, end, "4D 3C 2B 1A ? 5E"));
    CHECK(calls == 0);
    Now();
    CHECK(calls == 0);

    const uint8_t bytes[] = { 0x4D, 0x3C, 0x2B, 0x1A, 0x99, 0x5E };
    for (size_t i = 0; i < sizeof(bytes); ++i)
        buffer[16 + i] = bytes[i];
    Now();
    CHECK(calls == 1);
    Now();
    CHECK(calls == 1);

    // Found at registration: runs at once, no hook
    auto original = Prologue();
    CallbackHandler::RegisterCallbackAtGetSystemTimeAsFileTime([&] { ++calls; }, hook::range_pattern(begin, end, "4D 3C 2B 1A"));
    CHECK(calls == 2);
    CHECK(Prologue() == original);
}

TEST_CASE("callbacks: deferred callbacks registered from several threads all run once")
{
    std::atomic<int> calls = 0;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&]
        {
            for (int i = 0; i < 25; ++i)
                CallbackHandler::RegisterCallbackAtGetSystemTimeAsFileTime([&] { ++calls; });
        });
    for (auto& thread : threads)
        thread.join();
    Now();
    CHECK(calls == 100);
    Now();
    CHECK(calls == 100);
}

// Runs in a child process (FusionFixUtilsTests.exe --callbacks-exit): callbacks registered, a DLL loaded and unloaded,
// then the process ends normally
int CallbacksExitChild()
{
    static int unloads = 0;
    CallbackHandler::RegisterAnyModuleUnloadCallback([](std::wstring_view) { ++unloads; });
    CallbackHandler::RegisterModuleUnloadCallback(Probe, [] { ++unloads; });
    CallbackHandler::RegisterAnyModuleLoadCallback([](HMODULE) {});
    if (auto probe = LoadLibraryW(Probe))
        FreeLibrary(probe);
    return unloads == 2 ? 0 : 1;
}

TEST_CASE("callbacks: a process with DLL notifications ends cleanly (no calls into destroyed lists at exit)")
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring commandLine = L"\"" + std::wstring(exe) + L"\" --callbacks-exit";
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION info{};
    REQUIRE(CreateProcessW(exe, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info));
    CloseHandle(info.hThread);
    CHECK(WaitForSingleObject(info.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD code = 0xFFFFFFFF;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    CHECK(code == 0);
}
