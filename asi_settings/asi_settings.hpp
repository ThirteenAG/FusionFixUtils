/*
    ASI Settings 1.0
    https://github.com/ThirteenAG/FusionFixUtils/tree/master/asi_settings

    The shared implementation for plugins, menu hosts and editors (C++20, Windows, no other dependencies).
    Copy it together with asi_settings.h and asi_settings_json.hpp.

    In a plugin:
        asi_settings::Plugin settings;                  // one per module
        ASI_SETTINGS_EXPORT(settings)                   // the AsiSettings_GetApi export

        settings.Load();                                // description, translations, the .cfg (written when missing)
        int& gamma = settings.Int("GRAPHICS.ConsoleGamma");
        settings.OnChange("GRAPHICS.ConsoleGamma", [] { ApplyGamma(); });
        asi_settings::LiveLink::Start();                // optional: editors change the values while the game runs,
                                                        // and hand edits of the .cfg are read (WatchFile)

    Hand edits: Plugin::WatchFile() (LiveLink::Start calls it) reads the .cfg again whenever another program changes it,
    as Reload does, so OnChange callbacks and subscribers hear of it as of a change from a menu. The plugin's own writes
    are recognised by their text and read nothing. It runs on a thread of its own that is never waited for during
    startup, so both can be called from DllMain; StopWatching and LiveLink::Stop end it (not from DllMain). A plugin
    without the live link calls settings.WatchFile() itself.

    In a menu host:
        for (auto& plugin : asi_settings::FindLoadedPlugins(myModule, "fr")) ...

    In an editor:
        asi_settings::DescribedFile file;
        asi_settings::ReadPluginFile(L"...\\plugins\\NFSUnderground.WidescreenFix.asi", file);
*/
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")   // EnumProcessModules, also where PSAPI_VERSION is 1 (Windows XP targets)
#pragma comment(lib, "shell32.lib") // SHGetKnownFolderPath
#pragma comment(lib, "ole32.lib")   // CoTaskMemFree
#pragma comment(lib, "advapi32.lib") // AccessCheck
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <functional>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "asi_settings.h"
#include "asi_settings_json.hpp"

namespace asi_settings
{
    // ---------------------------------------------------------------------------------------------------------------
    // Text helpers

    namespace text
    {
        inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

        inline std::string ToLower(std::string_view s)
        {
            std::string out(s);
            for (auto& c : out)
                c = Lower(c);
            return out;
        }

        inline bool IEquals(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (Lower(a[i]) != Lower(b[i]))
                    return false;
            }
            return true;
        }

        inline std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
                s.remove_suffix(1);
            return s;
        }

        inline std::wstring Widen(std::string_view s)
        {
            int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
            std::wstring w(size_t(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
            return w;
        }

        inline std::string Narrow(std::wstring_view w)
        {
            int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(size_t(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
            return s;
        }

        // Shortest text that reads back as the same number
        inline std::string FormatNumber(double value)
        {
            if (std::floor(value) == value && std::fabs(value) < 1e15)
                return std::to_string(int64_t(value));
            char buffer[64];
            auto r = std::to_chars(buffer, buffer + sizeof(buffer), float(value));
            return std::string(buffer, r.ptr);
        }

        // snprintf-like copy for the runtime interface
        inline int32_t CopyOut(std::string_view value, char* buffer, uint32_t size)
        {
            if (buffer && size > 0)
            {
                size_t n = (std::min<size_t>)(value.size(), size - 1);
                std::memcpy(buffer, value.data(), n);
                buffer[n] = '\0';
            }
            return int32_t(value.size());
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The description

    enum class Type { Bool, Int, Float, Choice, String, Path, Key, Color, Action, Info, Unknown };

    // Types of this version, and the fallbacks of types it doesn't know yet (SPEC.md, Types)
    inline Type ParseType(std::string_view name)
    {
        static const std::pair<std::string_view, Type> types[] = {
            { "bool", Type::Bool }, { "int", Type::Int }, { "float", Type::Float }, { "choice", Type::Choice },
            { "string", Type::String }, { "path", Type::Path }, { "key", Type::Key }, { "color", Type::Color },
            { "action", Type::Action }, { "info", Type::Info },
        };
        for (auto& [n, t] : types)
        {
            if (n == name)
                return t;
        }
        return Type::Unknown;
    }

    struct Labelled
    {
        json::Value value;          // number or string
        std::string label;          // text
    };

    struct Condition
    {
        enum class Kind { Equals, NotEquals, In, All, Any, Not, Unknown };
        Kind kind = Kind::Unknown;
        std::string setting;
        std::vector<json::Value> values;
        std::vector<Condition> children;
    };

    struct Setting
    {
        std::string id, section, key;
        std::vector<std::string> aliases;
        std::string typeName;
        Type type = Type::Unknown;
        json::Value defaultValue;
        std::optional<double> min, max, step;
        int decimals = -1;
        std::string unit;
        std::vector<Labelled> choices, special, suggestions;
        bool dynamic = false;
        int maxLength = 0;
        bool folder = false;
        std::string relativeTo;
        std::string label, description, page, group;
        double order = 0.0;
        std::vector<std::string> controls;
        std::optional<Condition> visibleIf, enabledIf;
        std::string visibility = "menu";
        std::string apply = "live";
        std::optional<std::array<json::Value, 3>> presets;     // vanilla, recommended, enhanced
        std::string storage = "cfg";
        bool compatibility = false;     // on a page with section "compatibility" (SPEC.md, Pages)

        bool HasValue() const { return type != Type::Action && type != Type::Info; }
        // Shown in in-game menus (visibility "menu"); "editor" and "file" settings are not
        bool InMenu() const { return visibility == "menu"; }
        // Takes part in presets: its preset values differ between the presets, whatever its visibility or page
        // (SPEC.md, Presets). A setting without preset values, or with the same value in all three, is never changed by
        // a preset and doesn't decide which preset the values match
        bool InPresets() const { return presets.has_value() && presetsDiffer; }
        bool presetsDiffer = false;     // set when the description is read: the three preset values aren't all equal
        bool StringValued() const
        {
            switch (type)
            {
                case Type::String: case Type::Path: case Type::Key: case Type::Color: return true;
                case Type::Choice:
                    return !choices.empty() ? choices.front().value.IsString() : defaultValue.IsString();
                case Type::Unknown: return !defaultValue.IsNumber();
                default: return false;
            }
        }
        bool FloatValued() const { return type == Type::Float; }
    };

    struct Page
    {
        std::string id, title, section;
        bool join = false;
        double order = 0.0;
    };

    struct GameFile
    {
        std::string path;           // relative to the install root, '/' separated
        std::string label;
    };

    struct Game
    {
        std::string name;
        std::vector<GameFile> exe, launcher;
    };

    struct Description
    {
        int specMajor = 0, specMinor = 0;
        std::string id;             // empty: OriginalFilename of the version resource
        std::string name;
        double priority = 0.0;
        std::vector<Game> games;
        std::string storageFile;    // template, default "{name}.cfg"
        std::vector<Page> pages;
        std::vector<Setting> settings;
        json::Value hosts, extensions;

        const Setting* Find(std::string_view settingId) const
        {
            for (auto& s : settings)
            {
                if (text::IEquals(s.id, settingId))
                    return &s;
            }
            return nullptr;
        }
    };

    namespace detail
    {
        inline std::vector<Labelled> ReadLabelled(const json::Value* list)
        {
            std::vector<Labelled> out;
            if (list && list->IsArray())
            {
                for (auto& item : list->items)
                {
                    if (auto v = item.Find("value"))
                        out.push_back({ *v, std::string(item.Str("label")) });
                }
            }
            return out;
        }

        inline Condition ReadCondition(const json::Value& v)
        {
            Condition c;
            auto many = [&](const char* name, Condition::Kind kind)
            {
                auto list = v.Find(name);
                if (!list || !list->IsArray())
                    return false;
                c.kind = kind;
                for (auto& child : list->items)
                    c.children.push_back(ReadCondition(child));
                return true;
            };
            if (many("all", Condition::Kind::All) || many("any", Condition::Kind::Any))
                return c;
            if (auto inner = v.Find("not"))
            {
                c.kind = Condition::Kind::Not;
                c.children.push_back(ReadCondition(*inner));
                return c;
            }
            c.setting = v.Str("setting");
            if (auto e = v.Find("equals")) { c.kind = Condition::Kind::Equals; c.values.push_back(*e); }
            else if (auto n = v.Find("notEquals")) { c.kind = Condition::Kind::NotEquals; c.values.push_back(*n); }
            else if (auto in = v.Find("in"); in && in->IsArray()) { c.kind = Condition::Kind::In; c.values = in->items; }
            return c;
        }

        inline std::vector<GameFile> ReadGameFiles(const json::Value* list)
        {
            std::vector<GameFile> out;
            if (list && list->IsArray())
            {
                for (auto& item : list->items)
                {
                    if (item.IsString())
                        out.push_back({ item.string, {} });
                    else if (item.IsObject())
                        out.push_back({ std::string(item.Str("path")), std::string(item.Str("label")) });
                }
            }
            return out;
        }

        inline std::vector<std::string> ReadStrings(const json::Value* list)
        {
            std::vector<std::string> out;
            if (list && list->IsArray())
            {
                for (auto& item : list->items)
                {
                    if (item.IsString())
                        out.push_back(item.string);
                }
            }
            return out;
        }
    }

    // Reads a description. Fields it doesn't know are ignored (SPEC.md, Compatibility); errors are only reported for
    // what makes the description unusable.
    inline bool ParseDescription(std::string_view jsonText, Description& out, std::string* error = nullptr)
    {
        json::Value root;
        json::Error parseError;
        if (!json::Parse(jsonText, root, &parseError))
        {
            if (error)
                *error = "JSON error at byte " + std::to_string(parseError.offset) + ": " + parseError.message;
            return false;
        }
        if (root.Str("format") != "asi-settings")
        {
            if (error)
                *error = "not an asi-settings description";
            return false;
        }
        out = {};
        auto version = std::string(root.Str("version", "0.0"));
        out.specMajor = std::atoi(version.c_str());
        if (auto dot = version.find('.'); dot != std::string::npos)
            out.specMinor = std::atoi(version.c_str() + dot + 1);
        // Another major version may mean something else: not read. The 0.x drafts are read as 1.0 (SPEC.md, Versions)
        if (out.specMajor > ASI_SETTINGS_VERSION_MAJOR)
        {
            if (error)
                *error = "written for version " + version + ", a newer major version than this reader's " + std::to_string(ASI_SETTINGS_VERSION_MAJOR) + ".x";
            return false;
        }

        if (auto plugin = root.Find("plugin"))
        {
            out.id = plugin->Str("id");
            out.name = plugin->Str("name");
            out.priority = plugin->Num("priority", 0.0);
            if (auto games = plugin->Find("games"); games && games->IsArray())
            {
                for (auto& g : games->items)
                    out.games.push_back({ std::string(g.Str("name")), detail::ReadGameFiles(g.Find("exe")), detail::ReadGameFiles(g.Find("launcher")) });
            }
        }
        if (auto storage = root.Find("storage"))
            out.storageFile = storage->Str("file");
        if (out.storageFile.empty())
            out.storageFile = "{name}.cfg";

        if (auto pages = root.Find("pages"); pages && pages->IsArray())
        {
            double index = 0;
            for (auto& p : pages->items)
            {
                Page page{ std::string(p.Str("id")), std::string(p.Str("title")), std::string(p.Str("section", "other")), p.Bool("join"), p.Num("order", index) };
                index += 1;
                out.pages.push_back(std::move(page));
            }
            // Compatibility pages come after all others (SPEC.md, Pages)
            std::stable_sort(out.pages.begin(), out.pages.end(), [](const Page& a, const Page& b)
            {
                bool ca = text::IEquals(a.section, "compatibility"), cb = text::IEquals(b.section, "compatibility");
                if (ca != cb)
                    return cb;
                return a.order < b.order;
            });
        }

        auto settings = root.Find("settings");
        if (!settings || !settings->IsArray())
        {
            if (error)
                *error = "no settings";
            return false;
        }
        double index = 0;
        for (auto& v : settings->items)
        {
            Setting s;
            s.id = v.Str("id");
            auto dot = s.id.find('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 >= s.id.size())
            {
                if (error)
                    *error = "setting id \"" + s.id + "\" isn't SECTION.Key";
                return false;
            }
            s.section = s.id.substr(0, dot);
            s.key = s.id.substr(dot + 1);
            s.aliases = detail::ReadStrings(v.Find("aliases"));
            s.typeName = v.Str("type");
            s.type = ParseType(s.typeName);
            if (auto d = v.Find("default"))
                s.defaultValue = *d;
            if (auto m = v.Find("min"); m && m->IsNumber()) s.min = m->number;
            if (auto m = v.Find("max"); m && m->IsNumber()) s.max = m->number;
            if (auto m = v.Find("step"); m && m->IsNumber() && m->number > 0) s.step = m->number;
            s.decimals = int(v.Num("decimals", -1));
            s.unit = v.Str("unit");
            s.choices = detail::ReadLabelled(v.Find("choices"));
            s.special = detail::ReadLabelled(v.Find("special"));
            s.suggestions = detail::ReadLabelled(v.Find("suggestions"));
            s.dynamic = v.Bool("dynamic");
            s.maxLength = int(v.Num("maxLength", 0));
            s.folder = v.Bool("folder");
            s.relativeTo = v.Str("relativeTo");
            s.label = v.Str("label");
            s.description = v.Str("description");
            s.page = v.Str("page", out.pages.empty() ? "" : std::string_view(out.pages.front().id));
            s.group = v.Str("group");
            s.order = v.Num("order", index);
            s.controls = detail::ReadStrings(v.Find("control"));
            if (auto c = v.Find("visibleIf")) s.visibleIf = detail::ReadCondition(*c);
            if (auto c = v.Find("enabledIf")) s.enabledIf = detail::ReadCondition(*c);
            s.visibility = v.Str("visibility", "menu");
            s.apply = v.Str("apply", "live");
            if (auto p = v.Find("presets"); p && p->IsObject())
            {
                auto a = p->Find("vanilla"), b = p->Find("recommended"), c = p->Find("enhanced");
                if (a && b && c)
                {
                    s.presets = std::array<json::Value, 3>{ *a, *b, *c };
                    auto same = [](const json::Value& x, const json::Value& y)
                    {
                        if (x.IsNumber() || y.IsNumber() || x.IsBool() || y.IsBool())
                        {
                            auto n = [](const json::Value& v) { return v.IsBool() ? (v.boolean ? 1.0 : 0.0) : v.IsNumber() ? v.number : v.IsString() ? std::atof(v.string.c_str()) : 0.0; };
                            return n(x) == n(y);
                        }
                        return x.kind == y.kind && x.string == y.string;
                    };
                    s.presetsDiffer = !same(*a, *b) || !same(*b, *c);
                }
            }
            s.storage = v.Str("storage", "cfg");
            // An unknown type without a value of a known kind is shown nowhere but the file (SPEC.md, Types)
            if (s.type == Type::Unknown && !s.defaultValue.IsNumber() && !s.defaultValue.IsString())
                s.visibility = "file";
            for (auto& page : out.pages)
            {
                if (text::IEquals(page.id, s.page))
                {
                    s.compatibility = text::IEquals(page.section, "compatibility");
                    break;
                }
            }
            index += 1;
            out.settings.push_back(std::move(s));
        }
        if (auto h = root.Find("hosts")) out.hosts = *h;
        if (auto e = root.Find("extensions")) out.extensions = *e;
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Translations

    class Texts
    {
    public:
        // Tables by language tag (lower case), parsed lazily
        std::map<std::string, std::string> sources;

        // Picks the language: the tag, then shorter tags (RFC 4647 lookup), then "en"
        void Select(std::string_view tag)
        {
            chosen.clear();
            std::string want = text::ToLower(tag);
            while (!want.empty())
            {
                if (Use(want))
                    break;
                auto dash = want.rfind('-');
                want = dash == std::string::npos ? std::string() : want.substr(0, dash);
            }
            english.clear();
            if (auto it = sources.find("en"); it != sources.end())
                ParseTable(it->second, english);
        }

        // A text field: "@key" is looked up, "@@..." is a literal "@...", anything else is literal
        std::string Resolve(std::string_view value) const
        {
            if (value.size() >= 2 && value[0] == '@' && value[1] == '@')
                return std::string(value.substr(1));
            if (value.empty() || value[0] != '@')
                return std::string(value);
            std::string key(value.substr(1));
            if (auto it = chosen.find(key); it != chosen.end())
                return it->second;
            if (auto it = english.find(key); it != english.end())
                return it->second;
            return key;
        }

        bool Has(std::string_view key) const { return chosen.count(std::string(key)) || english.count(std::string(key)); }

    private:
        std::map<std::string, std::string> chosen, english;

        static void ParseTable(const std::string& source, std::map<std::string, std::string>& table)
        {
            json::Value root;
            if (!json::Parse(source, root) || !root.IsObject())
                return;
            for (auto& [key, value] : root.members)
            {
                if (value.IsString())
                    table[key] = value.string;
            }
        }

        bool Use(const std::string& tag)
        {
            auto it = sources.find(tag);
            if (it == sources.end())
                return false;
            ParseTable(it->second, chosen);
            return true;
        }
    };

    // ---------------------------------------------------------------------------------------------------------------
    // Resources

    namespace detail
    {
        inline std::string_view ResourceData(HMODULE module, const wchar_t* type, const wchar_t* name)
        {
            HRSRC resource = FindResourceW(module, name, type);
            if (!resource)
                return {};
            HGLOBAL handle = LoadResource(module, resource);
            auto data = handle ? static_cast<const char*>(LockResource(handle)) : nullptr;
            return data ? std::string_view(data, SizeofResource(module, resource)) : std::string_view();
        }

        inline BOOL CALLBACK CollectText(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR param)
        {
            if (IS_INTRESOURCE(name))
                return TRUE;
            auto texts = reinterpret_cast<Texts*>(param);
            auto data = ResourceData(module, type, name);
            texts->sources[text::ToLower(text::Narrow(name))] = std::string(data);
            return TRUE;
        }

        // The version resource's OriginalFilename without its extension
        inline std::string OriginalName(HMODULE module)
        {
            auto data = ResourceData(module, MAKEINTRESOURCEW(16) /* RT_VERSION */, MAKEINTRESOURCEW(1));
            if (data.empty())
                return {};
            // VerQueryValue needs a writable copy and version.dll; the string table is searched directly instead
            const wchar_t key[] = L"OriginalFilename";
            auto begin = reinterpret_cast<const wchar_t*>(data.data());
            size_t count = data.size() / sizeof(wchar_t);
            for (size_t i = 0; i + std::size(key) < count; ++i)
            {
                if (std::wmemcmp(begin + i, key, std::size(key) - 1) != 0)
                    continue;
                size_t j = i + std::size(key);
                while (j < count && begin[j] == 0)  // padding to the value
                    ++j;
                std::wstring value;
                while (j < count && begin[j] != 0)
                    value += begin[j++];
                auto name = std::filesystem::path(value).stem().wstring();
                return text::Narrow(name);
            }
            return {};
        }
    }

    // A plugin file's or module's description and translations
    struct DescribedFile
    {
        Description description;
        Texts texts;
        std::string id;             // description.id, or the original file name
        std::wstring path;          // the plugin file
    };

    // From a loaded module, or one loaded as data (LOAD_LIBRARY_AS_DATAFILE)
    inline bool ReadModule(HMODULE module, DescribedFile& out, std::string* error = nullptr)
    {
        auto data = detail::ResourceData(module, ASI_SETTINGS_RESOURCE_TYPE, ASI_SETTINGS_RESOURCE_NAME);
        if (data.empty())
        {
            if (error)
                *error = "no ASISETTINGS resource";
            return false;
        }
        if (!ParseDescription(data, out.description, error))
            return false;
        EnumResourceNamesW(module, ASI_SETTINGS_TEXT_RESOURCE_TYPE, detail::CollectText, reinterpret_cast<LONG_PTR>(&out.texts));
        out.texts.Select("en");
        out.id = out.description.id.empty() ? detail::OriginalName(module) : out.description.id;
        if (out.id.empty())
        {
            wchar_t path[MAX_PATH * 4] = {};
            GetModuleFileNameW(module, path, DWORD(std::size(path)));
            out.id = text::Narrow(std::filesystem::path(path).stem().wstring());
        }
        return true;
    }

    // For editors: reads a plugin file without running any of its code
    inline bool ReadPluginFile(const std::wstring& path, DescribedFile& out, std::string* error = nullptr)
    {
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!module)
        {
            if (error)
                *error = "can't open the file";
            return false;
        }
        bool ok = ReadModule(module, out, error);
        // The id from the version resource, which ReadModule can't take from GetModuleFileName for data modules
        if (ok && out.description.id.empty() && out.id.empty())
            out.id = text::Narrow(std::filesystem::path(path).stem().wstring());
        out.path = path;
        FreeLibrary(module);
        return ok;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Values

    // The current value of a setting, typed by the setting
    // The value a player chose (stored in the settings file), and for plugins also the value in effect: the same, or
    // the plugin's value while the setting isn't usable (SPEC.md, Availability)
    struct Slot
    {
        const Setting* setting = nullptr;
        int intValue = 0;           // bool, int, numeric choice
        float floatValue = 0.0f;    // float
        std::string stringValue;    // string, path, key, color, string choice, unknown types
        std::string unread;         // a line of the file that couldn't be read, kept until the setting changes
        bool stored = false;        // the settings file had the setting when it was read

        struct Effective
        {
            int intValue = 0;
            float floatValue = 0.0f;
            std::string stringValue;
        } effective;
    };

    namespace detail
    {
        inline bool ParseBool(std::string_view s, int& out)
        {
            static const std::pair<std::string_view, int> words[] = { { "0", 0 }, { "1", 1 }, { "false", 0 }, { "true", 1 },
                { "off", 0 }, { "on", 1 }, { "no", 0 }, { "yes", 1 } };
            for (auto& [w, v] : words)
            {
                if (text::IEquals(s, w))
                {
                    out = v;
                    return true;
                }
            }
            return false;
        }

        inline bool ParseInt(std::string_view s, int& out)
        {
            std::string t(s);
            if (t.empty())
                return false;
            bool hex = t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X');
            // Decimal with an optional sign, or 0x and hexadecimal digits only (SPEC.md, Reading)
            for (size_t i = hex ? 2 : 0; i < t.size(); ++i)
            {
                char c = t[i];
                bool digit = hex ? std::isxdigit(uint8_t(c)) != 0 : (c >= '0' && c <= '9') || (i == 0 && (c == '-' || c == '+') && t.size() > 1);
                if (!digit)
                    return false;
            }
            char* end = nullptr;
            long long v = hex ? std::strtoll(t.c_str() + 2, &end, 16) : std::strtoll(t.c_str(), &end, 10);
            if (end != t.c_str() + t.size())
                return false;
            out = int(v);
            return true;
        }

        inline bool ParseFloat(std::string_view s, float& out)
        {
            std::string t(s);
            // Decimal only, '.' as separator: no hexadecimal, inf or nan (SPEC.md, Reading)
            if (t.empty() || t.find_first_not_of("0123456789+-.eE") != std::string::npos)
                return false;
            char* end = nullptr;
            double v = std::strtod(t.c_str(), &end);
            if (end != t.c_str() + t.size() || !std::isfinite(v))
                return false;
            out = float(v);
            return true;
        }

        inline bool JsonEquals(const json::Value& v, std::string_view textValue, bool numeric)
        {
            if (v.IsNumber())
            {
                float f;
                return numeric && ParseFloat(textValue, f) && std::fabs(double(f) - v.number) < 1e-6;
            }
            if (v.IsBool())
            {
                int b;
                return numeric && ParseBool(textValue, b) && b == (v.boolean ? 1 : 0);
            }
            return v.IsString() && text::IEquals(v.string, textValue);
        }

        inline std::string JsonToText(const json::Value& v)
        {
            if (v.IsNumber()) return text::FormatNumber(v.number);
            if (v.IsBool()) return v.boolean ? "1" : "0";
            if (v.IsString()) return v.string;
            return {};
        }
    }

    // The value as text in the settings file's syntax (unquoted)
    inline std::string FormatValue(const Slot& slot)
    {
        auto& s = *slot.setting;
        if (s.StringValued())
            return slot.stringValue;
        if (s.FloatValued())
            return text::FormatNumber(slot.floatValue);
        return std::to_string(slot.intValue);
    }

    // Reads a value given as text into the slot; false when it can't be read or isn't allowed. checkRange: also
    // refuse numbers outside min/max (changes do; values from the file are kept as they are)
    inline bool ParseValue(Slot& slot, std::string_view value, bool checkRange)
    {
        auto& s = *slot.setting;
        value = text::Trim(value);
        auto isSpecial = [&](bool numeric)
        {
            return std::any_of(s.special.begin(), s.special.end(), [&](const Labelled& l) { return detail::JsonEquals(l.value, value, numeric); });
        };
        switch (s.type)
        {
            case Type::Bool:
                return detail::ParseBool(value, slot.intValue);
            case Type::Int:
            {
                int v;
                if (!detail::ParseInt(value, v))
                    return false;
                if (checkRange && !isSpecial(true) && ((s.min && v < *s.min) || (s.max && v > *s.max)))
                    return false;
                slot.intValue = v;
                return true;
            }
            case Type::Float:
            {
                float v;
                if (!detail::ParseFloat(value, v))
                    return false;
                if (checkRange && !isSpecial(true) && ((s.min && v < *s.min - 1e-6) || (s.max && v > *s.max + 1e-6)))
                    return false;
                slot.floatValue = v;
                return true;
            }
            case Type::Choice:
            {
                bool numeric = !s.StringValued();
                bool known = s.dynamic || isSpecial(numeric) ||
                    std::any_of(s.choices.begin(), s.choices.end(), [&](const Labelled& l) { return detail::JsonEquals(l.value, value, numeric); });
                if (!known)
                    return false;
                if (numeric)
                    return detail::ParseInt(value, slot.intValue);
                // The spelling of the description's value
                slot.stringValue = std::string(value);
                for (auto& c : s.choices)
                {
                    if (c.value.IsString() && text::IEquals(c.value.string, value))
                        slot.stringValue = c.value.string;
                }
                return true;
            }
            case Type::Color:
            {
                bool ok = (value.size() == 7 || value.size() == 9) && value[0] == '#' &&
                    std::all_of(value.begin() + 1, value.end(), [](char c) { return std::isxdigit(uint8_t(c)) != 0; });
                if (ok)
                    slot.stringValue = std::string(value);
                return ok;
            }
            case Type::String:
            case Type::Path:
            case Type::Key:
            case Type::Unknown:
                if (s.maxLength > 0 && int(value.size()) > s.maxLength)
                    return false;
                if (s.type == Type::Unknown && !s.StringValued())
                {
                    float f;
                    if (!detail::ParseFloat(value, f))
                        return false;
                    slot.floatValue = f;
                    slot.intValue = int(f);
                }
                slot.stringValue = std::string(value);
                return true;
            default:
                return false;
        }
    }

    inline void SetDefault(Slot& slot)
    {
        auto& d = slot.setting->defaultValue;
        slot.intValue = 0;
        slot.floatValue = 0.0f;
        slot.stringValue.clear();
        if (d.IsNumber())
        {
            slot.intValue = int(d.number);
            slot.floatValue = float(d.number);
            slot.stringValue = text::FormatNumber(d.number);
        }
        else if (d.IsBool())
            slot.intValue = d.boolean ? 1 : 0;
        else if (d.IsString())
            slot.stringValue = d.string;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The settings file

    struct CfgFile
    {
        struct Entry { std::string key, value; };
        struct Section { std::string name; std::vector<Entry> entries; };
        std::vector<Section> sections;

        const std::string* Find(std::string_view section, std::string_view key) const
        {
            for (auto& s : sections)
            {
                if (!text::IEquals(s.name, section))
                    continue;
                for (auto& e : s.entries)
                {
                    if (text::IEquals(e.key, key))
                        return &e.value;
                }
            }
            return nullptr;
        }
    };

    // One line of a settings file as the reader sees it, for editors that show the file as text
    struct CfgLine
    {
        enum class Kind { Blank, Comment, Section, Entry, Other };
        Kind kind = Kind::Blank;
        std::string section;        // Section: its name
        std::string key, value;     // Entry: the key and the value read (unquoted, without a comment behind it)
    };

    // Reads one line of a settings file (without its line end; a byte order mark is the caller's)
    inline CfgLine ParseCfgLine(std::string_view line)
    {
        CfgLine out;
        line = text::Trim(line);
        if (line.empty())
            return out;
        if (line[0] == ';' || line[0] == '#')
        {
            out.kind = CfgLine::Kind::Comment;
            return out;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            out.kind = CfgLine::Kind::Section;
            out.section = std::string(text::Trim(line.substr(1, line.size() - 2)));
            return out;
        }
        auto eq = line.find('=');
        if (eq == std::string_view::npos)
        {
            out.kind = CfgLine::Kind::Other;
            return out;
        }
        out.kind = CfgLine::Kind::Entry;
        out.key = std::string(text::Trim(line.substr(0, eq)));
        auto value = text::Trim(line.substr(eq + 1));
        if (!value.empty() && value[0] == '"')
        {
            for (size_t i = 1; i < value.size(); ++i)
            {
                if (value[i] == '\\' && i + 1 < value.size() && (value[i + 1] == '"' || value[i + 1] == '\\'))
                    out.value += value[++i];
                else if (value[i] == '"')
                    break;
                else
                    out.value += value[i];
            }
        }
        else
        {
            // A ';' after whitespace starts a comment
            for (size_t i = 0; i < value.size(); ++i)
            {
                if (value[i] == ';' && i > 0 && (value[i - 1] == ' ' || value[i - 1] == '\t'))
                {
                    value = text::Trim(value.substr(0, i));
                    break;
                }
            }
            out.value = std::string(value);
        }
        return out;
    }

    // Reads a settings file's text (SPEC.md, The settings file, Reading)
    inline CfgFile ParseCfg(std::string_view source)
    {
        if (source.substr(0, 3) == "\xEF\xBB\xBF")
            source.remove_prefix(3);
        CfgFile file;
        file.sections.push_back({});    // keys before the first section
        while (!source.empty())
        {
            auto end = source.find('\n');
            auto line = ParseCfgLine(source.substr(0, end));
            source.remove_prefix(end == std::string_view::npos ? source.size() : end + 1);
            if (line.kind == CfgLine::Kind::Section)
                file.sections.push_back({ std::move(line.section), {} });
            else if (line.kind == CfgLine::Kind::Entry)
                file.sections.back().entries.push_back({ std::move(line.key), std::move(line.value) });
        }
        return file;
    }

    namespace detail
    {
        inline std::string Quote(std::string_view value)
        {
            bool needs = !value.empty() && (value.front() == ' ' || value.back() == ' ' || value.front() == '"' ||
                value.find(';') != std::string_view::npos || value.find('#') == 0);
            if (!needs)
                return std::string(value);
            std::string out = "\"";
            for (char c : value)
            {
                if (c == '"' || c == '\\')
                    out += '\\';
                out += c;
            }
            return out + "\"";
        }

        inline std::string OneLine(std::string s)
        {
            std::replace(s.begin(), s.end(), '\n', ' ');
            return s;
        }

        // The comment above a setting: label, description, values, default, restart
        inline std::string Comment(const Setting& s, const Texts& english)
        {
            std::string out = "; " + english.Resolve(s.label);
            if (!s.description.empty())
                out += ": " + OneLine(english.Resolve(s.description));
            // skipBare: leaves out values without a label of their own (a text special value like "Auto", which the
            // description already explains)
            auto labelled = [&](const std::vector<Labelled>& list, bool skipBare = false)
            {
                std::string t;
                for (auto& l : list)
                {
                    auto value = detail::JsonToText(l.value);
                    auto label = l.label.empty() ? std::string() : english.Resolve(l.label);
                    if (skipBare && (label.empty() || text::IEquals(label, value)))
                        continue;
                    t += (t.empty() ? "" : ", ") + value + (label.empty() || text::IEquals(label, value) ? "" : " = " + label);
                }
                return t;
            };
            std::string values;
            switch (s.type)
            {
                case Type::Bool: values = "0 = Off, 1 = On"; break;
                case Type::Choice: values = labelled(s.choices); break;
                case Type::Int:
                case Type::Float:
                    if (s.min && s.max)
                        values = text::FormatNumber(*s.min) + " to " + text::FormatNumber(*s.max) + (s.unit.empty() ? "" : (s.unit == "°" || s.unit == "%" ? "" : " ") + s.unit);
                    break;
                case Type::Color: values = "#RRGGBB or #RRGGBBAA"; break;
                default: break;
            }
            if (auto special = labelled(s.special, s.StringValued()); !special.empty())
                values += (values.empty() ? "" : ", ") + special;
            // Sentences end with one period, whether the description has one or not
            auto sentence = [&](const std::string& part)
            {
                out += (!out.empty() && out.back() == '.') ? " " : ". ";
                out += part;
            };
            if (!values.empty())
                sentence(values);
            auto defaultText = detail::JsonToText(s.defaultValue);
            sentence("Default: " + (defaultText.empty() ? std::string("empty") : defaultText));
            if (s.apply == "restart")
                out += " (after restart)";
            return out;
        }
    }

    // The comment line above the settings of a section that aren't in the game's menu (SPEC.md, Writing)
    inline constexpr std::string_view CfgAdvancedBanner = "; ---- Advanced settings (not in the game's menu) ----";
    // The comment line above the compatibility settings of a section, after the advanced ones (SPEC.md, Writing)
    inline constexpr std::string_view CfgCompatibilityBanner = "; ---- Compatibility (only if something doesn't work) ----";

    // The whole file (SPEC.md, Writing): every setting with its comment, unknown keys of the old file kept; in each
    // section the settings shown in menus first, then the other non-compatibility ones under CfgAdvancedBanner, then
    // the compatibility ones under CfgCompatibilityBanner
    inline std::string WriteCfg(const Description& description, const std::string& pluginName, const Texts& english,
        const std::vector<Slot>& slots, const CfgFile* previous)
    {
        std::string out = "; " + pluginName + " settings\r\n";
        out += "; Change the settings here or in FusionFixHub; fixes with an in-game menu change them there too.\r\n";
        // Keys outside a section stay before the first one, so that they don't end up in another section
        if (previous)
        {
            bool first = true;
            for (auto& old : previous->sections)
            {
                if (!old.name.empty())
                    continue;
                for (auto& e : old.entries)
                {
                    out += (first ? "\r\n" : "") + e.key + " = " + detail::Quote(e.value) + "\r\n";
                    first = false;
                }
            }
        }

        std::vector<std::string> order;     // sections in the order of the description
        for (auto& s : description.settings)
        {
            if (s.HasValue() && s.storage == "cfg" && std::none_of(order.begin(), order.end(), [&](auto& n) { return text::IEquals(n, s.section); }))
                order.push_back(s.section);
        }
        auto known = [&](std::string_view section, std::string_view key)
        {
            auto setting = description.Find(std::string(section) + "." + std::string(key));
            if (setting)
                return true;
            // Aliases are read, not written again
            for (auto& s : description.settings)
            {
                for (auto& a : s.aliases)
                {
                    if (text::IEquals(a, std::string(section) + "." + std::string(key)))
                        return true;
                }
            }
            return false;
        };

        for (auto& section : order)
        {
            out += "\r\n[" + section + "]\r\n";
            bool first = true;
            // Settings shown in menus first, then the advanced ones, then the compatibility ones, each under its banner
            enum class Tier { Menu, Advanced, Compatibility };
            for (Tier tier : { Tier::Menu, Tier::Advanced, Tier::Compatibility })
            {
                bool banner = false;
                for (auto& slot : slots)
                {
                    auto& s = *slot.setting;
                    if (!s.HasValue() || s.storage != "cfg" || !text::IEquals(s.section, section))
                        continue;
                    if ((s.compatibility ? Tier::Compatibility : s.InMenu() ? Tier::Menu : Tier::Advanced) != tier)
                        continue;
                    if (!first)
                        out += "\r\n";
                    first = false;
                    if (tier != Tier::Menu && !banner)
                    {
                        out += std::string(tier == Tier::Advanced ? CfgAdvancedBanner : CfgCompatibilityBanner) + "\r\n\r\n";
                        banner = true;
                    }
                    out += detail::Comment(s, english) + "\r\n";
                    out += s.key + " = " + (slot.unread.empty() ? detail::Quote(FormatValue(slot)) : detail::Quote(slot.unread)) + "\r\n";
                }
            }
            if (previous)
            {
                for (auto& old : previous->sections)
                {
                    if (!text::IEquals(old.name, section))
                        continue;
                    for (auto& e : old.entries)
                    {
                        if (!known(section, e.key))
                            out += "\r\n" + e.key + " = " + detail::Quote(e.value) + "\r\n";
                    }
                }
            }
        }
        // Sections the description doesn't have
        if (previous)
        {
            for (auto& old : previous->sections)
            {
                if (old.entries.empty() || old.name.empty() || std::any_of(order.begin(), order.end(), [&](auto& n) { return text::IEquals(n, old.name); }))
                    continue;
                out += "\r\n[" + old.name + "]\r\n";
                for (auto& e : old.entries)
                    out += e.key + " = " + detail::Quote(e.value) + "\r\n";
            }
        }
        return out;
    }

    // Replaces a file safely: a temporary file in the same folder, then a rename
    inline bool WriteFileAtomic(const std::filesystem::path& path, std::string_view content)
    {
        auto temporary = path;
        temporary += L".tmp";
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        bool ok = WriteFile(file, content.data(), DWORD(content.size()), &written, nullptr) && written == content.size();
        FlushFileBuffers(file);
        CloseHandle(file);
        if (ok)
            ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok)
            DeleteFileW(temporary.c_str());
        return ok;
    }

    // error: why the file couldn't be opened (GetLastError)
    inline std::optional<std::string> ReadFileText(const std::filesystem::path& path, DWORD* error = nullptr)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (error)
            *error = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (file == INVALID_HANDLE_VALUE)
            return std::nullopt;
        LARGE_INTEGER size{};
        GetFileSizeEx(file, &size);
        std::string data(size_t(std::min<LONGLONG>(size.QuadPart, 16 << 20)), '\0');
        DWORD read = 0;
        BOOL ok = ReadFile(file, data.data(), DWORD(data.size()), &read, nullptr);
        if (!ok && error)
            *error = GetLastError();
        CloseHandle(file);
        if (!ok)
            return std::nullopt;
        data.resize(read);
        return data;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Placeholders of storage.file

    struct Locations
    {
        std::filesystem::path plugin;   // the plugin file
        std::filesystem::path exe;      // the game's exe
    };

    namespace detail
    {
        inline std::filesystem::path KnownFolder(REFKNOWNFOLDERID id)
        {
            PWSTR path = nullptr;
            std::filesystem::path result;
            if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)))
                result = path;
            CoTaskMemFree(path);
            return result;
        }
    }

    // The install root: the exe's path without the game entry it matches (SPEC.md, Games)
    inline std::filesystem::path InstallRoot(const Description& d, const std::filesystem::path& exe)
    {
        auto full = text::ToLower(text::Narrow(exe.wstring()));
        std::replace(full.begin(), full.end(), '/', '\\');
        for (auto& game : d.games)
        {
            for (auto& file : game.exe)
            {
                auto relative = text::ToLower(file.path);
                std::replace(relative.begin(), relative.end(), '/', '\\');
                if (full.size() > relative.size() && full.compare(full.size() - relative.size(), relative.size(), relative) == 0 &&
                    full[full.size() - relative.size() - 1] == '\\')
                {
                    auto rootText = exe.wstring().substr(0, full.size() - relative.size() - 1);
                    return std::filesystem::path(rootText);
                }
            }
        }
        return exe.parent_path();
    }

    // A path template with the placeholders of SPEC.md (File names); relative paths are relative to the plugin
    inline std::filesystem::path ResolvePath(const Description& d, const Locations& at, std::string_view pathTemplate)
    {
        auto known = [](REFKNOWNFOLDERID id) { return detail::KnownFolder(id).wstring(); };
        std::wstring t = text::Widen(pathTemplate);
        auto replace = [&](std::wstring_view name, const std::wstring& value)
        {
            for (size_t p; (p = t.find(name)) != std::wstring::npos;)
                t.replace(p, name.size(), value);
        };
        replace(L"{name}", at.plugin.stem().wstring());
        replace(L"{plugin}", at.plugin.parent_path().wstring());
        replace(L"{game}", at.exe.parent_path().wstring());
        replace(L"{root}", InstallRoot(d, at.exe).wstring());
        if (t.find(L"{documents}") != std::wstring::npos) replace(L"{documents}", known(FOLDERID_Documents));
        if (t.find(L"{localappdata}") != std::wstring::npos) replace(L"{localappdata}", known(FOLDERID_LocalAppData));
        if (t.find(L"{appdata}") != std::wstring::npos) replace(L"{appdata}", known(FOLDERID_RoamingAppData));
        std::filesystem::path path(t);
        if (path.is_relative())
            path = at.plugin.parent_path() / path;
        return path.lexically_normal();
    }

    inline std::filesystem::path ResolveStorage(const Description& d, const Locations& at)
    {
        return ResolvePath(d, at, d.storageFile);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Folders the user can't write to (SPEC.md)

    struct StorageFiles
    {
        std::filesystem::path file;         // storage.file, resolved
        std::filesystem::path fallback;     // in the user's FusionFixHub folder, for when the user can't write to file's folder
        std::filesystem::path virtualStore; // where Windows' file virtualization put the writes of older plugins
    };

    namespace detail
    {
        inline uint32_t Fnv1a(std::string_view s)
        {
            uint32_t hash = 2166136261u;
            for (unsigned char c : s)
                hash = (hash ^ c) * 16777619u;
            return hash;
        }

        // A path as the fallback's name uses it: lower case, backslashes, no trailing backslash
        inline std::string PathKey(const std::filesystem::path& path)
        {
            auto key = text::ToLower(text::Narrow(path.lexically_normal().wstring()));
            std::replace(key.begin(), key.end(), '/', '\\');
            while (key.size() > 1 && key.back() == '\\')
                key.pop_back();
            return key;
        }

        inline std::optional<FILETIME> LastWrite(const std::filesystem::path& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                return std::nullopt;
            return data.ftLastWriteTime;
        }
    }

    // The settings file, its fallback and its virtualized copy. The fallback is
    // {localappdata}\FusionFixHub\<install root folder name>-<hash of the install root path>\<path from the root>
    inline StorageFiles ResolveStorageFiles(const Description& d, const Locations& at)
    {
        StorageFiles files;
        files.file = ResolveStorage(d, at);
        auto local = detail::KnownFolder(FOLDERID_LocalAppData);
        if (local.empty())
            return files;

        auto root = InstallRoot(d, at.exe).lexically_normal();
        auto rootKey = detail::PathKey(root);
        auto fileKey = detail::PathKey(files.file);
        std::filesystem::path relative = files.file.filename();
        if (fileKey.size() > rootKey.size() + 1 && fileKey.compare(0, rootKey.size(), rootKey) == 0 && fileKey[rootKey.size()] == '\\')
            relative = files.file.lexically_normal().wstring().substr(rootKey.size() + 1);
        auto name = root.filename().wstring();
        char hash[16];
        snprintf(hash, sizeof(hash), "-%08x", detail::Fnv1a(rootKey));
        files.fallback = local / L"FusionFixHub" / ((name.empty() ? std::wstring(L"Game") : name) + text::Widen(hash)) / relative;
        files.virtualStore = local / L"VirtualStore" / files.file.lexically_normal().relative_path();
        return files;
    }

    // Whether the user can write the file: checked with the user's standard token, also when this process is elevated
    // or virtualized, so that the game and editors started either way come to the same answer
    inline bool UserCanWrite(const std::filesystem::path& file)
    {
        HANDLE process = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process))
            return true;
        HANDLE token = nullptr;
        TOKEN_ELEVATION_TYPE type{};
        DWORD size = 0;
        if (GetTokenInformation(process, TokenElevationType, &type, sizeof(type), &size) && type == TokenElevationTypeFull)
        {
            TOKEN_LINKED_TOKEN linked{};
            if (GetTokenInformation(process, TokenLinkedToken, &linked, sizeof(linked), &size))
                token = linked.LinkedToken;
        }
        if (!token && !DuplicateToken(process, SecurityIdentification, &token))
            token = nullptr;
        CloseHandle(process);
        if (!token)
            return true;

        auto allowed = [&](const std::filesystem::path& path, DWORD wanted)
        {
            const SECURITY_INFORMATION info = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
            DWORD needed = 0;
            GetFileSecurityW(path.c_str(), info, nullptr, 0, &needed);
            if (!needed)
                return false;
            std::vector<BYTE> descriptor(needed);
            if (!GetFileSecurityW(path.c_str(), info, descriptor.data(), needed, &needed))
                return false;
            GENERIC_MAPPING mapping = { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
            MapGenericMask(&wanted, &mapping);
            PRIVILEGE_SET privileges{};
            DWORD privilegesSize = sizeof(privileges), granted = 0;
            BOOL status = FALSE;
            return AccessCheck(descriptor.data(), token, wanted, &mapping, &privileges, &privilegesSize, &granted, &status) && status;
        };

        // The file is written to a temporary file next to it that then replaces it (WriteFileAtomic); missing folders
        // are made in the nearest one that exists
        bool result = false;
        auto folder = file.parent_path();
        for (DWORD attributes; !folder.empty(); folder = folder.parent_path())
        {
            attributes = GetFileAttributesW(folder.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES)
            {
                if (attributes & FILE_ATTRIBUTE_DIRECTORY)
                    result = allowed(folder, folder == file.parent_path() ? FILE_ADD_FILE : FILE_ADD_SUBDIRECTORY);
                break;
            }
            if (folder == folder.parent_path())
                break;
        }
        if (result)
        {
            DWORD attributes = GetFileAttributesW(file.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES)
                result = !(attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)) &&
                    (allowed(file, DELETE) || allowed(file.parent_path(), FILE_DELETE_CHILD));
        }
        CloseHandle(token);
        return result;
    }

    // The settings file in effect: the newest of the three, the settings file itself when none exists
    inline std::filesystem::path NewestCfg(const StorageFiles& files)
    {
        std::filesystem::path newest = files.file;
        std::optional<FILETIME> newestTime = detail::LastWrite(files.file);
        for (auto* candidate : { &files.fallback, &files.virtualStore })
        {
            auto time = detail::LastWrite(*candidate);
            if (time && (!newestTime || CompareFileTime(&*time, &*newestTime) > 0))
            {
                newest = *candidate;
                newestTime = time;
            }
        }
        return newest;
    }

    // Where the settings file is written: its own place when the user can write there, otherwise the fallback
    inline std::filesystem::path CfgWriteTarget(const StorageFiles& files)
    {
        return files.fallback.empty() || UserCanWrite(files.file) ? files.file : files.fallback;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Conditions and presets

    inline bool Evaluate(const Condition& c, const std::function<const Slot*(std::string_view)>& lookup)
    {
        switch (c.kind)
        {
            case Condition::Kind::All:
                return std::all_of(c.children.begin(), c.children.end(), [&](auto& x) { return Evaluate(x, lookup); });
            case Condition::Kind::Any:
                return std::any_of(c.children.begin(), c.children.end(), [&](auto& x) { return Evaluate(x, lookup); });
            case Condition::Kind::Not:
                return c.children.empty() || !Evaluate(c.children.front(), lookup);
            case Condition::Kind::Equals:
            case Condition::Kind::NotEquals:
            case Condition::Kind::In:
            {
                auto slot = lookup(c.setting);
                if (!slot)
                    return true;    // not understood: true (SPEC.md, Conditions)
                auto value = FormatValue(*slot);
                bool numeric = !slot->setting->StringValued();
                bool match = std::any_of(c.values.begin(), c.values.end(), [&](auto& v) { return detail::JsonEquals(v, value, numeric); });
                return c.kind == Condition::Kind::NotEquals ? !match : match;
            }
            default:
                return true;
        }
    }

    enum class Preset { Vanilla = 0, Recommended = 1, Enhanced = 2, Custom = -1 };

    // The preset all settings that take part in presets match, or Custom. Settings outside the presets (no preset
    // values, or the same in all three) don't count: a hand-edited one keeps the preset (SPEC.md, Presets)
    inline Preset CurrentPreset(const std::vector<Slot>& slots)
    {
        for (int p = 0; p < 3; ++p)
        {
            bool any = false, all = true;
            for (auto& slot : slots)
            {
                if (!slot.setting->InPresets())
                    continue;
                any = true;
                if (!detail::JsonEquals((*slot.setting->presets)[p], FormatValue(slot), !slot.setting->StringValued()))
                {
                    all = false;
                    break;
                }
            }
            if (any && all)
                return Preset(p);
        }
        return Preset::Custom;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // A plugin's settings at runtime

    class Plugin
    {
    public:
        // Reads the plugin's own description, translations and settings file, and writes the file in full when it's
        // missing or incomplete. module: the plugin's module, nullptr for the module of this code.
        bool Load(HMODULE module = nullptr)
        {
            std::lock_guard lock(mutex);
            if (!module)
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Instance), &module);
            if (!ReadModule(module, file, &error))
                return false;

            wchar_t path[MAX_PATH * 4] = {};
            GetModuleFileNameW(module, path, DWORD(std::size(path)));
            locations.plugin = path;
            GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
            locations.exe = path;
            storage = ResolveStorageFiles(file.description, locations);

            slots.clear();
            for (auto& s : file.description.settings)
            {
                Slot slot;
                slot.setting = &s;
                SetDefault(slot);
                slots.push_back(std::move(slot));
            }
            bool complete = ReadCfg();
            for (auto& slot : slots)
                UpdateEffective(slot);
            Instance() = this;
            if (!complete)
                SaveLocked();
            loaded = true;
            if (watchRequested || WatchWhenLoaded())
                WatchFile();
            return true;
        }

        ~Plugin()
        {
            StopWatching();
            if (Instance() == this)
                Instance() = nullptr;
        }

        bool Loaded() const { return loaded; }
        // Whether the last write of the settings file failed (the values stay in effect until the game is closed)
        bool SaveFailed() const { return saveFailed; }
        const std::string& Error() const { return error; }
        const Description& GetDescription() const { return file.description; }
        const std::string& Id() const { return file.id; }
        // The settings file in effect: the one last read or written
        const std::filesystem::path& CfgPath() const { return cfgPath; }
        Texts& GetTexts() { return file.texts; }

        // References to the values in effect, valid as long as the plugin is loaded.
        // They follow every change: from menus, editors, presets, and availability (SPEC.md, Availability). The code
        // reads them and doesn't write them; changes go through Set.
        const int& Int(std::string_view id) { auto s = Find(id); return s ? s->effective.intValue : dummy.effective.intValue; }
        const float& Float(std::string_view id) { auto s = Find(id); return s ? s->effective.floatValue : dummy.effective.floatValue; }
        bool Bool(std::string_view id) { return Int(id) != 0; }
        const std::string& String(std::string_view id) { auto s = Find(id); return s ? s->effective.stringValue : dummy.effective.stringValue; }

        // Whether the settings file had the setting when it was read (before Load wrote the missing ones), for plugins
        // whose default depends on the system, such as the installer's language
        bool Stored(std::string_view id)
        {
            std::lock_guard lock(mutex);
            auto s = Find(id);
            return s && s->stored;
        }

        // The player's choice as stored, which differs from the value in effect while a setting isn't usable
        std::string Wanted(std::string_view id)
        {
            std::lock_guard lock(mutex);
            auto s = Find(id);
            return s ? FormatValue(*s) : std::string();
        }

        // Applies a changed value; runs on the thread that changed it
        void OnChange(std::string_view id, std::function<void()> apply)
        {
            std::lock_guard lock(mutex);
            appliers.emplace_back(std::string(id), std::move(apply));
        }

        // The value in effect, as text
        std::string Get(std::string_view id)
        {
            std::lock_guard lock(mutex);
            auto s = Find(id);
            return s ? FormatEffective(*s) : std::string();
        }

        // Checks, applies and stores a value given as text. Values that can't be used now are refused
        // (ASI_SETTINGS_NOT_NOW), unless force: presets store them as usual, the value in effect falls back.
        int32_t Set(std::string_view id, std::string_view value, bool force = false)
        {
            std::string settingId;
            bool effectChanged = false;
            {
                std::lock_guard lock(mutex);
                auto s = Find(id);
                if (!s)
                    return ASI_SETTINGS_UNKNOWN_ID;
                if (!s->setting->HasValue())
                    return ASI_SETTINGS_UNSUPPORTED;
                Slot candidate = *s;
                if (!ParseValue(candidate, value, true))
                    return ASI_SETTINGS_INVALID_VALUE;
                if (!force && !Usable(s->setting->id, FormatValue(candidate)))
                    return ASI_SETTINGS_NOT_NOW;
                candidate.unread.clear();
                *s = std::move(candidate);
                settingId = s->setting->id;
                SaveLocked();
                effectChanged = UpdateEffective(*s);
            }
            if (effectChanged)
                RunAppliers(settingId);
            Notify(settingId);
            return ASI_SETTINGS_OK;
        }

        Preset CurrentPreset()
        {
            std::lock_guard lock(mutex);
            return asi_settings::CurrentPreset(slots);
        }

        // Sets every preset setting; true when a changed setting applies after a restart
        bool ApplyPreset(Preset preset)
        {
            if (preset == Preset::Custom)
                return false;
            std::vector<std::pair<std::string, std::string>> changes;
            bool restart = false;
            {
                std::lock_guard lock(mutex);
                for (auto& slot : slots)
                {
                    auto& p = slot.setting->presets;
                    if (!slot.setting->InPresets())
                        continue;
                    auto value = detail::JsonToText((*p)[int(preset)]);
                    if (value != FormatValue(slot))
                    {
                        changes.emplace_back(slot.setting->id, value);
                        restart |= slot.setting->apply == "restart";
                    }
                }
            }
            for (auto& [id, value] : changes)
                Set(id, value, true);
            return restart;
        }

        bool Visible(std::string_view id) { return Check(id, true); }
        bool Enabled(std::string_view id) { return Check(id, false); }

        // Availability (SPEC.md, Availability): usable(value) tells whether a value (as text) can be used now. The value in
        // effect is the player's choice when usable, otherwise the default, otherwise the first usable value; the
        // settings file keeps the choice. reason (optional): why values are missing, as text. Runs now and on Refresh.
        void Availability(std::string_view id, std::function<bool(std::string_view value)> usable,
            std::function<std::string()> reason = {})
        {
            {
                std::lock_guard lock(mutex);
                checks[text::ToLower(id)] = { std::move(usable), std::move(reason) };
            }
            Refresh(id);
        }

        // Asks a setting's availability again, when what it depends on may have changed (a display was switched,
        // files appeared, an hour passed). The value in effect and menus follow.
        void Refresh(std::string_view id)
        {
            std::string settingId;
            bool effectChanged = false;
            {
                std::lock_guard lock(mutex);
                auto s = Find(id);
                if (!s)
                    return;
                settingId = s->setting->id;
                effectChanged = UpdateEffective(*s);
            }
            if (effectChanged)
                RunAppliers(settingId);
            Notify(settingId);
        }

        void RefreshAll()
        {
            std::vector<std::string> ids;
            {
                std::lock_guard lock(mutex);
                for (auto& [id, c] : checks)
                    ids.push_back(id);
            }
            for (auto& id : ids)
                Refresh(id);
        }

        // Whether a value can be used now (always, for settings without availability)
        bool Usable(std::string_view id, std::string_view value)
        {
            std::lock_guard lock(mutex);
            auto it = checks.find(text::ToLower(id));
            return it == checks.end() || !it->second.usable || it->second.usable(value);
        }

        // The values usable now: the choices (or the two values of a bool) that pass the check, as text
        std::vector<std::string> UsableValues(std::string_view id)
        {
            std::lock_guard lock(mutex);
            std::vector<std::string> out;
            auto s = Find(id);
            if (!s)
                return out;
            for (auto& v : Candidates(*s->setting))
            {
                if (Usable(s->setting->id, v))
                    out.push_back(v);
            }
            return out;
        }

        // 0, or ASI_SETTINGS_STATE_HIDDEN when no value is usable now; reason: the plugin's text for it
        int32_t State(std::string_view id, std::string* reason = nullptr)
        {
            std::lock_guard lock(mutex);
            auto s = Find(id);
            if (!s)
                return ASI_SETTINGS_UNKNOWN_ID;
            auto it = checks.find(text::ToLower(s->setting->id));
            if (it == checks.end() || !it->second.usable)
                return 0;
            if (reason && it->second.reason)
                *reason = it->second.reason();
            auto candidates = Candidates(*s->setting);
            bool any = candidates.empty() ? it->second.usable(FormatValue(*s)) :
                std::any_of(candidates.begin(), candidates.end(), [&](const std::string& v) { return it->second.usable(v); });
            return any ? 0 : ASI_SETTINGS_STATE_HIDDEN;
        }

        // Reads the settings file again (an editor changed it)
        int32_t Reload()
        {
            std::vector<std::string> changed, effectChanged;
            {
                std::lock_guard lock(mutex);
                ReloadLocked(nullptr, nullptr, changed, effectChanged);
            }
            Apply(changed, effectChanged);
            return ASI_SETTINGS_OK;
        }

        // Reads the settings file again whenever something else changes it (a text editor), as Reload does: change
        // callbacks and subscribers hear of it as of any change. The plugin's own writes are recognised by their text
        // and read nothing. Runs on a thread of its own that is only started here and never waited for, so it can be
        // called from DllMain; called before Load, it starts once the plugin is loaded. LiveLink::Start calls it.
        bool WatchFile()
        {
            watchRequested = true;
            if (!loaded)
                return true;
            if (watcher.load())
                return true;
            auto w = new Watcher();
            w->plugin = this;
            w->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            Watcher* none = nullptr;
            if (!w->stop || !watcher.compare_exchange_strong(none, w))
            {
                if (w->stop)
                    CloseHandle(w->stop);
                delete w;
                return none != nullptr;
            }
            // The thread holds a reference to this module, so that it can't be unloaded under it; a plugin that is
            // unloaded on purpose calls StopWatching (or LiveLink::Stop) first
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Instance), &self);
            w->module = self != GetModuleHandleW(nullptr) ? self : nullptr;
            if (!w->module && self)
                FreeLibrary(self);
            w->thread = CreateThread(nullptr, 0, &WatchThread, w, 0, &w->threadId);
            if (!w->thread)
            {
                watcher = nullptr;
                if (w->module)
                    FreeLibrary(w->module);
                CloseHandle(w->stop);
                delete w;
                return false;
            }
            return true;
        }

        // Stops watching the settings file. Not from DllMain, which can't wait for the thread.
        void StopWatching()
        {
            watchRequested = false;
            auto w = watcher.exchange(nullptr);
            if (!w)
                return;
            SetEvent(w->stop);
            // From a change callback on the watching thread, or a thread that doesn't end: it keeps its data
            if (w->threadId == GetCurrentThreadId() || WaitForSingleObject(w->thread, 5000) != WAIT_OBJECT_0)
                return;
            CloseHandle(w->thread);
            CloseHandle(w->stop);
            delete w;
        }

        bool Watching() const { return watcher.load() != nullptr; }
        // How often a changed settings file was read by the watcher
        uint32_t FileReloads() const { return fileReloads; }
        // Set by LiveLink::Start: the plugin of this module watches its file once loaded
        static bool& WatchWhenLoaded()
        {
            static bool watch = false;
            return watch;
        }

        void Save()
        {
            std::lock_guard lock(mutex);
            SaveLocked();
        }

        const AsiSettingsApi* Api()
        {
            static AsiSettingsApi api = {
                sizeof(AsiSettingsApi), ASI_SETTINGS_VERSION, nullptr,
                &Thunk::Get, &Thunk::Set, &Thunk::GetChoices, &Thunk::Invoke, &Thunk::Subscribe, &Thunk::Unsubscribe, &Thunk::Reload,
                &Thunk::GetState,
            };
            api.pluginId = file.id.c_str();
            return loaded ? &api : nullptr;
        }

        // Choices of dynamic settings and actions, provided by the plugin's code
        std::function<std::string(std::string_view id)> dynamicChoices;     // returns a JSON array
        std::function<int32_t(std::string_view id)> invoke;

        static Plugin*& Instance()
        {
            static Plugin* instance = nullptr;
            return instance;
        }

    private:
        std::recursive_mutex mutex;
        DescribedFile file;
        Locations locations;
        StorageFiles storage;
        std::filesystem::path cfgPath;
        std::vector<Slot> slots;   // filled once by Load: references to values stay valid
        Slot dummy;
        std::vector<std::pair<std::string, std::function<void()>>> appliers;
        struct Subscriber { int32_t token; AsiSettingsChanged callback; void* context; };
        std::vector<Subscriber> subscribers;
        int32_t nextToken = 1;
        bool loaded = false;
        bool saveFailed = false;
        std::string error;
        std::optional<CfgFile> lastFile;
        struct Check { std::function<bool(std::string_view)> usable; std::function<std::string()> reason; };
        std::map<std::string, Check> checks;
        std::string cfgText;    // the text of cfgPath as last read or written
        struct Watcher
        {
            Plugin* plugin = nullptr;
            HANDLE stop = nullptr, thread = nullptr;
            DWORD threadId = 0;
            HMODULE module = nullptr;
        };
        std::atomic<Watcher*> watcher = nullptr;
        std::atomic<bool> watchRequested = false;
        std::atomic<uint32_t> fileReloads = 0;

        // The values a setting can take, as text, for checking which are usable: choices, or both values of a bool
        static std::vector<std::string> Candidates(const Setting& s)
        {
            std::vector<std::string> out;
            if (s.type == Type::Bool)
                return { "0", "1" };
            for (auto& c : s.choices)
                out.push_back(detail::JsonToText(c.value));
            for (auto& c : s.special)
                out.push_back(detail::JsonToText(c.value));
            for (auto& c : s.suggestions)
                out.push_back(detail::JsonToText(c.value));
            return out;
        }

        static std::string FormatEffective(const Slot& slot)
        {
            Slot view;
            view.setting = slot.setting;
            view.intValue = slot.effective.intValue;
            view.floatValue = slot.effective.floatValue;
            view.stringValue = slot.effective.stringValue;
            return FormatValue(view);
        }

        // The value in effect: the player's choice when usable, otherwise the default, otherwise the first usable value
        // (SPEC.md, Availability); true when it changed
        bool UpdateEffective(Slot& slot)
        {
            Slot source = slot;
            auto it = checks.find(text::ToLower(slot.setting->id));
            if (it != checks.end() && it->second.usable && !it->second.usable(FormatValue(slot)))
            {
                std::vector<std::string> order = { detail::JsonToText(slot.setting->defaultValue) };
                for (auto& v : Candidates(*slot.setting))
                    order.push_back(v);
                for (auto& v : order)
                {
                    if (it->second.usable(v) && ParseValue(source, v, false))
                        break;
                }
                // Nothing usable: the default (the setting is hidden)
                if (std::none_of(order.begin(), order.end(), [&](auto& v) { return it->second.usable(v); }))
                    ParseValue(source, order.front(), false);
            }
            bool changed = slot.effective.intValue != source.intValue || slot.effective.floatValue != source.floatValue ||
                slot.effective.stringValue != source.stringValue;
            slot.effective = { source.intValue, source.floatValue, source.stringValue };
            return changed;
        }

        void RunAppliers(const std::string& id)
        {
            std::vector<std::function<void()>> toRun;
            {
                std::lock_guard lock(mutex);
                for (auto& [name, apply] : appliers)
                {
                    if (text::IEquals(name, id))
                        toRun.push_back(apply);
                }
            }
            for (auto& apply : toRun)
                apply();
        }

        Slot* Find(std::string_view id)
        {
            for (auto& s : slots)
            {
                if (text::IEquals(s.setting->id, id))
                    return &s;
            }
            return nullptr;
        }

        // Fills the slots from the file; false when the file is missing or lacks settings
        bool ReadCfg()
        {
            cfgPath = NewestCfg(storage);
            auto source = ReadFileText(cfgPath);
            if (!source)
            {
                lastFile.reset();
                cfgText.clear();
                return false;
            }
            return ReadCfgText(*source);
        }

        bool ReadCfgText(const std::string& source)
        {
            cfgText = source;
            lastFile = ParseCfg(source);
            bool complete = true;
            for (auto& slot : slots)
            {
                auto& s = *slot.setting;
                slot.stored = false;
                if (!s.HasValue() || s.storage != "cfg")
                    continue;
                const std::string* value = lastFile->Find(s.section, s.key);
                for (size_t i = 0; !value && i < s.aliases.size(); ++i)
                {
                    auto dot = s.aliases[i].find('.');
                    if (dot != std::string::npos)
                        value = lastFile->Find(s.aliases[i].substr(0, dot), s.aliases[i].substr(dot + 1));
                }
                if (!value)
                {
                    complete = false;
                    continue;
                }
                slot.stored = true;
                if (!ParseValue(slot, *value, false))
                    slot.unread = *value;   // kept as it is until changed (SPEC.md, Reading)
            }
            return complete;
        }

        void SaveLocked()
        {
            Texts english = file.texts;
            english.Select("en");
            auto content = WriteCfg(file.description, english.Resolve(file.description.name), english, slots, lastFile ? &*lastFile : nullptr);
            // Where the user can write: the settings file's own place or the fallback (SPEC.md, Folders the user
            // can't write to); the file there then is the newest one
            auto target = CfgWriteTarget(storage);
            std::error_code ec;
            std::filesystem::create_directories(target.parent_path(), ec);
            saveFailed = !WriteFileAtomic(target, content);
            if (!saveFailed)
            {
                cfgPath = target;
                cfgText = content;
                lastFile = ParseCfg(content);
            }
        }

        // Reads the settings file again, from source (read from path) when given; the ids whose value changed, and
        // those whose value in effect changed
        void ReloadLocked(const std::filesystem::path* path, const std::string* source, std::vector<std::string>& changed,
            std::vector<std::string>& effectChanged)
        {
            std::vector<std::string> wanted;
            for (auto& s : slots)
                wanted.push_back(FormatValue(s));
            for (auto& s : slots)
                SetDefault(s);
            if (source)
            {
                cfgPath = *path;
                ReadCfgText(*source);
            }
            else
                ReadCfg();
            for (size_t i = 0; i < slots.size(); ++i)
            {
                bool effect = UpdateEffective(slots[i]);
                if (effect || FormatValue(slots[i]) != wanted[i])
                    changed.push_back(slots[i].setting->id);
                if (effect)
                    effectChanged.push_back(slots[i].setting->id);
            }
        }

        void Apply(const std::vector<std::string>& changed, const std::vector<std::string>& effectChanged)
        {
            for (auto& id : effectChanged)
                RunAppliers(id);
            for (auto& id : changed)
                Notify(id);
        }

        // The settings file in effect changed on disk: read again when its text isn't what the plugin last read or
        // wrote. False while it can't be read yet (an editor still writes it); a deleted file keeps the values.
        bool ReloadFromFile()
        {
            std::vector<std::string> changed, effectChanged;
            {
                std::lock_guard lock(mutex);
                auto path = NewestCfg(storage);
                DWORD error = 0;
                auto source = ReadFileText(path, &error);
                if (!source)
                    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
                if (source->empty())
                    return false;
                if (path == cfgPath && *source == cfgText)
                    return true;
                ReloadLocked(&path, &*source, changed, effectChanged);
                ++fileReloads;
            }
            Apply(changed, effectChanged);
            return true;
        }

        static DWORD WINAPI WatchThread(void* param)
        {
            auto w = static_cast<Watcher*>(param);
            HMODULE module = w->module;
            try { w->plugin->WatchLoop(w->stop); }
            catch (...) {}
            if (module)
                FreeLibraryAndExitThread(module, 0);
            return 0;
        }

        // Watches the folders of the three places of the settings file (ReadDirectoryChangesW). A folder that can't be
        // watched (missing, or a file system without notifications) is looked at every 2 seconds.
        void WatchLoop(HANDLE stop)
        {
            struct Folder
            {
                std::filesystem::path path;
                std::vector<std::wstring> names;
                std::vector<ULONGLONG> stamps;      // last write times, while the folder isn't watched
                HANDLE handle = INVALID_HANDLE_VALUE, event = nullptr;
                OVERLAPPED overlapped{};
                DWORD buffer[1024];                 // FILE_NOTIFY_INFORMATION entries, DWORD aligned
            };
            std::vector<std::unique_ptr<Folder>> folders;
            {
                std::lock_guard lock(mutex);
                for (auto* file : { &storage.file, &storage.fallback, &storage.virtualStore })
                {
                    if (file->empty())
                        continue;
                    auto key = detail::PathKey(file->parent_path());
                    auto it = std::find_if(folders.begin(), folders.end(), [&](auto& f) { return detail::PathKey(f->path) == key; });
                    if (it == folders.end())
                    {
                        folders.push_back(std::make_unique<Folder>());
                        folders.back()->path = file->parent_path();
                        it = folders.end() - 1;
                    }
                    (*it)->names.push_back(file->filename().wstring());
                }
            }
            auto stampsOf = [](const Folder& f)
            {
                std::vector<ULONGLONG> stamps;
                for (auto& name : f.names)
                {
                    auto time = detail::LastWrite(f.path / name);
                    stamps.push_back(time ? (ULONGLONG(time->dwHighDateTime) << 32 | time->dwLowDateTime) : 0);
                }
                return stamps;
            };
            auto arm = [](Folder& f)
            {
                if (f.handle == INVALID_HANDLE_VALUE)
                {
                    f.handle = CreateFileW(f.path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
                    if (f.handle == INVALID_HANDLE_VALUE)
                        return false;
                }
                f.overlapped = {};
                f.overlapped.hEvent = f.event;
                ResetEvent(f.event);
                if (ReadDirectoryChangesW(f.handle, f.buffer, DWORD(sizeof(f.buffer)), FALSE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE, nullptr, &f.overlapped, nullptr))
                    return true;
                CloseHandle(f.handle);
                f.handle = INVALID_HANDLE_VALUE;
                return false;
            };
            // Whether the notifications name a watched file (a lost buffer may have)
            auto names = [](const Folder& f, DWORD bytes)
            {
                if (bytes == 0)
                    return true;
                auto at = reinterpret_cast<const BYTE*>(f.buffer);
                for (DWORD offset = 0; offset + sizeof(FILE_NOTIFY_INFORMATION) <= bytes;)
                {
                    auto info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(at + offset);
                    std::wstring_view name(info->FileName, info->FileNameLength / sizeof(wchar_t));
                    if (info->Action != FILE_ACTION_REMOVED && info->Action != FILE_ACTION_RENAMED_OLD_NAME)
                    {
                        for (auto& n : f.names)
                        {
                            if (name.size() == n.size() && _wcsnicmp(name.data(), n.data(), n.size()) == 0)
                                return true;
                        }
                    }
                    if (!info->NextEntryOffset)
                        break;
                    offset += info->NextEntryOffset;
                }
                return false;
            };

            // Editors write in several steps: the file is read once it has been quiet for a moment, and again a little
            // later while it can't be read
            constexpr DWORD settle = 200, retryDelay = 100;
            constexpr int maxRetries = 20;
            bool due = true;            // a change between Load and now
            DWORD dueAt = GetTickCount();
            int retries = 0;
            auto schedule = [&](DWORD delay)
            {
                due = true;
                dueAt = GetTickCount() + delay;
            };
            for (auto& f : folders)
            {
                f->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                f->stamps = stampsOf(*f);
                if (f->event)
                    arm(*f);
            }

            DWORD nextPoll = GetTickCount() + 2000;
            for (;;)
            {
                HANDLE handles[4];
                Folder* watched[4] = {};
                DWORD count = 0;
                handles[count++] = stop;
                bool polling = false;
                for (auto& f : folders)
                {
                    if (f->handle != INVALID_HANDLE_VALUE)
                    {
                        watched[count] = f.get();
                        handles[count++] = f->event;
                    }
                    else
                        polling |= f->event != nullptr;
                }
                auto left = [](DWORD at) { return DWORD((std::max)(LONG(at - GetTickCount()), 0L)); };
                DWORD timeout = INFINITE;
                if (polling)
                    timeout = left(nextPoll);
                if (due)
                    timeout = (std::min)(timeout, left(dueAt));
                DWORD wait = WaitForMultipleObjects(count, handles, FALSE, timeout);
                if (wait == WAIT_OBJECT_0)
                    break;
                if (wait > WAIT_OBJECT_0 && wait < WAIT_OBJECT_0 + count)
                {
                    auto& f = *watched[wait - WAIT_OBJECT_0];
                    DWORD bytes = 0;
                    if (GetOverlappedResult(f.handle, &f.overlapped, &bytes, FALSE))
                    {
                        if (names(f, bytes))
                        {
                            schedule(settle);
                            retries = 0;
                        }
                        arm(f);
                    }
                    else
                    {
                        // The folder went away: looked for again
                        CloseHandle(f.handle);
                        f.handle = INVALID_HANDLE_VALUE;
                        f.stamps = stampsOf(f);
                    }
                }
                else if (wait == WAIT_FAILED)
                    Sleep(100);     // don't spin
                if (polling && left(nextPoll) == 0)
                {
                    nextPoll = GetTickCount() + 2000;
                    for (auto& f : folders)
                    {
                        if (f->handle != INVALID_HANDLE_VALUE || !f->event)
                            continue;
                        auto stamps = stampsOf(*f);
                        bool armed = arm(*f);
                        if (stamps != f->stamps || armed)
                            schedule(armed ? settle : 0);
                        f->stamps = std::move(stamps);
                    }
                }
                if (due && left(dueAt) == 0)
                {
                    due = false;
                    if (ReloadFromFile())
                        retries = 0;
                    else if (++retries < maxRetries)
                        schedule(retryDelay);
                    else
                        retries = 0;
                }
            }

            for (auto& f : folders)
            {
                if (f->handle != INVALID_HANDLE_VALUE)
                {
                    // The pending read writes into the folder's buffer until it's cancelled
                    DWORD bytes = 0;
                    CancelIo(f->handle);
                    GetOverlappedResult(f->handle, &f->overlapped, &bytes, TRUE);
                    CloseHandle(f->handle);
                }
                if (f->event)
                    CloseHandle(f->event);
            }
        }

        bool Check(std::string_view id, bool visible)
        {
            std::lock_guard lock(mutex);
            auto s = Find(id);
            if (!s)
                return false;
            auto& c = visible ? s->setting->visibleIf : s->setting->enabledIf;
            if (!c)
                return true;
            return Evaluate(*c, [this](std::string_view other) -> const Slot* { return Find(other); });
        }

        void Notify(const std::string& id)
        {
            std::vector<Subscriber> copy;
            {
                std::lock_guard lock(mutex);
                copy = subscribers;
            }
            for (auto& s : copy)
                s.callback(s.context, id.c_str());
        }

        struct Thunk
        {
            static int32_t ASI_SETTINGS_CALL Get(const char* id, char* value, uint32_t size)
            {
                auto p = Instance();
                if (!p || !id)
                    return ASI_SETTINGS_INTERNAL;
                try
                {
                    std::lock_guard lock(p->mutex);
                    auto s = p->Find(id);
                    if (!s)
                        return ASI_SETTINGS_UNKNOWN_ID;
                    return text::CopyOut(FormatEffective(*s), value, size);
                }
                catch (...) { return ASI_SETTINGS_INTERNAL; }
            }
            static int32_t ASI_SETTINGS_CALL Set(const char* id, const char* value)
            {
                auto p = Instance();
                if (!p || !id || !value)
                    return ASI_SETTINGS_INTERNAL;
                try { return p->Set(id, value); }
                catch (...) { return ASI_SETTINGS_INTERNAL; }
            }
            static int32_t ASI_SETTINGS_CALL GetChoices(const char* id, char* json, uint32_t size)
            {
                auto p = Instance();
                if (!p || !id)
                    return ASI_SETTINGS_INTERNAL;
                try
                {
                    auto s = p->GetDescription().Find(id);
                    if (!s)
                        return ASI_SETTINGS_UNKNOWN_ID;
                    if (s->dynamic && p->dynamicChoices)
                        return text::CopyOut(p->dynamicChoices(id), json, size);
                    bool hasCheck;
                    {
                        std::lock_guard lock(p->mutex);
                        hasCheck = p->checks.count(text::ToLower(s->id)) != 0;
                    }
                    if (!hasCheck)
                        return ASI_SETTINGS_UNSUPPORTED;
                    // The usable values, as {"value": ...} entries; labels come from the description
                    std::string out = "[";
                    for (auto& v : p->UsableValues(id))
                    {
                        out += out.size() > 1 ? "," : "";
                        if (!s->StringValued())
                        {
                            out += "{\"value\":" + v + "}";
                            continue;
                        }
                        // JSON string escapes: quotes, backslashes and control characters
                        std::string quoted;
                        for (char c : v)
                        {
                            if (c == '"' || c == '\\')
                                quoted += { '\\', c };
                            else if (static_cast<unsigned char>(c) < 0x20)
                            {
                                char u[8];
                                snprintf(u, sizeof(u), "\\u%04x", unsigned(static_cast<unsigned char>(c)));
                                quoted += u;
                            }
                            else
                                quoted += c;
                        }
                        out += "{\"value\":\"" + quoted + "\"}";
                    }
                    return text::CopyOut(out + "]", json, size);
                }
                catch (...) { return ASI_SETTINGS_INTERNAL; }
            }
            static int32_t ASI_SETTINGS_CALL Invoke(const char* id)
            {
                auto p = Instance();
                if (!p || !id)
                    return ASI_SETTINGS_INTERNAL;
                try { return p->invoke ? p->invoke(id) : ASI_SETTINGS_UNSUPPORTED; }
                catch (...) { return ASI_SETTINGS_INTERNAL; }
            }
            static int32_t ASI_SETTINGS_CALL Subscribe(AsiSettingsChanged callback, void* context)
            {
                auto p = Instance();
                if (!p || !callback)
                    return ASI_SETTINGS_INTERNAL;
                try
                {
                    std::lock_guard lock(p->mutex);
                    int32_t token = p->nextToken++;
                    p->subscribers.push_back({ token, callback, context });
                    return token;
                }
                catch (...) { return ASI_SETTINGS_INTERNAL; }
            }
            static int32_t ASI_SETTINGS_CALL Unsubscribe(int32_t token)
            {
                auto p = Instance();
                if (!p)
                    return ASI_SETTINGS_INTERNAL;
                try
                {
                    std::lock_guard lock(p->mutex);
                    auto it = std::find_if(p->subscribers.begin(), p->subscribers.end(), [&](auto& s) { return s.token == token; });
                    if (it == p->subscribers.end())
                        return ASI_SETTINGS_UNKNOWN_ID;
                    p->subscribers.erase(it);
                    return ASI_SETTINGS_OK;
                }
                catch (...) { return ASI_SETTINGS_INTERNAL; }
            }
            static int32_t ASI_SETTINGS_CALL Reload()
            {
                auto p = Instance();
                if (!p)
                    return ASI_SETTINGS_INTERNAL;
                try { return p->Reload(); }
                catch (...) { return ASI_SETTINGS_INTERNAL; }
            }
            static int32_t ASI_SETTINGS_CALL GetState(const char* id, char* reason, uint32_t reasonSize)
            {
                auto p = Instance();
                if (!p || !id)
                    return ASI_SETTINGS_INTERNAL;
                try
                {
                    std::string why;
                    int32_t flags = p->State(id, &why);
                    if (flags >= 0)
                        text::CopyOut(why, reason, reasonSize);
                    return flags;
                }
                catch (...) { return ASI_SETTINGS_INTERNAL; }
            }
        };
    };

    // ---------------------------------------------------------------------------------------------------------------
    // Menu hosts

    struct LoadedPlugin
    {
        HMODULE module = nullptr;
        const AsiSettingsApi* api = nullptr;
        DescribedFile file;
    };

    // The plugins of the process with settings, in menu order (SPEC.md, Order of plugins): the host first, then by
    // priority, then by English name and id. language: the game's language tag for their texts.
    inline std::vector<LoadedPlugin> FindLoadedPlugins(HMODULE host, std::string_view language)
    {
        std::vector<LoadedPlugin> plugins;
        HMODULE modules[1024];
        DWORD needed = 0;
        if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
            return plugins;
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < std::size(modules); ++i)
        {
            auto getApi = reinterpret_cast<AsiSettingsGetApi>(GetProcAddress(modules[i], ASI_SETTINGS_EXPORT_NAME));
            if (!getApi)
                continue;
            LoadedPlugin plugin;
            plugin.module = modules[i];
            plugin.api = getApi(ASI_SETTINGS_VERSION);
            // Another major version's table, or one without Get and Set, can't be used (SPEC.md, Versions)
            if (!ASI_SETTINGS_API_COMPATIBLE(plugin.api) || !ASI_SETTINGS_API_HAS(plugin.api, Get) || !ReadModule(modules[i], plugin.file))
                continue;
            plugin.file.texts.Select(language);
            plugins.push_back(std::move(plugin));
        }
        std::stable_sort(plugins.begin(), plugins.end(), [&](const LoadedPlugin& a, const LoadedPlugin& b)
        {
            if ((a.module == host) != (b.module == host))
                return a.module == host;
            if (a.file.description.priority != b.file.description.priority)
                return a.file.description.priority > b.file.description.priority;
            Texts ea = a.file.texts, eb = b.file.texts;
            ea.Select("en");
            eb.Select("en");
            auto na = text::ToLower(ea.Resolve(a.file.description.name)), nb = text::ToLower(eb.Resolve(b.file.description.name));
            if (na != nb)
                return na < nb;
            return text::ToLower(a.file.id) < text::ToLower(b.file.id);
        });
        return plugins;
    }

    // Whether this host draws other plugins' pages (SPEC.md, Menu hosts): the first host to ask keeps the role
    inline bool ClaimHostRole()
    {
        static HANDLE mutex = nullptr;
        if (mutex)
            return true;
        wchar_t name[64];
        swprintf_s(name, L"Local\\AsiSettingsHost.%lu", GetCurrentProcessId());
        HANDLE handle = CreateMutexW(nullptr, FALSE, name);
        if (!handle)
            return false;
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(handle);
            return false;
        }
        mutex = handle;
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The live link (SPEC.md, Live link): editors read and change the running plugins' settings through a named pipe,
    // \\.\pipe\AsiSettings.<process id>, served by one plugin of the process for all of them. Every call goes through
    // the plugins' AsiSettings_GetApi tables, so the running plugins stay the only source of truth: they check, apply
    // (on their own thread when they queue changes) and store the values as for their menus. Opt-in: a plugin that
    // never calls LiveLink::Start has none of this code. Starting it also watches the plugin's settings file for hand
    // edits (Plugin::WatchFile), whichever plugin serves.
    //     settings.Load();
    //     asi_settings::LiveLink::Start();     // the first plugin of the process to call it serves; also from DllMain

    namespace detail
    {
        // A JSON string with its quotes
        inline std::string JsonQuote(std::string_view s)
        {
            std::string out = "\"";
            for (char c : s)
            {
                if (c == '"' || c == '\\')
                    out += { '\\', c };
                else if (c == '\n')
                    out += "\\n";
                else if (c == '\r')
                    out += "\\r";
                else if (c == '\t')
                    out += "\\t";
                else if (static_cast<unsigned char>(c) < 0x20)
                {
                    char u[8];
                    snprintf(u, sizeof(u), "\\u%04x", unsigned(static_cast<unsigned char>(c)));
                    out += u;
                }
                else
                    out += c;
            }
            return out + "\"";
        }

        // FileVersion of the version resource as a.b.c.d (its fixed part, read without version.dll), empty without one
        inline std::string FileVersion(HMODULE module)
        {
            auto data = ResourceData(module, MAKEINTRESOURCEW(16) /* RT_VERSION */, MAKEINTRESOURCEW(1));
            for (size_t i = 0; i + sizeof(VS_FIXEDFILEINFO) <= data.size(); i += 4)
            {
                VS_FIXEDFILEINFO info;
                std::memcpy(&info, data.data() + i, sizeof(info));
                if (info.dwSignature != 0xFEEF04BD)
                    continue;
                return std::to_string(HIWORD(info.dwFileVersionMS)) + "." + std::to_string(LOWORD(info.dwFileVersionMS)) + "." +
                    std::to_string(HIWORD(info.dwFileVersionLS)) + "." + std::to_string(LOWORD(info.dwFileVersionLS));
            }
            return {};
        }

        // The text of a snprintf-like call of the runtime interface: grows the buffer until it fits; the result code
        // when it's negative
        template<typename Call>
        inline int32_t CallText(Call call, std::string& out)
        {
            std::string buffer(256, '\0');
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                int32_t n = call(buffer.data(), uint32_t(buffer.size()));
                if (n < 0)
                    return n;
                if (uint32_t(n) < buffer.size())
                {
                    out.assign(buffer.data(), size_t(n));
                    return n;
                }
                buffer.assign(size_t(n) + 1, '\0');
            }
            return ASI_SETTINGS_INTERNAL;
        }

        inline const char* ResultName(int32_t result)
        {
            switch (result)
            {
                case ASI_SETTINGS_UNKNOWN_ID: return "UNKNOWN_ID";
                case ASI_SETTINGS_INVALID_VALUE: return "INVALID_VALUE";
                case ASI_SETTINGS_NOT_NOW: return "NOT_NOW";
                case ASI_SETTINGS_UNSUPPORTED: return "UNSUPPORTED";
                default: return result >= 0 ? "OK" : "INTERNAL";
            }
        }
    }

    class LiveLink
    {
    public:
        static constexpr size_t MaxClients = 8;
        static constexpr size_t MaxLine = 64 * 1024;     // a longer request closes the connection

        // Serves the process' plugins unless another plugin of the process already does: the first to call it serves
        // (the mutex Local\AsiSettingsLink.<process id>). True when this module serves. Either way, this module's
        // plugin watches its settings file for hand edits (Plugin::WatchFile). Can be called from DllMain: the pipe
        // is made on the link's own thread.
        static bool Start()
        {
            Plugin::WatchWhenLoaded() = true;
            if (auto p = Plugin::Instance(); p && p->Loaded())
                p->WatchFile();
            auto& s = Instance();
            if (s)
                return true;
            wchar_t name[64];
            swprintf_s(name, L"%s%lu", ASI_SETTINGS_LINK_MUTEX, GetCurrentProcessId());
            HANDLE mutex = CreateMutexW(nullptr, FALSE, name);
            if (!mutex)
                return false;
            if (GetLastError() == ERROR_ALREADY_EXISTS)
            {
                CloseHandle(mutex);
                return false;
            }
            auto state = new State();
            state->mutex = mutex;
            state->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            state->changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            // The thread holds a reference to this module, so that it can't be unloaded under it; a plugin that is
            // unloaded on purpose calls Stop first
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Instance), &self);
            state->module = self != GetModuleHandleW(nullptr) ? self : nullptr;
            if (!state->module && self)
                FreeLibrary(self);  // the exe itself: nothing to keep loaded
            state->thread = CreateThread(nullptr, 0, &Run, state, 0, nullptr);
            if (!state->thread)
            {
                if (state->module)
                    FreeLibrary(state->module);
                CloseHandle(state->stop);
                CloseHandle(state->changed);
                CloseHandle(mutex);
                delete state;
                return false;
            }
            s = state;
            return true;
        }

        // Stops serving (only when this module serves): clients are disconnected and another plugin may serve. Stops
        // watching this module's settings file too. Not from DllMain, which can't wait for the threads.
        static void Stop()
        {
            Plugin::WatchWhenLoaded() = false;
            if (auto p = Plugin::Instance())
                p->StopWatching();
            auto& s = Instance();
            if (!s)
                return;
            SetEvent(s->stop);
            WaitForSingleObject(s->thread, 5000);
            CloseHandle(s->thread);
            CloseHandle(s->mutex);
            // The state stays allocated: a plugin may still be calling a change callback that points at it
            s = nullptr;
        }

        static bool Serving() { return Instance() != nullptr; }

    private:
        struct State;

        // A plugin of the process, found by its export; kept while serving (callbacks point at it)
        struct Known
        {
            State* owner = nullptr;
            HMODULE module = nullptr;
            const AsiSettingsApi* api = nullptr;
            DescribedFile file;
            std::string name, version, path;
            int32_t token = 0;

            bool Has(size_t fieldEnd) const { return api->size >= fieldEnd; }
        };

        struct Client
        {
            HANDLE pipe = INVALID_HANDLE_VALUE;
            HANDLE readEvent = nullptr, writeEvent = nullptr;
            OVERLAPPED read{};
            char buffer[4096];
            std::string line;
            bool subscribed = false;
        };

        struct State
        {
            HANDLE mutex = nullptr, stop = nullptr, changed = nullptr, thread = nullptr;
            HMODULE module = nullptr;
            std::mutex lock;    // pending, filled by the plugins' change callbacks on any thread
            std::vector<std::pair<Known*, std::string>> pending;
            std::vector<std::unique_ptr<Known>> plugins;
            std::vector<std::unique_ptr<Client>> clients;
        };

        static State*& Instance()
        {
            static State* state = nullptr;
            return state;
        }

        // A plugin's settings changed (value or state), on whatever thread changed them: noted, sent by the link's thread
        static void ASI_SETTINGS_CALL Changed(void* context, const char* id)
        {
            auto known = static_cast<Known*>(context);
            if (!known || !id)
                return;
            try
            {
                std::lock_guard guard(known->owner->lock);
                known->owner->pending.emplace_back(known, id);
            }
            catch (...) { return; }
            SetEvent(known->owner->changed);
        }

        static DWORD WINAPI Run(void* param)
        {
            auto s = static_cast<State*>(param);
            try { Serve(*s); }
            catch (...) {}
            for (auto& c : s->clients)
                Close(*c);
            s->clients.clear();
            for (auto& k : s->plugins)
            {
                if (k->token > 0 && k->Has(offsetof(AsiSettingsApi, Unsubscribe) + sizeof(void*)) && k->api->Unsubscribe)
                    k->api->Unsubscribe(k->token);
                k->token = 0;
            }
            if (s->module)
                FreeLibraryAndExitThread(s->module, 0);
            return 0;
        }

        // Only the user the game runs as may open the pipe (and make its other instances)
        struct UserOnly
        {
            SECURITY_ATTRIBUTES attributes{ sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE };
            SECURITY_DESCRIPTOR descriptor{};
            std::vector<BYTE> user, acl;

            bool Make()
            {
                HANDLE token = nullptr;
                if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                    return false;
                DWORD size = 0;
                GetTokenInformation(token, TokenUser, nullptr, 0, &size);
                user.resize(size);
                bool ok = size && GetTokenInformation(token, TokenUser, user.data(), size, &size);
                CloseHandle(token);
                if (!ok)
                    return false;
                PSID sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
                DWORD aclSize = DWORD(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(sid));
                acl.resize(aclSize);
                auto list = reinterpret_cast<PACL>(acl.data());
                if (!InitializeAcl(list, aclSize, ACL_REVISION) || !AddAccessAllowedAce(list, ACL_REVISION, GENERIC_ALL, sid))
                    return false;
                if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) || !SetSecurityDescriptorDacl(&descriptor, TRUE, list, FALSE))
                    return false;
                attributes.lpSecurityDescriptor = &descriptor;
                return true;
            }
        };

        static void Serve(State& s)
        {
            UserOnly security;
            if (!security.Make())
                return;
            wchar_t name[64];
            swprintf_s(name, L"%s%lu", ASI_SETTINGS_LINK_PIPE, GetCurrentProcessId());
#ifndef PIPE_REJECT_REMOTE_CLIENTS
            constexpr DWORD PIPE_REJECT_REMOTE_CLIENTS = 0x00000008;
#endif
            HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            OVERLAPPED connect{};
            HANDLE listening = INVALID_HANDLE_VALUE;
            bool first = true;
            Refresh(s);

            for (;;)
            {
                if (listening == INVALID_HANDLE_VALUE && s.clients.size() < MaxClients)
                {
                    // The first instance must be ours: nobody else can have taken the name before
                    listening = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
                        DWORD(MaxLine), DWORD(MaxLine), 0, &security.attributes);
                    if (listening == INVALID_HANDLE_VALUE && first)
                        break;
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

                HANDLE handles[3 + MaxClients];
                DWORD count = 0;
                handles[count++] = s.stop;
                handles[count++] = s.changed;
                bool waitConnect = listening != INVALID_HANDLE_VALUE;
                if (waitConnect)
                    handles[count++] = connectEvent;
                DWORD clientsAt = count;
                for (auto& c : s.clients)
                    handles[count++] = c->readEvent;
                // Plugins that load later are looked for every 2 seconds while someone is connected
                DWORD wait = WaitForMultipleObjects(count, handles, FALSE, s.clients.empty() ? (waitConnect ? INFINITE : 500) : 2000);
                if (wait == WAIT_OBJECT_0)
                    break;
                if (wait == WAIT_TIMEOUT)
                {
                    if (!s.clients.empty() && Refresh(s))
                        Broadcast(s, "{\"event\":\"settings.plugins\",\"plugins\":" + PluginList(s) + "}");
                    continue;
                }
                if (wait == WAIT_OBJECT_0 + 1)
                {
                    Deliver(s);
                    continue;
                }
                if (waitConnect && wait == WAIT_OBJECT_0 + 2)
                {
                    DWORD bytes = 0;
                    if (GetOverlappedResult(listening, &connect, &bytes, FALSE) || GetLastError() == ERROR_PIPE_CONNECTED)
                    {
                        auto c = std::make_unique<Client>();
                        c->pipe = listening;
                        c->readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                        c->writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                        if (c->readEvent && c->writeEvent && StartRead(*c))
                            s.clients.push_back(std::move(c));
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
                    auto& c = *s.clients[index];
                    DWORD bytes = 0;
                    bool ok = GetOverlappedResult(c.pipe, &c.read, &bytes, FALSE) != FALSE;
                    if (ok)
                    {
                        c.line.append(c.buffer, bytes);
                        size_t start = 0, end;
                        while (ok && (end = c.line.find('\n', start)) != std::string::npos)
                        {
                            std::string_view request(c.line.data() + start, end - start);
                            if (!request.empty() && request.back() == '\r')
                                request.remove_suffix(1);
                            if (!request.empty())
                                ok = Write(c, Handle(s, c, request));
                            start = end + 1;
                        }
                        c.line.erase(0, start);
                        ok = ok && c.line.size() <= MaxLine && StartRead(c);
                    }
                    if (!ok)
                    {
                        Close(c);
                        s.clients.erase(s.clients.begin() + ptrdiff_t(index));
                    }
                    continue;
                }
                // Anything else (WAIT_FAILED): don't spin
                Sleep(100);
            }
            if (listening != INVALID_HANDLE_VALUE)
            {
                CancelIo(listening);
                CloseHandle(listening);
            }
            CloseHandle(connectEvent);
        }

        static bool StartRead(Client& c)
        {
            c.read = {};
            c.read.hEvent = c.readEvent;
            ResetEvent(c.readEvent);
            // Completed at once or pending: either way the event is set when there is data
            return ReadFile(c.pipe, c.buffer, DWORD(sizeof(c.buffer)), nullptr, &c.read) || GetLastError() == ERROR_IO_PENDING;
        }

        static bool Write(Client& c, const std::string& line)
        {
            std::string data = line + "\n";
            OVERLAPPED write{};
            write.hEvent = c.writeEvent;
            ResetEvent(c.writeEvent);
            if (!WriteFile(c.pipe, data.data(), DWORD(data.size()), nullptr, &write) && GetLastError() != ERROR_IO_PENDING)
                return false;
            // A client that doesn't read is dropped rather than holding up the others
            DWORD bytes = 0;
            if (WaitForSingleObject(c.writeEvent, 5000) != WAIT_OBJECT_0)
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
            if (c.readEvent)
                CloseHandle(c.readEvent);
            if (c.writeEvent)
                CloseHandle(c.writeEvent);
            c.readEvent = c.writeEvent = nullptr;
        }

        static void Broadcast(State& s, const std::string& line)
        {
            for (size_t i = 0; i < s.clients.size();)
            {
                auto& c = *s.clients[i];
                if (c.subscribed && !Write(c, line))
                {
                    Close(c);
                    s.clients.erase(s.clients.begin() + ptrdiff_t(i));
                    continue;
                }
                ++i;
            }
        }

        // Looks for plugins not known yet (loaded later, or not loaded far enough to answer before); true when it found any
        static bool Refresh(State& s)
        {
            HMODULE modules[1024];
            DWORD needed = 0;
            if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
                return false;
            bool found = false;
            for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < std::size(modules); ++i)
            {
                if (std::any_of(s.plugins.begin(), s.plugins.end(), [&](auto& k) { return k->module == modules[i]; }))
                    continue;
                auto getApi = reinterpret_cast<AsiSettingsGetApi>(GetProcAddress(modules[i], ASI_SETTINGS_EXPORT_NAME));
                if (!getApi)
                    continue;
                auto known = std::make_unique<Known>();
                known->owner = &s;
                known->module = modules[i];
                known->api = getApi(ASI_SETTINGS_VERSION);
                if (!ASI_SETTINGS_API_COMPATIBLE(known->api) || !ASI_SETTINGS_API_HAS(known->api, Get) || !ReadModule(modules[i], known->file))
                    continue;
                if (known->api->pluginId && *known->api->pluginId)
                    known->file.id = known->api->pluginId;
                Texts english = known->file.texts;
                english.Select("en");
                known->name = english.Resolve(known->file.description.name);
                known->version = detail::FileVersion(modules[i]);
                wchar_t path[MAX_PATH * 4] = {};
                GetModuleFileNameW(modules[i], path, DWORD(std::size(path)));
                known->path = text::Narrow(path);
                if (known->Has(offsetof(AsiSettingsApi, Subscribe) + sizeof(void*)) && known->api->Subscribe)
                    known->token = known->api->Subscribe(&Changed, known.get());
                s.plugins.push_back(std::move(known));
                found = true;
            }
            // Editor order (SPEC.md, Order of plugins)
            std::stable_sort(s.plugins.begin(), s.plugins.end(), [](auto& a, auto& b) { return EditorOrder(a->file, b->file); });
            return found;
        }

        static Known* FindPlugin(State& s, std::string_view id)
        {
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                for (auto& k : s.plugins)
                {
                    if (text::IEquals(k->file.id, id))
                        return k.get();
                }
                if (attempt == 0 && !Refresh(s))
                    break;
            }
            return nullptr;
        }

        static std::string PluginList(State& s)
        {
            std::string out = "[";
            for (auto& k : s.plugins)
            {
                out += out.size() > 1 ? "," : "";
                out += "{\"id\":" + detail::JsonQuote(k->file.id) + ",\"name\":" + detail::JsonQuote(k->name) + ",\"version\":" +
                    detail::JsonQuote(k->version) + ",\"path\":" + detail::JsonQuote(k->path) + ",\"interface\":" + std::to_string(k->api->version) + "}";
            }
            return out + "]";
        }

        static int32_t GetValue(const Known& k, const std::string& id, std::string& value)
        {
            return detail::CallText([&](char* b, uint32_t n) { return k.api->Get(id.c_str(), b, n); }, value);
        }

        static int32_t GetState(const Known& k, const std::string& id, std::string& reason)
        {
            reason.clear();
            if (!k.Has(offsetof(AsiSettingsApi, GetState) + sizeof(void*)) || !k.api->GetState)
                return 0;
            // GetState returns the flags, so the reason gets a buffer that fits any sensible one
            char text[1024] = {};
            int32_t flags = k.api->GetState(id.c_str(), text, uint32_t(sizeof(text)));
            if (flags >= 0)
                reason = text;
            return flags;
        }

        // "value":...,"state":n[,"reason":...] of a setting, as replies and events have them
        static std::string ValueFields(const Known& k, const std::string& id)
        {
            std::string value, reason;
            int32_t got = GetValue(k, id, value);
            int32_t state = GetState(k, id, reason);
            std::string out = got >= 0 ? "\"value\":" + detail::JsonQuote(value) : "\"value\":null";
            out += ",\"state\":" + std::to_string(state < 0 ? 0 : state);
            if (!reason.empty())
                out += ",\"reason\":" + detail::JsonQuote(reason);
            return out;
        }

        // The changes the plugins reported since the last time, once per setting, to the clients that subscribed
        static void Deliver(State& s)
        {
            std::vector<std::pair<Known*, std::string>> changes;
            {
                std::lock_guard guard(s.lock);
                changes.swap(s.pending);
            }
            if (std::none_of(s.clients.begin(), s.clients.end(), [](auto& c) { return c->subscribed; }))
                return;
            std::vector<std::pair<Known*, std::string>> once;
            for (auto& [known, id] : changes)
            {
                if (std::none_of(once.begin(), once.end(), [&](auto& o) { return o.first == known && text::IEquals(o.second, id); }))
                    once.emplace_back(known, id);
            }
            for (auto& [known, id] : once)
            {
                Broadcast(s, "{\"event\":\"settings.changed\",\"plugin\":" + detail::JsonQuote(known->file.id) + ",\"setting\":" +
                    detail::JsonQuote(id) + "," + ValueFields(*known, id) + "}");
            }
        }

        // One request, its reply (SPEC.md, Live link)
        static std::string Handle(State& s, Client& c, std::string_view line)
        {
            json::Value request;
            json::Error error;
            if (!json::Parse(line, request, &error) || !request.IsObject())
                return "{\"id\":null,\"ok\":false,\"error\":" + detail::JsonQuote("not a JSON object: " + error.message) + "}";
            std::string id = "null";
            if (auto v = request.Find("id"))
                id = v->IsNumber() ? text::FormatNumber(v->number) : v->IsString() ? detail::JsonQuote(v->string) : "null";
            auto reply = [&](bool ok, const std::string& fields) { return "{\"id\":" + id + ",\"ok\":" + (ok ? "true" : "false") + (fields.empty() ? "" : "," + fields) + "}"; };
            auto fail = [&](const std::string& message, int32_t result = 0)
            {
                return reply(false, (result < 0 ? "\"result\":" + std::to_string(result) + "," : std::string()) + "\"error\":" + detail::JsonQuote(message));
            };
            auto result = [&](int32_t r, const std::string& fields)
            {
                std::string text = "\"result\":" + std::to_string(r) + (fields.empty() ? "" : "," + fields);
                return r >= 0 ? reply(true, text) : reply(false, text + ",\"error\":" + detail::JsonQuote(detail::ResultName(r)));
            };

            std::string cmd(request.Str("cmd"));
            if (cmd == "hello")
            {
                Refresh(s);
                wchar_t exe[MAX_PATH * 4] = {};
                GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
                return reply(true, "\"protocol\":" + std::to_string(ASI_SETTINGS_LINK_PROTOCOL) + ",\"interface\":" + std::to_string(ASI_SETTINGS_VERSION) +
                    ",\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"exe\":" + detail::JsonQuote(text::Narrow(exe)) + ",\"plugins\":" + PluginList(s));
            }
            if (cmd == "subscribe")
            {
                c.subscribed = request.Bool("on", true);
                return reply(true, {});
            }
            if (cmd == "plugins")
            {
                Refresh(s);
                return reply(true, "\"plugins\":" + PluginList(s));
            }
            if (!cmd.starts_with("settings."))
                return fail("unknown command: " + cmd);

            // settings.reload without a plugin: all of them
            std::string pluginId(request.Str("plugin"));
            if (cmd == "settings.reload" && pluginId.empty())
            {
                int32_t worst = 0;
                for (auto& k : s.plugins)
                {
                    if (k->Has(offsetof(AsiSettingsApi, Reload) + sizeof(void*)) && k->api->Reload)
                        worst = (std::min)(worst, k->api->Reload());
                }
                return result(worst, {});
            }
            auto k = FindPlugin(s, pluginId);
            if (!k)
                return fail("no such plugin: " + pluginId, ASI_SETTINGS_UNKNOWN_ID);
            auto& d = k->file.description;

            if (cmd == "settings.list")
            {
                std::string out = "[";
                for (auto& setting : d.settings)
                {
                    if (!setting.HasValue())
                        continue;
                    out += out.size() > 1 ? "," : "";
                    out += "{\"setting\":" + detail::JsonQuote(setting.id) + "," + ValueFields(*k, setting.id) + "}";
                }
                return reply(true, "\"plugin\":" + detail::JsonQuote(k->file.id) + ",\"values\":" + out + "]");
            }
            if (cmd == "settings.reload")
            {
                if (!k->Has(offsetof(AsiSettingsApi, Reload) + sizeof(void*)) || !k->api->Reload)
                    return result(ASI_SETTINGS_UNSUPPORTED, {});
                return result(k->api->Reload(), {});
            }
            if (cmd == "settings.preset")
            {
                // Presets change the menu's settings only (SPEC.md, Presets), each through Set like a menu host does;
                // values that can't be used now are refused by the plugin and listed
                int preset = -1;
                if (auto p = request.Find("preset"))
                {
                    if (p->IsNumber())
                        preset = int(p->number);
                    else if (p->IsString())
                        preset = text::IEquals(p->string, "vanilla") ? 0 : text::IEquals(p->string, "recommended") ? 1 : text::IEquals(p->string, "enhanced") ? 2 : -1;
                }
                if (preset < 0 || preset > 2)
                    return fail("preset: vanilla, recommended or enhanced", ASI_SETTINGS_INVALID_VALUE);
                std::string changed = "[", refused = "[";
                bool restart = false;
                for (auto& setting : d.settings)
                {
                    if (!setting.InPresets())
                        continue;
                    auto& wanted = (*setting.presets)[size_t(preset)];
                    std::string current;
                    if (GetValue(*k, setting.id, current) >= 0 && detail::JsonEquals(wanted, current, !setting.StringValued()))
                        continue;
                    int32_t r = k->api->Set(setting.id.c_str(), detail::JsonToText(wanted).c_str());
                    if (r >= 0)
                    {
                        changed += (changed.size() > 1 ? "," : "") + detail::JsonQuote(setting.id);
                        restart |= setting.apply == "restart";
                    }
                    else
                        refused += (refused.size() > 1 ? "," : "") + std::string("{\"setting\":") + detail::JsonQuote(setting.id) + ",\"result\":" + std::to_string(r) + "}";
                }
                return reply(true, "\"changed\":" + changed + "],\"refused\":" + refused + "],\"restart\":" + (restart ? "true" : "false"));
            }

            std::string settingId(request.Str("setting"));
            auto setting = d.Find(settingId);
            if (!setting)
                return fail("no such setting: " + settingId, ASI_SETTINGS_UNKNOWN_ID);
            settingId = setting->id;

            if (cmd == "settings.get")
                return reply(true, ValueFields(*k, settingId));
            if (cmd == "settings.set")
            {
                auto v = request.Find("value");
                if (!v || !(v->IsString() || v->IsNumber() || v->IsBool()))
                    return fail("value: the value as text", ASI_SETTINGS_INVALID_VALUE);
                std::string value = v->IsString() ? v->string : v->IsBool() ? (v->boolean ? "1" : "0") : text::FormatNumber(v->number);
                int32_t r = k->api->Set(settingId.c_str(), value.c_str());
                // The value in effect after it, whatever the result
                return result(r, ValueFields(*k, settingId));
            }
            if (cmd == "settings.state")
            {
                std::string reason;
                int32_t r = GetState(*k, settingId, reason);
                return result(r, r >= 0 ? "\"state\":" + std::to_string(r) + ",\"reason\":" + detail::JsonQuote(reason) : std::string());
            }
            if (cmd == "settings.choices")
            {
                if (!k->Has(offsetof(AsiSettingsApi, GetChoices) + sizeof(void*)) || !k->api->GetChoices)
                    return result(ASI_SETTINGS_UNSUPPORTED, {});
                std::string choices;
                int32_t r = detail::CallText([&](char* b, uint32_t n) { return k->api->GetChoices(settingId.c_str(), b, n); }, choices);
                json::Value parsed;
                if (r >= 0 && !json::Parse(choices, parsed))
                    r = ASI_SETTINGS_INTERNAL;
                return result(r, r >= 0 ? "\"choices\":" + choices : std::string());
            }
            if (cmd == "settings.invoke")
            {
                if (!k->Has(offsetof(AsiSettingsApi, Invoke) + sizeof(void*)) || !k->api->Invoke)
                    return result(ASI_SETTINGS_UNSUPPORTED, {});
                return result(k->api->Invoke(settingId.c_str()), {});
            }
            return fail("unknown command: " + cmd);
        }
    };

    // ---------------------------------------------------------------------------------------------------------------
    // Editors

    // The order of plugins in editors (SPEC.md, Order of plugins): by priority, then by English name and id
    inline bool EditorOrder(const DescribedFile& a, const DescribedFile& b)
    {
        if (a.description.priority != b.description.priority)
            return a.description.priority > b.description.priority;
        Texts ea = a.texts, eb = b.texts;
        ea.Select("en");
        eb.Select("en");
        auto na = text::ToLower(ea.Resolve(a.description.name)), nb = text::ToLower(eb.Resolve(b.description.name));
        if (na != nb)
            return na < nb;
        return text::ToLower(a.id) < text::ToLower(b.id);
    }

    // The plugins with a description where Ultimate ASI Loader loads them from (SPEC.md, Editors): the game folder,
    // scripts, plugins and update, in editor order. Read as data, none of their code runs. Folders named in the
    // loader's own ini aren't looked at.
    inline std::vector<DescribedFile> FindPluginFiles(const std::filesystem::path& gameFolder)
    {
        std::vector<DescribedFile> found;
        std::error_code ec;
        for (auto sub : { L"", L"scripts", L"plugins", L"update" })
        {
            auto folder = *sub ? gameFolder / sub : gameFolder;
            if (!std::filesystem::is_directory(folder, ec))
                continue;
            std::vector<std::filesystem::path> files;
            for (auto& entry : std::filesystem::directory_iterator(folder, ec))
            {
                // .asi only: .asi0 and similar are how players turn plugins off
                if (entry.is_regular_file(ec) && text::IEquals(text::Narrow(entry.path().extension().wstring()), ".asi"))
                    files.push_back(entry.path());
            }
            std::sort(files.begin(), files.end());
            for (auto& path : files)
            {
                DescribedFile file;
                if (ReadPluginFile(path.wstring(), file))
                    found.push_back(std::move(file));
            }
        }
        std::stable_sort(found.begin(), found.end(), EditorOrder);
        return found;
    }

    // The game's exe of a plugin file: the first exe of plugin.games found in the plugin's folder or a folder above it,
    // nearest first (SPEC.md, Games); empty when none is there
    inline std::filesystem::path FindGameExe(const Description& d, const std::filesystem::path& pluginFile)
    {
        std::error_code ec;
        auto folder = pluginFile.parent_path();
        for (int level = 0; level < 8 && !folder.empty(); ++level)
        {
            for (auto& game : d.games)
            {
                for (auto& exe : game.exe)
                {
                    auto candidate = (folder / text::Widen(exe.path)).lexically_normal();
                    if (!std::filesystem::is_regular_file(candidate, ec))
                        continue;
                    // The file name as it is on disk
                    WIN32_FIND_DATAW data{};
                    HANDLE find = FindFirstFileW(candidate.c_str(), &data);
                    if (find != INVALID_HANDLE_VALUE)
                    {
                        FindClose(find);
                        candidate.replace_filename(data.cFileName);
                    }
                    return candidate;
                }
            }
            if (folder == folder.parent_path())
                break;
            folder = folder.parent_path();
        }
        return {};
    }

    // Whether this process runs as another user than the one signed in to the session, as when an administrator's
    // account was entered in the elevation prompt: {localappdata} is then another user's, and editors don't write
    // settings files (SPEC.md, Editors). False when it can't be told.
    inline bool RunsAsOtherUser()
    {
        using QueryFn = BOOL(WINAPI*)(HANDLE, DWORD, int, LPWSTR*, DWORD*);
        using FreeFn = void(WINAPI*)(PVOID);
        HMODULE wts = LoadLibraryExW(L"wtsapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!wts)
            return false;
        auto query = reinterpret_cast<QueryFn>(GetProcAddress(wts, "WTSQuerySessionInformationW"));
        auto release = reinterpret_cast<FreeFn>(GetProcAddress(wts, "WTSFreeMemory"));
        std::wstring account;
        if (query && release)
        {
            // WTSUserName (5) and WTSDomainName (7) of the current session
            LPWSTR user = nullptr, domain = nullptr;
            DWORD size = 0;
            if (query(nullptr, DWORD(-1), 5, &user, &size) && user && *user)
            {
                if (query(nullptr, DWORD(-1), 7, &domain, &size) && domain && *domain)
                    account = std::wstring(domain) + L"\\";
                account += user;
            }
            if (user) release(user);
            if (domain) release(domain);
        }
        FreeLibrary(wts);
        if (account.empty())
            return false;

        BYTE sessionSid[SECURITY_MAX_SID_SIZE];
        DWORD sidSize = sizeof(sessionSid), domainSize = 0;
        SID_NAME_USE use{};
        LookupAccountNameW(nullptr, account.c_str(), sessionSid, &sidSize, nullptr, &domainSize, &use);
        std::wstring domainName(domainSize, L'\0');
        sidSize = sizeof(sessionSid);
        if (!LookupAccountNameW(nullptr, account.c_str(), sessionSid, &sidSize, domainName.data(), &domainSize, &use))
            return false;

        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;
        BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
        DWORD size = 0;
        bool other = GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size) &&
            !EqualSid(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, sessionSid);
        CloseHandle(token);
        return other;
    }

    // An editor's view of one plugin: its description, translations and the values of its settings file, without the
    // plugin running. Every valid value can be chosen (SPEC.md, Availability). Not copyable: slots point into the
    // description.
    //     asi_settings::Document doc;
    //     auto exe = asi_settings::FindGameExe(file.description, file.path);
    //     doc.Open(std::move(file), exe);
    //     doc.Set("GRAPHICS.FOV", "10");
    //     doc.Save();
    class Document
    {
    public:
        Document() = default;
        Document(const Document&) = delete;
        Document& operator=(const Document&) = delete;

        // described: from ReadPluginFile or FindPluginFiles; exe: the game's exe (FindGameExe), which may be missing.
        // Reads the settings file in effect.
        void Open(DescribedFile described, const std::filesystem::path& exe)
        {
            file = std::move(described);
            locations = { std::filesystem::path(file.path), exe };
            storage = ResolveStorageFiles(file.description, locations);
            slots.clear();
            for (auto& s : file.description.settings)
            {
                Slot slot;
                slot.setting = &s;
                slots.push_back(std::move(slot));
            }
            Read();
        }

        // Reads the settings file in effect (the newest, NewestCfg) again; false when there is none and the values are
        // the defaults
        bool Read()
        {
            cfgPath = NewestCfg(storage);
            auto source = ReadFileText(cfgPath);
            exists = source.has_value();
            fileText = source.value_or(std::string());
            ReadText(fileText);
            return exists;
        }

        // Takes the values from a settings file's text, as if the file had it (an editor's text view of the file). Settings
        // it lacks get their defaults; values that can't be read count as missing and their text is kept until changed.
        void ReadText(std::string_view source)
        {
            lastFile = ParseCfg(source);
            for (auto& slot : slots)
            {
                auto& s = *slot.setting;
                SetDefault(slot);
                slot.unread.clear();
                slot.stored = false;
                if (!s.HasValue() || s.storage != "cfg")
                    continue;
                const std::string* value = lastFile.Find(s.section, s.key);
                for (size_t i = 0; !value && i < s.aliases.size(); ++i)
                {
                    auto dot = s.aliases[i].find('.');
                    if (dot != std::string::npos)
                        value = lastFile.Find(s.aliases[i].substr(0, dot), s.aliases[i].substr(dot + 1));
                }
                if (!value)
                    continue;
                slot.stored = true;
                if (!ParseValue(slot, *value, false))
                    slot.unread = *value;
            }
        }

        const Slot* Find(std::string_view id) const
        {
            for (auto& s : slots)
            {
                if (text::IEquals(s.setting->id, id))
                    return &s;
            }
            return nullptr;
        }

        // The setting a key of the file stands for: its id or one of its aliases; nullptr for keys the description
        // doesn't know
        const Setting* FindKey(std::string_view section, std::string_view key) const
        {
            std::string id = std::string(section) + "." + std::string(key);
            if (auto s = file.description.Find(id))
                return s;
            for (auto& s : file.description.settings)
            {
                for (auto& a : s.aliases)
                {
                    if (text::IEquals(a, id))
                        return &s;
                }
            }
            return nullptr;
        }

        // The value as text in the file's syntax: the stored one, or the text that couldn't be read
        std::string Get(std::string_view id) const
        {
            auto s = Find(id);
            return !s ? std::string() : s->unread.empty() ? FormatValue(*s) : s->unread;
        }

        // Checks and takes a value given as text (in range, one of the choices); the file is written by Save
        int32_t Set(std::string_view id, std::string_view value)
        {
            auto s = FindSlot(id);
            if (!s)
                return ASI_SETTINGS_UNKNOWN_ID;
            if (!s->setting->HasValue() || s->setting->storage != "cfg")
                return ASI_SETTINGS_UNSUPPORTED;
            Slot candidate = *s;
            if (!ParseValue(candidate, value, true))
                return ASI_SETTINGS_INVALID_VALUE;
            candidate.unread.clear();
            *s = std::move(candidate);
            return ASI_SETTINGS_OK;
        }

        bool Visible(std::string_view id) const { return Check(id, true); }
        bool Enabled(std::string_view id) const { return Check(id, false); }

        Preset CurrentPreset() const { return asi_settings::CurrentPreset(slots); }

        // The settings a preset changes, with their new values as text, to show before it's applied
        std::vector<std::pair<const Setting*, std::string>> PresetChanges(Preset preset) const
        {
            std::vector<std::pair<const Setting*, std::string>> out;
            if (preset == Preset::Custom)
                return out;
            for (auto& slot : slots)
            {
                auto& p = slot.setting->presets;
                if (!slot.setting->InPresets())
                    continue;
                auto value = detail::JsonToText((*p)[int(preset)]);
                if (!slot.unread.empty() || !detail::JsonEquals((*p)[int(preset)], FormatValue(slot), !slot.setting->StringValued()))
                    out.emplace_back(slot.setting, value);
            }
            return out;
        }

        // Sets every preset setting; true when a changed setting applies after a restart
        bool ApplyPreset(Preset preset)
        {
            bool restart = false;
            for (auto& [setting, value] : PresetChanges(preset))
            {
                auto s = FindSlot(setting->id);
                if (s && ParseValue(*s, value, false))
                {
                    s->unread.clear();
                    restart |= setting->apply == "restart";
                }
            }
            return restart;
        }

        // The whole settings file as Save writes it (SPEC.md, Writing): unknown keys of the text last read are kept
        std::string Content() const
        {
            Texts english = file.texts;
            english.Select("en");
            return WriteCfg(file.description, english.Resolve(file.description.name), english, slots, &lastFile);
        }

        // Writes the file in full where the user can write (CfgWriteTarget); it is then the file in effect
        bool Save()
        {
            auto content = Content();
            auto target = CfgWriteTarget(storage);
            std::error_code ec;
            std::filesystem::create_directories(target.parent_path(), ec);
            if (!WriteFileAtomic(target, content))
                return false;
            cfgPath = target;
            exists = true;
            fileText = content;
            lastFile = ParseCfg(content);
            return true;
        }

        const DescribedFile& File() const { return file; }
        const Description& GetDescription() const { return file.description; }
        Texts& GetTexts() { return file.texts; }
        const Texts& GetTexts() const { return file.texts; }
        const std::vector<Slot>& Slots() const { return slots; }
        const Locations& GetLocations() const { return locations; }
        const StorageFiles& Storage() const { return storage; }
        // The file in effect, last read or written, and whether it exists
        const std::filesystem::path& CfgPath() const { return cfgPath; }
        bool CfgExists() const { return exists; }
        // The text of the file in effect as last read or written
        const std::string& FileText() const { return fileText; }
        // Where Save writes (asks the file system each time)
        std::filesystem::path WriteTarget() const { return CfgWriteTarget(storage); }

    private:
        DescribedFile file;
        Locations locations;
        StorageFiles storage;
        std::vector<Slot> slots;
        std::filesystem::path cfgPath;
        bool exists = false;
        std::string fileText;
        CfgFile lastFile;

        Slot* FindSlot(std::string_view id)
        {
            for (auto& s : slots)
            {
                if (text::IEquals(s.setting->id, id))
                    return &s;
            }
            return nullptr;
        }

        bool Check(std::string_view id, bool visible) const
        {
            auto s = Find(id);
            if (!s)
                return false;
            auto& c = visible ? s->setting->visibleIf : s->setting->enabledIf;
            if (!c)
                return true;
            return Evaluate(*c, [this](std::string_view other) -> const Slot* { return Find(other); });
        }
    };
}

// The export of a plugin: ASI_SETTINGS_EXPORT(settings) at namespace scope, settings being an asi_settings::Plugin
#define ASI_SETTINGS_EXPORT(plugin) \
    extern "C" __declspec(dllexport) const AsiSettingsApi* ASI_SETTINGS_CALL AsiSettings_GetApi(uint32_t hostVersion) \
    { \
        (void)hostVersion; \
        return (plugin).Api(); \
    }
