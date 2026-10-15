// Tests of asi_settings.hpp. The exe carries the NFSU example description and its English text as its own resources
// and plays the plugin: it loads its settings, writes and reads its .cfg, and answers through its own export.
#include "../asi_settings.hpp"
#include <cstdio>
#include <cstdlib>
#include <crtdbg.h>

asi_settings::Plugin settings;
ASI_SETTINGS_EXPORT(settings)

namespace
{
    int failures = 0, checks = 0;

    void Check(bool condition, const char* what, int line)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::printf("FAILED line %d: %s\n", line, what);
        }
    }
#define CHECK(x) Check((x), #x, __LINE__)

    std::string Cfg()
    {
        return asi_settings::ReadFileText(settings.CfgPath()).value_or("");
    }

    void WriteCfg(const std::string& text)
    {
        asi_settings::WriteFileAtomic(settings.CfgPath(), text);
    }

    std::string Replaced(std::string text, std::string_view from, std::string_view to)
    {
        auto at = text.find(from);
        if (at != std::string::npos)
            text.replace(at, from.size(), to);
        return text;
    }

    // As most text editors save: the file truncated and written again in place, here in two steps. share: what others
    // may do with it meanwhile; hold: how long it stays open after writing
    bool WriteInPlace(const std::filesystem::path& path, const std::string& text, DWORD share = FILE_SHARE_READ, DWORD hold = 0)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, share, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        FlushFileBuffers(file);
        Sleep(20);
        DWORD written = 0;
        bool ok = WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr) && written == text.size();
        Sleep(hold);
        CloseHandle(file);
        return ok;
    }

    void Json()
    {
        using namespace asi_settings::json;
        Value v;
        CHECK(Parse(R"({"a": [1, 2.5, -3e2], "b": "x\u00e9\ud83d\ude00\n", "c": true, "d": null})", v));
        CHECK(v.Find("a")->items.size() == 3 && v.Find("a")->items[2].number == -300);
        CHECK(v.Find("b")->string == "x\xC3\xA9\xF0\x9F\x98\x80\n");
        CHECK(v.Find("c")->boolean && v.Find("d")->IsNull());
        Error e;
        CHECK(!Parse(R"({"a": 1,})", v, &e) && !e.message.empty());
        CHECK(!Parse("[1] 2", v));
        CHECK(Parse("\xEF\xBB\xBF{}", v) && v.IsObject());
        // Booleans in presets and conditions compare as 0 and 1
        CHECK(Parse("[true, false]", v));
        CHECK(asi_settings::detail::JsonEquals(v.items[0], "1", true) && asi_settings::detail::JsonEquals(v.items[1], "0", true));
        CHECK(!asi_settings::detail::JsonEquals(v.items[0], "0", true));
    }

    // Versions (SPEC.md, Versions): 1.x and the 0.x drafts are read, another major version isn't; the table of the
    // runtime interface by its version and size
    void Versions()
    {
        auto parses = [](const char* version)
        {
            asi_settings::Description d;
            std::string error;
            bool ok = asi_settings::ParseDescription(std::string(R"({"format":"asi-settings","version":")") + version +
                R"(","plugin":{"name":"V"},"settings":[{"id":"MAIN.A","type":"bool","default":true,"label":"A"}]})", d, &error);
            return ok && d.settings.size() == 1 && d.settings[0].defaultValue.IsBool();
        };
        CHECK(parses("1.0") && parses("1.7") && parses("0.1"));
        CHECK(!parses("2.0") && !parses("10.0"));
        CHECK(ASI_SETTINGS_VERSION == 1000 && ASI_SETTINGS_VERSION_MAJOR_OF(1) == 1 && ASI_SETTINGS_VERSION_MAJOR_OF(1003) == 1 &&
              ASI_SETTINGS_VERSION_MAJOR_OF(2000) == 2);

        // Fields that are gone in 1.0 are unknown fields: ignored
        asi_settings::Description d;
        CHECK(asi_settings::ParseDescription(R"({"format":"asi-settings","version":"1.0","plugin":{"name":"V"},"future":1,"settings":[
            {"id":"MAIN.A","type":"int","default":1,"label":"A","availability":"mainMenu","replaces":"x","visibility":"later","apply":"later","storage":"later"}]})", d));
        CHECK(d.settings.size() == 1 && !d.settings[0].InMenu() && d.settings[0].apply == "later" && d.settings[0].storage != "cfg");

        // The table: drafts (version < 1000) count as 1.0; another major version and tables without Set are refused
        AsiSettingsApi api{};
        api.size = sizeof(api);
        api.version = ASI_SETTINGS_VERSION;
        api.Get = [](const char*, char*, uint32_t) -> int32_t { return 0; };
        api.Set = [](const char*, const char*) -> int32_t { return 0; };
        CHECK(ASI_SETTINGS_API_COMPATIBLE(&api) && ASI_SETTINGS_API_HAS(&api, Get) && !ASI_SETTINGS_API_HAS(&api, GetState));
        CHECK(ASI_SETTINGS_API_SIZE_1_0 == sizeof(AsiSettingsApi));
        api.version = 1;
        CHECK(ASI_SETTINGS_API_COMPATIBLE(&api));
        api.version = 1009;
        CHECK(ASI_SETTINGS_API_COMPATIBLE(&api));
        api.version = 2000;
        CHECK(!ASI_SETTINGS_API_COMPATIBLE(&api));
        api.version = ASI_SETTINGS_VERSION;
        api.size = uint32_t(offsetof(AsiSettingsApi, Set));
        CHECK(!ASI_SETTINGS_API_COMPATIBLE(&api) && !ASI_SETTINGS_API_HAS(&api, Set));
        const AsiSettingsApi* none = nullptr;
        CHECK(!ASI_SETTINGS_API_COMPATIBLE(none));
    }

    // Numbers in the file (SPEC.md, Reading): decimal or 0x hexadecimal for int, decimal only for float
    void Numbers()
    {
        int i = 0;
        float f = 0;
        using asi_settings::detail::ParseInt, asi_settings::detail::ParseFloat;
        CHECK(ParseInt("0x1F", i) && i == 31 && ParseInt("-12", i) && i == -12 && ParseInt("+5", i) && i == 5);
        CHECK(!ParseInt("0x-1", i) && !ParseInt("0x0x1", i) && !ParseInt("0x", i) && !ParseInt("-", i) && !ParseInt("1 2", i) && !ParseInt("1.5", i));
        CHECK(ParseFloat("1.5", f) && f == 1.5f && ParseFloat(".5", f) && ParseFloat("-2e1", f) && f == -20.0f);
        CHECK(!ParseFloat("0x10", f) && !ParseFloat("inf", f) && !ParseFloat("nan", f) && !ParseFloat("1,5", f) && !ParseFloat("1.2.3", f));
    }

    void CfgParsing()
    {
        auto f = asi_settings::ParseCfg("; comment\r\n[MAIN]\r\nA = 1 ; behind\r\nB=\"; kept \\\" \"\r\n# also comment\n[Other]\nC = x;y\n");
        CHECK(*f.Find("main", "a") == "1");
        CHECK(*f.Find("MAIN", "B") == "; kept \" ");
        CHECK(*f.Find("Other", "C") == "x;y");
        CHECK(f.Find("MAIN", "C") == nullptr);
    }

    // The order of the written file: menu settings first, then the advanced ones under the banner, unknown keys last
    void CfgOrder()
    {
        asi_settings::Description d;
        CHECK(asi_settings::ParseDescription(R"({"format":"asi-settings","version":"0.1","plugin":{"name":"T"},"settings":[
            {"id":"MAIN.A","type":"bool","default":0,"label":"A"},
            {"id":"MAIN.B","type":"bool","default":0,"label":"B","visibility":"editor"},
            {"id":"MAIN.C","type":"bool","default":0,"label":"C"},
            {"id":"MAIN.D","type":"bool","default":0,"label":"D","visibility":"file"},
            {"id":"GRAPHICS.G","type":"bool","default":0,"label":"G"},
            {"id":"DEBUG.Log","type":"bool","default":0,"label":"Log","visibility":"file"}]})", d));
        CHECK(d.settings.size() == 6 && d.settings[0].InMenu() && !d.settings[1].InMenu() && !d.settings[3].InMenu());
        std::vector<asi_settings::Slot> slots;
        for (auto& s : d.settings)
        {
            asi_settings::Slot slot;
            slot.setting = &s;
            asi_settings::SetDefault(slot);
            slots.push_back(std::move(slot));
        }
        slots[1].intValue = 1;
        asi_settings::Texts english;
        auto banner = std::string(asi_settings::CfgAdvancedBanner) + "\r\n";
        auto entry = [](const char* key, int value)
        {
            return std::string("; ") + key + ". 0 = Off, 1 = On. Default: 0\r\n" + key + " = " + std::to_string(value) + "\r\n";
        };
        std::string expected = "; T settings\r\n"
            "; Change the settings here or in FusionFixHub; fixes with an in-game menu change them there too.\r\n"
            "\r\n[MAIN]\r\n" + entry("A", 0) + "\r\n" + entry("C", 0) + "\r\n" + banner + "\r\n" + entry("B", 1) + "\r\n" +
            entry("D", 0) + "\r\nMine = 1\r\n"
            "\r\n[GRAPHICS]\r\n" + entry("G", 0) +                                  // only menu settings: no banner
            "\r\n[DEBUG]\r\n" + banner + "\r\n" + entry("Log", 0);                  // only advanced settings
        // An older file in the old order, with an unknown key
        auto previous = asi_settings::ParseCfg("[MAIN]\r\nA = 0\r\nB = 1\r\nMine = 1\r\nC = 0\r\n");
        auto text = asi_settings::WriteCfg(d, "T", english, slots, &previous);
        CHECK(text == expected);
        size_t count = 0;
        for (size_t at = text.find(banner); at != std::string::npos; at = text.find(banner, at + 1))
            ++count;
        CHECK(count == 2);                                                      // once in MAIN, once in DEBUG

        // Writing the written file again gives the same bytes
        previous = asi_settings::ParseCfg(text);
        CHECK(asi_settings::WriteCfg(d, "T", english, slots, &previous) == expected);
        // Banners left in odd places by hand or by an update in place are comments: not repeated, not kept
        previous = asi_settings::ParseCfg(banner + banner + "[MAIN]\r\n" + banner + "A = 0\r\n" + banner + "Mine = 1\r\n[DEBUG]\r\n" + banner);
        CHECK(asi_settings::WriteCfg(d, "T", english, slots, &previous) == expected);

        // Keys outside a section stay before the first section, where they are read the same way again; sections the
        // description doesn't have go last
        previous = asi_settings::ParseCfg("Loose = 1\r\n[OTHER]\r\nX = y\r\n[MAIN]\r\nMine = 1\r\n");
        text = asi_settings::WriteCfg(d, "T", english, slots, &previous);
        auto header = std::string("; T settings\r\n; Change the settings here or in FusionFixHub; fixes with an in-game menu change them there too.\r\n");
        CHECK(text == header + "\r\nLoose = 1\r\n" + expected.substr(header.size()) + "\r\n[OTHER]\r\nX = y\r\n");
        previous = asi_settings::ParseCfg(text);
        CHECK(asi_settings::WriteCfg(d, "T", english, slots, &previous) == text);
        CHECK(previous.Find("", "Loose") && *previous.Find("", "Loose") == "1" && !previous.Find("DEBUG", "Loose"));

        // No advanced settings at all: no banner
        asi_settings::Description menuOnly;
        CHECK(asi_settings::ParseDescription(R"({"format":"asi-settings","version":"0.1","plugin":{"name":"M"},"settings":[
            {"id":"MAIN.A","type":"bool","default":0,"label":"A"}]})", menuOnly));
        std::vector<asi_settings::Slot> menuSlots(1);
        menuSlots[0].setting = &menuOnly.settings[0];
        asi_settings::SetDefault(menuSlots[0]);
        text = asi_settings::WriteCfg(menuOnly, "M", english, menuSlots, nullptr);
        CHECK(text.find("Advanced") == std::string::npos && text.find("\r\n[MAIN]\r\n" + entry("A", 0)) != std::string::npos);
    }

    // Presets are data: a setting takes part when its preset values differ, whatever its visibility or page; one without
    // them or with the same value in all three keeps its value on a preset switch and doesn't decide which preset
    // matches (SPEC.md, Presets)
    void PresetsMenuOnly()
    {
        asi_settings::DescribedFile f;
        CHECK(asi_settings::ParseDescription(R"({"format":"asi-settings","version":"0.1","plugin":{"name":"P"},"settings":[
            {"id":"MAIN.A","type":"bool","default":1,"label":"A","presets":{"vanilla":0,"recommended":1,"enhanced":1}},
            {"id":"MAIN.B","type":"bool","default":1,"label":"B","visibility":"editor","presets":{"vanilla":0,"recommended":1,"enhanced":1}},
            {"id":"MAIN.C","type":"int","default":2,"min":0,"max":9,"label":"C","visibility":"file","presets":{"vanilla":0,"recommended":2,"enhanced":9}},
            {"id":"MAIN.D","type":"bool","default":0,"label":"D","visibility":"editor"},
            {"id":"MAIN.E","type":"bool","default":0,"label":"E","visibility":"file","presets":{"vanilla":1,"recommended":1,"enhanced":1}}]})", f.description));
        auto& set = f.description.settings;
        CHECK(set[0].InPresets() && set[1].InPresets() && set[2].InPresets() && !set[3].InPresets() && !set[4].InPresets());
        f.id = "P";
        auto game = std::filesystem::current_path() / L"preset-test";
        std::error_code ec;
        std::filesystem::remove_all(game, ec);
        std::filesystem::create_directories(game);
        f.path = (game / L"P.asi").wstring();
        asi_settings::Document doc;
        doc.Open(std::move(f), game / L"P.exe");
        CHECK(doc.CurrentPreset() == asi_settings::Preset::Recommended);
        // A file setting with differing preset values off its value: Custom
        CHECK(doc.Set("MAIN.C", "5") == ASI_SETTINGS_OK && doc.CurrentPreset() == asi_settings::Preset::Custom);
        CHECK(doc.Set("MAIN.C", "2") == ASI_SETTINGS_OK && doc.CurrentPreset() == asi_settings::Preset::Recommended);
        // Settings outside the presets edited by hand: still Recommended, and kept by a preset switch
        CHECK(doc.Set("MAIN.D", "1") == ASI_SETTINGS_OK && doc.CurrentPreset() == asi_settings::Preset::Recommended);
        auto changes = doc.PresetChanges(asi_settings::Preset::Vanilla);
        CHECK(changes.size() == 3);
        doc.ApplyPreset(asi_settings::Preset::Vanilla);
        CHECK(doc.Get("MAIN.A") == "0" && doc.Get("MAIN.B") == "0" && doc.Get("MAIN.C") == "0" && doc.Get("MAIN.D") == "1" && doc.Get("MAIN.E") == "0");
        CHECK(doc.CurrentPreset() == asi_settings::Preset::Vanilla);
        doc.ApplyPreset(asi_settings::Preset::Recommended);
        CHECK(doc.Get("MAIN.D") == "1");
        doc.ApplyPreset(asi_settings::Preset::Enhanced);
        CHECK(doc.Get("MAIN.C") == "9" && doc.Get("MAIN.D") == "1" && doc.Get("MAIN.E") == "0");
        // Back to the defaults (an empty settings file): everything, the hand-edited one too
        doc.ReadText("");
        CHECK(doc.Get("MAIN.D") == "0" && doc.Get("MAIN.C") == "2" && doc.CurrentPreset() == asi_settings::Preset::Recommended);
        std::filesystem::remove_all(game, ec);

        // Only a setting with the same value in all presets: the plugin has no presets
        asi_settings::Description d;
        CHECK(asi_settings::ParseDescription(R"({"format":"asi-settings","version":"0.1","plugin":{"name":"Q"},"settings":[
            {"id":"MAIN.B","type":"bool","default":1,"label":"B","visibility":"file","presets":{"vanilla":1,"recommended":1,"enhanced":1}}]})", d));
        std::vector<asi_settings::Slot> slots(1);
        slots[0].setting = &d.settings[0];
        asi_settings::SetDefault(slots[0]);
        CHECK(!d.settings[0].InPresets() && asi_settings::CurrentPreset(slots) == asi_settings::Preset::Custom);
    }

    // Compatibility settings (on a page with section "compatibility"): their pages come last, the file has them after
    // the advanced ones under their own banner; with preset values they take part in presets like any other
    void Compatibility()
    {
        asi_settings::Description d;
        CHECK(asi_settings::ParseDescription(R"({"format":"asi-settings","version":"0.1","plugin":{"name":"T"},"pages":[
            {"id":"compat","title":"Compatibility","section":"compatibility"},
            {"id":"main","title":"Main","section":"display"},
            {"id":"more","title":"More","section":"advanced"}],"settings":[
            {"id":"MAIN.X","type":"bool","default":0,"label":"X","page":"compat","presets":{"vanilla":1,"recommended":0,"enhanced":1}},
            {"id":"MAIN.A","type":"bool","default":0,"label":"A","presets":{"vanilla":1,"recommended":0,"enhanced":1}},
            {"id":"MAIN.Y","type":"bool","default":0,"label":"Y","page":"compat","visibility":"editor"},
            {"id":"MAIN.B","type":"bool","default":0,"label":"B","page":"more","visibility":"editor"},
            {"id":"MAIN.TextLanguage","type":"choice","default":"auto","choices":[{"value":"auto","label":"Auto"},{"value":"en","label":"English"}],"label":"L","page":"compat","visibility":"editor"},
            {"id":"OTHER.Z","type":"bool","default":0,"label":"Z","page":"compat","visibility":"file"}]})", d));
        CHECK(d.pages.size() == 3 && d.pages[0].id == "main" && d.pages[1].id == "more" && d.pages[2].id == "compat");
        CHECK(d.settings[0].compatibility && !d.settings[1].compatibility && d.settings[2].compatibility && !d.settings[3].compatibility);
        CHECK(d.settings[1].page == "main");                                    // the default page isn't the compatibility one
        CHECK(d.settings[0].InPresets() && d.settings[1].InPresets() && !d.settings[2].InPresets());
        std::vector<asi_settings::Slot> slots;
        for (auto& s : d.settings)
        {
            asi_settings::Slot slot;
            slot.setting = &s;
            asi_settings::SetDefault(slot);
            slots.push_back(std::move(slot));
        }
        CHECK(asi_settings::CurrentPreset(slots) == asi_settings::Preset::Recommended);
        asi_settings::Texts english;
        auto advanced = std::string(asi_settings::CfgAdvancedBanner) + "\r\n";
        auto compat = std::string(asi_settings::CfgCompatibilityBanner) + "\r\n";
        auto entry = [](const char* key)
        {
            return std::string("; ") + key + ". 0 = Off, 1 = On. Default: 0\r\n" + key + " = 0\r\n";
        };
        std::string expected = "; T settings\r\n"
            "; Change the settings here or in FusionFixHub; fixes with an in-game menu change them there too.\r\n"
            "\r\n[MAIN]\r\n" + entry("A") + "\r\n" + advanced + "\r\n" + entry("B") + "\r\n" + compat + "\r\n" + entry("X") + "\r\n" +
            entry("Y") + "\r\n; L. auto, en = English. Default: auto\r\nTextLanguage = auto\r\n\r\nMine = 1\r\n"
            "\r\n[OTHER]\r\n" + compat + "\r\n" + entry("Z");                    // only compatibility settings
        auto previous = asi_settings::ParseCfg("[MAIN]\r\nX = 0\r\nMine = 1\r\nA = 0\r\n" + compat + advanced);
        auto text = asi_settings::WriteCfg(d, "T", english, slots, &previous);
        CHECK(text == expected);
        // Writing the written file again gives the same bytes
        previous = asi_settings::ParseCfg(text);
        CHECK(asi_settings::WriteCfg(d, "T", english, slots, &previous) == expected);
    }

    void Plugin()
    {
        DeleteFileW(L"AsiSettingsTests.cfg");
        CHECK(settings.Load(GetModuleHandleW(nullptr)));
        CHECK(settings.GetDescription().settings.size() == 22);
        CHECK(!settings.Stored("GRAPHICS.FOV"));                        // a new file

        // Written in full on first load, with comments
        auto text = Cfg();
        CHECK(text.find("[GRAPHICS]\r\n") != std::string::npos);
        CHECK(text.find("; Field of View: Extra field of view of the driving cameras. 0 to 20%. Default: 0\r\nFOV = 0\r\n") != std::string::npos);
        CHECK(text.find("SkipIntro = 1") != std::string::npos);
        CHECK(text.find("(after restart)") != std::string::npos);
        // Advanced settings after the menu settings of their section
        auto graphics = text.find("[GRAPHICS]\r\n"), advanced = text.find(asi_settings::CfgAdvancedBanner, graphics);
        CHECK(advanced != std::string::npos && advanced < text.find("HideAABug = ") && text.find("SMAA = ") < advanced);

        // Defaults and typed references
        const int& gamma = settings.Int("GRAPHICS.ConsoleGamma");
        const float& fov = settings.Float("GRAPHICS.FOV");
        CHECK(gamma == 1 && fov == 0.0f);
        CHECK(settings.String("LANGUAGE.Language") == "English US");
        CHECK(settings.Int("MAIN.FPSLimit") == -1);

        // Changes through the class
        CHECK(settings.Set("GRAPHICS.FOV", "10") == ASI_SETTINGS_OK && fov == 10.0f);
        CHECK(Cfg().find("FOV = 10\r\n") != std::string::npos);
        CHECK(settings.Set("GRAPHICS.FOV", "30") == ASI_SETTINGS_INVALID_VALUE && fov == 10.0f);
        CHECK(settings.Set("GRAPHICS.SMAA", "maybe") == ASI_SETTINGS_INVALID_VALUE);
        CHECK(settings.Set("GRAPHICS.SMAA", "on") == ASI_SETTINGS_OK && settings.Int("GRAPHICS.SMAA") == 1);
        CHECK(settings.Set("NOPE.Nope", "1") == ASI_SETTINGS_UNKNOWN_ID);
        CHECK(settings.Set("MAIN.FPSLimit", "-2") == ASI_SETTINGS_OK);         // a special value below min
        CHECK(settings.Set("MAIN.FPSLimit", "-7") == ASI_SETTINGS_INVALID_VALUE);
        CHECK(settings.Set("GAMEPAD.GamepadIcons", "3") == ASI_SETTINGS_INVALID_VALUE);
        CHECK(settings.Set("LANGUAGE.Language", "german") == ASI_SETTINGS_OK && settings.String("LANGUAGE.Language") == "German");
        CHECK(settings.Set("MAIN.HudAspectRatioConstraint", "32:9") == ASI_SETTINGS_OK);

        // Apply callbacks
        int applied = 0;
        settings.OnChange("GRAPHICS.ConsoleGamma", [&] { ++applied; });
        settings.Set("GRAPHICS.ConsoleGamma", "0");
        CHECK(applied == 1 && gamma == 0);

        // Presets (the settings without preset values changed above don't count)
        settings.Set("GRAPHICS.ConsoleGamma", "1");
        settings.Set("GRAPHICS.FOV", "0");
        settings.Set("GRAPHICS.SMAA", "0");
        CHECK(settings.CurrentPreset() == asi_settings::Preset::Recommended);
        CHECK(!settings.ApplyPreset(asi_settings::Preset::Enhanced));             // only live settings change
        CHECK(settings.CurrentPreset() == asi_settings::Preset::Enhanced && fov == 10.0f && settings.Int("GRAPHICS.SMAA") == 1);
        CHECK(settings.ApplyPreset(asi_settings::Preset::Vanilla));               // Skip Intro applies after a restart
        CHECK(settings.CurrentPreset() == asi_settings::Preset::Vanilla);
        settings.Set("GRAPHICS.FOV", "5");
        CHECK(settings.CurrentPreset() == asi_settings::Preset::Custom);

        // Conditions
        settings.Set("CAMERA.FreeLook", "1");
        CHECK(settings.Enabled("CAMERA.StickSensitivity"));
        settings.Set("CAMERA.FreeLook", "0");
        CHECK(!settings.Enabled("CAMERA.StickSensitivity"));
        CHECK(settings.Visible("CAMERA.StickSensitivity"));

        // An editor changed the file: unknown keys and sections are kept, values that can't be read too, comments
        // after values are read
        WriteCfg("[GRAPHICS]\r\nFOV = 15 ; by hand\r\nSMAA = maybe\r\nMyOwnKey = 42\r\n[OTHERTOOL]\r\nX = y\r\n");
        CHECK(settings.Reload() == ASI_SETTINGS_OK);
        CHECK(settings.Stored("GRAPHICS.FOV") && !settings.Stored("GRAPHICS.ConsoleGamma"));
        CHECK(fov == 15.0f);
        CHECK(settings.Int("GRAPHICS.SMAA") == 0);        // the default
        settings.Save();
        text = Cfg();
        CHECK(text.find("SMAA = maybe") != std::string::npos);
        CHECK(text.find("MyOwnKey = 42") != std::string::npos);
        CHECK(text.find("[OTHERTOOL]\r\nX = y") != std::string::npos);
        CHECK(text.find("FOV = 15\r\n") != std::string::npos);
        CHECK(text.find("ConsoleGamma = 1") != std::string::npos);   // missing keys written with their defaults

        // The export, as a menu host sees it
        auto getApi = reinterpret_cast<AsiSettingsGetApi>(GetProcAddress(GetModuleHandleW(nullptr), ASI_SETTINGS_EXPORT_NAME));
        CHECK(getApi != nullptr);
        auto api = getApi ? getApi(ASI_SETTINGS_VERSION) : nullptr;
        CHECK(api && api->size == sizeof(AsiSettingsApi));
        if (api)
        {
            char tiny[2];
            CHECK(api->Get("GRAPHICS.FOV", tiny, sizeof(tiny)) == 2 && std::string(tiny) == "1");
            char value[32];
            CHECK(api->Get("GRAPHICS.FOV", value, sizeof(value)) == 2 && std::string(value) == "15");
            CHECK(api->Get("GRAPHICS.FOV", nullptr, 0) == 2);
            static int notified = 0;
            int token = api->Subscribe([](void*, const char* id) { if (std::string(id) == "GRAPHICS.FOV") ++notified; }, nullptr);
            CHECK(token > 0);
            CHECK(api->Set("GRAPHICS.FOV", "7") == ASI_SETTINGS_OK && fov == 7.0f && notified == 1);
            CHECK(api->Unsubscribe(token) == ASI_SETTINGS_OK);
            api->Set("GRAPHICS.FOV", "8");
            CHECK(notified == 1);
            CHECK(api->GetChoices("LANGUAGE.Language", value, sizeof(value)) == ASI_SETTINGS_UNSUPPORTED);
        }

        // Availability: windowed modes 1 and 2 need something that isn't there (like DLAA without its DLLs)
        static bool present = false;
        const int& windowed = settings.Int("MAIN.WindowedMode");
        settings.Set("MAIN.WindowedMode", "2");
        CHECK(windowed == 2);
        int windowedApplied = 0;
        settings.OnChange("MAIN.WindowedMode", [&] { ++windowedApplied; });
        settings.Availability("MAIN.WindowedMode", [](std::string_view v) { return present || (v != "1" && v != "2"); });
        CHECK(windowed == 0 && windowedApplied == 1);                           // the default is in effect
        CHECK(settings.Wanted("MAIN.WindowedMode") == "2");                     // the choice is kept
        CHECK(Cfg().find("WindowedMode = 2\r\n") != std::string::npos);
        CHECK(settings.Get("MAIN.WindowedMode") == "0");
        CHECK(settings.Set("MAIN.WindowedMode", "1") == ASI_SETTINGS_NOT_NOW); // menus can't pick it now
        CHECK(settings.UsableValues("MAIN.WindowedMode") == std::vector<std::string>({ "0", "3", "4", "5" }));
        if (api)
        {
            char choices[128], reason[8];
            CHECK(api->GetChoices("MAIN.WindowedMode", choices, sizeof(choices)) > 0 &&
                  std::string(choices) == "[{\"value\":0},{\"value\":3},{\"value\":4},{\"value\":5}]");
            CHECK(api->GetState("MAIN.WindowedMode", reason, sizeof(reason)) == 0);
            char value[8];
            CHECK(api->Get("MAIN.WindowedMode", value, sizeof(value)) == 1 && std::string(value) == "0");
        }
        // The default isn't usable either: the first usable value
        settings.Availability("MAIN.WindowedMode", [](std::string_view v) { return present || v == "4"; });
        CHECK(windowed == 4);
        // Nothing usable: hidden, the default in effect
        settings.Availability("MAIN.WindowedMode", [](std::string_view) { return present; });
        CHECK(settings.State("MAIN.WindowedMode") == ASI_SETTINGS_STATE_HIDDEN && windowed == 0);
        // It's there now: the kept choice comes back by itself
        present = true;
        settings.Refresh("MAIN.WindowedMode");
        CHECK(windowed == 2 && settings.State("MAIN.WindowedMode") == 0);
        settings.Availability("MAIN.WindowedMode", nullptr);

        // A menu host finds the plugin
        auto plugins = asi_settings::FindLoadedPlugins(nullptr, "fr-CA");
        CHECK(plugins.size() == 1 && plugins[0].api == api);
        if (!plugins.empty())
            CHECK(plugins[0].file.texts.Resolve("@fov") == "Field of View");    // fr-CA -> fr -> en
    }

    void Paths()
    {
        asi_settings::Description d;
        asi_settings::ParseDescription(R"({"format":"asi-settings","version":"0.1","plugin":{"name":"x","games":[
            {"name":"GTA III DE","exe":["Gameface/Binaries/Win64/LibertyCity.exe"]}]},
            "storage":{"file":"{root}/{name}.cfg"},"settings":[]})", d);
        auto root = asi_settings::InstallRoot(d, L"X:\\Games\\GTA III DE\\Gameface\\Binaries\\Win64\\LibertyCity.exe");
        CHECK(root == L"X:\\Games\\GTA III DE");
        auto path = asi_settings::ResolveStorage(d, { L"X:\\Games\\GTA III DE\\Gameface\\Binaries\\Win64\\plugins\\1.asi",
            L"X:\\Games\\GTA III DE\\Gameface\\Binaries\\Win64\\LibertyCity.exe" });
        CHECK(path == L"X:\\Games\\GTA III DE\\1.cfg");
        d.storageFile = "{name}.cfg";
        path = asi_settings::ResolveStorage(d, { L"C:\\G\\plugins\\Renamed.asi", L"C:\\G\\game.exe" });
        CHECK(path == L"C:\\G\\plugins\\Renamed.cfg");

        // The fallback: the same for every program, whatever case or slashes the path is written in
        auto files = asi_settings::ResolveStorageFiles(d, { L"C:\\Program Files\\G\\plugins\\Renamed.asi", L"C:\\Program Files\\G\\game.exe" });
        auto again = asi_settings::ResolveStorageFiles(d, { L"c:/program files/g/plugins/Renamed.asi", L"c:/program files/g/game.exe" });
        auto local = asi_settings::detail::KnownFolder(FOLDERID_LocalAppData);
        CHECK(asi_settings::detail::PathKey(files.fallback) == asi_settings::detail::PathKey(again.fallback));
        CHECK(files.fallback.parent_path().parent_path().parent_path() == local / L"FusionFixHub");
        CHECK(files.fallback.parent_path().parent_path().filename().wstring().starts_with(L"G-"));
        CHECK(files.fallback.filename() == L"Renamed.cfg" && files.fallback.parent_path().filename() == L"plugins");
        CHECK(files.virtualStore == local / L"VirtualStore\\Program Files\\G\\plugins\\Renamed.cfg");

        // Writable folders, judged with the standard token also when the test runs elevated
        wchar_t windows[MAX_PATH];
        GetSystemDirectoryW(windows, MAX_PATH);
        // Not where the user is an administrator without UAC (CI runners): there even the standard token may write there
        bool fullAdmin = false;
        if (HANDLE token; OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            TOKEN_ELEVATION_TYPE type{};
            DWORD size = 0;
            BYTE adminsSid[SECURITY_MAX_SID_SIZE];
            DWORD sidSize = sizeof(adminsSid);
            BOOL member = FALSE;
            if (GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size) && type == TokenElevationTypeDefault &&
                CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminsSid, &sidSize) && CheckTokenMembership(nullptr, adminsSid, &member))
                fullAdmin = member != FALSE;
            CloseHandle(token);
        }
        if (!fullAdmin)
            CHECK(!asi_settings::UserCanWrite(std::filesystem::path(windows) / L"asi-settings-test.cfg"));
        auto here = std::filesystem::current_path();
        CHECK(asi_settings::UserCanWrite(here / L"asi-settings-test.cfg"));
        CHECK(asi_settings::UserCanWrite(here / L"missing folder" / L"asi-settings-test.cfg"));

        // The newest file is in effect; the file itself when there's none
        asi_settings::StorageFiles test{ here / L"newest-a.cfg", here / L"newest-b.cfg", here / L"newest-c.cfg" };
        for (auto& f : { test.file, test.fallback, test.virtualStore })
            DeleteFileW(f.c_str());
        CHECK(asi_settings::NewestCfg(test) == test.file);
        auto touch = [](const std::filesystem::path& f, int64_t minutes)
        {
            asi_settings::WriteFileAtomic(f, "x");
            HANDLE h = CreateFileW(f.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER t{ { now.dwLowDateTime, now.dwHighDateTime } };
            t.QuadPart += minutes * 600000000LL;
            FILETIME time{ t.LowPart, t.HighPart };
            SetFileTime(h, nullptr, nullptr, &time);
            CloseHandle(h);
        };
        touch(test.file, -10);
        touch(test.fallback, -5);
        CHECK(asi_settings::NewestCfg(test) == test.fallback);
        touch(test.file, 0);
        CHECK(asi_settings::NewestCfg(test) == test.file);
        CHECK(asi_settings::CfgWriteTarget(test) == test.file);
        for (auto& f : { test.file, test.fallback, test.virtualStore })
            DeleteFileW(f.c_str());
    }

    // An editor: finds the test exe copied as a plugin into a game folder and edits its settings without running it
    void Editor()
    {
        using asi_settings::CfgLine;
        auto line = asi_settings::ParseCfgLine("  FOV = \"1; 2\" ");
        CHECK(line.kind == CfgLine::Kind::Entry && line.key == "FOV" && line.value == "1; 2");
        CHECK(asi_settings::ParseCfgLine("[ GRAPHICS ]").section == "GRAPHICS");
        CHECK(asi_settings::ParseCfgLine("# x").kind == CfgLine::Kind::Comment);
        CHECK(asi_settings::ParseCfgLine("junk").kind == CfgLine::Kind::Other);
        CHECK(asi_settings::ParseCfgLine(" \t").kind == CfgLine::Kind::Blank);

        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        auto game = std::filesystem::current_path() / L"editor-test";
        std::error_code ec;
        std::filesystem::remove_all(game, ec);
        std::filesystem::create_directories(game / L"scripts");
        std::filesystem::create_directories(game / L"plugins");
        CopyFileW(exe, (game / L"scripts" / L"Test.asi").c_str(), FALSE);
        CopyFileW(exe, (game / L"plugins" / L"Off.asi0").c_str(), FALSE);     // turned off: not a plugin
        asi_settings::WriteFileAtomic(game / L"Speed.exe", "");

        auto found = asi_settings::FindPluginFiles(game);
        CHECK(found.size() == 1);
        if (found.empty())
            return;
        auto gameExe = asi_settings::FindGameExe(found[0].description, found[0].path);
        CHECK(gameExe == game / L"Speed.exe");                                  // speed.exe in the root, found from scripts
        CHECK(asi_settings::InstallRoot(found[0].description, gameExe) == game);

        asi_settings::Document doc;
        doc.Open(std::move(found[0]), gameExe);
        CHECK(!doc.CfgExists() && doc.CfgPath() == game / L"scripts" / L"Test.cfg");
        CHECK(doc.Get("GRAPHICS.FOV") == "0" && doc.Get("LANGUAGE.Language") == "English US");
        CHECK(doc.CurrentPreset() == asi_settings::Preset::Recommended);
        CHECK(doc.Set("GRAPHICS.FOV", "30") == ASI_SETTINGS_INVALID_VALUE);
        CHECK(doc.Set("GRAPHICS.FOV", "12.5") == ASI_SETTINGS_OK && doc.Get("GRAPHICS.FOV") == "12.5");
        CHECK(doc.Set("NOPE.Nope", "1") == ASI_SETTINGS_UNKNOWN_ID);
        CHECK(doc.Set("LANGUAGE.Language", "russian") == ASI_SETTINGS_OK && doc.Get("LANGUAGE.Language") == "Russian");
        CHECK(doc.CurrentPreset() == asi_settings::Preset::Custom);
        CHECK(doc.WriteTarget() == doc.Storage().file);
        CHECK(doc.Save() && doc.CfgExists());
        auto text = asi_settings::ReadFileText(game / L"scripts" / L"Test.cfg").value_or("");
        CHECK(text.find("FOV = 12.5\r\n") != std::string::npos && text.find("[CAMERA]\r\n") != std::string::npos);
        CHECK(text == doc.Content() && text == doc.FileText());

        // A hand edit: unknown keys kept, an unreadable value kept as it is and shown as such
        asi_settings::WriteFileAtomic(game / L"scripts" / L"Test.cfg", "[GRAPHICS]\r\nFOV = 3\r\nSMAA = maybe\r\nMine = 1\r\n[CAMERA]\r\nFreeLook = 0\r\n");
        CHECK(doc.Read() && doc.Get("GRAPHICS.FOV") == "3" && doc.Get("GRAPHICS.SMAA") == "maybe");
        CHECK(doc.Find("GRAPHICS.SMAA")->unread == "maybe" && doc.Get("LANGUAGE.Language") == "English US");
        CHECK(!doc.Enabled("CAMERA.StickSensitivity") && doc.Visible("CAMERA.StickSensitivity"));
        CHECK(doc.FindKey("graphics", "fov") == doc.GetDescription().Find("GRAPHICS.FOV") && !doc.FindKey("GRAPHICS", "Mine"));
        CHECK(doc.Save());
        text = doc.FileText();
        CHECK(text.find("SMAA = maybe\r\n") != std::string::npos && text.find("Mine = 1\r\n") != std::string::npos);

        // Presets: the changes are known before applying, the unreadable value is replaced
        auto changes = doc.PresetChanges(asi_settings::Preset::Enhanced);
        CHECK(std::any_of(changes.begin(), changes.end(), [](auto& c) { return c.first->id == "GRAPHICS.SMAA" && c.second == "1"; }));
        CHECK(!doc.ApplyPreset(asi_settings::Preset::Enhanced));               // only live settings change
        CHECK(doc.CurrentPreset() == asi_settings::Preset::Enhanced && doc.Get("GRAPHICS.SMAA") == "1" && doc.Get("GRAPHICS.FOV") == "10");
        CHECK(doc.Enabled("CAMERA.StickSensitivity"));
        CHECK(doc.PresetChanges(asi_settings::Preset::Enhanced).empty());

        // Text from an editor's text view
        doc.ReadText("[GRAPHICS]\nFOV = 7\n");
        CHECK(doc.Get("GRAPHICS.FOV") == "7" && doc.Get("GRAPHICS.SMAA") == "0" && doc.Find("GRAPHICS.FOV")->stored);

        CHECK(!asi_settings::RunsAsOtherUser());
        std::filesystem::remove_all(game, ec);
    }

    void Texts()
    {
        asi_settings::Texts t;
        t.sources["en"] = R"({"a": "A", "b": "B"})";
        t.sources["pt"] = R"({"a": "Á"})";
        t.sources["pt-br"] = R"({"a": "Â"})";
        t.Select("pt-BR");
        CHECK(t.Resolve("@a") == "\xC3\x82" && t.Resolve("@b") == "B");
        t.Select("pt-PT");
        CHECK(t.Resolve("@a") == "\xC3\x81");
        t.Select("de");
        CHECK(t.Resolve("@a") == "A" && t.Resolve("@missing") == "missing");
        CHECK(t.Resolve("@@at") == "@at" && t.Resolve("plain") == "plain");
    }

    // A 32-bit little endian MIPS ELF with these sections, as console plugins are built
    std::string MakeElf(const std::vector<std::pair<std::string, std::string>>& sections)
    {
        auto put16 = [](std::string& s, size_t at, uint16_t v) { s[at] = char(v & 0xFF); s[at + 1] = char(v >> 8); };
        auto put32 = [&](std::string& s, size_t at, uint32_t v) { put16(s, at, uint16_t(v & 0xFFFF)); put16(s, at + 2, uint16_t(v >> 16)); };
        std::string names(1, '\0'), body;
        struct Entry
        {
            uint32_t name, type, offset, size;
        };
        std::vector<Entry> entries = { {} };
        for (auto& [name, data] : sections)
        {
            entries.push_back({ uint32_t(names.size()), 1, uint32_t(52 + body.size()), uint32_t(data.size()) });
            names += name + '\0';
            body += data;
        }
        entries.push_back({ uint32_t(names.size()), 3, uint32_t(52 + body.size()), 0 });
        names += ".shstrtab";
        names.push_back('\0');
        entries.back().size = uint32_t(names.size());
        body += names;
        std::string elf(52, '\0');
        elf.replace(0, 4, "\x7F" "ELF");
        elf[4] = 1;
        elf[5] = 1;
        elf[6] = 1;
        put16(elf, 16, 1);                              // ET_REL
        put16(elf, 18, 8);                              // EM_MIPS
        put32(elf, 32, uint32_t(52 + body.size()));     // section headers after the data
        put16(elf, 40, 52);
        put16(elf, 46, 40);
        put16(elf, 48, uint16_t(entries.size()));
        put16(elf, 50, uint16_t(entries.size() - 1));
        elf += body;
        for (auto& e : entries)
        {
            std::string header(40, '\0');
            put32(header, 0, e.name);
            put32(header, 4, e.type);
            put32(header, 16, e.offset);
            put32(header, 20, e.size);
            elf += header;
        }
        return elf;
    }

    // Console plugins (SPEC.md, Console plugins): description files next to the module or its ELF sections, read as
    // data within the limits; the settings file stays beside the module
    void ConsolePlugins()
    {
        CHECK(asi_settings::IsLanguageTag("en") && asi_settings::IsLanguageTag("pt-BR") && asi_settings::IsLanguageTag("zh-Hans"));
        CHECK(!asi_settings::IsLanguageTag("en_US") && !asi_settings::IsLanguageTag("x") && !asi_settings::IsLanguageTag("") && !asi_settings::IsLanguageTag("en-"));
        CHECK(asi_settings::IsConsolePlugin(L"a\\B.PRX") && asi_settings::IsConsolePlugin(L"b.elf") && !asi_settings::IsConsolePlugin(L"c.asi"));

        auto root = std::filesystem::current_path() / L"console-test";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        auto psp = root / L"memstick" / L"PSP" / L"PLUGINS" / L"Test.Console";
        std::filesystem::create_directories(psp);
        auto description = [](std::string storage = {}, std::string name = "@plugin.name")
        {
            return R"({"format": "asi-settings", "version": "1.0", "plugin": {"name": ")" + name + R"(", "version": "2.0"},)" +
                (storage.empty() ? std::string() : R"("storage": {"file": ")" + storage + R"("},)") +
                R"("settings": [{"id": "MAIN.SkipIntro", "type": "bool", "default": 1, "label": "@skip", "presets": {"vanilla": 0}}]})";
        };
        auto module = psp / L"Test.Console.prx";
        asi_settings::WriteFileAtomic(module, MakeElf({}));
        asi_settings::WriteFileAtomic(psp / L"Test.Console.asisettings.json", description());
        asi_settings::WriteFileAtomic(psp / L"Test.Console.asisettings.en.json", R"({"plugin.name": "Test Console", "skip": "Skip intro"})");
        asi_settings::WriteFileAtomic(psp / L"Test.Console.asisettings.de.json", R"({"skip": "Intro überspringen"})");
        asi_settings::WriteFileAtomic(psp / L"Test.Console.asisettings.en_US.json", "{}");      // not a language tag: ignored
        asi_settings::WriteFileAtomic(psp / L"Other.asisettings.fr.json", "{}");                // another plugin's

        asi_settings::DescribedFile file;
        std::string error;
        CHECK(asi_settings::ReadPluginFile(module.wstring(), file, &error));
        CHECK(file.id == "Test.Console" && file.description.pluginVersion == "2.0" && file.description.settings.size() == 1);
        CHECK(file.texts.sources.size() == 2 && file.texts.sources.count("en") && file.texts.sources.count("de"));
        CHECK(file.texts.Resolve(file.description.name) == "Test Console");
        asi_settings::Document doc;
        doc.Open(std::move(file), {});
        CHECK(doc.CfgPath() == psp / L"Test.Console.cfg" && !doc.CfgExists());
        CHECK(doc.Set("MAIN.SkipIntro", "0") == ASI_SETTINGS_OK && doc.Save());
        CHECK(asi_settings::ReadFileText(psp / L"Test.Console.cfg").value_or("").find("[MAIN]\r\n") != std::string::npos);

        // The settings file stays in the plugin's folder
        for (auto [storage, ok] : { std::pair{ "{name}.ini", true }, std::pair{ "{plugin}/sub/{name}.cfg", true }, std::pair{ "../{name}.cfg", false },
            std::pair{ "sub/../../x.cfg", false }, std::pair{ "{game}/x.cfg", false }, std::pair{ "{documents}/x.cfg", false }, std::pair{ "C:/x.cfg", false },
            std::pair{ "/x.cfg", false }, std::pair{ "C:x.cfg", false }, std::pair{ "x.cfg:stream", false }, std::pair{ ".", false } })
        {
            asi_settings::WriteFileAtomic(psp / L"Test.Console.asisettings.json", description(storage));
            asi_settings::DescribedFile f;
            bool read = asi_settings::ReadPluginFile(module.wstring(), f);
            if (read != ok)
                std::printf("storage.file %s\n", storage);
            CHECK(read == ok);
        }

        // Malformed and oversized files, a folder in a file's place, a missing module
        asi_settings::WriteFileAtomic(psp / L"Test.Console.asisettings.json", "{\"format\": \"asi-settings\", ");
        CHECK(!asi_settings::ReadPluginFile(module.wstring(), file));
        asi_settings::WriteFileAtomic(psp / L"Test.Console.asisettings.json", description(std::string(), std::string(asi_settings::MaxDescriptionSize, 'x')));
        CHECK(!asi_settings::ReadPluginFile(module.wstring(), file, &error) && error.find("larger than") != std::string::npos);
        asi_settings::WriteFileAtomic(psp / L"Test.Console.asisettings.json", description());
        asi_settings::WriteFileAtomic(psp / L"Test.Console.asisettings.fr.json", std::string(asi_settings::MaxDescriptionSize + 1, ' '));
        CHECK(!asi_settings::ReadPluginFile(module.wstring(), file));
        DeleteFileW((psp / L"Test.Console.asisettings.fr.json").c_str());
        std::filesystem::create_directories(psp / L"Test.Console.asisettings.it.json");     // a folder: left out
        CHECK(asi_settings::ReadPluginFile(module.wstring(), file) && !file.texts.sources.count("it"));
        CHECK(!asi_settings::ReadPluginFile((psp / L"Missing.prx").wstring(), file));
        std::filesystem::create_directories(psp / L"Folder.prx");
        CHECK(!asi_settings::ReadPluginFile((psp / L"Folder.prx").wstring(), file));

        // ELF sections, padded with zeros, when there are no description files
        auto ps2 = root / L"PLUGINS";
        std::filesystem::create_directories(ps2);
        auto elf = ps2 / L"Test.Ps2.elf";
        asi_settings::WriteFileAtomic(elf, MakeElf({ { ".text", std::string(64, '\x01') }, { ".asisettings", description("{name}.ini") + std::string(13, '\0') },
            { ".asisettings.en", R"({"plugin.name": "PS2 Test"})" }, { ".asisettings.pt-BR", R"({"plugin.name": "Teste"})" }, { ".asisettings.bad_tag", "{}" } }));
        CHECK(asi_settings::ReadPluginFile(elf.wstring(), file, &error) && file.id == "Test.Ps2" && file.texts.sources.size() == 2 && file.texts.sources.count("pt-br"));
        doc.Open(std::move(file), {});
        CHECK(doc.CfgPath() == ps2 / L"Test.Ps2.ini");
        // Description files win over the sections
        asi_settings::WriteFileAtomic(ps2 / L"Test.Ps2.asisettings.json", description({}, "Beside"));
        CHECK(asi_settings::ReadPluginFile(elf.wstring(), file) && file.description.name == "Beside" && file.texts.sources.empty());
        DeleteFileW((ps2 / L"Test.Ps2.asisettings.json").c_str());

        // ELF files that lie about their sections, or have none
        auto bad = ps2 / L"Bad.elf";
        auto good = MakeElf({ { ".asisettings", description() } });
        auto patched = [&](size_t at, uint32_t value)
        {
            auto copy = good;
            std::memcpy(copy.data() + at, &value, 4);
            return copy;
        };
        uint32_t table = 0;
        std::memcpy(&table, good.data() + 32, 4);
        for (auto& data : { std::string("\x7F" "ELF"), good.substr(0, 60), patched(32, 0xFFFFFFF0), patched(table + 40 + 16, 0x7FFFFFFF),
            patched(table + 40 + 20, 0xFFFFFFFF), MakeElf({ { ".text", "x" } }), std::string(4096, 'E') })
        {
            asi_settings::WriteFileAtomic(bad, data);
            CHECK(!asi_settings::ReadPluginFile(bad.wstring(), file));
        }
        asi_settings::WriteFileAtomic(bad, good);
        CHECK(asi_settings::ReadPluginFile(bad.wstring(), file));
        std::filesystem::remove_all(root, ec);
    }

    // A client of the live link, as an editor would be: requests and their replies, events kept in between
    struct LinkClient
    {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        std::string pending;
        std::vector<asi_settings::json::Value> events;
        int next = 1;

        ~LinkClient() { Close(); }

        // The link's thread makes the pipe a moment after Start: waits for it up to timeout
        bool Open(DWORD timeout = 2000)
        {
            wchar_t name[64];
            swprintf_s(name, L"%s%lu", ASI_SETTINGS_LINK_PIPE, GetCurrentProcessId());
            ULONGLONG until = GetTickCount64() + timeout;
            for (;;)
            {
                pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (pipe != INVALID_HANDLE_VALUE)
                    return true;
                if (GetLastError() == ERROR_PIPE_BUSY)
                    WaitNamedPipeW(name, 100);
                else
                    Sleep(10);
                if (GetTickCount64() > until)
                    return false;
            }
        }

        void Close()
        {
            if (pipe != INVALID_HANDLE_VALUE)
                CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }

        bool Send(const std::string& line)
        {
            std::string data = line + "\n";
            DWORD written = 0;
            return WriteFile(pipe, data.data(), DWORD(data.size()), &written, nullptr) && written == data.size();
        }

        // The next line, empty when none came within the time
        std::string Line(DWORD timeout = 3000)
        {
            ULONGLONG until = GetTickCount64() + timeout;
            for (;;)
            {
                if (auto end = pending.find('\n'); end != std::string::npos)
                {
                    auto line = pending.substr(0, end);
                    pending.erase(0, end + 1);
                    return line;
                }
                DWORD available = 0;
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
                    return {};
                if (available)
                {
                    std::string buffer(available, '\0');
                    DWORD read = 0;
                    if (!ReadFile(pipe, buffer.data(), available, &read, nullptr))
                        return {};
                    pending.append(buffer.data(), read);
                    continue;
                }
                if (GetTickCount64() > until)
                    return {};
                Sleep(2);
            }
        }

        // Sends a request ({"cmd":...} without its id) and returns its reply; events that come first are kept
        asi_settings::json::Value Request(const std::string& body)
        {
            int id = next++;
            asi_settings::json::Value reply;
            if (!Send("{\"id\":" + std::to_string(id) + "," + body.substr(1)))
                return reply;
            for (std::string line; !(line = Line()).empty();)
            {
                asi_settings::json::Value v;
                if (!asi_settings::json::Parse(line, v))
                    continue;
                if (v.Find("event"))
                {
                    events.push_back(std::move(v));
                    continue;
                }
                if (v.Num("id", -1) == id)
                    return v;
            }
            return reply;
        }

        // Waits for a change event of a setting; its value, or "(none)"
        std::string Event(std::string_view plugin, std::string_view setting, DWORD timeout = 3000)
        {
            ULONGLONG until = GetTickCount64() + timeout;
            for (;;)
            {
                for (size_t i = 0; i < events.size(); ++i)
                {
                    auto& e = events[i];
                    if (e.Str("event") == "settings.changed" && e.Str("plugin") == plugin && e.Str("setting") == setting)
                    {
                        std::string value(e.Str("value"));
                        events.erase(events.begin() + ptrdiff_t(i));
                        return value;
                    }
                }
                ULONGLONG now = GetTickCount64();
                if (now > until)
                    return "(none)";
                auto line = Line(DWORD(until - now));
                asi_settings::json::Value v;
                if (!line.empty() && asi_settings::json::Parse(line, v) && v.Find("event"))
                    events.push_back(std::move(v));
            }
        }
    };

    // The live link: two plugins (LinkPluginA.dll, LinkPluginB.dll) loaded into this exe, which plays the game and is a
    // plugin itself; the first plugin to start the link serves all of them. A client reads, changes and watches their
    // settings through the pipe, and the game's own menu changes values too.
    void LiveLink()
    {
        using Init = bool(__cdecl*)();
        using Stop = void(__cdecl*)();
        using SetFn = int32_t(__cdecl*)(const char*, const char*);
        using UsableFn = void(__cdecl*)(bool);
        auto folder = std::filesystem::current_path();
        for (auto name : { L"LinkPluginA.cfg", L"LinkPluginB.cfg" })
            DeleteFileW((folder / name).c_str());
        HMODULE a = LoadLibraryW((folder / L"LinkPluginA.dll").c_str());
        HMODULE b = LoadLibraryW((folder / L"LinkPluginB.dll").c_str());
        CHECK(a && b);
        if (!a || !b)
            return;
        auto fn = [](HMODULE m, const char* name) { return GetProcAddress(m, name); };
        CHECK(reinterpret_cast<Init>(fn(a, "LinkTest_Init"))());     // A starts first: it serves
        CHECK(!reinterpret_cast<Init>(fn(b, "LinkTest_Init"))());    // B finds the link served
        auto setA = reinterpret_cast<SetFn>(fn(a, "LinkTest_Set"));
        auto setB = reinterpret_cast<SetFn>(fn(b, "LinkTest_Set"));
        auto usableB = reinterpret_cast<UsableFn>(fn(b, "LinkTest_Usable"));

        LinkClient client;
        CHECK(client.Open());
        ULONG server = 0;
        CHECK(GetNamedPipeServerProcessId(client.pipe, &server) && server == GetCurrentProcessId());

        // hello: the game and its three plugins (this exe's NFSU description too), in editor order
        auto hello = client.Request(R"({"cmd":"hello","protocol":1})");
        CHECK(hello.Bool("ok") && hello.Num("protocol") == ASI_SETTINGS_LINK_PROTOCOL && hello.Num("pid") == GetCurrentProcessId());
        auto plugins = hello.Find("plugins");
        CHECK(plugins && plugins->items.size() == 3);
        if (plugins && plugins->items.size() == 3)
        {
            CHECK(plugins->items[0].Str("name") == "Widescreen Fix" && plugins->items[1].Str("id") == "LinkA" && plugins->items[2].Str("id") == "LinkB");
            CHECK(plugins->items[1].Str("name") == "Link A" && std::string(plugins->items[1].Str("path")).ends_with("LinkPluginA.dll"));
        }

        // list, get
        auto list = client.Request(R"({"cmd":"settings.list","plugin":"linka"})");
        auto values = list.Find("values");
        CHECK(list.Bool("ok") && values && values->items.size() == 4);
        if (values && values->items.size() == 4)
            CHECK(values->items[1].Str("setting") == "MAIN.Level" && values->items[1].Str("value") == "3" && values->items[1].Num("state", -1) == 0);
        auto get = client.Request(R"({"cmd":"settings.get","plugin":"LinkB","setting":"main.speed"})");
        CHECK(get.Bool("ok") && get.Str("value") == "1.5");
        CHECK(!client.Request(R"({"cmd":"settings.get","plugin":"Nope","setting":"x"})").Bool("ok", true));
        CHECK(client.Request(R"({"cmd":"settings.get","plugin":"LinkA","setting":"x"})").Num("result") == ASI_SETTINGS_UNKNOWN_ID);

        // set: checked and stored by the plugin, the value in effect returned; the change comes back as an event
        CHECK(client.Request(R"({"cmd":"subscribe"})").Bool("ok"));
        auto set = client.Request(R"({"cmd":"settings.set","plugin":"LinkA","setting":"MAIN.Level","value":"7"})");
        CHECK(set.Bool("ok") && set.Num("result", -9) == 0 && set.Str("value") == "7");
        CHECK(client.Event("LinkA", "MAIN.Level") == "7");
        auto cfgA = asi_settings::ReadFileText(folder / L"LinkPluginA.cfg").value_or("");
        CHECK(cfgA.find("Level = 7\r\n") != std::string::npos);
        set = client.Request(R"({"cmd":"settings.set","plugin":"LinkA","setting":"MAIN.Level","value":"11"})");
        CHECK(!set.Bool("ok", true) && set.Num("result") == ASI_SETTINGS_INVALID_VALUE && set.Str("value") == "7");
        set = client.Request(R"({"cmd":"settings.set","plugin":"LinkA","setting":"MAIN.Name","value":"a \"b\"; c"})");
        CHECK(set.Bool("ok") && set.Str("value") == "a \"b\"; c");
        CHECK(client.Event("LinkA", "MAIN.Name") == "a \"b\"; c");

        // The game's menu changes a value: the client hears of it
        CHECK(setB("MAIN.Speed", "2.5") == ASI_SETTINGS_OK);
        CHECK(client.Event("LinkB", "MAIN.Speed") == "2.5");

        // A hand edit of B's file: B doesn't serve the link, it watches its own file all the same
        auto cfgB = folder / L"LinkPluginB.cfg";
        CHECK(WriteInPlace(cfgB, Replaced(asi_settings::ReadFileText(cfgB).value_or(""), "Speed = 2.5\r\n", "Speed = 3\r\n")));
        CHECK(client.Event("LinkB", "MAIN.Speed") == "3");
        CHECK(setB("MAIN.Speed", "2.5") == ASI_SETTINGS_OK && client.Event("LinkB", "MAIN.Speed") == "2.5");

        // Availability: Mode 2 can't be used now, its choices leave it out
        set = client.Request(R"({"cmd":"settings.set","plugin":"LinkB","setting":"MAIN.Mode","value":"2"})");
        CHECK(!set.Bool("ok", true) && set.Num("result") == ASI_SETTINGS_NOT_NOW && set.Str("value") == "0");
        auto choices = client.Request(R"({"cmd":"settings.choices","plugin":"LinkB","setting":"MAIN.Mode"})");
        CHECK(choices.Bool("ok") && choices.Find("choices") && choices.Find("choices")->items.size() == 2);
        auto state = client.Request(R"({"cmd":"settings.state","plugin":"LinkB","setting":"MAIN.Mode"})");
        CHECK(state.Bool("ok") && state.Num("state", -1) == 0 && state.Str("reason") == "Mode 2 needs the test's runtime");
        // ...until what it needs appears: the event says so, and it can be set
        usableB(true);
        CHECK(client.Event("LinkB", "MAIN.Mode") == "0");
        set = client.Request(R"({"cmd":"settings.set","plugin":"LinkB","setting":"MAIN.Mode","value":2})");
        CHECK(set.Bool("ok") && set.Str("value") == "2");
        CHECK(client.Event("LinkB", "MAIN.Mode") == "2");
        // ...and goes away again: the value in effect falls back, the stored choice stays
        usableB(false);
        CHECK(client.Event("LinkB", "MAIN.Mode") == "0");
        CHECK(asi_settings::ReadFileText(cfgB).value_or("").find("Mode = 2\r\n") != std::string::npos);

        // Presets: every setting whose preset values differ, editor-only ones too; values not usable now are refused and listed
        auto preset = client.Request(R"({"cmd":"settings.preset","plugin":"LinkA","preset":"enhanced"})");
        CHECK(preset.Bool("ok") && preset.Find("changed") && preset.Find("changed")->items.size() == 2 && !preset.Bool("restart", true));
        CHECK(client.Request(R"({"cmd":"settings.get","plugin":"LinkA","setting":"MAIN.Level"})").Str("value") == "10");
        CHECK(client.Request(R"({"cmd":"settings.get","plugin":"LinkA","setting":"MAIN.Hidden"})").Str("value") == "9");
        CHECK(client.Event("LinkA", "MAIN.Level") == "10");
        preset = client.Request(R"({"cmd":"settings.preset","plugin":"LinkB","preset":"vanilla"})");
        CHECK(preset.Bool("ok") && preset.Find("changed") && preset.Find("changed")->items.size() == 1 && preset.Bool("restart"));
        preset = client.Request(R"({"cmd":"settings.preset","plugin":"LinkB","preset":2})");
        auto refused = preset.Find("refused");
        CHECK(preset.Bool("ok") && refused && refused->items.size() == 1 && refused->items[0].Str("setting") == "MAIN.Mode" &&
            refused->items[0].Num("result") == ASI_SETTINGS_NOT_NOW);
        CHECK(!client.Request(R"({"cmd":"settings.preset","plugin":"LinkB","preset":"loud"})").Bool("ok", true));

        // Reload: a file changed while the game runs
        asi_settings::WriteFileAtomic(folder / L"LinkPluginA.cfg", "[MAIN]\r\nLevel = 4\r\n");
        CHECK(client.Request(R"({"cmd":"settings.reload","plugin":"LinkA"})").Bool("ok"));
        CHECK(client.Event("LinkA", "MAIN.Level") == "4");

        // Junk, then a second client at the same time
        CHECK(client.Send("not json"));
        asi_settings::json::Value junk;
        while (asi_settings::json::Parse(client.Line(), junk) && junk.Find("event"))
            ;   // the other settings the reload changed
        CHECK(junk.Find("ok") && !junk.Bool("ok", true));
        LinkClient second;
        CHECK(second.Open() && second.Request(R"({"cmd":"settings.get","plugin":"LinkA","setting":"MAIN.Level"})").Str("value") == "4");
        CHECK(second.Request(R"({"cmd":"settings.set","plugin":"LinkA","setting":"MAIN.Gamma","value":"0"})").Bool("ok"));
        CHECK(client.Event("LinkA", "MAIN.Gamma") == "0");
        CHECK(setA("MAIN.Gamma", "1") == ASI_SETTINGS_OK && client.Event("LinkA", "MAIN.Gamma") == "1");
        second.Close();

        // A stops serving: the pipe goes, B can serve instead
        client.Close();
        reinterpret_cast<Stop>(fn(a, "LinkTest_Stop"))();
        CHECK(!reinterpret_cast<Init>(fn(a, "LinkTest_Serving"))());
        LinkClient gone;
        CHECK(!gone.Open(200));
        CHECK(reinterpret_cast<Init>(fn(b, "LinkTest_Start"))());
        LinkClient again;
        CHECK(again.Open() && again.Request(R"({"cmd":"settings.get","plugin":"LinkA","setting":"MAIN.Level"})").Str("value") == "4");
        again.Close();
        reinterpret_cast<Stop>(fn(b, "LinkTest_Stop"))();
        // The plugins stay loaded: a change callback may still point into them
        for (auto name : { L"LinkPluginA.cfg", L"LinkPluginB.cfg" })
            DeleteFileW((folder / name).c_str());
    }

    DWORD WINAPI LoadPluginC(void* module)
    {
        *static_cast<HMODULE*>(module) = LoadLibraryW((std::filesystem::current_path() / L"LinkPluginC.dll").c_str());
        return 0;
    }

    // Hand edits: LinkPluginC.dll loads its settings, watches its file and starts the live link from DllMain, under
    // the loader lock. Another program changes the file the ways editors save; the plugin reads it again each time,
    // and its own writes read nothing.
    void HandEdits()
    {
        using IntFn = int(__cdecl*)();
        using BoolFn = bool(__cdecl*)();
        using SetFn = int32_t(__cdecl*)(const char*, const char*);
        using StopFn = void(__cdecl*)();
        auto folder = std::filesystem::current_path();
        auto cfg = folder / L"LinkPluginC.cfg";
        DeleteFileW(cfg.c_str());

        // Loaded on a thread of its own: a DllMain that waits for a thread fails the test instead of hanging it
        HMODULE c = nullptr;
        HANDLE loader = CreateThread(nullptr, 0, &LoadPluginC, &c, 0, nullptr);
        if (!loader || WaitForSingleObject(loader, 10000) != WAIT_OBJECT_0)
        {
            std::printf("FAILED: LinkPluginC.dll's DllMain didn't return\n");
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 1);
        }
        CloseHandle(loader);
        CHECK(c != nullptr);
        if (!c)
            return;
        auto applied = reinterpret_cast<IntFn>(GetProcAddress(c, "WatchTest_Applied"));
        auto level = reinterpret_cast<IntFn>(GetProcAddress(c, "WatchTest_Level"));
        auto reloads = reinterpret_cast<IntFn>(GetProcAddress(c, "WatchTest_Reloads"));
        auto watching = reinterpret_cast<BoolFn>(GetProcAddress(c, "WatchTest_Watching"));
        auto set = reinterpret_cast<SetFn>(GetProcAddress(c, "LinkTest_Set"));
        auto stop = reinterpret_cast<StopFn>(GetProcAddress(c, "LinkTest_Stop"));
        CHECK(watching() && level() == 3);
        CHECK(asi_settings::ReadFileText(cfg).value_or("").find("Level = 3\r\n") != std::string::npos);
        LinkClient client;
        CHECK(client.Open() && client.Request(R"({"cmd":"subscribe"})").Bool("ok"));

        // Saved in place: the change callback runs and the link reports it, as for a change from a menu
        CHECK(WriteInPlace(cfg, Replaced(asi_settings::ReadFileText(cfg).value_or(""), "Level = 3\r\n", "Level = 6\r\n")));
        CHECK(client.Event("WatchC", "MAIN.Level") == "6");
        CHECK(level() == 6 && applied() == 1 && reloads() == 1);

        // The plugin's own write: applied once, the file isn't read again
        CHECK(set("MAIN.Level", "2") == ASI_SETTINGS_OK && level() == 2 && applied() == 2);
        CHECK(client.Event("WatchC", "MAIN.Level") == "2");
        CHECK(client.Event("WatchC", "MAIN.Level", 800) == "(none)");
        CHECK(applied() == 2 && reloads() == 1);

        // Saved to a temporary file that is renamed over the settings file
        auto temporary = folder / L"LinkPluginC.cfg~1a2b";
        CHECK(asi_settings::WriteFileAtomic(temporary, Replaced(asi_settings::ReadFileText(cfg).value_or(""), "Level = 2\r\n", "Level = 8\r\n")));
        CHECK(MoveFileExW(temporary.c_str(), cfg.c_str(), MOVEFILE_REPLACE_EXISTING));
        CHECK(client.Event("WatchC", "MAIN.Level") == "8" && level() == 8);

        // Replaced with a backup of the old file (ReplaceFile)
        auto backup = folder / L"LinkPluginC.cfg.bak";
        CHECK(asi_settings::WriteFileAtomic(temporary, Replaced(asi_settings::ReadFileText(cfg).value_or(""), "Level = 8\r\n", "Level = 9\r\n")));
        CHECK(ReplaceFileW(cfg.c_str(), temporary.c_str(), backup.c_str(), 0, nullptr, nullptr));
        CHECK(client.Event("WatchC", "MAIN.Level") == "9" && level() == 9);
        DeleteFileW(backup.c_str());

        // Kept locked for a moment while it's written: read once it's free
        CHECK(WriteInPlace(cfg, Replaced(asi_settings::ReadFileText(cfg).value_or(""), "Level = 9\r\n", "Level = 5\r\n"), 0, 600));
        CHECK(client.Event("WatchC", "MAIN.Level") == "5" && level() == 5);

        // Only the settings file counts
        CHECK(WriteInPlace(folder / L"LinkPluginC.log", "Level = 1\r\n"));
        CHECK(client.Event("WatchC", "MAIN.Level", 600) == "(none)");
        DeleteFileW((folder / L"LinkPluginC.log").c_str());

        // Deleted: the values stay, the next change writes it again
        CHECK(DeleteFileW(cfg.c_str()));
        CHECK(client.Event("WatchC", "MAIN.Level", 600) == "(none)" && level() == 5);
        CHECK(set("MAIN.Level", "4") == ASI_SETTINGS_OK && client.Event("WatchC", "MAIN.Level") == "4");
        CHECK(asi_settings::ReadFileText(cfg).value_or("").find("Level = 4\r\n") != std::string::npos);

        // Stopped with the link: hand edits aren't read any more
        int before = reloads();
        client.Close();
        stop();
        CHECK(!watching());
        CHECK(WriteInPlace(cfg, Replaced(asi_settings::ReadFileText(cfg).value_or(""), "Level = 4\r\n", "Level = 1\r\n")));
        Sleep(600);
        CHECK(level() == 4 && reloads() == before);
        DeleteFileW(cfg.c_str());
    }
}

int wmain()
{
    // A failure is printed, never shown in a dialog
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
    for (int type : { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT })
    {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    // The test's .cfg is next to the exe
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SetCurrentDirectoryW(std::filesystem::path(exe).parent_path().c_str());

    Json();
    Versions();
    Numbers();
    CfgParsing();
    CfgOrder();
    PresetsMenuOnly();
    Compatibility();
    Plugin();
    Paths();
    Editor();
    Texts();
    ConsolePlugins();
    LiveLink();
    HandEdits();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
