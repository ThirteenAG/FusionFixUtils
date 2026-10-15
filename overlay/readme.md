# In-game overlay

A developer menu drawn inside the game (ImGui), opened with the fix's hotkey (Ctrl+F10) or Back + Start. It mirrors
FusionFixHub: a tree of plugins and their pages on the left (the hub's order: the host first, then by priority),
the page on the right, one line per setting. It is an editor in the sense of ASI Settings
([SPEC.md](../asi_settings/SPEC.md), Roles): values change only through each plugin's runtime interface; the settings
file is only read, for the player's saved choice ("FSR (running: SMAA)").

The hotkey is `Overlay::hotkey` (a virtual-key code, default `VK_F10`) with `Overlay::hotkeyCtrl` (default `true`:
Ctrl must be held).

- `Overlay.hpp`: the window, input and the UI. `OverlayD3D9.hpp` / `OverlayD3D11.hpp`: the renderers.
- `Supporter.hpp`: the supporter token check and the panel's texts (below).
- `SettingsModel.hpp`: the model without ImGui (pages in the hub's order, presets, saved vs running values, search).
- `Extensions.hpp`: the registry of extra pages, rows and menus (below).

What it shows: every setting, all tiers. Menu pages with their groups; then **Advanced** (editor and file-only
settings) and **Compatibility**, both grouped by section of the file. Empty pages and groups never show. Markers
after a label: `●` not the default, `R` applies after a restart (orange: restart to apply), `file` file-only. The
hovered or focused item's description is in the status line at the bottom. One preset combo per plugin page (no
game-wide preset in the game). Right-click a row (pad: Y) for Reset to default / Copy id / special values;
Delete or the pad's X resets the focused setting. LB/RB (Ctrl+PgUp/PgDn) change pages, Ctrl+F searches.
View menu: Show ids, expand/collapse groups, dark/light (default: Windows' app theme, like the hub).

## Supporter feature

The overlay is a Supporter feature. When it opens it reads `%LOCALAPPDATA%\FusionFixHub\supporter.token`, the token
FusionFixHub keeps for a signed-in supporter (fusionfix.io signs it; the hub renews it whenever it runs and removes it
on sign-out or when the support ends), and checks it offline (`Supporter.hpp`: ECDSA P-256 with the public keys in
`supporter::Keys`, Windows' CNG loaded from System32 at the first open; expiry). Valid: the overlay as described here.
Missing, changed, signed with another key or expired: a small panel instead, saying what the overlay is, how to get it
(sign in to FusionFixHub; a button to the supporters page) and that settings can still be changed in the settings file
with any text editor or in FusionFixHub's text editor. Nothing happens before the first open, and nothing goes online.

- `Overlay::supporterFile` / `Overlay::supporterKeys`: where the token is and which keys sign it (tests set their own).
- `ShowsSupporterPanel()`, `SupporterStatus()`: what the last open found.
- Development and automation builds may define `ASI_OVERLAY_NO_SUPPORTER_CHECK`: the overlay opens without a token.
  Release builds never define it.
- The token's format: the comment at the top of `Supporter.hpp`.

## Extending the overlay

Fixes add their own pages (debug tools, toggles, readouts, game-specific pages), rows on a settings page and items
in the menu bar. Register from any thread, at any time (a fix's init is fine); the overlay shows what exists when it
draws. Each fix DLL has its own registry.

```cpp
#include <overlay/OverlayD3D9.hpp>   // or OverlayD3D11.hpp

static bool freeze = false;
static float speed = 1.0f;

// A page in the tree: under a node of its own ("Debug", "Tools", ...) or under a plugin (its id)
asi_overlay::Overlay::AddPage("Debug", "Camera", [](asi_overlay::OverlayUI& ui)
{
    ui.Toggle("Freeze camera", freeze, "Stops the camera where it is");
    ui.Slider("Speed", speed, 0.1f, 4.0f, "%.1f x");
    if (ui.Button("Teleport to marker")) { /* ... */ }
    ui.Value("Position", "123.4, 56.7, 8.9");
    if (ui.Section("More"))
        ImGui::TextUnformatted("Raw ImGui works too: the overlay's context is current.");
});

// Rows on an existing settings page: plugin id, page id ("advanced"/"compatibility" for those),
// where (Top, Bottom, BeforeGroup, AfterGroup) and the group's title (as shown, or the file section)
asi_overlay::Overlay::AddRows("GTAIV.EFLC.FusionFix", "graphics", [](asi_overlay::OverlayUI& ui)
{
    ui.Value("Shadow cascades", "4");
}, asi_overlay::Where::Bottom);

// Items in a menu of the menu bar (menus of the same name are merged)
asi_overlay::Overlay::AddMenu("Debug", [](asi_overlay::OverlayUI&)
{
    if (ImGui::MenuItem("Reload shaders")) { /* ... */ }
});
```

- Every `Add*` takes an optional order key (smaller first; equal keys keep the registration order) and returns an
  id for `asi_overlay::Overlay::Remove(id)`.
- The draw function runs on the render thread inside the overlay's frame, only while that page/menu is shown.
- A function that throws (or crashes) is switched off and its page shows the error; the game goes on.
- The overlay saves nothing for extensions: keep your state yourself (statics, your own objects, your settings).
- `OverlayUI` helpers: `Toggle`, `Slider`, `SliderInt`, `Combo`, `Button`, `Value`, `Text`, `Section`, `Separator`,
  `Hint` (the status line text for the last item), `LabelWidth`/`ControlWidth` for your own rows.
