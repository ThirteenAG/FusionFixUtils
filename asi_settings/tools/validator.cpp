// ASI Settings validator: checks a description against SPEC.md. Plugins run it in their build, so that a broken
// description never ships.
//
//   asi-settings-validate <description.json> [<translation.json>...]
//   asi-settings-validate <plugin.asi or .dll>          (the description and translations in its resources)
//   asi-settings-validate <plugin.prx or .elf>          (a console plugin: its description files, or its ELF sections)
//
// Exit code 0 when there are no errors (warnings don't fail), 1 otherwise.
#include "../asi_settings.hpp"
#include <cstdio>
#include <set>

namespace
{
    using namespace asi_settings;

    int errors = 0, warnings = 0;
    bool consolePlugin = false;     // a .prx or .elf: its settings file stays in its folder (SPEC.md, Console plugins)

    void Error(const std::string& where, const std::string& message)
    {
        std::printf("error: %s: %s\n", where.c_str(), message.c_str());
        ++errors;
    }

    void Warning(const std::string& where, const std::string& message)
    {
        std::printf("warning: %s: %s\n", where.c_str(), message.c_str());
        ++warnings;
    }

    bool OneOf(std::string_view value, std::initializer_list<std::string_view> allowed)
    {
        return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
    }

    // Every "@key" of the description, to check against the English translation
    void CollectKeys(const json::Value& v, std::set<std::string>& keys)
    {
        if (v.IsString() && v.string.size() > 1 && v.string[0] == '@' && v.string[1] != '@')
            keys.insert(v.string.substr(1));
        for (auto& item : v.items)
            CollectKeys(item, keys);
        for (auto& [name, member] : v.members)
        {
            if (name != "hosts" && name != "extensions")
                CollectKeys(member, keys);
        }
    }

    void CheckValue(const Setting& s, const json::Value& v, const std::string& where, bool allowOutOfRange)
    {
        Slot slot;
        slot.setting = &s;
        if (!(v.IsNumber() || v.IsString() || v.IsBool()))
        {
            Error(where, "has no value");
            return;
        }
        if (!ParseValue(slot, detail::JsonToText(v), !allowOutOfRange))
            Error(where, "\"" + detail::JsonToText(v) + "\" isn't a valid value of this setting");
    }

    // Fields of 1.0 (SPEC.md); others are ignored by readers, so a misspelt field silently does nothing: warned
    bool warnUnknown = true;

    void KnownFields(const json::Value* v, std::initializer_list<std::string_view> known, const std::string& where)
    {
        if (!warnUnknown || !v || !v->IsObject())
            return;
        for (auto& [name, member] : v->members)
        {
            if (!OneOf(name, known))
                Warning(where, "unknown field \"" + name + "\" (ignored)");
        }
    }

    void KnownConditionFields(const json::Value* v, const std::string& where)
    {
        if (!v || !v->IsObject())
            return;
        KnownFields(v, { "setting", "equals", "notEquals", "in", "all", "any", "not" }, where);
        for (auto name : { "all", "any" })
        {
            if (auto list = v->Find(name); list && list->IsArray())
            {
                for (auto& child : list->items)
                    KnownConditionFields(&child, where);
            }
        }
        KnownConditionFields(v->Find("not"), where);
    }

    void KnownLabelledFields(const json::Value* list, const std::string& where)
    {
        if (list && list->IsArray())
        {
            for (auto& item : list->items)
                KnownFields(&item, { "value", "label" }, where);
        }
    }

    bool LanguageTag(std::string_view tag)
    {
        return IsLanguageTag(tag);
    }

    void CheckCondition(const Description& d, const Condition& c, const std::string& where)
    {
        if (c.kind == Condition::Kind::Unknown)
            Warning(where, "condition not understood (counts as true)");
        if (c.kind == Condition::Kind::Equals || c.kind == Condition::Kind::NotEquals || c.kind == Condition::Kind::In)
        {
            if (!d.Find(c.setting))
                Error(where, "refers to unknown setting " + c.setting);
        }
        for (auto& child : c.children)
            CheckCondition(d, child, where);
    }

    void Validate(const std::string& jsonText, const Texts& texts, bool haveTexts)
    {
        if (jsonText.size() > (1u << 20))
            Error("description", "larger than 1 MiB");

        Description d;
        std::string error;
        if (!ParseDescription(jsonText, d, &error))
        {
            Error("description", error);
            return;
        }
        json::Value root;
        json::Parse(jsonText, root);
        // "major.minor"; another major version doesn't parse; the 0.x drafts are read as 1.0 (SPEC.md, Versions)
        {
            auto version = std::string(root.Str("version"));
            auto dot = version.find('.');
            bool digits = dot != std::string::npos && dot > 0 && dot + 1 < version.size() &&
                version.find_first_not_of("0123456789.") == std::string::npos && version.find('.', dot + 1) == std::string::npos;
            if (!digits)
                Error("version", "missing or not \"major.minor\" (\"1.0\")");
        }
        if (d.specMajor < ASI_SETTINGS_VERSION_MAJOR)
            Warning("version", "written for draft " + std::to_string(d.specMajor) + "." + std::to_string(d.specMinor) + ", read as 1.0: write \"1.0\"");
        bool newer = d.specMajor == ASI_SETTINGS_VERSION_MAJOR && d.specMinor > ASI_SETTINGS_VERSION_MINOR;
        if (newer)
            Warning("version", "newer than this validator: unknown fields and types aren't checked");
        warnUnknown = !newer;
        if (d.name.empty())
            Error("plugin.name", "missing");
        if (d.settings.size() > 4096)
            Error("settings", "more than 4096 settings");
        for (auto name : { "hosts", "extensions" })
        {
            if (auto v = root.Find(name); v && !v->IsObject())
                Error(name, "not an object");
        }

        // Fields readers don't know (hosts and extensions hold anything)
        KnownFields(&root, { "$schema", "format", "version", "plugin", "storage", "pages", "settings", "hosts", "extensions" }, "description");
        if (auto plugin = root.Find("plugin"))
        {
            KnownFields(plugin, { "id", "name", "priority", "version", "games" }, "plugin");
            if (auto games = plugin->Find("games"); games && games->IsArray())
            {
                for (auto& g : games->items)
                {
                    KnownFields(&g, { "name", "exe", "launcher" }, "plugin.games");
                    for (auto list : { g.Find("exe"), g.Find("launcher") })
                    {
                        if (list && list->IsArray())
                        {
                            for (auto& file : list->items)
                                KnownFields(&file, { "path", "label" }, "plugin.games");
                        }
                    }
                }
            }
        }
        KnownFields(root.Find("storage"), { "file" }, "storage");
        if (auto pages = root.Find("pages"); pages && pages->IsArray())
        {
            for (auto& p : pages->items)
                KnownFields(&p, { "id", "title", "section", "join", "order" }, "page " + std::string(p.Str("id")));
        }
        if (auto settings = root.Find("settings"); settings && settings->IsArray())
        {
            for (auto& v : settings->items)
            {
                std::string where(v.Str("id"));
                KnownFields(&v, { "id", "aliases", "type", "default", "min", "max", "step", "unit", "decimals", "special", "suggestions",
                    "choices", "dynamic", "maxLength", "folder", "relativeTo", "devices", "alpha", "label", "description", "page", "group",
                    "order", "control", "visibleIf", "enabledIf", "visibility", "apply", "presets", "storage" }, where);
                for (auto name : { "choices", "special", "suggestions" })
                    KnownLabelledFields(v.Find(name), where + " " + name);
                KnownConditionFields(v.Find("visibleIf"), where + " visibleIf");
                KnownConditionFields(v.Find("enabledIf"), where + " enabledIf");
                if (auto presets = v.Find("presets"); presets && presets->IsObject())
                {
                    for (auto& [name, member] : presets->members)
                    {
                        if (!OneOf(name, { "vanilla", "recommended", "enhanced" }))
                            Error(where, "unknown preset \"" + name + "\": there are only vanilla, recommended and enhanced");
                    }
                }
            }
        }

        // Storage: a template found without running the plugin (SPEC.md, File names)
        {
            std::string t = d.storageFile;
            for (size_t open; (open = t.find('{')) != std::string::npos;)
            {
                auto close = t.find('}', open);
                if (close == std::string::npos)
                {
                    Error("storage.file", "unclosed placeholder");
                    break;
                }
                auto name = t.substr(open, close - open + 1);
                if (!OneOf(name, { "{name}", "{plugin}", "{game}", "{root}", "{documents}", "{localappdata}", "{appdata}" }))
                    Error("storage.file", "unknown placeholder " + name);
                t.erase(open, close - open + 1);
            }
            if (consolePlugin && !detail::StaysBeside(d.storageFile))
                Error("storage.file", "a console plugin's settings file must stay in its folder");
        }

        for (auto& g : d.games)
        {
            if (g.name.empty())
                Error("plugin.games", "a game without a name");
            if (g.exe.empty())
                Error("plugin.games " + g.name, "no exe");
            for (auto* list : { &g.exe, &g.launcher })
            {
                for (auto& f : *list)
                {
                    if (f.path.empty())
                        Error("plugin.games " + g.name, "empty path");
                    if (f.path.find('\\') != std::string::npos)
                        Warning("plugin.games " + g.name, f.path + ": use '/' as separator");
                }
            }
        }

        std::set<std::string> pageIds;
        for (auto& p : d.pages)
        {
            if (p.id.empty() || !pageIds.insert(text::ToLower(p.id)).second)
                Error("page " + p.id, "missing or duplicate id");
            if (p.title.empty())
                Error("page " + p.id, "no title");
            if (!OneOf(p.section, { "display", "graphics", "camera", "controls", "gamepad", "audio", "gameplay", "language", "advanced", "other", "compatibility" }))
                Warning("page " + p.id, "unknown section " + p.section + " (counts as other)");
        }

        std::set<std::string> ids;
        for (auto& s : d.settings)
        {
            std::string where = s.id;
            if (s.id.size() > 128)
                Error(where, "id longer than 128 bytes");
            if (!ids.insert(text::ToLower(s.id)).second)
                Error(where, "duplicate id");
            for (auto& a : s.aliases)
            {
                if (!ids.insert(text::ToLower(a)).second)
                    Error(where, "alias " + a + " is already used");
            }
            if (s.type == Type::Unknown)
            {
                if (newer)
                    Warning(where, "type " + s.typeName + " unknown to this validator");
                else
                    Error(where, "unknown type " + s.typeName);
                continue;
            }
            if (s.type != Type::Info && s.label.empty())
                Error(where, "no label");
            if (!s.page.empty() && !pageIds.count(text::ToLower(s.page)))
                Error(where, "unknown page " + s.page);
            if (!OneOf(s.visibility, { "menu", "editor", "file" })) Error(where, "invalid visibility " + s.visibility);
            if (!OneOf(s.apply, { "live", "restart", "nextLevel", "menu" })) Error(where, "invalid apply " + s.apply);
            if (!OneOf(s.storage, { "cfg", "native" })) Error(where, "invalid storage " + s.storage);
            if (!s.relativeTo.empty() && !OneOf(s.relativeTo, { "game", "plugin" })) Error(where, "invalid relativeTo " + s.relativeTo);
            if (auto raw = root.Find("settings"))
            {
                // devices: keyboard, mouse, gamepad
                for (auto& v : raw->items)
                {
                    if (!text::IEquals(v.Str("id"), s.id))
                        continue;
                    if (auto devices = v.Find("devices"); devices && devices->IsArray())
                    {
                        for (auto& device : devices->items)
                        {
                            if (!device.IsString() || !OneOf(device.string, { "keyboard", "mouse", "gamepad" }))
                                Error(where, "invalid devices entry");
                        }
                    }
                    break;
                }
            }
            // The text language of a menu host (SPEC.md, Menu hosts): a choice of auto and language tags, for editors
            if (text::IEquals(s.id, "MAIN.TextLanguage"))
            {
                if (s.type != Type::Choice || !s.defaultValue.IsString() || s.defaultValue.string != "auto" ||
                    std::none_of(s.choices.begin(), s.choices.end(), [](const Labelled& c) { return c.value.IsString() && c.value.string == "auto"; }))
                    Error(where, "the reserved MAIN.TextLanguage is a choice with the value and default \"auto\"");
                for (auto& c : s.choices)
                {
                    if (c.value.IsString() && c.value.string != "auto" && !LanguageTag(c.value.string))
                        Error(where, "\"" + c.value.string + "\" isn't a language tag");
                }
                if (s.visibility != "editor")
                    Warning(where, "MAIN.TextLanguage is meant for editors (visibility editor)");
                if (!s.compatibility)
                    Warning(where, "MAIN.TextLanguage belongs on a compatibility page");
            }
            for (auto& c : s.controls)
            {
                if (!OneOf(c, { "switch", "checkbox", "cycler", "dropdown", "segmented", "slider", "stepper", "text", "keycapture", "colorpicker", "button", "label" }))
                    Warning(where, "unknown control " + c);
            }
            if (s.visibleIf) CheckCondition(d, *s.visibleIf, where + " visibleIf");
            if (s.enabledIf) CheckCondition(d, *s.enabledIf, where + " enabledIf");

            if (!s.HasValue())
            {
                if (s.presets)
                    Error(where, "an " + s.typeName + " can't take part in presets");
                continue;
            }
            if (s.min && s.max && *s.min > *s.max)
                Error(where, "min is greater than max");
            if (s.type == Type::Choice)
            {
                if (s.choices.empty() && !s.dynamic)
                    Error(where, "a choice needs choices or dynamic");
                bool numbers = !s.choices.empty() && s.choices.front().value.IsNumber();
                std::set<std::string> values;
                for (auto& c : s.choices)
                {
                    if (c.value.IsNumber() != numbers)
                        Error(where, "choices mix numbers and strings");
                    if (c.label.empty())
                        Error(where, "choice " + detail::JsonToText(c.value) + " has no label");
                    if (!values.insert(text::ToLower(detail::JsonToText(c.value))).second)
                        Error(where, "choice " + detail::JsonToText(c.value) + " is listed twice");
                }
            }
            if (s.defaultValue.IsNull())
                Error(where, "no default");
            else
                CheckValue(s, s.defaultValue, where + " default", false);
            if (s.presets)
            {
                static const char* names[] = { "vanilla", "recommended", "enhanced" };
                for (int i = 0; i < 3; ++i)
                    CheckValue(s, (*s.presets)[i], where + " preset " + names[i], false);
                if (detail::JsonToText((*s.presets)[1]) != detail::JsonToText(s.defaultValue))
                    Warning(where, "recommended differs from the default");
                // A preset changes it although the game's menu doesn't show it: allowed, but only where a preset needs it
                if (s.InPresets() && (s.visibility == "editor" || s.visibility == "file"))
                    Warning(where, "visibility " + s.visibility + " with presets that differ (vanilla " + detail::JsonToText((*s.presets)[0]) + ", recommended " +
                        detail::JsonToText((*s.presets)[1]) + ", enhanced " + detail::JsonToText((*s.presets)[2]) + "): applying a preset in the game's menu changes it");
            }
        }

        if (haveTexts)
        {
            std::set<std::string> keys;
            CollectKeys(root, keys);
            for (auto& key : keys)
            {
                if (key.size() > 128)
                    Error("text @" + key, "key longer than 128 bytes");
                if (!texts.Has(key))
                    Error("text @" + key, "missing from the English translation");
            }
        }
        else
            Warning("translations", "none given, text keys aren't checked");
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        std::printf("usage: asi-settings-validate <description.json> [<translation.json>...]\n"
                    "       asi-settings-validate <plugin.asi>\n"
                    "       asi-settings-validate <plugin.prx or .elf>\n");
        return 2;
    }
    std::filesystem::path input = argv[1];
    auto extension = text::ToLower(text::Narrow(input.extension().wstring()));

    std::string description;
    Texts texts;
    bool haveTexts = false;
    if (IsConsolePlugin(input))
    {
        consolePlugin = true;
        std::string error;
        if (!ReadConsoleSources(input, description, texts, &error))
        {
            std::printf("error: %ls: %s\n", argv[1], error.c_str());
            return 1;
        }
    }
    else if (extension == ".asi" || extension == ".dll")
    {
        HMODULE module = LoadLibraryExW(input.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!module)
        {
            std::printf("error: can't open %ls\n", argv[1]);
            return 1;
        }
        description = std::string(detail::ResourceData(module, ASI_SETTINGS_RESOURCE_TYPE, ASI_SETTINGS_RESOURCE_NAME));
        EnumResourceNamesW(module, ASI_SETTINGS_TEXT_RESOURCE_TYPE, detail::CollectText, reinterpret_cast<LONG_PTR>(&texts));
        FreeLibrary(module);
        if (description.empty())
        {
            std::printf("error: %ls has no ASISETTINGS resource\n", argv[1]);
            return 1;
        }
    }
    else
    {
        auto data = ReadFileText(input);
        if (!data)
        {
            std::printf("error: can't read %ls\n", argv[1]);
            return 1;
        }
        description = *data;
        // Translations: <anything>.<tag>.json or <tag>.json; each one given brings the other languages beside it
        // named the same way (en.json: fr.json, de.json...; x.en.json: x.fr.json...)
        std::vector<std::filesystem::path> files;
        auto add = [&](const std::filesystem::path& file)
        {
            if (std::none_of(files.begin(), files.end(), [&](const std::filesystem::path& f) { return std::filesystem::equivalent(f, file); }))
                files.push_back(file);
        };
        for (int i = 2; i < argc; ++i)
        {
            std::filesystem::path file = argv[i];
            if (!std::filesystem::is_regular_file(file))
            {
                std::printf("error: can't read %ls\n", argv[i]), ++errors;
                continue;
            }
            add(file);
            auto stem = text::Narrow(file.stem().wstring());
            auto dot = stem.rfind('.');
            auto prefix = dot == std::string::npos ? std::string() : stem.substr(0, dot + 1);
            std::error_code ec;
            for (auto& entry : std::filesystem::directory_iterator(file.parent_path().empty() ? "." : file.parent_path(), ec))
            {
                auto name = text::Narrow(entry.path().stem().wstring());
                if (!entry.is_regular_file() || text::ToLower(text::Narrow(entry.path().extension().wstring())) != ".json" ||
                    name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
                    continue;
                auto tag = name.substr(prefix.size());
                if (tag.find('.') == std::string::npos && LanguageTag(tag))
                    add(entry.path());
            }
        }
        for (auto& file : files)
        {
            auto stem = text::Narrow(file.stem().wstring());
            auto tag = stem.substr(stem.rfind('.') == std::string::npos ? 0 : stem.rfind('.') + 1);
            if (auto t = ReadFileText(file))
            {
                json::Value check;
                json::Error e;
                if (!json::Parse(*t, check, &e) || !check.IsObject())
                    std::printf("error: %ls: not a JSON object (%s)\n", file.c_str(), e.message.c_str()), ++errors;
                texts.sources[text::ToLower(tag)] = *t;
            }
            else
                std::printf("error: can't read %ls\n", file.c_str()), ++errors;
        }
    }
    haveTexts = !texts.sources.empty();
    if (haveTexts && !texts.sources.count("en"))
        Error("translations", "no English (en) translation");
    // Each translation: named by a language tag, at most 1 MiB, an object of texts (SPEC.md, Text and translations)
    for (auto& [tag, source] : texts.sources)
    {
        if (!LanguageTag(tag))
            Error("translation " + tag, "not a BCP 47 language tag");
        if (source.size() > (1u << 20))
            Error("translation " + tag, "larger than 1 MiB");
        json::Value table;
        if (json::Parse(source, table) && table.IsObject())
        {
            for (auto& [key, value] : table.members)
            {
                if (!value.IsString())
                    Error("translation " + tag, "\"" + key + "\" isn't text");
            }
        }
    }
    // Every other language has the English keys: a missing one shows English (a warning), one English lacks is a stray key
    if (auto en = texts.sources.find("en"); en != texts.sources.end())
    {
        json::Value english;
        if (json::Parse(en->second, english) && english.IsObject())
        {
            for (auto& [tag, source] : texts.sources)
            {
                json::Value table;
                if (tag == "en" || !json::Parse(source, table) || !table.IsObject())
                    continue;
                for (auto& [key, value] : english.members)
                {
                    if (!table.Find(key))
                        Warning("translation " + tag, "\"" + key + "\" is missing (shown in English)");
                }
                for (auto& [key, value] : table.members)
                {
                    if (!english.Find(key))
                        Error("translation " + tag, "\"" + key + "\" isn't in the English text");
                }
            }
        }
    }
    texts.Select("en");

    Validate(description, texts, haveTexts);
    std::printf("%d error(s), %d warning(s)\n", errors, warnings);
    return errors ? 1 : 0;
}
