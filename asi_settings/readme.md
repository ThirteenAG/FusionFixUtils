# ASI Settings

**Version 1.0**. From 1.0 on, later versions only add (SPEC.md, Compatibility); changes are listed in SPEC.md, Changelog.

A shared way for ASI plugins to describe their settings, so that

- fixes with an in-game options menu can show other plugins' settings in the game's own menu,
- FusionFixHub and other editors can edit them without running the game,
- every program writes the same settings file.

It doesn't depend on Ultimate ASI Loader: plugins copy the files they need, and work with any loader.

| File | What it is |
|---|---|
| [SPEC.md](SPEC.md) | The specification: description format, settings file, runtime interface, presets, order, compatibility |
| [asi_settings.h](asi_settings.h) | The runtime interface; copy it into your plugin |
| [asi_settings.hpp](asi_settings.hpp), [asi_settings_json.hpp](asi_settings_json.hpp) | Shared implementation for plugins, menu hosts and editors (C++20): descriptions, translations, the settings file, presets, conditions, the export, the live link |
| [tools/validator.cpp](tools/validator.cpp) | Checks a description; run it in the plugin's build |
| [tests/](tests) | Tests of the implementation (FusionFixUtils' `premake5.lua` builds them with the validator, `tools\run-tests.bat` runs them) |
| [schema/asi-settings.schema.json](schema/asi-settings.schema.json) | JSON Schema of the description |

## For a plugin

1. Write a description (`settings.json`) and an English text file (`text/en.json`).
2. Add them to the plugin's resources:

   ```rc
   SETTINGS ASISETTINGS "settings.json"
   en       ASISETTINGS_TEXT "text/en.json"
   ```

3. Copy `asi_settings.h`, `asi_settings.hpp` and `asi_settings_json.hpp`, then:

   ```cpp
   asi_settings::Plugin settings;
   ASI_SETTINGS_EXPORT(settings)

   settings.Load();                                   // reads the description and writes/reads the .cfg
   float& fov = settings.Float("GRAPHICS.FOV");       // stays valid; changes from menus update it
   settings.OnChange("GRAPHICS.FOV", [] { ApplyFov(); });
   asi_settings::LiveLink::Start();                   // optional: editors change values while the game runs, and
                                                      // hand edits of the .cfg are read (settings.WatchFile() without it)
   ```

4. Run the validator on the description in the build (`asi-settings-validate settings.json text/*.json`): errors fail the build, warnings (unknown fields, draft versions) don't.
