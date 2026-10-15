#pragma once
// Extensions of the in-game overlay: pages, rows and menu items that fixes add to the dev menu (debug tools, toggles,
// readouts, game-specific pages). No ImGui here: the overlay draws them (Overlay.hpp), this only keeps the list.
// See overlay/readme.md, "Extending the overlay".
#include <algorithm>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace asi_overlay
{
    class OverlayUI;
    using DrawFunction = std::function<void(OverlayUI&)>;

    // Where rows go on a settings page of a plugin
    enum class Where
    {
        Top,            // above the page's first group
        Bottom,         // below its last group
        BeforeGroup,    // above the group titled `group` (its title as shown, or its file section)
        AfterGroup,     // below that group
    };

    struct Extension
    {
        enum class Kind { Page, Rows, Menu };
        int id = 0;
        Kind kind = Kind::Page;
        std::string parent;     // Page: the tree node ("Debug", "Tools", or a plugin id: under that plugin's pages)
        std::string title;      // Page: its title in the tree; Menu: the menu bar menu ("Debug")
        std::string plugin, page, group; // Rows: plugin id, page id ("advanced"/"compatibility" for those), group
        Where where = Where::Bottom;
        double order = 0;       // smaller first; equal keys keep the order of registration
        DrawFunction draw;
        std::string error;      // set when the draw function threw: it isn't called again
    };

    // Thread-safe: register from any thread at any time (a fix's init, a later module). The overlay shows only what
    // exists when it draws. Ids stay valid until removed.
    class Extensions
    {
    public:
        int AddPage(std::string parent, std::string title, DrawFunction draw, double order = 0)
        {
            Extension e;
            e.kind = Extension::Kind::Page;
            e.parent = std::move(parent);
            e.title = std::move(title);
            e.order = order;
            e.draw = std::move(draw);
            return Add(std::move(e));
        }

        int AddRows(std::string plugin, std::string page, DrawFunction draw, Where where = Where::Bottom, std::string group = {}, double order = 0)
        {
            Extension e;
            e.kind = Extension::Kind::Rows;
            e.plugin = std::move(plugin);
            e.page = std::move(page);
            e.group = std::move(group);
            e.where = where;
            e.order = order;
            e.draw = std::move(draw);
            return Add(std::move(e));
        }

        // Items of a menu in the window's menu bar (the function draws ImGui::MenuItem etc.; the menu is opened for it)
        int AddMenu(std::string menu, DrawFunction draw, double order = 0)
        {
            Extension e;
            e.kind = Extension::Kind::Menu;
            e.title = std::move(menu);
            e.order = order;
            e.draw = std::move(draw);
            return Add(std::move(e));
        }

        bool Remove(int id)
        {
            std::lock_guard lock(mutex);
            auto it = std::find_if(list.begin(), list.end(), [&](auto& e) { return e.id == id; });
            if (it == list.end())
                return false;
            list.erase(it);
            return true;
        }

        void Fail(int id, std::string error)
        {
            std::lock_guard lock(mutex);
            for (auto& e : list)
                if (e.id == id)
                    e.error = std::move(error);
        }

        // A copy, sorted by order key then registration (draw functions are called outside the lock)
        std::vector<Extension> List(Extension::Kind kind) const
        {
            std::lock_guard lock(mutex);
            std::vector<Extension> out;
            for (auto& e : list)
                if (e.kind == kind)
                    out.push_back(e);
            std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.order < b.order; });
            return out;
        }

        // The tree nodes of pages that aren't under a plugin, in the order of their first page
        std::vector<std::string> Nodes(const std::vector<std::string>& pluginIds) const
        {
            std::vector<std::string> nodes;
            for (auto& e : List(Extension::Kind::Page))
                if (std::find(pluginIds.begin(), pluginIds.end(), e.parent) == pluginIds.end() && std::find(nodes.begin(), nodes.end(), e.parent) == nodes.end())
                    nodes.push_back(e.parent);
            return nodes;
        }

    private:
        int Add(Extension e)
        {
            std::lock_guard lock(mutex);
            e.id = ++lastId;
            list.push_back(std::move(e));
            return lastId;
        }

        mutable std::mutex mutex;
        std::vector<Extension> list;
        int lastId = 0;
    };

    // The registry of this module (each fix DLL has its own overlay and its own registry)
    inline Extensions& Registry()
    {
        static Extensions registry;
        return registry;
    }
}
