// overlay/SettingsModel.hpp: the model of the in-game settings overlay: stored choice vs value in effect, menu-only
// presets, API rejection, restart markers, conditions, choices, language fallback, old API tables. Overlay.hpp and its
// Direct3D 9 and 11 parts need ImGui and a device: the standalone header check compiles them.

#include <windows.h>
#include "../overlay/SettingsModel.hpp"
#include "../overlay/Extensions.hpp"
#include "../overlay/MessageGate.hpp"
#include "check.h"
#include <chrono>
#include <thread>

static std::map<std::string, std::string> values;
static std::vector<std::string> writes;
static int32_t Get(const char* id, char* b, uint32_t n)
{
    auto it = values.find(id);
    return it == values.end() ? ASI_SETTINGS_UNKNOWN_ID : asi_settings::text::CopyOut(it->second, b, n);
}
static int32_t Set(const char* id, const char* value)
{
    if (std::string(id) == "MAIN.Refused") return ASI_SETTINGS_NOT_NOW;
    values[id] = value; writes.push_back(id); return ASI_SETTINGS_OK;
}
static int32_t Choices(const char*, char* b, uint32_t n)
{
    return asi_settings::text::CopyOut("[{\"value\":1,\"label\":\"@one\"}]", b, n);
}
static int32_t State(const char* id, char*, uint32_t) { return std::string(id) == "MAIN.Hidden" ? ASI_SETTINGS_STATE_HIDDEN : 0; }

TEST_CASE("overlay: settings model")
{
    AsiSettingsApi api{ sizeof(api), ASI_SETTINGS_VERSION, "test", Get, Set, Choices, nullptr, nullptr, nullptr, nullptr, State };
    asi_overlay::SettingsModel model;
    asi_settings::LoadedPlugin plugin;
    plugin.api = &api;
    plugin.file.id = "test";
    std::string error;
    REQUIRE(asi_settings::ParseDescription(R"({"format":"asi-settings","version":"0.1","plugin":{"id":"test","name":"Test"},
      "pages":[{"id":"main","title":"Main"},{"id":"compat","title":"Compatibility","section":"compatibility"}],
      "settings":[
        {"id":"MAIN.Menu","type":"bool","default":0,"label":"Menu","apply":"restart","presets":{"vanilla":0,"recommended":1,"enhanced":1}},
        {"id":"MAIN.Editor","type":"int","default":7,"label":"Editor","visibility":"editor","presets":{"vanilla":0,"recommended":1,"enhanced":2}},
        {"id":"MAIN.File","type":"int","default":9,"label":"File","visibility":"file","presets":{"vanilla":0,"recommended":1,"enhanced":2}},
        {"id":"MAIN.Compatibility","type":"bool","default":0,"label":"Compatibility","page":"compat","presets":{"vanilla":0,"recommended":1,"enhanced":1}},
        {"id":"MAIN.Refused","type":"choice","default":0,"label":"Refused","choices":[{"value":0},{"value":1}],"presets":{"vanilla":0,"recommended":1,"enhanced":1}},
        {"id":"MAIN.Hidden","type":"bool","default":0,"label":"Hidden","visibleIf":{"setting":"MAIN.Menu","equals":1}}
      ]})", plugin.file.description, &error));
    plugin.file.texts.sources["en"] = R"({"one":"One"})";
    model.plugins.push_back(std::move(plugin));
    model.SetLanguage("ru-RU");
    for (auto& s : model.plugins[0].file.description.settings) values[s.id] = asi_settings::detail::JsonToText(s.defaultValue);
    auto changes = model.PresetChanges(1);
    CHECK(changes.size() == 5); // every setting whose preset values differ, whatever its visibility or page
    model.Apply(changes);
    CHECK(writes.size() == 4 && writes[0] == "MAIN.Menu");     // MAIN.Refused is refused by the plugin
    CHECK(values["MAIN.Menu"] == "1" && values["MAIN.Editor"] == "1" && values["MAIN.File"] == "1" && values["MAIN.Compatibility"] == "1");
    CHECK(model.restartChanges.count("test:MAIN.Menu"));
    CHECK(model.message.find("Cannot be used now") != std::string::npos);
    auto& description = model.plugins[0].file.description;
    CHECK(asi_overlay::SettingsModel::Hidden(&api, *description.Find("MAIN.Hidden")));
    auto snapshot = model.Snapshot(0);
    CHECK(asi_overlay::SettingsModel::Condition(description.Find("MAIN.Hidden")->visibleIf, snapshot));
    values["MAIN.Menu"] = "0";
    snapshot = model.Snapshot(0);
    CHECK(!asi_overlay::SettingsModel::Condition(description.Find("MAIN.Hidden")->visibleIf, snapshot));
    auto choices = asi_overlay::SettingsModel::Choices(&api, *description.Find("MAIN.Refused"));
    CHECK(choices.size() == 1 && model.plugins[0].file.texts.Resolve(choices[0].label) == "One");
    // Tables of the 0.x drafts count as 1.0; another major version is not used (SPEC.md, Versions of the table)
    api.version = 1;
    CHECK(asi_overlay::SettingsModel::Hidden(&api, *description.Find("MAIN.Hidden")));
    api.version = 2000;
    CHECK(!asi_overlay::SettingsModel::Hidden(&api, *description.Find("MAIN.Hidden")));
    api.version = ASI_SETTINGS_VERSION;
    api.size = offsetof(AsiSettingsApi, GetState);
    CHECK(!asi_overlay::SettingsModel::Hidden(&api, *description.Find("MAIN.Hidden")));
    api.size = offsetof(AsiSettingsApi, GetChoices);
    CHECK(asi_overlay::SettingsModel::Choices(&api, *description.Find("MAIN.Refused")).size() == 2);
    api.size = offsetof(AsiSettingsApi, Set);
    CHECK(!model.Set(0, *description.Find("MAIN.Menu"), "1"));
    // Availability: the file holds the player's choice (2), Get returns the fallback in effect (1)
    {
        api.size = sizeof(api);
        asi_overlay::SettingsModel stored;
        asi_settings::LoadedPlugin p;
        p.api = &api; p.file.id = "aa";
        REQUIRE(asi_settings::ParseDescription(R"({"format":"asi-settings","version":"0.1","plugin":{"id":"aa","name":"AA"},
          "settings":[{"id":"MISC.AA","type":"choice","default":1,"label":"AA","choices":[{"value":0,"label":"Off"},{"value":1,"label":"SMAA"},{"value":2,"label":"FSR"}],
          "presets":{"vanilla":0,"recommended":2,"enhanced":2}}]})", p.file.description, &error));
        stored.plugins.push_back(std::move(p));
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        auto cfg = std::filesystem::path(exe).parent_path() / "model-stored.cfg";
        asi_settings::WriteFileAtomic(cfg, "[MISC]\r\nAA = 2\r\n");
        asi_settings::StorageFiles storage; storage.file = cfg;
        stored.UseStorage(0, storage);
        values["MISC.AA"] = "1";
        auto& aa = *stored.plugins[0].file.description.Find("MISC.AA");
        std::string choice; CHECK(stored.Stored(0, aa, choice) && choice == "2");
        CHECK(stored.PresetChanges(2).empty()); // the choice already is the preset's
        auto vanilla = stored.PresetChanges(0);
        CHECK(vanilla.size() == 1 && vanilla[0].before == "2" && vanilla[0].beforeText == "FSR (running: SMAA)" && vanilla[0].afterText == "Off");
        CHECK(stored.Snapshot(0, true)[0].intValue == 2 && stored.Snapshot(0)[0].intValue == 1);
        // Recommended and enhanced are the same: one preset is shown, the first that matches (from the choice, not the fallback)
        CHECK(stored.CurrentPreset(0) == asi_settings::Preset::Recommended);
        asi_settings::WriteFileAtomic(cfg, "[MISC]\r\nAA = 1\r\n");
        stored.UseStorage(0, storage);
        CHECK(stored.CurrentPreset(0) == asi_settings::Preset::Custom);
        DeleteFileW(cfg.c_str());
    }
}

// The developer menu's layout (the hub's order): menu pages by group, then Advanced and Compatibility by file section;
// every tier, file-only settings too; empty pages left out; search by label or id
TEST_CASE("overlay: page layout and search")
{
    asi_overlay::SettingsModel model;
    asi_settings::LoadedPlugin plugin;
    plugin.file.id = "layout";
    std::string error;
    REQUIRE(asi_settings::ParseDescription(R"({"format":"asi-settings","version":"1.0","plugin":{"id":"layout","name":"Layout","games":[{"name":"Game","exe":["game.exe"]}]},
      "pages":[{"id":"game","title":"Game"},{"id":"empty","title":"Empty"},{"id":"display","title":"Display"},{"id":"compat","title":"Compatibility","section":"compatibility"}],
      "settings":[
        {"id":"MAIN.A","type":"bool","default":0,"label":"Alpha","page":"game","group":"First"},
        {"id":"MAIN.B","type":"bool","default":0,"label":"Beta","page":"display"},
        {"id":"MAIN.C","type":"bool","default":0,"label":"Gamma","page":"game","group":"Second"},
        {"id":"MAIN.D","type":"bool","default":0,"label":"Delta","page":"game","group":"First"},
        {"id":"SHADOWS.E","type":"int","default":1,"label":"Epsilon","page":"game","visibility":"editor"},
        {"id":"SHADOWS.F","type":"int","default":1,"label":"Zeta","page":"game","visibility":"file"},
        {"id":"MISC.G","type":"int","default":1,"label":"Eta","page":"display","visibility":"editor"},
        {"id":"MAIN.H","type":"bool","default":0,"label":"Theta","page":"compat"}
      ]})", plugin.file.description, &error));
    model.plugins.push_back(std::move(plugin));
    auto pages = model.Layout(0);
    REQUIRE(pages.size() == 4); // Game, Display, Advanced, Compatibility; "Empty" left out
    CHECK(pages[0].id == "game" && pages[1].id == "display" && pages[2].id == "advanced" && pages[3].id == "compatibility");
    CHECK(pages[2].tier == asi_overlay::Tier::Advanced && pages[3].tier == asi_overlay::Tier::Compatibility);
    // Groups together where their first setting is
    CHECK(pages[0].groups.size() == 2 && pages[0].groups[0].title == "First" && pages[0].groups[0].settings.size() == 2 && pages[0].groups[1].title == "Second");
    // Advanced: editor and file-only settings, grouped by section of the file
    CHECK(pages[2].Count() == 3 && pages[2].groups.size() == 2 && pages[2].groups[0].title == "SHADOWS" && pages[2].groups[1].title == "MISC");
    CHECK(pages[3].Count() == 1 && pages[3].groups[0].settings[0]->id == "MAIN.H");
    auto& d = model.plugins[0].file.description;
    CHECK(model.Matches(0, *d.Find("MAIN.C"), "gam") && model.Matches(0, *d.Find("MAIN.C"), "main.c") && !model.Matches(0, *d.Find("MAIN.C"), "zeta"));
    CHECK(asi_overlay::SettingsModel::IsDefault(*d.Find("SHADOWS.E"), "1") && !asi_overlay::SettingsModel::IsDefault(*d.Find("SHADOWS.E"), "2"));
}

// Extensions: order keys then registration order, nodes in the order of their first page, removal, a failed one kept
TEST_CASE("overlay: extensions")
{
    asi_overlay::Extensions registry;
    int calls = 0;
    auto draw = [&](asi_overlay::OverlayUI&) { ++calls; };
    int info = registry.AddPage("Debug", "Info", draw);
    int camera = registry.AddPage("Debug", "Camera", draw, -1);
    int under = registry.AddPage("layout", "Plugin page", draw);
    int tools = registry.AddPage("Tools", "Capture", draw, 5);
    registry.AddRows("layout", "game", draw, asi_overlay::Where::AfterGroup, "First");
    registry.AddMenu("Debug", draw);
    auto pages = registry.List(asi_overlay::Extension::Kind::Page);
    REQUIRE(pages.size() == 4);
    CHECK(pages[0].id == camera && pages[1].id == info && pages[2].id == under && pages[3].id == tools);
    auto nodes = registry.Nodes({ "layout" });
    CHECK(nodes.size() == 2 && nodes[0] == "Debug" && nodes[1] == "Tools");
    CHECK(registry.List(asi_overlay::Extension::Kind::Rows).size() == 1 && registry.List(asi_overlay::Extension::Kind::Menu).size() == 1);
    registry.Fail(info, "boom");
    pages = registry.List(asi_overlay::Extension::Kind::Page);
    CHECK(pages[1].error == "boom");
    CHECK(registry.Remove(camera) && !registry.Remove(camera));
    CHECK(registry.List(asi_overlay::Extension::Kind::Page).size() == 3);
    CHECK(registry.Nodes({ "layout" }).size() == 2);
    registry.Remove(tools);
    CHECK(registry.Nodes({ "layout" }).size() == 1);
    CHECK(calls == 0); // the registry never draws
}

// The window procedure never waits for the overlay's lock (MessageGate.hpp): with the lock held on the same thread it
// goes in (a recursive lock), with the lock held on another thread it returns at once, keeps a focus loss for the next
// frame and leaves other messages to the game. Waiting here would deadlock the game (Alt+Tab in exclusive fullscreen).
TEST_CASE("overlay: the window procedure never waits for the overlay's lock")
{
    std::recursive_mutex mutex;
    asi_overlay::MessageGate gate(mutex);

    // The same thread holds a Scope (the overlay's own call) and the window procedure re-enters
    {
        std::unique_lock<std::recursive_mutex> scope(mutex);
        std::unique_lock<std::recursive_mutex> taken;
        CHECK(gate.Enter(taken, WM_KEYDOWN, VK_F10));
        CHECK(taken.owns_lock());
    }
    CHECK(gate.Dropped() == 0);

    // Another thread (the render thread in Present) holds the lock
    std::atomic<bool> held = false, release = false;
    std::thread render([&]
    {
        std::unique_lock<std::recursive_mutex> scope(mutex);
        held = true;
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    while (!held) std::this_thread::sleep_for(std::chrono::milliseconds(1));

    auto start = std::chrono::steady_clock::now();
    std::unique_lock<std::recursive_mutex> taken;
    CHECK(!gate.Enter(taken, WM_KILLFOCUS, 0));
    CHECK(!taken.owns_lock());
    CHECK(!gate.Enter(taken, WM_KEYDOWN, VK_F10));
    CHECK(!gate.Enter(taken, WM_ACTIVATEAPP, FALSE));
    auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(waited < 200);
    CHECK(gate.Dropped() == 3);

    // The focus loss is remembered once, for the frame that follows
    CHECK(gate.TakePendingClose());
    CHECK(!gate.TakePendingClose());

    release = true;
    render.join();
    CHECK(gate.Enter(taken, WM_KEYDOWN, VK_F10));
    CHECK(taken.owns_lock());
}
