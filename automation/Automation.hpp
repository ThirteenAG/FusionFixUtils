#pragma once

// The core of the dev-only automation plugin: a game is driven from inside its process without OS input, scenes are
// bookmarked and restored, settings are swept and each value's frame is grabbed before and after the UI (for setting
// previews). Never shipped, and inert unless enabled: Start() does nothing without automation.enable next to the
// plugin or ASI_AUTOMATION=1 in the environment. Header-only; needs asi_settings/ next to this folder.
//
//     struct MyGame : asi_automation::GameAdapter { ... };      // the per-game part
//     asi_automation::Core::Start(new MyGame);                 // from InitializeASI / DllMain; inert unless enabled
//     // the adapter's game-thread tick hook calls asi_automation::Core::Tick() once per game frame
//
// Channel: \\.\pipe\AsiAutomation.<process id>, JSON lines in the live link's envelope (asi_settings/SPEC.md, Live
// link): {"cmd":...,"id":...} -> {"id":...,"ok":true,...} or {"id":...,"ok":false,"error":...}, plus {"event":...}.
// Commands: hello, plugins, state, subscribe, settings.list/get/set/choices/state/invoke, journal.begin/restore/state,
// bookmark, scene.apply, freeze, ui, camera, capture, sweep, action, wait, quit, adapter.<name>. Commands that touch
// the game run on the game thread, in order, and reply when done (scene.apply, capture and sweep can take seconds).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "../asi_settings/asi_settings.hpp"
#include "asi_automation_frames.h"
#include "AutomationCapture.hpp"

#define ASI_AUTOMATION_PIPE        L"\\\\.\\pipe\\AsiAutomation."
#define ASI_AUTOMATION_MUTEX       L"Local\\AsiAutomation."
#define ASI_AUTOMATION_PROTOCOL    1
#define ASI_AUTOMATION_MARKER      L"automation.enable"
#define ASI_AUTOMATION_ENV         L"ASI_AUTOMATION"

namespace asi_automation
{
    using Json = asi_settings::json::Value;

    // ---------------------------------------------------------------------------------------------------------------
    // JSON text

    inline std::string Quote(std::string_view s) { return asi_settings::detail::JsonQuote(s); }

    inline std::string Number(double v)
    {
        if (!std::isfinite(v))
            return "null";
        if (std::floor(v) == v && std::fabs(v) < 1e15)
            return std::to_string(int64_t(v));
        char text[40];
        snprintf(text, sizeof(text), "%.6g", v);
        return text;
    }

    // A parsed value as JSON text again
    inline std::string Dump(const Json& v)
    {
        switch (v.kind)
        {
            case Json::Kind::Null: return "null";
            case Json::Kind::Bool: return v.boolean ? "true" : "false";
            case Json::Kind::Number: return Number(v.number);
            case Json::Kind::String: return Quote(v.string);
            case Json::Kind::Array:
            {
                std::string out = "[";
                for (size_t i = 0; i < v.items.size(); ++i)
                    out += (i ? "," : "") + Dump(v.items[i]);
                return out + "]";
            }
            case Json::Kind::Object:
            {
                std::string out = "{";
                for (size_t i = 0; i < v.members.size(); ++i)
                    out += (i ? "," : "") + Quote(v.members[i].first) + ":" + Dump(v.members[i].second);
                return out + "}";
            }
        }
        return "null";
    }

    inline std::optional<Json> Parse(std::string_view text)
    {
        Json v;
        if (!asi_settings::json::Parse(text, v))
            return std::nullopt;
        return v;
    }

    // A value as the settings file's text: strings as they are, numbers formatted, booleans 0/1
    inline std::string ValueText(const Json& v)
    {
        if (v.IsString()) return v.string;
        if (v.IsBool()) return v.boolean ? "1" : "0";
        if (v.IsNumber()) return asi_settings::text::FormatNumber(v.number);
        return {};
    }

    inline std::wstring Widen(std::string_view s) { return asi_settings::text::Widen(s); }
    inline std::string Narrow(std::wstring_view s) { return asi_settings::text::Narrow(s); }

    inline bool ReadText(const std::filesystem::path& path, std::string& out)
    {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f)
            return false;
        out.clear();
        char buffer[16384];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0)
            out.append(buffer, n);
        fclose(f);
        return true;
    }

    inline bool WriteText(const std::filesystem::path& path, std::string_view data)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        auto temp = path;
        temp += L".tmp";
        FILE* f = _wfopen(temp.c_str(), L"wb");
        if (!f)
            return false;
        bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
        ok = fclose(f) == 0 && ok;
        return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // What an adapter gives the core

    struct Vec3
    {
        float x = 0, y = 0, z = 0;
    };

    struct CameraPose
    {
        Vec3 pos, rot;      // rot: pitch, roll, yaw in degrees (RAGE order)
        float fov = 0;      // degrees, 0 = leave it
    };

    enum class Readiness { Ready, Loading, InMenu, Streaming, Unknown };

    inline const char* ReadinessName(Readiness r)
    {
        switch (r)
        {
            case Readiness::Ready: return "ready";
            case Readiness::Loading: return "loading";
            case Readiness::InMenu: return "menu";
            case Readiness::Streaming: return "streaming";
            default: return "unknown";
        }
    }

    struct Step
    {
        enum Kind { Running, Done, Failed } kind = Running;
        std::string reason;
        static Step Run() { return {}; }
        static Step Ok() { return { Done, {} }; }
        static Step Fail(std::string why) { return { Failed, std::move(why) }; }
    };

    inline std::string PoseJson(const CameraPose& p)
    {
        return "{\"pos\":[" + Number(p.pos.x) + "," + Number(p.pos.y) + "," + Number(p.pos.z) + "],\"rot\":[" + Number(p.rot.x) + "," +
            Number(p.rot.y) + "," + Number(p.rot.z) + "],\"fov\":" + Number(p.fov) + "}";
    }

    inline bool ReadVec3(const Json* v, Vec3& out)
    {
        if (!v || !v->IsArray() || v->items.size() < 3)
            return false;
        out = { float(v->items[0].number), float(v->items[1].number), float(v->items[2].number) };
        return true;
    }

    inline bool ReadPose(const Json& v, CameraPose& out)
    {
        if (!ReadVec3(v.Find("pos"), out.pos))
            return false;
        ReadVec3(v.Find("rot"), out.rot);
        out.fov = float(v.Num("fov", 0.0));
        return true;
    }

    // The per-game part. Everything but GameId, Caps and InstallHooks is called on the game thread only (from Tick).
    struct GameAdapter
    {
        virtual ~GameAdapter() = default;
        virtual const char* GameId() = 0;                               // "gtaiv", "mp3", "tcnyc"
        virtual std::string Caps() = 0;                                 // JSON object: what ReadScene/ApplyScene support
        virtual bool InstallHooks() = 0;                                // the game-thread tick (calls Core::Tick)
        virtual std::string ReadScene() = 0;                            // JSON members (no braces) for a bookmark
        virtual Step ApplyScene(const Json& scene, bool first) = 0;     // state machine, called until Done/Failed
        virtual void Freeze(bool on) = 0;                               // world stops, rendering continues
        virtual void SetCamera(const CameraPose* pose) = 0;             // nullptr: back to the game's camera
        virtual void HideUI(bool hidden) = 0;                           // when it changes; OnTick re-applies it
        virtual Readiness IsReady(std::string& detail) = 0;
        virtual bool InjectAction(std::string_view) { return false; }   // up/down/left/right/accept/back
        virtual void OnTick() {}                                        // every tick, before the commands
        virtual std::string State() { return {}; }                      // extra JSON members for the state reply
        // adapter.<name>: game-specific commands; return the reply's members, or set error
        virtual std::string Call(std::string_view name, const Json& request, std::string& error)
        {
            error = "unknown adapter command: " + std::string(name);
            return {};
        }
        virtual void Quit() { TerminateProcess(GetCurrentProcess(), 0); }
    };

    // ---------------------------------------------------------------------------------------------------------------
    // The core

    class Core
    {
    public:
        // Starts serving when enabled (marker file next to the plugin or ASI_AUTOMATION=1); otherwise does nothing and
        // returns false. Takes the adapter. Can be called from DllMain: the pipe is made on the pipe thread.
        static bool Start(GameAdapter* adapter)
        {
            auto& c = Get();
            if (c.adapter)
                return true;
            if (!Enabled())
            {
                delete adapter;
                return false;
            }
            wchar_t name[64];
            swprintf_s(name, L"%s%lu", ASI_AUTOMATION_MUTEX, GetCurrentProcessId());
            HANDLE mutex = CreateMutexW(nullptr, FALSE, name);
            if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
            {
                if (mutex)
                    CloseHandle(mutex);
                delete adapter;
                return false;
            }
            c.adapter = adapter;
            c.writer = new ImageWriter();   // never destroyed: its thread is gone at process exit anyway
            c.outgoing = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            c.journalPending = std::filesystem::exists(c.JournalPath());
            c.Log("started in " + Narrow(ModulePath().wstring()) + (c.journalPending ? ", a journal of a previous session is restored on the first tick" : ""));
            if (!adapter->InstallHooks())
                c.Log("the adapter's hooks failed; game-thread commands won't run");
            CreateThread(nullptr, 0, &PipeThread, &c, 0, nullptr);
            return true;
        }

        static bool Running() { return Get().adapter != nullptr; }

        // For adapters: frames of a stage so far, game ticks so far, whether a fix reports frame stages
        uint64_t StageFrames(int stage) const { return stage >= 0 && stage < ASI_AUTOMATION_STAGE_COUNT ? frames[stage].load() : 0; }
        uint64_t TickCount() const { return ticks.load(); }
        bool FrameStages() const { return framesRegistered; }

        // The plugin file and its folder
        static std::filesystem::path ModulePath()
        {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Get), &self);
            wchar_t path[MAX_PATH * 4] = {};
            GetModuleFileNameW(self, path, DWORD(std::size(path)));
            return path;
        }

        // automation\ next to the plugin: journal, default scene and capture folder, log
        static std::filesystem::path DataFolder() { return ModulePath().parent_path() / L"automation"; }

        static bool Enabled()
        {
            wchar_t env[8] = {};
            if (GetEnvironmentVariableW(ASI_AUTOMATION_ENV, env, DWORD(std::size(env))) && env[0] == L'1')
                return true;
            std::error_code ec;
            return std::filesystem::exists(ModulePath().parent_path() / ASI_AUTOMATION_MARKER, ec);
        }

        // From the adapter's tick hook, once per game frame, on the game thread
        static void Tick()
        {
            auto& c = Get();
            if (!c.adapter)
                return;
            c.ticks++;
            c.gameThread = GetCurrentThreadId();
            c.RegisterFrames();
            if (c.journalPending && c.ticks > 30)
            {
                c.journalPending = false;
                std::string report;
                c.RestoreJournal(report);
                c.Log("journal of a previous session restored: " + report);
            }
            c.adapter->OnTick();
            c.Hotkeys();
            c.RunCommands();
            c.UpdateState();
        }

        void Log(const std::string& line)
        {
            std::lock_guard guard(logLock);
            auto path = DataFolder() / L"automation.log";
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            if (FILE* f = _wfopen(path.c_str(), L"ab"))
            {
                SYSTEMTIME t;
                GetLocalTime(&t);
                fprintf(f, "%02d:%02d:%02d.%03d %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line.c_str());
                fclose(f);
            }
        }

        static Core& Get()
        {
            static Core core;
            return core;
        }

        // ---------------------------------------------------------------------------------------------------------------
    private:
        struct Client
        {
            uint64_t id = 0;
            HANDLE pipe = INVALID_HANDLE_VALUE;
            HANDLE readEvent = nullptr, writeEvent = nullptr;
            OVERLAPPED read{};
            char buffer[4096];
            std::string line;
            bool subscribed = false;
        };

        struct Request
        {
            uint64_t client = 0;
            std::string id;     // JSON text of the request's id
            std::string cmd;
            Json body;
        };

        // A command that runs over several ticks; Step returns true when it replied
        struct Job
        {
            Request request;
            uint64_t startTick = 0;
            ULONGLONG startTime = 0;
            int phase = 0;
            virtual ~Job() = default;
            virtual bool Step(Core& c) = 0;
        };

        GameAdapter* adapter = nullptr;
        ImageWriter* writer = nullptr;
        HANDLE outgoing = nullptr;
        std::mutex logLock;

        // Pipe thread <-> game thread
        std::mutex lock;
        std::deque<Request> incoming;
        std::deque<std::pair<uint64_t, std::string>> outbox;   // client 0: every subscribed client
        std::atomic<uint64_t> ticks = 0;
        std::atomic<DWORD> gameThread = 0;
        std::string stateJson = "{}";                           // the last state, for the pipe thread
        bool journalPending = false;

        // Game thread
        std::deque<std::unique_ptr<Job>> jobs;
        bool frozen = false, uiHidden = false, scriptCamera = false;
        bool framesRegistered = false;
        uint64_t nextFrameLookup = 0;
        bool hotkeyDown = false;

        // Render thread
        std::atomic<uint64_t> frames[ASI_AUTOMATION_STAGE_COUNT] = {};
        struct Grab
        {
            std::mutex lock;
            std::vector<std::filesystem::path> paths[ASI_AUTOMATION_STAGE_COUNT];
            size_t next[ASI_AUTOMATION_STAGE_COUNT] = {};
            uint64_t from[ASI_AUTOMATION_STAGE_COUNT] = {};
            std::atomic<bool> active = false;
            std::atomic<int> pending = 0;                       // queued for the writer and not written yet
            std::vector<std::string> results;                   // JSON objects of written frames
            std::vector<std::string> errors;
        } grab;

        // ---------------------------------------------------------------------------------------------------------------
        // Frames

        static void __cdecl OnFrame(int stage, void* device, void*)
        {
            auto& c = Get();
            uint64_t frame = ++c.frames[stage];
            if (!c.grab.active.load(std::memory_order_acquire))
                return;
            std::filesystem::path path;
            {
                std::lock_guard guard(c.grab.lock);
                auto& list = c.grab.paths[stage];
                if (c.grab.next[stage] >= list.size() || frame <= c.grab.from[stage])
                    return;
                path = list[c.grab.next[stage]++];
                c.grab.pending++;
            }
            Image image;
            std::string error;
            if (!asi_automation::Grab(device, image, error))
            {
                std::lock_guard guard(c.grab.lock);
                c.grab.errors.push_back(error);
                c.grab.pending--;
                return;
            }
            const char* stageName = stage == ASI_AUTOMATION_STAGE_BEFORE_UI ? "beforeUI" : "afterUI";
            c.writer->Write(std::move(image), path, [stageName, frame](const ImageWriter::Done& done)
            {
                auto& c = Get();
                std::lock_guard guard(c.grab.lock);
                if (done.ok)
                    c.grab.results.push_back("{\"file\":" + Quote(Narrow(done.path.wstring())) + ",\"stage\":\"" + stageName + "\",\"frame\":" +
                        std::to_string(frame) + ",\"width\":" + std::to_string(done.width) + ",\"height\":" + std::to_string(done.height) + "}");
                else
                    c.grab.errors.push_back(done.error);
                c.grab.pending--;
            });
        }

        // The fixes' AsiAutomation_SetFrameCallback, looked for until one is found (the fix may load after this plugin)
        void RegisterFrames()
        {
            if (framesRegistered || ticks < nextFrameLookup)
                return;
            nextFrameLookup = ticks + 120;
            HMODULE modules[1024];
            DWORD needed = 0;
            if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
                return;
            for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < std::size(modules); ++i)
            {
                auto set = reinterpret_cast<AsiAutomationSetFrameCallbackFn>(GetProcAddress(modules[i], ASI_AUTOMATION_FRAMES_EXPORT_NAME));
                if (!set)
                    continue;
                set(ASI_AUTOMATION_STAGE_BEFORE_UI, &OnFrame, nullptr);
                set(ASI_AUTOMATION_STAGE_AFTER_UI, &OnFrame, nullptr);
                framesRegistered = true;
                wchar_t path[MAX_PATH] = {};
                GetModuleFileNameW(modules[i], path, MAX_PATH);
                Log("frame stages from " + Narrow(path));
                return;
            }
        }

        // Frames drawn so far: Present-time frames when the fix reports them, otherwise game ticks
        uint64_t FrameCount() const { return framesRegistered ? frames[ASI_AUTOMATION_STAGE_AFTER_UI].load() : ticks.load(); }
        // Frames of the 3D scene: loading screens and movies present without drawing it
        uint64_t SceneFrameCount() const { return framesRegistered ? frames[ASI_AUTOMATION_STAGE_BEFORE_UI].load() : ticks.load(); }

        static int StageOf(std::string_view name)
        {
            if (asi_settings::text::IEquals(name, "beforeUI")) return ASI_AUTOMATION_STAGE_BEFORE_UI;
            if (asi_settings::text::IEquals(name, "afterUI")) return ASI_AUTOMATION_STAGE_AFTER_UI;
            return -1;
        }

        // Starts grabbing: per stage, the paths of the next frames
        bool BeginGrab(std::vector<std::filesystem::path> paths[ASI_AUTOMATION_STAGE_COUNT], std::string& error)
        {
            if (!framesRegistered)
            {
                error = "no fix with frame stages (AsiAutomation_SetFrameCallback) in the process";
                return false;
            }
            std::lock_guard guard(grab.lock);
            for (int s = 0; s < ASI_AUTOMATION_STAGE_COUNT; ++s)
            {
                grab.paths[s] = std::move(paths[s]);
                grab.next[s] = 0;
                // The next frame, not one already being drawn
                grab.from[s] = frames[s].load() + 1;
            }
            grab.results.clear();
            grab.errors.clear();
            grab.active = true;
            return true;
        }

        // True when every frame is written (or failed)
        bool GrabDone()
        {
            std::lock_guard guard(grab.lock);
            for (int s = 0; s < ASI_AUTOMATION_STAGE_COUNT; ++s)
            {
                if (grab.next[s] < grab.paths[s].size())
                    return false;
            }
            if (grab.pending > 0)
                return false;
            grab.active = false;
            return true;
        }

        std::string GrabResults(bool& ok)
        {
            std::lock_guard guard(grab.lock);
            std::string out = "\"frames\":[";
            for (size_t i = 0; i < grab.results.size(); ++i)
                out += (i ? "," : "") + grab.results[i];
            out += "]";
            ok = grab.errors.empty();
            if (!ok)
            {
                out += ",\"errors\":[";
                for (size_t i = 0; i < grab.errors.size(); ++i)
                    out += (i ? "," : "") + Quote(grab.errors[i]);
                out += "]";
            }
            return out;
        }

        void CancelGrab()
        {
            std::lock_guard guard(grab.lock);
            grab.active = false;
            for (auto& p : grab.paths)
                p.clear();
        }

        // ---------------------------------------------------------------------------------------------------------------
        // Settings: the plugins of the process through AsiSettings_GetApi, as the live link does

        struct PluginRef
        {
            HMODULE module = nullptr;
            const AsiSettingsApi* api = nullptr;
            asi_settings::DescribedFile file;
            std::string version;
        };

        std::mutex pluginsLock;
        std::vector<std::shared_ptr<PluginRef>> plugins;

        void RefreshPlugins()
        {
            HMODULE modules[1024];
            DWORD needed = 0;
            if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
                return;
            std::lock_guard guard(pluginsLock);
            for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < std::size(modules); ++i)
            {
                if (std::any_of(plugins.begin(), plugins.end(), [&](auto& p) { return p->module == modules[i]; }))
                    continue;
                auto getApi = reinterpret_cast<AsiSettingsGetApi>(GetProcAddress(modules[i], ASI_SETTINGS_EXPORT_NAME));
                if (!getApi)
                    continue;
                auto p = std::make_shared<PluginRef>();
                p->module = modules[i];
                p->api = getApi(ASI_SETTINGS_VERSION);
                if (!p->api || p->api->size < offsetof(AsiSettingsApi, Set) + sizeof(void*) || !asi_settings::ReadModule(modules[i], p->file))
                    continue;
                if (p->api->pluginId && *p->api->pluginId)
                    p->file.id = p->api->pluginId;
                p->version = asi_settings::detail::FileVersion(modules[i]);
                plugins.push_back(std::move(p));
            }
        }

        std::shared_ptr<PluginRef> FindPlugin(std::string_view id)
        {
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                {
                    std::lock_guard guard(pluginsLock);
                    for (auto& p : plugins)
                    {
                        if (asi_settings::text::IEquals(p->file.id, id))
                            return p;
                    }
                }
                RefreshPlugins();
            }
            return nullptr;
        }

        std::vector<std::shared_ptr<PluginRef>> AllPlugins()
        {
            RefreshPlugins();
            std::lock_guard guard(pluginsLock);
            return plugins;
        }

        static int32_t GetValue(const PluginRef& p, const std::string& id, std::string& value)
        {
            return asi_settings::detail::CallText([&](char* b, uint32_t n) { return p.api->Get(id.c_str(), b, n); }, value);
        }

        static int32_t GetStateOf(const PluginRef& p, const std::string& id, std::string& reason)
        {
            reason.clear();
            if (p.api->size < offsetof(AsiSettingsApi, GetState) + sizeof(void*) || !p.api->GetState)
                return 0;
            char text[1024] = {};
            int32_t flags = p.api->GetState(id.c_str(), text, uint32_t(sizeof(text)));
            if (flags >= 0)
                reason = text;
            return flags;
        }

        static std::string ValueFields(const PluginRef& p, const std::string& id)
        {
            std::string value, reason;
            int32_t got = GetValue(p, id, value);
            int32_t state = GetStateOf(p, id, reason);
            std::string out = got >= 0 ? "\"value\":" + Quote(value) : "\"value\":null";
            out += ",\"state\":" + std::to_string(state < 0 ? 0 : state);
            if (!reason.empty())
                out += ",\"reason\":" + Quote(reason);
            return out;
        }

        static std::string PluginJson(const PluginRef& p)
        {
            asi_settings::Texts english = p.file.texts;
            english.Select("en");
            wchar_t path[MAX_PATH * 4] = {};
            GetModuleFileNameW(p.module, path, DWORD(std::size(path)));
            return "{\"id\":" + Quote(p.file.id) + ",\"name\":" + Quote(english.Resolve(p.file.description.name)) + ",\"version\":" + Quote(p.version) +
                ",\"path\":" + Quote(Narrow(path)) + ",\"interface\":" + std::to_string(p.api->version) + "}";
        }

        // The values a sweep of a setting goes through when the request names none: bool 0/1, the choices usable now,
        // numbers min..max by step (at most 64, evenly spaced)
        std::vector<std::string> DefaultValues(const PluginRef& p, const asi_settings::Setting& s)
        {
            using T = asi_settings::Type;
            std::vector<std::string> values;
            if (s.type == T::Bool)
                return { "0", "1" };
            if (s.type == T::Choice)
            {
                std::string choices;
                if (p.api->size >= offsetof(AsiSettingsApi, GetChoices) + sizeof(void*) && p.api->GetChoices &&
                    asi_settings::detail::CallText([&](char* b, uint32_t n) { return p.api->GetChoices(s.id.c_str(), b, n); }, choices) >= 0)
                {
                    if (auto parsed = Parse(choices); parsed && parsed->IsArray())
                    {
                        for (auto& item : parsed->items)
                        {
                            auto v = item.IsObject() ? item.Find("value") : &item;
                            if (v)
                                values.push_back(ValueText(*v));
                        }
                    }
                }
                if (values.empty())
                {
                    for (auto& c : s.choices)
                        values.push_back(ValueText(c.value));
                }
                return values;
            }
            if ((s.type == T::Int || s.type == T::Float) && s.min && s.max && *s.max >= *s.min)
            {
                double step = s.step && *s.step > 0 ? *s.step : (s.type == T::Int ? 1.0 : (*s.max - *s.min) / 16.0);
                double count = std::floor((*s.max - *s.min) / step + 1e-9) + 1;
                if (count > 64)
                {
                    step = (*s.max - *s.min) / 63.0;
                    count = 64;
                }
                for (int i = 0; i < int(count); ++i)
                {
                    double v = *s.min + step * i;
                    values.push_back(s.type == T::Int ? std::to_string(int64_t(std::llround(v))) : asi_settings::text::FormatNumber(v));
                }
            }
            return values;
        }

        // ---------------------------------------------------------------------------------------------------------------
        // Journal

        std::filesystem::path JournalPath() const { return DataFolder() / L"journal.json"; }

        bool JournalExists() const
        {
            std::error_code ec;
            return std::filesystem::exists(JournalPath(), ec);
        }

        // Before the first change through the channel: every value of every plugin and a copy of their settings files,
        // unless a journal exists already (one left by a crash is restored on an early tick of the next start)
        bool BeginJournal(std::string& error)
        {
            if (JournalExists())
                return true;
            auto folder = DataFolder() / L"journal";
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            std::string out = "{\"format\":1,\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"plugins\":[";
            int fileIndex = 0;
            bool firstPlugin = true;
            for (auto& p : AllPlugins())
            {
                out += std::string(firstPlugin ? "" : ",") + "{\"id\":" + Quote(p->file.id) + ",\"values\":{";
                firstPlugin = false;
                bool first = true;
                for (auto& s : p->file.description.settings)
                {
                    if (!s.HasValue())
                        continue;
                    std::string value;
                    if (GetValue(*p, s.id, value) < 0)
                        continue;
                    out += std::string(first ? "" : ",") + Quote(s.id) + ":" + Quote(value);
                    first = false;
                }
                out += "},\"files\":[";
                wchar_t path[MAX_PATH * 4] = {};
                GetModuleFileNameW(p->module, path, DWORD(std::size(path)));
                wchar_t exe[MAX_PATH * 4] = {};
                GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
                auto storage = asi_settings::ResolveStorageFiles(p->file.description, { path, exe });
                first = true;
                for (auto& file : { storage.file, storage.fallback, storage.virtualStore })
                {
                    if (file.empty() || !std::filesystem::is_regular_file(file, ec))
                        continue;
                    auto backup = folder / (std::to_wstring(fileIndex++) + L".cfg");
                    if (!CopyFileW(file.c_str(), backup.c_str(), FALSE))
                    {
                        error = "couldn't copy " + Narrow(file.wstring());
                        return false;
                    }
                    out += std::string(first ? "" : ",") + "{\"path\":" + Quote(Narrow(file.wstring())) + ",\"backup\":" + Quote(Narrow(backup.wstring())) + "}";
                    first = false;
                }
                out += "]}";
            }
            out += "]}";
            if (!WriteText(JournalPath(), out))
            {
                error = "couldn't write the journal";
                return false;
            }
            Log("journal written");
            return true;
        }

        // Values back through Set, then the files back byte for byte; the journal is deleted when that worked
        bool RestoreJournal(std::string& report)
        {
            std::string text;
            if (!ReadText(JournalPath(), text))
            {
                report = "no journal";
                return true;
            }
            auto journal = Parse(text);
            if (!journal || !journal->IsObject())
            {
                report = "the journal can't be read";
                return false;
            }
            int set = 0, failed = 0, files = 0;
            bool ok = true;
            if (auto list = journal->Find("plugins"); list && list->IsArray())
            {
                for (auto& entry : list->items)
                {
                    auto p = FindPlugin(entry.Str("id"));
                    if (p)
                    {
                        if (auto values = entry.Find("values"); values && values->IsObject())
                        {
                            for (auto& [id, v] : values->members)
                            {
                                std::string current;
                                if (GetValue(*p, id, current) >= 0 && current == v.string)
                                    continue;
                                if (p->api->Set(id.c_str(), v.string.c_str()) >= 0)
                                    set++;
                                else
                                    failed++;
                            }
                        }
                    }
                    if (auto list2 = entry.Find("files"); list2 && list2->IsArray())
                    {
                        for (auto& f : list2->items)
                        {
                            auto path = std::filesystem::path(Widen(f.Str("path"))), backup = std::filesystem::path(Widen(f.Str("backup")));
                            if (CopyFileW(backup.c_str(), path.c_str(), FALSE))
                                files++;
                            else
                                ok = false;
                        }
                    }
                }
            }
            report = std::to_string(set) + " values set back, " + std::to_string(failed) + " refused, " + std::to_string(files) + " files restored";
            if (ok)
            {
                std::error_code ec;
                std::filesystem::remove(JournalPath(), ec);
                std::filesystem::remove_all(DataFolder() / L"journal", ec);
            }
            Log("journal restored: " + report);
            return ok;
        }

        // ---------------------------------------------------------------------------------------------------------------
        // Pipe (same shape as asi_settings::LiveLink)

        static constexpr size_t MaxClients = 4;
        static constexpr size_t MaxLine = 256 * 1024;
        std::vector<std::unique_ptr<Client>> clients;
        uint64_t nextClient = 1;

        static DWORD WINAPI PipeThread(void* param)
        {
            try { static_cast<Core*>(param)->Serve(); }
            catch (...) {}
            return 0;
        }

        void Serve()
        {
            // Only the user the game runs as
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                return;
            DWORD size = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            std::vector<BYTE> user(size);
            bool ok = size && GetTokenInformation(token, TokenUser, user.data(), size, &size);
            CloseHandle(token);
            if (!ok)
                return;
            PSID sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
            DWORD aclSize = DWORD(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(sid));
            std::vector<BYTE> acl(aclSize);
            SECURITY_DESCRIPTOR descriptor{};
            SECURITY_ATTRIBUTES attributes{ sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE };
            if (!InitializeAcl(reinterpret_cast<PACL>(acl.data()), aclSize, ACL_REVISION) ||
                !AddAccessAllowedAce(reinterpret_cast<PACL>(acl.data()), ACL_REVISION, GENERIC_ALL, sid) ||
                !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
                !SetSecurityDescriptorDacl(&descriptor, TRUE, reinterpret_cast<PACL>(acl.data()), FALSE))
                return;

            wchar_t name[64];
            swprintf_s(name, L"%s%lu", ASI_AUTOMATION_PIPE, GetCurrentProcessId());
#ifndef PIPE_REJECT_REMOTE_CLIENTS
            constexpr DWORD PIPE_REJECT_REMOTE_CLIENTS = 0x00000008;
#endif
            HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            OVERLAPPED connect{};
            HANDLE listening = INVALID_HANDLE_VALUE;
            bool first = true;
            Log("serving " + Narrow(name));

            for (;;)
            {
                if (listening == INVALID_HANDLE_VALUE && clients.size() < MaxClients)
                {
                    listening = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
                        DWORD(MaxLine), DWORD(MaxLine), 0, &attributes);
                    if (listening == INVALID_HANDLE_VALUE && first)
                    {
                        Log("couldn't create the pipe");
                        return;
                    }
                    first = false;
                    if (listening != INVALID_HANDLE_VALUE)
                    {
                        connect = {};
                        connect.hEvent = connectEvent;
                        ResetEvent(connectEvent);
                        if (!ConnectNamedPipe(listening, &connect))
                        {
                            DWORD e = GetLastError();
                            if (e == ERROR_PIPE_CONNECTED)
                                SetEvent(connectEvent);
                            else if (e != ERROR_IO_PENDING)
                            {
                                CloseHandle(listening);
                                listening = INVALID_HANDLE_VALUE;
                            }
                        }
                    }
                }

                HANDLE handles[2 + MaxClients];
                DWORD count = 0;
                handles[count++] = outgoing;
                bool waitConnect = listening != INVALID_HANDLE_VALUE;
                if (waitConnect)
                    handles[count++] = connectEvent;
                DWORD clientsAt = count;
                for (auto& c : clients)
                    handles[count++] = c->readEvent;
                DWORD wait = WaitForMultipleObjects(count, handles, FALSE, 1000);
                if (wait == WAIT_TIMEOUT)
                    continue;
                if (wait == WAIT_OBJECT_0)
                {
                    Flush();
                    continue;
                }
                if (waitConnect && wait == WAIT_OBJECT_0 + 1)
                {
                    DWORD bytes = 0;
                    if (GetOverlappedResult(listening, &connect, &bytes, FALSE) || GetLastError() == ERROR_PIPE_CONNECTED)
                    {
                        auto c = std::make_unique<Client>();
                        c->id = nextClient++;
                        c->pipe = listening;
                        c->readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                        c->writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                        if (c->readEvent && c->writeEvent && StartRead(*c))
                            clients.push_back(std::move(c));
                        else
                            Close(*c);
                    }
                    else
                        CloseHandle(listening);
                    listening = INVALID_HANDLE_VALUE;
                    continue;
                }
                if (wait >= WAIT_OBJECT_0 + clientsAt && wait < WAIT_OBJECT_0 + count)
                {
                    size_t index = wait - WAIT_OBJECT_0 - clientsAt;
                    auto& c = *clients[index];
                    DWORD bytes = 0;
                    bool alive = GetOverlappedResult(c.pipe, &c.read, &bytes, FALSE) != FALSE;
                    if (alive)
                    {
                        c.line.append(c.buffer, bytes);
                        size_t start = 0, end;
                        while ((end = c.line.find('\n', start)) != std::string::npos)
                        {
                            std::string_view text(c.line.data() + start, end - start);
                            if (!text.empty() && text.back() == '\r')
                                text.remove_suffix(1);
                            if (!text.empty())
                                Receive(c, text);
                            start = end + 1;
                        }
                        c.line.erase(0, start);
                        alive = c.line.size() <= MaxLine && StartRead(c);
                    }
                    if (!alive)
                    {
                        Close(c);
                        clients.erase(clients.begin() + ptrdiff_t(index));
                    }
                    Flush();
                    continue;
                }
                Sleep(50);
            }
        }

        static bool StartRead(Client& c)
        {
            c.read = {};
            c.read.hEvent = c.readEvent;
            ResetEvent(c.readEvent);
            return ::ReadFile(c.pipe, c.buffer, DWORD(sizeof(c.buffer)), nullptr, &c.read) || GetLastError() == ERROR_IO_PENDING;
        }

        static bool Write(Client& c, const std::string& line)
        {
            std::string data = line + "\n";
            OVERLAPPED write{};
            write.hEvent = c.writeEvent;
            ResetEvent(c.writeEvent);
            if (!::WriteFile(c.pipe, data.data(), DWORD(data.size()), nullptr, &write) && GetLastError() != ERROR_IO_PENDING)
                return false;
            DWORD bytes = 0;
            if (WaitForSingleObject(c.writeEvent, 10000) != WAIT_OBJECT_0)
            {
                CancelIo(c.pipe);
                GetOverlappedResult(c.pipe, &write, &bytes, TRUE);
                return false;
            }
            return GetOverlappedResult(c.pipe, &write, &bytes, FALSE) && bytes == data.size();
        }

        static void Close(Client& c)
        {
            if (c.pipe != INVALID_HANDLE_VALUE)
            {
                CancelIo(c.pipe);
                DWORD bytes = 0;
                GetOverlappedResult(c.pipe, &c.read, &bytes, TRUE);
                DisconnectNamedPipe(c.pipe);
                CloseHandle(c.pipe);
                c.pipe = INVALID_HANDLE_VALUE;
            }
            if (c.readEvent) CloseHandle(c.readEvent);
            if (c.writeEvent) CloseHandle(c.writeEvent);
            c.readEvent = c.writeEvent = nullptr;
        }

        // Sends what the game thread queued
        void Flush()
        {
            std::deque<std::pair<uint64_t, std::string>> out;
            {
                std::lock_guard guard(lock);
                out.swap(outbox);
            }
            for (auto& [to, line] : out)
            {
                for (size_t i = 0; i < clients.size();)
                {
                    auto& c = *clients[i];
                    bool target = to ? c.id == to : c.subscribed;
                    if (target && !Write(c, line))
                    {
                        Close(c);
                        clients.erase(clients.begin() + ptrdiff_t(i));
                        continue;
                    }
                    ++i;
                }
            }
        }

        void Send(uint64_t client, std::string line)
        {
            {
                std::lock_guard guard(lock);
                outbox.emplace_back(client, std::move(line));
            }
            SetEvent(outgoing);
        }

        void Reply(const Request& r, bool ok, const std::string& fields)
        {
            Send(r.client, "{\"id\":" + r.id + ",\"ok\":" + (ok ? "true" : "false") + (fields.empty() ? "" : "," + fields) + "}");
        }

        void Fail(const Request& r, const std::string& error, int32_t result = 0)
        {
            Reply(r, false, (result < 0 ? "\"result\":" + std::to_string(result) + "," : std::string()) + "\"error\":" + Quote(error));
        }

        // An event to the client of a request
        void Event(const Request& r, const std::string& fields)
        {
            Send(r.client, "{" + fields + "}");
        }

        // ---------------------------------------------------------------------------------------------------------------
        // Requests (pipe thread)

        void Receive(Client& c, std::string_view line)
        {
            Request r;
            r.client = c.id;
            r.id = "null";
            auto parsed = Parse(line);
            if (!parsed || !parsed->IsObject())
            {
                Fail(r, "not a JSON object");
                return;
            }
            r.body = std::move(*parsed);
            if (auto v = r.body.Find("id"))
                r.id = v->IsNumber() ? asi_settings::text::FormatNumber(v->number) : v->IsString() ? Quote(v->string) : "null";
            r.cmd = r.body.Str("cmd");

            if (r.cmd == "hello")
            {
                wchar_t exe[MAX_PATH * 4] = {};
                GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
                std::string list = "[";
                for (auto& p : AllPlugins())
                    list += (list.size() > 1 ? "," : "") + PluginJson(*p);
                list += "]";
                Reply(r, true, "\"protocol\":" + std::to_string(ASI_AUTOMATION_PROTOCOL) + ",\"game\":" + Quote(adapter->GameId()) + ",\"pid\":" +
                    std::to_string(GetCurrentProcessId()) + ",\"exe\":" + Quote(Narrow(exe)) + ",\"exeVersion\":" + Quote(asi_settings::detail::FileVersion(nullptr)) +
                    ",\"automation\":" + Quote(Narrow(ModulePath().wstring())) + ",\"data\":" + Quote(Narrow(DataFolder().wstring())) +
                    ",\"frameStages\":" + (framesRegistered ? "true" : "false") + ",\"caps\":" + adapter->Caps() + ",\"plugins\":" + list);
                return;
            }
            if (r.cmd == "subscribe")
            {
                c.subscribed = r.body.Bool("on", true);
                Reply(r, true, {});
                return;
            }
            if (r.cmd == "plugins")
            {
                std::string list = "[";
                for (auto& p : AllPlugins())
                    list += (list.size() > 1 ? "," : "") + PluginJson(*p);
                Reply(r, true, "\"plugins\":" + list + "]");
                return;
            }
            if (r.cmd == "state")
            {
                std::string state;
                {
                    std::lock_guard guard(lock);
                    state = stateJson;
                }
                Reply(r, true, "\"ticks\":" + std::to_string(ticks.load()) + ",\"frames\":{\"beforeUI\":" + std::to_string(frames[0].load()) +
                    ",\"afterUI\":" + std::to_string(frames[1].load()) + "},\"journal\":" + (JournalExists() ? "true" : "false") + ",\"state\":" + state);
                return;
            }
            if (r.cmd == "journal.state")
            {
                Reply(r, true, "\"exists\":" + std::string(JournalExists() ? "true" : "false") + ",\"path\":" + Quote(Narrow(JournalPath().wstring())));
                return;
            }
            if (r.cmd == "journal.begin")
            {
                std::string error;
                if (BeginJournal(error))
                    Reply(r, true, "\"path\":" + Quote(Narrow(JournalPath().wstring())));
                else
                    Fail(r, error);
                return;
            }
            if (r.cmd == "journal.restore")
            {
                std::string report;
                bool ok = RestoreJournal(report);
                Reply(r, ok, "\"report\":" + Quote(report));
                return;
            }
            if (r.cmd.starts_with("settings."))
            {
                Settings(r);
                return;
            }
            // Everything else runs on the game thread
            {
                std::lock_guard guard(lock);
                incoming.push_back(std::move(r));
            }
        }

        void Settings(const Request& r)
        {
            auto p = FindPlugin(r.body.Str("plugin"));
            if (!p)
            {
                Fail(r, "no such plugin: " + std::string(r.body.Str("plugin")), ASI_SETTINGS_UNKNOWN_ID);
                return;
            }
            auto& d = p->file.description;
            if (r.cmd == "settings.baseline")
            {
                // Every setting the menu or an editor shows to a fixed value, so captures of one setting don't carry
                // the player's others: the preset's value where the setting takes part in presets ("vanilla",
                // "recommended", "enhanced"), else the description's default. Settings applied after a restart and
                // file-only ones are left alone; "except" lists ids to leave too. Through the journal like any change.
                std::string error;
                if (!BeginJournal(error))
                {
                    Fail(r, "journal: " + error);
                    return;
                }
                std::string presetName(r.body.Str("preset", "recommended"));
                int preset = presetName == "vanilla" ? 0 : presetName == "enhanced" ? 2 : presetName == "none" ? -1 : 1;
                std::vector<std::string> except;
                if (auto e = r.body.Find("except"); e && e->IsArray())
                {
                    for (auto& item : e->items)
                        except.push_back(item.string);
                }
                std::string values = "{", refused = "[";
                for (auto& s : d.settings)
                {
                    if (!s.HasValue() || s.apply == "restart" || s.visibility == "file" ||
                        std::any_of(except.begin(), except.end(), [&](auto& id) { return asi_settings::text::IEquals(id, s.id); }))
                        continue;
                    const Json& wanted = preset >= 0 && s.presets ? (*s.presets)[size_t(preset)] : s.defaultValue;
                    if (wanted.IsNull())
                        continue;
                    std::string text = ValueText(wanted);
                    int32_t code = p->api->Set(s.id.c_str(), text.c_str());
                    if (code >= 0)
                        values += (values.size() > 1 ? "," : "") + Quote(s.id) + ":" + Quote(text);
                    else
                        refused += (refused.size() > 1 ? "," : "") + std::string("{\"setting\":") + Quote(s.id) + ",\"result\":" + std::to_string(code) + "}";
                }
                Reply(r, true, "\"plugin\":" + Quote(p->file.id) + ",\"preset\":" + Quote(presetName) + ",\"values\":" + values + "},\"refused\":" + refused + "]");
                return;
            }
            if (r.cmd == "settings.list")
            {
                std::string out = "[";
                for (auto& s : d.settings)
                {
                    if (!s.HasValue())
                        continue;
                    out += (out.size() > 1 ? "," : "") + std::string("{\"setting\":") + Quote(s.id) + ",\"type\":" + Quote(s.typeName) + ",\"apply\":" +
                        Quote(s.apply) + "," + ValueFields(*p, s.id) + "}";
                }
                Reply(r, true, "\"plugin\":" + Quote(p->file.id) + ",\"version\":" + Quote(p->version) + ",\"values\":" + out + "]");
                return;
            }
            auto s = d.Find(r.body.Str("setting"));
            if (!s)
            {
                Fail(r, "no such setting: " + std::string(r.body.Str("setting")), ASI_SETTINGS_UNKNOWN_ID);
                return;
            }
            std::string id = s->id;
            auto result = [&](int32_t code, const std::string& fields)
            {
                std::string text = "\"result\":" + std::to_string(code) + (fields.empty() ? "" : "," + fields);
                Reply(r, code >= 0, code >= 0 ? text : text + ",\"error\":" + Quote(asi_settings::detail::ResultName(code)));
            };
            if (r.cmd == "settings.get")
                Reply(r, true, ValueFields(*p, id));
            else if (r.cmd == "settings.set")
            {
                auto v = r.body.Find("value");
                if (!v || !(v->IsString() || v->IsNumber() || v->IsBool()))
                {
                    Fail(r, "value: the value as text", ASI_SETTINGS_INVALID_VALUE);
                    return;
                }
                std::string error;
                if (!BeginJournal(error))
                {
                    Fail(r, "journal: " + error);
                    return;
                }
                result(p->api->Set(id.c_str(), ValueText(*v).c_str()), ValueFields(*p, id));
            }
            else if (r.cmd == "settings.state")
            {
                std::string reason;
                int32_t code = GetStateOf(*p, id, reason);
                result(code, code >= 0 ? "\"state\":" + std::to_string(code) + ",\"reason\":" + Quote(reason) : std::string());
            }
            else if (r.cmd == "settings.choices")
            {
                if (p->api->size < offsetof(AsiSettingsApi, GetChoices) + sizeof(void*) || !p->api->GetChoices)
                {
                    result(ASI_SETTINGS_UNSUPPORTED, {});
                    return;
                }
                std::string choices;
                int32_t code = asi_settings::detail::CallText([&](char* b, uint32_t n) { return p->api->GetChoices(id.c_str(), b, n); }, choices);
                std::string values = "[";
                for (auto& v : DefaultValues(*p, *s))
                    values += (values.size() > 1 ? "," : "") + Quote(v);
                result(code, code >= 0 ? "\"choices\":" + choices + ",\"sweep\":" + values + "]" : std::string());
            }
            else if (r.cmd == "settings.invoke")
            {
                if (p->api->size < offsetof(AsiSettingsApi, Invoke) + sizeof(void*) || !p->api->Invoke)
                    result(ASI_SETTINGS_UNSUPPORTED, {});
                else
                    result(p->api->Invoke(id.c_str()), {});
            }
            else
                Fail(r, "unknown command: " + r.cmd);
        }

        // ---------------------------------------------------------------------------------------------------------------
        // Game thread

        void UpdateState()
        {
            if (ticks % 10)
                return;
            std::string detail;
            auto ready = adapter->IsReady(detail);
            std::string extra = adapter->State();
            std::string s = "{\"readiness\":" + Quote(ReadinessName(ready)) + (detail.empty() ? "" : ",\"detail\":" + Quote(detail)) +
                ",\"frozen\":" + (frozen ? "true" : "false") + ",\"uiHidden\":" + (uiHidden ? "true" : "false") + ",\"scriptCamera\":" +
                (scriptCamera ? "true" : "false") + ",\"busy\":" + (jobs.empty() ? "false" : "true") + (extra.empty() ? "" : "," + extra) + "}";
            std::lock_guard guard(lock);
            stateJson = std::move(s);
        }

        // Ctrl+Shift+F11 while the game is in front: bookmark with a timestamp name
        void Hotkeys()
        {
            bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F11) & 0x8000);
            if (down && !hotkeyDown)
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(GetForegroundWindow(), &pid);
                if (pid == GetCurrentProcessId())
                {
                    char name[32];
                    std::time_t now = std::time(nullptr);
                    std::tm t{};
                    localtime_s(&t, &now);
                    std::strftime(name, sizeof(name), "hotkey-%Y%m%d-%H%M%S", &t);
                    std::string error;
                    auto path = Bookmark(name, {}, error);
                    Log(path.empty() ? "hotkey bookmark failed: " + error : "hotkey bookmark: " + Narrow(path.wstring()));
                }
            }
            hotkeyDown = down;
        }

        // Writes the scene as a bookmark file and returns its path
        std::filesystem::path Bookmark(const std::string& name, std::filesystem::path out, std::string& error, std::string* json = nullptr)
        {
            char date[16];
            std::time_t now = std::time(nullptr);
            std::tm t{};
            localtime_s(&t, &now);
            std::strftime(date, sizeof(date), "%Y-%m-%d", &t);
            std::string fix;
            for (auto& p : AllPlugins())
                fix += (fix.empty() ? "" : ", ") + p->file.id + " " + p->version;
            std::string text = "{\"format\":1,\"game\":" + Quote(adapter->GameId()) + ",\"name\":" + Quote(name) + "," + adapter->ReadScene() +
                ",\"captured\":{\"exe\":" + Quote(asi_settings::detail::FileVersion(nullptr)) + ",\"fix\":" + Quote(fix) + ",\"date\":" + Quote(date) + "}}";
            if (out.empty())
                out = DataFolder() / L"scenes" / Widen(adapter->GameId()) / (Widen(name) + L".json");
            if (!WriteText(out, text))
            {
                error = "couldn't write " + Narrow(out.wstring());
                return {};
            }
            if (json)
                *json = text;
            return out;
        }

        void RunCommands()
        {
            std::deque<Request> queue;
            {
                std::lock_guard guard(lock);
                queue.swap(incoming);
            }
            for (auto& r : queue)
                Start(std::move(r));
            // One job at a time, in order
            while (!jobs.empty())
            {
                bool finished = false;
                try { finished = jobs.front()->Step(*this); }
                catch (...)
                {
                    Fail(jobs.front()->request, "exception in the job");
                    finished = true;
                }
                if (!finished)
                    break;
                jobs.pop_front();
            }
        }

        template<typename T>
        void Queue(Request r)
        {
            auto job = std::make_unique<T>();
            job->request = std::move(r);
            jobs.push_back(std::move(job));
        }

        // Commands of the game thread; the quick ones reply at once unless a job is running (then they queue behind it)
        void Start(Request r)
        {
            if (!jobs.empty())
            {
                Queue<Deferred>(std::move(r));
                return;
            }
            Execute(std::move(r));
        }

        // A quick command that waited for the jobs before it
        struct Deferred : Job
        {
            bool Step(Core& c) override
            {
                c.Execute(std::move(request), true);
                return true;
            }
        };

        void Execute(Request r, bool fromQueue = false)
        {
            auto& b = r.body;
            if (r.cmd == "freeze")
            {
                frozen = b.Bool("on", true);
                adapter->Freeze(frozen);
                Reply(r, true, "\"frozen\":" + std::string(frozen ? "true" : "false"));
            }
            else if (r.cmd == "ui")
            {
                auto v = b.Find("visible");
                uiHidden = !(v && v->IsBool() ? v->boolean : b.Str("visible") != "false");
                if (auto h = b.Find("hidden"); h && h->IsBool())
                    uiHidden = h->boolean;
                adapter->HideUI(uiHidden);
                Reply(r, true, "\"uiHidden\":" + std::string(uiHidden ? "true" : "false"));
            }
            else if (r.cmd == "camera")
            {
                auto pose = b.Find("pose");
                if (pose && pose->IsObject())
                {
                    CameraPose p;
                    if (!ReadPose(*pose, p))
                    {
                        Fail(r, "pose: {pos:[x,y,z], rot:[pitch,roll,yaw], fov}");
                        return;
                    }
                    adapter->SetCamera(&p);
                    scriptCamera = true;
                }
                else
                {
                    adapter->SetCamera(nullptr);
                    scriptCamera = false;
                }
                Reply(r, true, "\"scriptCamera\":" + std::string(scriptCamera ? "true" : "false"));
            }
            else if (r.cmd == "bookmark")
            {
                std::string name(b.Str("name", "bookmark")), error, json;
                auto path = Bookmark(name, Widen(b.Str("out")), error, &json);
                if (path.empty())
                    Fail(r, error);
                else
                    Reply(r, true, "\"file\":" + Quote(Narrow(path.wstring())) + ",\"scene\":" + json);
            }
            else if (r.cmd == "action")
            {
                bool ok = adapter->InjectAction(b.Str("action"));
                ok ? Reply(r, true, {}) : Fail(r, "the adapter can't inject " + std::string(b.Str("action")));
            }
            else if (r.cmd == "scene.apply")
                fromQueue ? (void)jobs.insert(jobs.begin() + 1, MakeJob<SceneJob>(std::move(r))) : Queue<SceneJob>(std::move(r));
            else if (r.cmd == "capture")
                fromQueue ? (void)jobs.insert(jobs.begin() + 1, MakeJob<CaptureJob>(std::move(r))) : Queue<CaptureJob>(std::move(r));
            else if (r.cmd == "sweep")
                fromQueue ? (void)jobs.insert(jobs.begin() + 1, MakeJob<SweepJob>(std::move(r))) : Queue<SweepJob>(std::move(r));
            else if (r.cmd == "wait")
                fromQueue ? (void)jobs.insert(jobs.begin() + 1, MakeJob<WaitJob>(std::move(r))) : Queue<WaitJob>(std::move(r));
            else if (r.cmd == "quit")
            {
                std::string report = "kept";
                if (b.Bool("restore", true))
                    RestoreJournal(report);
                Reply(r, true, "\"journal\":" + Quote(report));
                Log("quit");
                // Let the pipe thread send the reply
                Sleep(300);
                adapter->Quit();
            }
            else if (r.cmd.starts_with("adapter."))
            {
                std::string error;
                auto fields = adapter->Call(std::string_view(r.cmd).substr(8), b, error);
                error.empty() ? Reply(r, true, fields) : Fail(r, error);
            }
            else
                Fail(r, "unknown command: " + r.cmd);
        }

        template<typename T>
        std::unique_ptr<Job> MakeJob(Request r)
        {
            auto job = std::make_unique<T>();
            job->request = std::move(r);
            return job;
        }

        // Waits frames, or until ready: {"frames":n} / {"ready":true,"maxSeconds":s}
        struct WaitJob : Job
        {
            uint64_t until = 0;
            bool Step(Core& c) override
            {
                auto& b = request.body;
                if (phase == 0)
                {
                    phase = 1;
                    startTime = GetTickCount64();
                    until = c.FrameCount() + uint64_t(b.Num("frames", 0));
                }
                if (c.FrameCount() < until)
                    return false;
                if (b.Bool("ready", false))
                {
                    std::string detail;
                    if (c.adapter->IsReady(detail) != Readiness::Ready)
                    {
                        if (GetTickCount64() - startTime > ULONGLONG(b.Num("maxSeconds", 60) * 1000))
                        {
                            c.Fail(request, "not ready: " + detail);
                            return true;
                        }
                        return false;
                    }
                }
                c.Reply(request, true, "\"frames\":" + std::to_string(c.FrameCount()));
                return true;
            }
        };

        // scene.apply {file | scene}: the adapter applies the start and the player, then camera and UI, then it settles
        // (ready for settle.minFrames frames), then the world is frozen and settles again (settle.frozenFrames)
        struct SceneJob : Job
        {
            Json scene;
            uint64_t readySince = 0, frozenAt = 0;
            bool Step(Core& c) override
            {
                auto& b = request.body;
                if (phase == 0)
                {
                    startTime = GetTickCount64();
                    if (auto s = b.Find("scene"); s && s->IsObject())
                        scene = *s;
                    else
                    {
                        std::string text;
                        auto file = std::filesystem::path(Widen(b.Str("file")));
                        if (file.is_relative())
                            file = DataFolder() / L"scenes" / Widen(c.adapter->GameId()) / file;
                        if (file.extension().empty())
                            file += L".json";
                        auto parsed = ReadText(file, text) ? Parse(text) : std::nullopt;
                        if (!parsed || !parsed->IsObject())
                        {
                            c.Fail(request, "can't read the scene " + Narrow(file.wstring()));
                            return true;
                        }
                        scene = *parsed;
                    }
                    // Unfrozen while loading
                    if (c.frozen)
                    {
                        c.adapter->Freeze(false);
                        c.frozen = false;
                    }
                    phase = 1;
                }
                double maxSeconds = 120;
                if (auto settle = scene.Find("settle"))
                    maxSeconds = settle->Num("maxSeconds", maxSeconds);
                if (GetTickCount64() - startTime > ULONGLONG(maxSeconds * 1000))
                {
                    std::string detail;
                    auto ready = c.adapter->IsReady(detail);
                    c.Fail(request, "timed out in phase " + std::to_string(phase) + " (" + ReadinessName(ready) + (detail.empty() ? "" : ": " + detail) + ")");
                    return true;
                }
                if (phase == 1)
                {
                    auto step = c.adapter->ApplyScene(scene, startTick == 0);
                    startTick = 1;
                    if (step.kind == Step::Failed)
                    {
                        c.Fail(request, step.reason);
                        return true;
                    }
                    if (step.kind == Step::Running)
                        return false;
                    // Camera and UI
                    if (auto cam = scene.Find("camera"); cam && cam->IsObject() && cam->Str("mode") == "fixed")
                    {
                        CameraPose pose;
                        if (ReadPose(*cam, pose))
                        {
                            c.adapter->SetCamera(&pose);
                            c.scriptCamera = true;
                        }
                    }
                    else if (c.scriptCamera)
                    {
                        c.adapter->SetCamera(nullptr);
                        c.scriptCamera = false;
                    }
                    c.uiHidden = scene.Str("ui", "hidden") == "hidden";
                    c.adapter->HideUI(c.uiHidden);
                    phase = 2;
                    readySince = 0;
                }
                auto settle = scene.Find("settle");
                uint64_t minFrames = uint64_t(settle ? settle->Num("minFrames", 120) : 120);
                uint64_t frozenFrames = uint64_t(settle ? settle->Num("frozenFrames", 48) : 48);
                if (phase == 2)
                {
                    // readyWhen "applied": the scene is what the start gives, not playable (a cutscene movie for its
                    // subtitles): minFrames presented frames, no readiness
                    if (scene.Str("readyWhen") == "applied")
                    {
                        if (!readySince)
                            readySince = c.FrameCount();
                        if (c.FrameCount() - readySince < minFrames)
                            return false;
                    }
                    else
                    {
                        std::string detail;
                        if (c.adapter->IsReady(detail) != Readiness::Ready)
                        {
                            readySince = 0;
                            return false;
                        }
                        if (!readySince)
                            readySince = c.SceneFrameCount();
                        if (c.SceneFrameCount() - readySince < minFrames)
                            return false;
                    }
                    bool freeze = true;
                    if (auto f = scene.Find("freeze"); f && f->IsBool())
                        freeze = f->boolean;
                    if (freeze)
                    {
                        c.adapter->Freeze(true);
                        c.frozen = true;
                    }
                    frozenAt = c.FrameCount();
                    phase = 3;
                }
                if (phase == 3)
                {
                    if (c.FrameCount() - frozenAt < frozenFrames)
                        return false;
                    c.Reply(request, true, "\"scene\":" + Quote(scene.Str("name")) + ",\"frames\":" + std::to_string(c.FrameCount()) + ",\"seconds\":" +
                        Number((GetTickCount64() - startTime) / 1000.0));
                    c.Event(request, "\"event\":\"scene.ready\",\"scene\":" + Quote(scene.Str("name")));
                    return true;
                }
                return false;
            }
        };

        // Paths of `frames` frames per stage under `out` (a file: "_NNNN" goes before the extension when more than one)
        static void CapturePaths(const std::filesystem::path& out, const std::vector<int>& stages, int count, std::vector<std::filesystem::path> paths[ASI_AUTOMATION_STAGE_COUNT])
        {
            for (int stage : stages)
            {
                for (int i = 0; i < count; ++i)
                {
                    auto path = out;
                    auto ext = path.extension().wstring();
                    if (ext.empty())
                        ext = L".png";
                    std::wstring stem = path.stem().wstring();
                    if (stages.size() > 1)
                        stem += stage == ASI_AUTOMATION_STAGE_BEFORE_UI ? L".beforeUI" : L".afterUI";
                    if (count > 1)
                    {
                        wchar_t n[16];
                        swprintf_s(n, L"_%04d", i);
                        stem += n;
                    }
                    paths[stage].push_back(path.parent_path() / (stem + ext));
                }
            }
        }

        static std::vector<int> StagesOf(const Json& b, std::string& error)
        {
            std::vector<int> stages;
            auto v = b.Find("stage");
            if (!v)
                v = b.Find("stages");
            if (!v)
                return { ASI_AUTOMATION_STAGE_BEFORE_UI };
            auto add = [&](const Json& item)
            {
                int s = item.IsString() ? StageOf(item.string) : -1;
                if (s < 0)
                    error = "stage: beforeUI or afterUI";
                else if (std::find(stages.begin(), stages.end(), s) == stages.end())
                    stages.push_back(s);
            };
            if (v->IsArray())
            {
                for (auto& item : v->items)
                    add(item);
            }
            else
                add(*v);
            return stages;
        }

        // capture {stage|stages, frames, out}
        struct CaptureJob : Job
        {
            bool Step(Core& c) override
            {
                auto& b = request.body;
                if (phase == 0)
                {
                    std::string error;
                    auto stages = StagesOf(b, error);
                    if (!error.empty())
                    {
                        c.Fail(request, error);
                        return true;
                    }
                    auto out = std::filesystem::path(Widen(b.Str("out")));
                    if (out.empty())
                        out = DataFolder() / L"captures" / L"capture.png";
                    std::vector<std::filesystem::path> paths[ASI_AUTOMATION_STAGE_COUNT];
                    CapturePaths(out, stages, std::max(1, int(b.Num("frames", 1))), paths);
                    if (!c.BeginGrab(paths, error))
                    {
                        c.Fail(request, error);
                        return true;
                    }
                    startTime = GetTickCount64();
                    phase = 1;
                }
                if (!c.GrabDone())
                {
                    if (GetTickCount64() - startTime > 30000)
                    {
                        c.CancelGrab();
                        c.Fail(request, "no frames came (is the game drawing?)");
                        return true;
                    }
                    return false;
                }
                bool ok;
                auto results = c.GrabResults(ok);
                c.Reply(request, ok, results);
                return true;
            }
        };

        // sweep {plugin, setting, values?, settle, stage|stages, out, restore, unfreezeFrames}: for each value Set, wait
        // 2 + settle frames, grab; events sweep.frame; the reply lists every value with its frames and the other
        // settings in effect. unfreezeFrames: the world runs that many scene frames after each Set (frozen scenes only)
        struct SweepJob : Job
        {
            std::shared_ptr<PluginRef> plugin;
            std::string setting, original;
            std::vector<std::string> values;
            std::vector<int> stages;
            std::filesystem::path out;
            size_t index = 0;
            uint64_t waitUntil = 0;
            std::string entries, current;
            int32_t setResult = 0;
            std::string inEffect;

            bool Step(Core& c) override
            {
                auto& b = request.body;
                if (phase == 0)
                {
                    plugin = c.FindPlugin(b.Str("plugin"));
                    if (!plugin)
                    {
                        c.Fail(request, "no such plugin: " + std::string(b.Str("plugin")));
                        return true;
                    }
                    auto s = plugin->file.description.Find(b.Str("setting"));
                    if (!s)
                    {
                        c.Fail(request, "no such setting: " + std::string(b.Str("setting")));
                        return true;
                    }
                    setting = s->id;
                    if (auto v = b.Find("values"); v && v->IsArray())
                    {
                        for (auto& item : v->items)
                            values.push_back(ValueText(item));
                    }
                    else
                        values = c.DefaultValues(*plugin, *s);
                    if (values.empty())
                    {
                        c.Fail(request, "no values to sweep");
                        return true;
                    }
                    std::string error;
                    stages = StagesOf(b, error);
                    if (!error.empty() || !c.BeginJournal(error))
                    {
                        c.Fail(request, error);
                        return true;
                    }
                    out = std::filesystem::path(Widen(b.Str("out")));
                    if (out.empty())
                        out = DataFolder() / L"captures" / Widen(plugin->file.id) / Widen(setting);
                    GetValue(*plugin, setting, original);
                    phase = 1;
                }
                int settle = int(b.Num("settle", 6));
                if (phase == 1)
                {
                    // Set the next value
                    if (index >= values.size())
                    {
                        if (b.Bool("restore", true))
                            plugin->api->Set(setting.c_str(), original.c_str());
                        c.Reply(request, true, "\"plugin\":" + Quote(plugin->file.id) + ",\"version\":" + Quote(plugin->version) + ",\"setting\":" +
                            Quote(setting) + ",\"original\":" + Quote(original) + ",\"values\":[" + entries + "],\"others\":" + Others());
                        return true;
                    }
                    current = values[index];
                    setResult = plugin->api->Set(setting.c_str(), current.c_str());
                    // Settings that only show while the game updates (the gameplay camera's FOV): the world runs a
                    // few scene frames, then it's frozen again
                    if (int pulse = int(b.Num("unfreezeFrames", 0)); pulse > 0 && c.frozen)
                    {
                        c.adapter->Freeze(false);
                        waitUntil = c.SceneFrameCount() + 2 + uint64_t(pulse);
                        phase = 4;
                        return false;
                    }
                    waitUntil = c.FrameCount() + 2 + uint64_t(std::max(0, settle));
                    phase = 2;
                    return false;
                }
                if (phase == 4)
                {
                    if (c.SceneFrameCount() < waitUntil)
                        return false;
                    c.adapter->Freeze(true);
                    waitUntil = c.FrameCount() + 2 + uint64_t(std::max(0, settle));
                    phase = 2;
                    return false;
                }
                if (phase == 2)
                {
                    if (c.FrameCount() < waitUntil)
                        return false;
                    GetValue(*plugin, setting, inEffect);
                    std::vector<std::filesystem::path> paths[ASI_AUTOMATION_STAGE_COUNT];
                    wchar_t n[16];
                    swprintf_s(n, L"%04zu_", index);
                    std::wstring safe;
                    for (wchar_t ch : Widen(current))
                        safe += iswalnum(ch) || ch == L'.' || ch == L'-' ? ch : L'_';
                    CapturePaths(out / (std::wstring(n) + safe + L".png"), stages, std::max(1, int(b.Num("frames", 1))), paths);
                    std::string error;
                    if (!c.BeginGrab(paths, error))
                    {
                        c.Fail(request, error);
                        return true;
                    }
                    startTime = GetTickCount64();
                    phase = 3;
                }
                if (phase == 3)
                {
                    if (!c.GrabDone())
                    {
                        if (GetTickCount64() - startTime > 30000)
                        {
                            c.CancelGrab();
                            c.Fail(request, "no frames came for value " + current);
                            return true;
                        }
                        return false;
                    }
                    bool ok;
                    auto frames = c.GrabResults(ok);
                    std::string entry = "{\"index\":" + std::to_string(index) + ",\"value\":" + Quote(current) + ",\"result\":" + std::to_string(setResult) +
                        ",\"inEffect\":" + Quote(inEffect) + "," + frames + "}";
                    entries += (index ? "," : "") + entry;
                    c.Event(request, "\"event\":\"sweep.frame\"," + entry.substr(1, entry.size() - 2));
                    index++;
                    phase = 1;
                }
                return false;
            }

            // The other settings of the plugin while capturing, so a clip can be made again
            std::string Others()
            {
                std::string out = "{";
                for (auto& s : plugin->file.description.settings)
                {
                    if (!s.HasValue() || s.id == setting)
                        continue;
                    std::string v;
                    if (GetValue(*plugin, s.id, v) >= 0)
                        out += (out.size() > 1 ? "," : "") + Quote(s.id) + ":" + Quote(v);
                }
                return out + "}";
            }
        };
    };
}
