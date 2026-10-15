# ASI Settings — specification

**Status: 1.0** (2026-10-15). The rules in [Compatibility](#compatibility) apply: nothing is removed or redefined, later
versions only add. Sections marked *experimental* are outside that guarantee. Changes since the drafts are in the
[Changelog](#changelog).

ASI Settings lets a plugin (an `.asi`, or a `.dll` loaded some other way) describe its settings once, so that

- a fix that draws an in-game options menu can show them as pages or rows of that menu,
- FusionFixHub (or any other editor) can show and edit them without running the plugin,
- every program writes the same settings file.

The plugin keeps full control of its settings: it stores them, applies them and validates them. Nothing depends on
Ultimate ASI Loader being present or on its version.

## Contents

1. [Roles](#roles)
2. [Overview](#overview)
3. [The settings description](#the-settings-description)
4. [Settings](#settings)
5. [Pages, sections and order](#pages-sections-and-order)
6. [Presets](#presets)
7. [Text and translations](#text-and-translations)
8. [The settings file](#the-settings-file)
9. [The runtime interface](#the-runtime-interface)
10. [Menu hosts](#menu-hosts)
11. [Editors](#editors)
12. [Live link](#live-link)
13. [Console plugins](#console-plugins) (experimental)
14. [Versions](#versions)
15. [Compatibility](#compatibility)
16. [Limits](#limits)
17. [Changelog](#changelog)

## Roles

| Role | What it is | Example |
|---|---|---|
| **Plugin** | Has settings and describes them | NFSUnderground.WidescreenFix.asi, XboxRainDroplets.asi |
| **Menu host** | A plugin that draws settings inside the game's own menu | NFSU's Widescreen Fix (Advanced Options), Max Payne 3 FusionFix |
| **Editor** | A program outside the game that edits settings | FusionFixHub |
| **Overlay** | An editor drawn inside the running game, on top of it | The FusionFixUtils overlay (GTA IV, Max Payne 3) |

A menu host is usually also a plugin with its own settings. Several plugins can be in one game; at most one menu host
draws the other plugins' pages at a time. An overlay follows the rules for editors, except that it changes values only
through the [runtime interface](#the-runtime-interface) (it never writes the settings file; it may read it, for the
player's stored choice) and offers no game-wide preset; it doesn't change the game's menu and needs no host role.

## Overview

```
plugin.asi
 ├─ resource ASISETTINGS/SETTINGS      the description: settings, pages, presets (JSON, UTF-8)
 ├─ resource ASISETTINGS_TEXT/<lang>   translations of its text (JSON, UTF-8), "en" required
 └─ export AsiSettings_GetApi          live values, only while the game runs
plugin.cfg                             the settings file, same folder as the plugin
```

- **Static part:** the description and its translations are resources. Editors read them with
  `LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)`, which runs no code of the
  plugin and works for 32-bit plugins in 64-bit editors. Menu hosts read them from the loaded module with
  `FindResourceW`.
- **Live part:** a menu host asks the running plugin for values and changes through one exported function that returns
  a table of functions. Values cross as text in the settings file's syntax.
- **Storage:** the plugin's own `.cfg`, in a format all programs write identically.

## The settings description

Resource type `ASISETTINGS`, name `SETTINGS`, language neutral. UTF-8 JSON without a byte order mark. A JSON Schema is
in [schema/asi-settings.schema.json](schema/asi-settings.schema.json).

```json
{
  "format": "asi-settings",
  "version": "1.0",
  "plugin": {
    "name": "@plugin.name",
    "priority": 100,
    "games": [ { "name": "Need for Speed Underground", "exe": [ "speed.exe" ] } ]
  },
  "pages": [ ... ],
  "settings": [ ... ],
  "hosts": { },
  "extensions": { }
}
```

| Field | Required | Meaning |
|---|---|---|
| `format` | yes | Always `asi-settings` |
| `version` | yes | `major.minor` of this specification the description was written for: `"1.0"`. See [Versions](#versions) |
| `plugin.id` | no | Stable identifier. Default: `OriginalFilename` of the version resource without its extension; required when the plugin has no version resource. Never changes between versions and stays the same when the file is renamed: order, hidden pages and editors' data refer to it |
| `plugin.name` | yes | Display name (text, see [Text](#text-and-translations)) |
| `plugin.priority` | no | Where the plugin's pages appear among other plugins: higher first; default 0. See [Order of plugins](#order-of-plugins) |
| `plugin.games` | no | Games the plugin is for, see below |
| `storage.file` | no | Settings file, a template (see below). Default `{name}.cfg`. Usually left out |
| `pages` | no | See [Pages](#pages-sections-and-order) |
| `settings` | yes | See [Settings](#settings) |
| `hosts` | no | Hints for specific menu hosts: an object by host plugin id. Every other reader ignores them |
| `extensions` | no | Anything else: an object by vendor name. Readers ignore what they don't know |

Readers ignore fields they don't know, at every level; the validator warns about them, to catch typos.

**No duplicates.** A description holds only what isn't stored elsewhere. Editors take the plugin's version
(`FileVersion`, `ProductVersion`), author (`CompanyName`, `LegalCopyright`), home page (`UpdateUrl`, when present) and
original name (`OriginalFilename`) from its version resource (`VS_VERSION_INFO`). Default values live in the description
only: plugins read them from it instead of repeating them in code.

**File names.** The description is built into the plugin, so `storage.file` is a template that whoever reads it fills
in. Relative paths are relative to the plugin's folder; `/` and `\` both separate folders.

| Placeholder | Stands for |
|---|---|
| `{name}` | The plugin's current file name without its extension: `1.asi` gives `1` |
| `{plugin}` | The plugin's folder |
| `{game}` | The folder of the game's exe, where the loader and plugins usually are |
| `{root}` | The game's install root (see Games) |
| `{documents}`, `{localappdata}`, `{appdata}` | The user's folders |

Plugins resolve them from their own module (`GetModuleFileNameW`), editors from the file they read and the game they
found. Without `storage.file`, the file is `{name}.cfg`, so it follows the plugin when it's renamed; a fixed name like
`"MyMod.cfg"` stays the same whatever the plugin is called.

The settings file must be found from the description and the plugin's location alone, so that editors find it without
the game running. A plugin that keeps its settings somewhere only it can work out while the game runs can't use this
specification for them.

**Games.** Each entry of `plugin.games` names one game and the files that identify it:

```json
"games": [
  { "name": "Red Dead Redemption 2", "exe": [ "RDR2.exe" ], "launcher": [ "PlayRDR2.exe" ] },
  { "name": "GTA III - Definitive Edition", "exe": [ "Gameface/Binaries/Win64/LibertyCity.exe" ], "launcher": [ "PlayGTAIII.exe" ] },
  { "name": "Resident Evil 2", "launcher": [ "4249110_Launcher.exe" ], "exe": [
      { "path": "english/LeonU.exe", "label": "@leon" },
      { "path": "english/ClaireU.exe", "label": "@claire" },
      { "path": "french/LeonF.exe", "label": "@leon.french" },
      { "path": "LeonU.exe", "label": "@leon" },
      { "path": "ClaireU.exe", "label": "@claire" } ] }
]
```

| Field | Meaning |
|---|---|
| `name` | Display name of the game (required) |
| `exe` | The game's executables (required, at least one), any number of them (store versions, patched builds, languages, characters). Each is a path relative to the game's install root with `/` as separator (a file name when the exe is in the root), or an object with `path` and `label` (text the plugin chooses: what this exe starts, such as a character, a mode or a language version). Compared case-insensitively |
| `launcher` | Programs that start the game but aren't it, in the same form, relative to the install root. Menu hosts ignore launchers |

The install root is found from the exe: an editor that finds `X:\Games\GTA III DE\Gameface\Binaries\Win64\LibertyCity.exe`
for `Gameface/Binaries/Win64/LibertyCity.exe` takes `X:\Games\GTA III DE` as the root; a running plugin does the same with
the process' exe.

An editor that starts the game starts the first launcher, or the first `exe` when there is none. When several exes or
launchers are installed, it offers them by their `label`, or by file name when they have none, so the player picks one.

**Several copies.** An install can contain the plugin more than once, typically one copy per language folder, each next
to its own exes and loader. Every copy has its own settings file, and editors never mix them up. How an editor offers
them is its choice: as variants of the same game named by their folder (`english`, `french`), or only the copy next to
the exe it manages. In-game menus are unaffected: a running game only sees its own copy.

## Settings

Each setting is one value in the settings file.

```json
{
  "id": "GRAPHICS.FOV",
  "type": "float",
  "default": 0,
  "min": 0, "max": 20, "step": 1, "unit": "%",
  "label": "@fov", "description": "@fov.description",
  "page": "graphics", "group": "@group.camera", "order": 20,
  "control": [ "slider" ],
  "apply": "live",
  "presets": { "vanilla": 0, "recommended": 0, "enhanced": 10 }
}
```

### Identity

| Field | Meaning |
|---|---|
| `id` | `SECTION.Key`: the section and key in the settings file. Never changes once released |
| `aliases` | Earlier ids of the same setting (`["MAIN.OldName"]`), read when the id itself isn't in the file. The next write stores the value under the id and drops the alias' line |

### Types

Every type names the simpler type it falls back to, so a reader that doesn't know a type still shows something correct.

| Type | Value | Fields | Falls back to |
|---|---|---|---|
| `bool` | `0` or `1` | | — |
| `int` | whole number | `min`, `max`, `step`, `unit`, `special`, `suggestions` | — |
| `float` | decimal number, `.` as separator | `min`, `max`, `step`, `unit`, `decimals`, `special`, `suggestions` | — |
| `choice` | one of `choices` | `choices` or `dynamic` | `int` or `string` (by the values) |
| `string` | text | `maxLength`, `suggestions`, `special` | — |
| `path` | a file or folder | `folder` (bool), `relativeTo` (`game`, `plugin`) | `string` |
| `key` | a keyboard key or gamepad button | `devices` (`keyboard`, `mouse`, `gamepad`) | `string` |
| `color` | `#RRGGBB` or `#RRGGBBAA` | `alpha` (bool) | `string` |
| `action` | no value; something the plugin does | | not shown |
| `info` | no value; a read-only line of text | | not shown |

A reader treats a type it doesn't know as its fallback, and if there is none, shows the setting nowhere but the
settings file.

Details:

- **Values in the description** (`default`, `presets`, `choices`, conditions) are JSON numbers or strings; a `bool`'s
  may also be `true` and `false`, which mean `1` and `0`.
- **`choices`**: `[{ "value": 1, "label": "@icons.xbox" }, ...]`. Values are all numbers or all strings.
- **`dynamic`**: the choices come from the running plugin (`GetChoices`), for lists like resolutions, installed languages
  or gamepads. `choices` may still list the values an editor can show without the game running. A setting with
  `dynamic: true` and no `choices` is shown by editors as its fallback type.
- **`special`**: values with a meaning of their own, shown as a label: `[{ "value": 0, "label": "@auto" }]`. For "0 means
  the desktop resolution", "-1 means the monitor's refresh rate", "Auto" aspect ratio and similar.
- **`suggestions`**: common values a menu steps through or lists, while any other value stays allowed and is shown as
  it is: `[{ "value": "16:9" }, { "value": "21:9" }]`, or frame rate limits `30`, `60`, `144`. A host that can't
  enter free values steps through `special` and `suggestions` only.
- **`unit`**: shown after the value: `%`, `°`, `ms`, `fps`, `px` or any text.
- **`min`/`max`/`step`**: inclusive range and step of numbers. A value outside the range from the file is kept as it is
  and shown as such until changed.

### Display

| Field | Meaning |
|---|---|
| `label` | Name of the setting (text). Required except for `info` |
| `description` | One or two sentences shown as help (text) |
| `page` | Page id, see [Pages](#pages-sections-and-order). Default: the plugin's first page in page order that isn't a `compatibility` one |
| `group` | A heading within the page (text); settings with the same group are shown together |
| `order` | Number, smaller first within the page; default: the order in `settings` |
| `control` | Preferred ways to show it, best first. A host uses the first one it supports, otherwise its default for the type |
| `visibleIf`, `enabledIf` | [Conditions](#conditions) on other settings of the same plugin |
| `visibility` | `menu` (default: in-game menus, editors, file), `editor` (editors and file only) or `file` (changed in the file only: advanced and debug settings; editors may list them read-only) |

Controls: `switch`, `checkbox`, `cycler` (left/right through values), `dropdown`, `segmented`, `slider`, `stepper`,
`text`, `keycapture`, `colorpicker`, `button`, `label`. A host may ignore every hint.

A `visibility` value a reader doesn't know counts as `editor`.

### Behaviour

| Field | Meaning |
|---|---|
| `default` | Value when the file has none. Required except for `action` and `info` |
| `apply` | When a change takes effect: `live` (default), `restart` (next start of the game), `nextLevel` (next level or race), `menu` (when the menu is closed). A value a reader doesn't know counts as `live` |
| `presets` | The setting's value in each [preset](#presets) |
| `storage` | `cfg` (default: the plugin's settings file) or `native` (the game's own settings, see below). A value a reader doesn't know counts as `native` |

`storage: "native"` marks a value the game stores itself (its own settings file or save). Only the running plugin
can read or change it; the settings file doesn't have it, and editors show it read-only or not at all. Settings a fix
*extends* in the game's own menus, like an extended frame rate limiter, should be `cfg` settings instead (a host
places them with its `hosts` hints), so that they are editable everywhere and the game's own file keeps a value the
original game understands.

A setting that may only be changed at certain moments (in the main menu, not during a loading screen) says so at
run time: `Set` refuses the change with `ASI_SETTINGS_NOT_NOW`, and `GetState` can give the reason.

### Availability

Some values can only be used when something is present: DLAA and FSR need DXVK and the vendor DLLs, HDR needs an HDR
display, a plugin might offer an option only at certain hours. The running plugin alone knows; the description has
nothing for it.

- **Usable values.** The plugin knows which values of a setting can be used right now. Menus offer only those: without
  the DLLs, anti-aliasing cycles through SMAA, FXAA and TAA, and DLAA and FSR don't appear. Menus get them from
  `GetChoices`. A setting with no usable value at all is hidden (`GetState` returns `ASI_SETTINGS_STATE_HIDDEN`); one with
  a single usable value shows just that value.
- **The value in effect.** When the player's choice can't be used, the value in effect is the setting's `default`; when
  that can't be used either, the first usable value. A player who chose DLAA starts the game without the DLLs and sees
  (and gets) SMAA.
- **The player's choice is kept.** The settings file keeps the stored value as long as the player doesn't change the
  setting: DLAA comes back by itself once the DLLs are installed. Only a change by the player (in a menu, an editor or
  the file) replaces it.
- **Editors and the file treat every value as usable.** They run without the game and can't know its situation, so any
  valid value can be chosen there; the plugin works out the value in effect when it reads it.
- `Get` returns the value in effect; `Set` refuses values that can't be used now (`ASI_SETTINGS_NOT_NOW`). When what a
  setting depends on changes (the DLLs appear, the hour passes), the plugin calls its subscribers with the setting's id,
  so menus ask again.

### Conditions

`visibleIf` and `enabledIf` take one condition or a combination:

```json
{ "setting": "GRAPHICS.MSAA", "equals": 1 }
{ "setting": "MAIN.WindowedMode", "notEquals": 0 }
{ "setting": "MAIN.FPSLimit", "in": [ -1, -2 ] }
{ "all": [ { ... }, { ... } ] }
{ "any": [ { ... }, { ... } ] }
{ "not": { ... } }
```

Conditions only refer to settings of the same plugin. A condition a reader doesn't understand, or one about a setting
it doesn't know, counts as true.

## Pages, sections and order

```json
"pages": [
  { "id": "graphics", "title": "@page.graphics", "section": "graphics", "order": 10 },
  { "id": "camera", "title": "@page.camera", "section": "camera", "order": 20 }
]
```

| Field | Meaning |
|---|---|
| `id` | Unique within the plugin |
| `title` | Page title (text) |
| `section` | One of the common sections below: what the page is about |
| `join` | `true`: the plugin would like its page shown as a group inside the menu host's page of the same `section` instead of a page of its own. The host decides |
| `order` | Number, smaller first; default: the order in `pages` |

Common sections: `display`, `graphics`, `camera`, `controls`, `gamepad`, `audio`, `gameplay`, `language`, `advanced`,
`other`, `compatibility`. A section a reader doesn't know counts as `other`.

`compatibility` pages hold workarounds for problem setups (text language override, graphics API fallbacks, save
locations, interop with other mods). They come last everywhere: after all other pages in menus and editors (editors
collapse them like advanced settings), and in the settings file after the advanced settings, under their own banner.
A setting on such a page is a compatibility setting, whatever its `visibility`; presets never change it.

### Order of plugins

In a menu host and in editors, plugins appear in this order:

1. The menu host itself, since it draws the menu.
2. The other plugins by `plugin.priority`, higher first.
3. Plugins with the same priority alphabetically by their English `plugin.name`, then by `plugin.id`.

This is the same whatever order the plugins were loaded in. Within a plugin, pages and settings follow their `order`.
Every plugin with a description shows its pages; there is no list of hidden plugins.

## Presets

There are exactly three presets, the same in every plugin:

| Id | Meaning |
|---|---|
| `vanilla` | The original game: only what is needed for it to run correctly |
| `recommended` | The plugin's defaults |
| `enhanced` | Everything turned up |

- A setting takes part in presets by listing a value for all three (`"presets": { "vanilla": …, "recommended": …,
  "enhanced": … }`). Settings without `presets` are never changed by a preset.
- Presets are data: a setting takes part when its three `presets` values differ, whatever its `visibility` (menu,
  editor, file) or page (compatibility settings too); applying a preset, in the game's menu too, sets it. A setting
  without `presets`, or with the same value in all three, is outside the presets: a preset never changes it and it
  doesn't decide which preset the values match, so a value set by hand stays across preset switches. Policy: list
  differing values on an editor or file setting only where a preset really needs it (the validator warns, listing
  them).
- `recommended` should equal `default`. The validator warns when it doesn't.
- **Matching:** a plugin's values match a preset when every setting that takes part has that preset's value;
  settings outside the presets don't count (a hand-edited one keeps the preset). Whether the values differ from the
  defaults is another question, which editors answer for themselves (a website treats any value off its default as a
  customization). Since
  two presets may set the same values, several can match at once. A selector marks every preset that matches, or, when
  it marks only one, the first match in the order `vanilla`, `recommended`, `enhanced`; *Custom* when none matches.
- **Per plugin:** a plugin's pages start with its preset selector. Choosing a preset sets its settings only after a
  confirmation that lists the changes.
- **Game-wide:** a feature of editors outside the game only. An editor may offer one selector for all plugins at once;
  a preset matches when it matches every plugin with presets. Nothing inside the game offers it, neither menu hosts
  nor in-game overlays: there every plugin has its own selector, inside that plugin's page.
- All three are always shown, even when two of them set the same values.
- Plugins can't add presets. A style such as "Console look" is a setting of the plugin.

## Text and translations

Text fields (`plugin.name`, `label`, `description`, `group`, `title`, choice and special labels) are either literal
text or a text key: a string starting with `@`, such as `@fov`. `@@` at the start stands for a literal `@`.

Translations are resources of type `ASISETTINGS_TEXT`, named by language tag, each a UTF-8 JSON object of keys
(without the `@`) and text:

```json
{ "fov": "Field of View", "fov.description": "Extra field of view of the driving cameras." }
```

- **Language tags** follow [BCP 47](https://www.rfc-editor.org/info/bcp47): a language from ISO 639, optionally a script
  from ISO 15924 and a region from ISO 3166 or UN M.49. Use the shortest tag that identifies the translation: `en`, `fr`,
  `de`, `ja`, `ru`; add a region or script only when versions differ: `pt-BR`, `es-MX` (or `es-419` for Latin America),
  `zh-Hans`, `zh-Hant`. Tags are compared case-insensitively. These are the same names Windows uses for locales.
- `en` is required.
- **Fallback** follows the lookup of [RFC 4647](https://www.rfc-editor.org/info/rfc4647): a reader that wants `fr-CA` tries
  `fr-CA`, then `fr`, then `en`; a missing key falls back the same way, and finally to the key itself.
- Hosts pick the game's language; editors pick the game's language when they know it, otherwise their own.
- Text is plain: no markup. Line breaks (`\n`) only in descriptions.

## The settings file

One file per plugin, in the plugin's folder: `{name}.cfg` unless `storage.file` says otherwise. All programs read and write it in this format, so that
the file looks the same whoever wrote it last.

```ini
; Widescreen Fix settings
; Change the settings here or in FusionFixHub; fixes with an in-game menu change them there too.

[GRAPHICS]
; Field of View: Extra field of view of the driving cameras. 0 to 20%. Default: 0
FOV = 10

; Console Gamma: deeper contrast, like the Xbox 360 version. 0 = Off, 1 = On. Default: 1
ConsoleGamma = 1

; ---- Advanced settings (not in the game's menu) ----

; Hide AA Bug: draws 1 pixel lines at the screen's edges to hide the antialiasing bug of MSAA. 0 = Off, 1 = Top and left, 2 = All sides. Default: 2 (after restart)
HideAABug = 2
```

**Reading**

- UTF-8 (a byte order mark is skipped); line ends CRLF or LF.
- `[SECTION]` and `Key = value` lines; section and key names are case-insensitive. Spaces around `=` and the value are
  ignored.
- Lines starting with `;` or `#` are comments. A `;` after whitespace ends an unquoted value (older ini files have
  comments behind values).
- Values may be quoted: `"..."` with `\"` and `\\` as escapes. Quoting keeps leading and trailing spaces and `;`.
- `bool`: `0`/`1`, also `true`/`false`, `on`/`off`, `yes`/`no`. Numbers: decimal, `0x` hexadecimal for `int`, `.` as the
  decimal separator. `key`: a key name (`F2`, `Escape`, `A`, `Pad.A`, `Mouse.Left`) or a virtual-key code (`0x71`).
- A value that can't be read counts as missing: the default applies, and the line is kept as it is until the setting
  is changed.

**Writing**

- UTF-8 without a byte order mark, CRLF.
- The file starts with two comment lines: `; ` followed by the plugin's English `plugin.name` and ` settings`, then
  `; Change the settings here or in FusionFixHub; fixes with an in-game menu change them there too.`
- The whole file is written: every setting that has a value and isn't `storage` `native`, in the order of the
  description, grouped by section, each preceded by one comment line made from its English label, description, range
  or choices, default and `(after restart)` when `apply` is `restart`. Comment lines are always regenerated; comments
  added by hand are not kept.
- The exact text of the file is that of `WriteCfg` in [asi_settings.hpp](asi_settings.hpp), the reference writer.
  Every other writer produces the same bytes from the same description and values (FusionFixHub uses the reference
  writer; the website's port is compared with it byte for byte by its tests).
- Within each section, the settings shown in menus (`visibility` `menu`) come first. The others (`editor`, `file`)
  follow after one blank line and the comment line `; ---- Advanced settings (not in the game's menu) ----`, then a
  blank line. A section with only such settings starts with that line; a section without any has none. Sections stay
  in the order of the description. Since comments are regenerated, writing a file again never repeats the line.
- Compatibility settings ([Pages](#pages-sections-and-order)), whatever their `visibility`, come last in their
  section, in the same way under the comment line `; ---- Compatibility (only if something doesn't work) ----`.
- Keys the description doesn't know are kept, at the end of their section (after the advanced and compatibility
  settings), unchanged. Sections the description doesn't have are kept at the end of the file,
  keys outside any section after the two header lines. A key that is an alias of a setting isn't kept: its value is written under the setting's id.
- Numbers are written without needless digits (`0.5`, not `0.500000`), strings quoted only when needed.
- Write to a temporary file in the same folder and replace the old one, so that a crash never leaves half a file.
- **Updating in place** is allowed for writers with tight limits, such as a proxy DLL that runs before the plugin
  (GTA IV's `d3d9.dll` changing the graphics API after a failure), console plugins and emulator services: they
  rewrite only the lines of the settings that changed, append missing keys at the end of their section (adding the
  section if needed), and keep every other line as it is, the banner lines too. They write to the same place as a full
  write (below). The file stays valid; the next full write tidies it up. So readers accept settings, banners and
  unknown keys in any order.

**Folders the user can't write to**

Games are often installed where a standard user can't write, such as Program Files. Windows then redirects the writes
of older games to `%LOCALAPPDATA%\VirtualStore`, which other programs don't see, and gives newer games an error. So
that the game, editors and a settings file downloaded from elsewhere all agree on one file, the file can be in three
places:

1. **The file:** `storage.file`, resolved as above.
2. **The fallback:** `{localappdata}\FusionFixHub\<root name>-<hash>\<path>`. `<root name>` is the name of the install
   root's folder (`Game` for a drive root), `<hash>` the FNV-1a 32-bit hash of the install root's full path in UTF-8, with
   ASCII letters in lower case, `\` as separator and no trailing `\`, as 8 lower-case hexadecimal digits, and `<path>` the file's path
   relative to the install root, or its name when it isn't inside the root.
3. **The virtualized copy:** `{localappdata}\VirtualStore\<the file's path without its drive>`, left by older plugins.

**Reading:** of the three, the one written last is in effect (on a tie, the first in this list); when none exists, the
file. **Writing:** to the file when the user can write there, otherwise to the fallback, making its folders. "The user can write there" is decided with
the user's standard (not elevated) access token, also when the program runs elevated or virtualized: the folder (or
the nearest existing one) allows adding files, and an existing file isn't read-only and may be replaced. So a game
started normally or as administrator and an editor started either way read and write the same file.

A settings file downloaded and copied next to the plugin is newer than the fallback and takes effect at the next start;
the next change is written to the fallback, from its values. Editors show which file is in effect.

## The runtime interface

A plugin with settings exports one function, under this exact name and with C linkage:

```c
const AsiSettingsApi* __cdecl AsiSettings_GetApi(uint32_t hostVersion);
```

`hostVersion` is the interface version the caller was built with. The plugin returns its table, which stays valid until
the plugin is unloaded, or `NULL` when it can't serve this caller. See [asi_settings.h](asi_settings.h) for the
declarations.

```c
typedef struct AsiSettingsApi
{
    uint32_t size;          // sizeof(AsiSettingsApi) of the plugin: functions past it don't exist
    uint32_t version;       // interface version of the plugin: major * 1000 + minor (1.0 = 1000)
    const char* pluginId;   // as in the description
    int32_t (__cdecl* Get)(const char* id, char* value, uint32_t valueSize);
    int32_t (__cdecl* Set)(const char* id, const char* value);
    int32_t (__cdecl* GetChoices)(const char* id, char* json, uint32_t jsonSize);
    int32_t (__cdecl* Invoke)(const char* id);
    int32_t (__cdecl* Subscribe)(AsiSettingsChanged callback, void* context);
    int32_t (__cdecl* Unsubscribe)(int32_t token);
    int32_t (__cdecl* Reload)(void);
    int32_t (__cdecl* GetState)(const char* id, char* reason, uint32_t reasonSize);
} AsiSettingsApi;
```

| Function | Does |
|---|---|
| `Get` | Writes the setting's current value, as text in the settings file's syntax, into `value` |
| `Set` | Checks, applies (or schedules, see `apply`) and stores a value given as text |
| `GetChoices` | Writes the values usable now as a JSON array like `choices`: the current choices of a `dynamic` setting, or the usable values of a setting with [availability](#availability) |
| `Invoke` | Runs an `action` setting |
| `Subscribe` | Calls `callback(context, id)` after a setting's value or state changed, whoever changed it; returns a token > 0 |
| `Unsubscribe` | Removes a subscription by its token |
| `Reload` | Reads the settings file again, after an editor changed it while the game runs |
| `GetState` | `0`, or flags: `ASI_SETTINGS_STATE_HIDDEN` (1) when the setting has no usable value now (see [Availability](#availability)); writes a reason (text) into `reason` when the plugin has one |

**Results:** `>= 0` success, negative an error. `Get` and `GetChoices` work like `snprintf`: they return the length of
the whole text without its terminating zero, and write as much as fits in the buffer, always zero-terminated (when the
size isn't 0). A result `>= size` means the text didn't fit; call again with a buffer of result + 1. A null buffer with
size 0 only asks for the length. `GetState` returns its flags, so its reason is cut to fit the buffer: callers pass at
least 1024 bytes and plugins keep reasons shorter. Errors:

| Code | Name | Meaning |
|---|---|---|
| -1 | `ASI_SETTINGS_UNKNOWN_ID` | No such setting |
| -2 | `ASI_SETTINGS_INVALID_VALUE` | The value can't be read or is out of range |
| -3 | `ASI_SETTINGS_NOT_NOW` | Can't be changed at this moment (a value that can't be used now, see [Availability](#availability); only in the main menu; a loading screen) |
| -4 | `ASI_SETTINGS_UNSUPPORTED` | The plugin doesn't support this function for this setting |
| -5 | `ASI_SETTINGS_INTERNAL` | Anything else |

A caller treats a negative code it doesn't know like `ASI_SETTINGS_INTERNAL`, and ignores `GetState` flags it doesn't
know.

**Versions of the table**

- `version` is `major * 1000 + minor` of the header the plugin was built with: `1000` for 1.0, `1001` for 1.1. Tables
  of the drafts before 1.0 report `version` below 1000; they have the same layout as 1.0 and count as 1.0.
- A caller uses a table only when its major version is the caller's (`ASI_SETTINGS_API_COMPATIBLE`) and it has at
  least `Get` and `Set`; it ignores the plugin otherwise.
- A caller uses a function only when the plugin's `size` covers its field and the pointer isn't `NULL`
  (`ASI_SETTINGS_API_HAS`). A plugin built with an older header has a shorter table; a plugin may leave a function it
  doesn't support `NULL`, which counts as `ASI_SETTINGS_UNSUPPORTED`. `Get` and `Set` are never `NULL`.
- Later minor versions only add fields at the end of the table, flags of `GetState` and result codes; the functions
  of 1.0 keep their meaning. A caller relies on behaviour that a later minor version adds only when `version` is at
  least that version.
- A 1.x plugin returns the same table whatever `hostVersion` says, also to callers built with a newer or older header.
  A future major version would come with a new table: a plugin that serves both picks the table by `hostVersion`.
- `pluginId` may be `NULL` or empty; callers then use the id they work out from the description and the file
  ([plugin.id](#the-settings-description)).

**Rules**

- All strings are UTF-8 and zero-terminated; ids are compared case-insensitively.
- No C++ exceptions cross the interface; functions are `noexcept` in effect.
- Functions may be called from any thread. A plugin that must apply changes on a particular thread queues them.
- Callbacks may come from any thread and must not call back into the same plugin's `Set`.
- Strings passed in are copied if kept; strings returned through buffers belong to the caller.
- Not callable from `DllMain`.
- `Set` stores the value in the settings file (at once or shortly after; a burst of changes may be written together).

## Menu hosts

A menu host draws plugin settings in the game's own menu.

- **Finding plugins:** each time its menu is opened, the host enumerates the process' modules and looks for the
  `AsiSettings_GetApi` export. This makes load order irrelevant and picks up plugins loaded late. The host may cache
  what it found per module handle.
- **Descriptions:** read with `FindResourceW(module, L"SETTINGS", L"ASISETTINGS")` from the loaded module.
- **One host draws other plugins.** When its menu is opened, a host that wants to show other plugins creates the named
  mutex `Local\AsiSettingsHost.<process id>`. If it already exists and isn't its own, another host has the role and this
  host shows only its own settings.
- **Order** as in [Order of plugins](#order-of-plugins).
- **Layout.** Recommended, one of two, decided per game by whether the game's menu has room next to its own options:
  - *Inline:* the game's menu has tabs or pages with space (e.g. GTA IV). The host's own settings sit next to the
    game's matching options; settings the game has no place for get a sub-page. One extra entry ("Plugins") holds one
    entry per plugin: the host first (its preset selector, its settings stay next to the game's options), then every
    other plugin (its preset selector at the top of its first page, then its settings).
  - *One entry:* the game's menu is a simple list (most games). The host adds a single entry to the game's options
    menu ("Advanced Options"); inside, as pages: the host's own pages, then every other plugin's pages, each plugin's
    first page starting with its preset selector.
  - Only the host changes the game's menu. Other plugins never add rows of their own; they appear through their
    `AsiSettings_GetApi` tables. Without a host they have no in-game menu (editors and the file only).
- **Text language:** the host decides which translation every plugin's text is shown in: the game's language, as a
  BCP 47 tag. A host may let players choose another one with a setting of the reserved id `MAIN.TextLanguage` (type
  `choice`, visibility `editor`, on a `compatibility` page, default `auto`; values `auto` and the tags of the
  translations it has), for players whose game language doesn't match the language they read, e.g. a fan translation
  installed over one of the game's languages. `auto` uses the game's language; a host may also recognise a known fan
  translation when that can be checked reliably. The host passes the chosen tag to every plugin's texts. Editors show
  the texts of all plugins of a game in the language of the first plugin, in editor order, that has
  `MAIN.TextLanguage` set to a tag; with `auto` or none, in the game's language when they know it, otherwise their own.
- **Presets:** per plugin only, inside that plugin's page (at the top of its first page). Hosts offer no game-wide
  selector; a preset of all plugins at once is a feature of editors outside the game ([Presets](#presets)). A preset
  applies only after a confirmation and changes only the settings that list preset values.
- **Descriptions in the menu:** a host shows the selected row's description where the game shows its own option help
  (e.g. the info text of GTA IV's menu), in the game's font, and leaves lines the game uses for other information
  (status lines, version lines) to the game.
- **Fallbacks:** a host shows what it can and leaves out the rest: unknown types fall back as described, unknown
  controls use the host's default, settings it can't show at all remain in editors and the file.
- **Hints:** `hosts.<host id>` in a description is for that host only, for things like "place this row into the game's
  Display screen". Hosts document their own hints.

## Editors

- Read descriptions and translations without running code (`LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE`).
- Look for plugins at least where Ultimate ASI Loader loads them: the game folder, `scripts`, `plugins` and `update`;
  folders named in the loader's own ini may be looked at too. Files the loader wouldn't load (for example `.asi0`) are
  not shown as plugins.
- Write the settings file only in the format above, to the place given under Folders the user can't write to, and
  show which of the three files is in effect. An editor elevated as another user than the one who plays doesn't write
  settings files: its `{localappdata}` is another user's. When the game runs and serves the [live link](#live-link),
  an editor changes values through it instead of writing the file; without it, an editor may tell plugins to call
  `Reload` through another channel, or changes apply at the next start.
- Plugins without a description: an editor may show their ini as text.
- An [overlay](#roles) is an editor inside the game: it changes values only through the runtime interface and offers
  presets per plugin only.

## Live link

While the game runs, an editor reads and changes the running plugins' settings through a named pipe, so a change
applies in the game at once and the running plugins stay the only source of truth (the settings file is the source
only while the game isn't running).

- **Who serves.** Any plugin may start the link (`asi_settings::LiveLink::Start()` in asi_settings.hpp, after `Load`).
  The first plugin of the process to start it serves it for all plugins, owning the named mutex
  `Local\AsiSettingsLink.<process id>`; the others do nothing. Which plugin serves makes no difference: it finds the
  plugins like a menu host (their `AsiSettings_GetApi` export, looked for again when a client says hello and every
  2 seconds while one is connected) and calls only their runtime interface. Plugins that don't start the link have
  none of its code.
- **The pipe.** `\\.\pipe\AsiSettings.<process id>` (decimal), byte mode, local clients only, opened only by the user
  the game runs as. The first instance is made with `FILE_FLAG_FIRST_PIPE_INSTANCE`, and a client checks
  `GetNamedPipeServerProcessId` against the process id, so no other process can pose as the game. Up to 8 clients.
- **Messages.** JSON lines in UTF-8, one object per line (`\n`, at most 64 KB). A request has `cmd` and an `id` (number
  or string) that the reply repeats: `{"id":7,"ok":true,...}`, or `{"id":7,"ok":false,"error":"...","result":-3}`.
  Calls of the runtime interface put its result code in `result` (Results, above); `ok` is `result >= 0`.
- **Values** are text in the settings file's syntax, as `Get` and `Set` take them (`"value":"1.5"`); a request may also
  give a number or `true`/`false`. What a reply or event says is the value in effect (`Get`), its `state` (`GetState`)
  and, when the plugin gives one, the `reason`.

| Request | Reply |
|---|---|
| `{"cmd":"hello","protocol":1}` | `protocol` (the server's, 1), `interface` (the serving plugin's interface version), `pid`, `exe`, `plugins`: `[{"id","name","version","path","interface"}]` in editor order (`interface`: the plugin table's `version`) |
| `{"cmd":"plugins"}` | `plugins`, as in hello |
| `{"cmd":"settings.list","plugin":P}` | `values`: `[{"setting","value","state","reason"?}]`, every setting that has a value |
| `{"cmd":"settings.get","plugin":P,"setting":S}` | `value`, `state`, `reason`? |
| `{"cmd":"settings.set","plugin":P,"setting":S,"value":V}` | `result`, and the `value` in effect afterwards whatever the result (`NOT_NOW` from [availability](#availability): the old value) |
| `{"cmd":"settings.choices","plugin":P,"setting":S}` | `result`, `choices`: the JSON array of `GetChoices` |
| `{"cmd":"settings.state","plugin":P,"setting":S}` | `result`, `state`, `reason` |
| `{"cmd":"settings.invoke","plugin":P,"setting":S}` | `result` of `Invoke` |
| `{"cmd":"settings.preset","plugin":P,"preset":"enhanced"}` | `changed`: setting ids, `refused`: `[{"setting","result"}]`, `restart`: whether a changed setting applies after a restart |
| `{"cmd":"settings.reload","plugin":P}` | `result` of `Reload`; without `plugin`, all plugins reload |
| `{"cmd":"subscribe","on":true}` | events from now on (`"on":false` stops them; `on` defaults to `true`) |

- **Events** go to the clients that subscribed: `{"event":"settings.changed","plugin":P,"setting":S,"value":V,"state":0}`
  after a setting's value or state changed, whoever changed it (the game's menu, an editor, a preset, availability,
  `Reload`, a hand edit of the settings file), once per setting for changes close together; `{"event":"settings.plugins","plugins":[...]}` when plugins
  appeared.
- **Presets** (`settings.preset`, also `0`, `1`, `2`) change the menu's settings only ([Presets](#presets)), through
  `Set` like a menu host: values that can't be used now are refused and listed, not stored.
- **Changes are the plugin's.** `Set` checks, applies (on the plugin's own thread when it queues changes) and stores the
  value in the settings file as it does for a menu. An editor connected to the game changes values through the link
  only and doesn't write the file itself; it reads the file again after change events to show it as the plugin wrote
  it. When the pipe closes (the game quit), the file is the source again.
- **Hand edits.** A plugin that starts the link also watches its settings file: when another program (a text
  editor) changes it, the plugin reads it again as with `Reload`. Its own writes don't count. asi_settings.hpp does this
  on a thread of its own (`Plugin::WatchFile`, which plugins without the link may call too).

**Protocol versions**

- The protocol has one number, `protocol`, 1 for ASI Settings 1.0. A client sends its own in `hello`; the server
  answers with its own and never changes its behaviour by the client's number.
- Later versions only add: commands, fields of requests, replies and events, and events. Each addition raises the
  number by one. A client uses a command or field added in protocol *n* only when the server's `protocol` is at least
  *n*.
- Both sides ignore fields and events they don't know. A command the server doesn't know gets `"ok":false` with an
  `error`.
- A change that can't be made by adding would use a new pipe name (`\\.\pipe\AsiSettings2.<process id>`), served beside
  the old one for as long as editors need it; the number of protocol 1 never means anything else.

## Console plugins

*Experimental: outside the 1.0 guarantee ([Compatibility](#compatibility)). This section may change or be removed in a
minor version.*

Plugins for PSP (PPSSPP) and PS2 (PCSX2 forks) are MIPS ELF or PRX files. They carry the same description and
translations either in an ELF section named `.asisettings` (description) and `.asisettings.<lang>` (translations), or in
files next to the plugin: `<plugin>.asisettings.json` and `<plugin>.asisettings.<lang>.json`, `<plugin>` being the
module's file name without its extension. The runtime interface and the live link don't apply: an editor's changes take
effect when the game starts again. Their settings file follows the same format; they usually keep their existing name
with `"file": "{name}.ini"` and update it in place (see [Writing](#the-settings-file)).

- **Which one.** When `<plugin>.asisettings.json` exists, the files are read and the sections are not; otherwise the
  sections. Sections may be padded with zero bytes.
- **Reading.** Readers never run the module and never follow links or junctions to these files. The limits apply to
  each file and section ([Limits](#limits)); a translation file whose `<lang>` isn't a language tag is ignored.
- **Settings file.** It stays in the plugin's folder: `storage.file` may use only `{name}` and `{plugin}`, is relative
  and doesn't lead out with `..`. Readers refuse a description whose file would be elsewhere.
- **Version.** Console plugins have no version resource: `plugin.version` (text, such as `"2.0"`) names their version,
  and editors show none without it. Other plugins leave it out.
- **Where editors look.** PSP: `memstick/PSP/PLUGINS/<folder>/` of a portable PPSSPP, each module named by
  `filename` in the `[options]` of its folder's `plugin.ini`. PS2: `PLUGINS` of the emulator's folder and the folders
  below it, `.elf` files.

## Versions

ASI Settings has one version, `major.minor`, for the whole specification: 1.0 now. Each part says what it means for
it:

| Part | Where the version is | Rule |
|---|---|---|
| Description | `version` (`"1.0"`) | Readers read every 1.x description; one of another major version is not read (the plugin counts as having no description). Descriptions of the drafts (`0.x`) are read as 1.0; the validator warns |
| Translations | none | Same rules as the description they belong to |
| Settings file | none | The format of 1.0 stays readable by every 1.x reader; later versions only add what older readers already skip (comment lines, keys) |
| Runtime interface | `AsiSettingsApi.version` (`1000`), `hostVersion` | See [Versions of the table](#the-runtime-interface) |
| Live link | `protocol` in `hello` (`1`) | See [Protocol versions](#live-link) |
| Console plugins | — | Experimental, not versioned yet |

The C header defines `ASI_SETTINGS_VERSION_MAJOR`, `ASI_SETTINGS_VERSION_MINOR`, `ASI_SETTINGS_VERSION` (the interface
version) and `ASI_SETTINGS_LINK_PROTOCOL`.

## Compatibility

From version 1.0:

1. **Only additions.** Nothing is removed, renamed or given a new meaning. New fields, values, types, controls and
   functions come in minor versions (1.1, 1.2, ...).
2. **Unknown things are ignored.** Readers skip fields, conditions and hints they don't know, treat unknown types as
   their fallback, and unknown values of `visibility`, `apply`, `storage` and page `section` as described for each.
3. **Every new type names an older type to fall back to.**
4. **The runtime table only grows at its end.** Callers check `size` before using a function past the 1.0 table and
   `version` before relying on newer behaviour ([Versions of the table](#the-runtime-interface)).
5. **The live link only grows**, by protocol numbers ([Protocol versions](#live-link)).
6. **Extensions** go into `extensions.<vendor>` and `hosts.<host id>`; good ones are adopted into a later minor version
   under a standard name, and readers keep accepting the old place.
7. **Old plugins keep working.** A description, translation or plugin built for 1.x is valid for every later 1.y.
   Plugins built with different versions work side by side in one game.
8. **A new major version** only when unavoidable; hosts and editors then support both majors for a long time.
9. **Experimental sections** ([Console plugins](#console-plugins)) are outside these rules until a minor version
   makes them part of the specification.

## Limits

Every menu host and editor accepts descriptions, translations and values within these limits, and may refuse anything
beyond them. The limits make that promise the same for every reader, and keep hosts safe when they parse descriptions
while the game runs. They are far above what plugins need: a complete description of a large fix is about 10 KiB.

| What | Limit |
|---|---|
| Description resource | 1 MiB |
| Translation resource | 1 MiB per language |
| Settings per plugin | 4096 |
| Id length | 128 bytes |
| Text key length | 128 bytes |
| Value length (`string`, `path`) | 4096 bytes |
| `GetState` reason | 1023 bytes |
| Live link message | 64 KB per line |
| Live link clients | 8 at a time |

## Changelog

**1.0** (2026-10-15), from draft 0.1:

- Status 1.0; [Versions](#versions) section: one version for the whole specification, and what it means for the
  description, the settings file, the runtime interface and the live link.
- Description: `version` is `"1.0"`; readers read 0.x drafts as 1.0 and don't read another major version.
- Description: removed `availability` (`always`/`mainMenu`; it clashed with [Availability](#availability)): a setting
  that may only change at certain moments refuses with `ASI_SETTINGS_NOT_NOW` at run time. Removed `replaces`: hosts
  use `hosts.<host id>` hints.
- Description: removed the page section `presets` (preset selectors are per plugin); unknown sections count as
  `other`. Unknown `visibility` counts as `editor`, unknown `apply` as `live`, unknown `storage` as `native`.
- Description: `true`/`false` allowed as values of a `bool`; `plugin.games[].name` and `exe` required; `hosts` and
  `extensions` are objects; the validator warns about unknown fields.
- `visibility` `file`: editors may list such settings read-only.
- Presets: several presets can match at once; a selector marks every match or the first in order. Game-wide presets
  only in editors outside the game, never in menus or overlays. Roles: the overlay.
- Settings file: the header lines as written; the reference writer's text is normative; keys outside a section stay
  before the first section; alias lines aren't kept; in-place updates write to the same place as full writes.
  Numbers: `0x` only for `int` and with hexadecimal digits only; floats are decimal.
- Several copies, plugin search, `MAIN.TextLanguage` (optional for hosts; editors use the first plugin that has it):
  relaxed.
- Runtime interface: `version` 1000; tables of the drafts count as 1.0; `ASI_SETTINGS_API_COMPATIBLE` and
  `ASI_SETTINGS_API_HAS`; `NULL` functions; unknown result codes and state flags; `hostVersion` doesn't change the
  table; the `GetState` reason limit.
- Live link: protocol 1 frozen with rules for additions.
- Console plugins marked experimental.
