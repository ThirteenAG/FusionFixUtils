// include/FileWatch.hpp: change notifications in a folder next to the test exe (added, modified, renamed, removed), a
// single watched file (how the fixes reload their .cfg) and a filter.

#include "../include/FileWatch.hpp"
#include "check.h"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace std::chrono_literals;

    struct Recorder
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<std::pair<std::wstring, filewatch::Event>> events;

        void operator()(const std::wstring& path, filewatch::Event event)
        {
            std::lock_guard lock(mutex);
            events.emplace_back(path, event);
            changed.notify_all();
        }

        // Waits until the event arrived (or the time is up)
        bool WaitFor(const std::wstring& path, filewatch::Event event, std::chrono::milliseconds timeout = 5000ms)
        {
            std::unique_lock lock(mutex);
            return changed.wait_for(lock, timeout, [&]
            {
                for (auto& e : events)
                    if (e.first == path && e.second == event)
                        return true;
                return false;
            });
        }

        bool Seen(const std::wstring& path)
        {
            std::lock_guard lock(mutex);
            for (auto& e : events)
                if (e.first == path)
                    return true;
            return false;
        }
    };

    // A fresh folder next to the exe (never the system's temp folder)
    fs::path Folder(const wchar_t* name)
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        auto folder = fs::path(exe).parent_path() / L"filewatch-test" / name;
        std::error_code ec;
        fs::remove_all(folder, ec);
        fs::create_directories(folder);
        return folder;
    }

    void WriteFile(const fs::path& path, const char* text)
    {
        std::ofstream(path, std::ios::binary | std::ios::app) << text;
    }
}

TEST_CASE("filewatch: a folder's added, modified, renamed and removed files")
{
    auto folder = Folder(L"folder");
    Recorder recorder;
    {
        filewatch::FileWatch<std::wstring> watch(folder.wstring(), [&](const std::wstring& path, const filewatch::Event event) { recorder(path, event); });
        std::this_thread::sleep_for(100ms);

        WriteFile(folder / L"a.cfg", "x");
        CHECK(recorder.WaitFor(L"a.cfg", filewatch::Event::added));
        WriteFile(folder / L"a.cfg", "y");
        CHECK(recorder.WaitFor(L"a.cfg", filewatch::Event::modified));
        fs::rename(folder / L"a.cfg", folder / L"b.cfg");
        CHECK(recorder.WaitFor(L"a.cfg", filewatch::Event::renamed_old));
        CHECK(recorder.WaitFor(L"b.cfg", filewatch::Event::renamed_new));
        fs::remove(folder / L"b.cfg");
        CHECK(recorder.WaitFor(L"b.cfg", filewatch::Event::removed));
    }
    // The watch is gone with its object
    size_t count = 0;
    {
        std::lock_guard lock(recorder.mutex);
        count = recorder.events.size();
    }
    WriteFile(folder / L"c.cfg", "z");
    std::this_thread::sleep_for(300ms);
    std::lock_guard lock(recorder.mutex);
    CHECK(recorder.events.size() == count);
}

TEST_CASE("filewatch: watching one file reports only that file")
{
    auto folder = Folder(L"single");
    auto cfg = folder / L"Plugin.cfg";
    WriteFile(cfg, "[MAIN]\r\n");
    Recorder recorder;
    filewatch::FileWatch<std::wstring> watch(cfg.wstring(), [&](const std::wstring& path, const filewatch::Event event) { recorder(path, event); });
    std::this_thread::sleep_for(100ms);

    WriteFile(folder / L"Other.cfg", "x");
    WriteFile(cfg, "A = 1\r\n");
    CHECK(recorder.WaitFor(L"Plugin.cfg", filewatch::Event::modified));
    std::this_thread::sleep_for(200ms);
    CHECK(!recorder.Seen(L"Other.cfg"));
}

TEST_CASE("filewatch: a filter limits the files reported")
{
    auto folder = Folder(L"filter");
    Recorder recorder;
    filewatch::FileWatch<std::wstring> watch(folder.wstring(), std::wregex(L".*\\.ini"), [&](const std::wstring& path, const filewatch::Event event) { recorder(path, event); });
    std::this_thread::sleep_for(100ms);

    WriteFile(folder / L"skip.txt", "x");
    WriteFile(folder / L"keep.ini", "x");
    CHECK(recorder.WaitFor(L"keep.ini", filewatch::Event::added));
    std::this_thread::sleep_for(200ms);
    CHECK(!recorder.Seen(L"skip.txt"));
}
