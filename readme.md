# FusionFixUtils

The code the fixes share: one copy of every common header, used by each fix as the git submodule
`external/FusionFixUtils`, with tests for all of it.

Used by ResidentEvilClassicTrilogy.FusionFix, III.VC.SA.IV.Project2DFX, XboxRainDroplets, MaxPayne3.FusionFix,
GTAIV.EFLC.FusionFix and FusionFixHub.

## What is in it

| Folder | What | Needs |
|---|---|---|
| `include/` | `callbacks.h` (module load/unload callbacks, deferred init at `GetSystemTimeAsFileTime`), `gameref.hpp` (`GameRef`, `ProtectedGameRef`), `ModuleList.hpp`, `FileWatch.hpp` (Thomas Monkman's filewatch, MIT), `WindowStyle.hpp` (window modes: square corners, a bordered window at desktop size with its frame outside the screen) | injector + safetyhook, Hooking.Patterns (callbacks.h, gameref.hpp) |
| `asi_settings/` | ASI Settings: `SPEC.md`, `asi_settings.h`, `asi_settings.hpp`, `asi_settings_json.hpp`, schema, example, `tools/validator.cpp`, tests | nothing |
| `overlay/` | the in-game settings overlay, a developer menu fixes can extend (`Overlay.hpp`, `OverlayD3D9.hpp`, `OverlayD3D11.hpp`, `SettingsModel.hpp`, `Extensions.hpp`, `Supporter.hpp`; see `overlay/readme.md`) | Dear ImGui (the `imgui` submodule here) |
| `automation/` | the dev-only automation plugin core and the frame-export contract (`asi_automation_frames.h`) | asi_settings |
| `upscaler/` | the DLAA/FSR bridge to `FusionFixHub.exe --upscaler`, shared by the hub and the fixes that use it: `upscaler_bridge_protocol.h`, and the plugin side `upscaler_bridge_client.h` | nothing |
| `upscaler/bridge/` | the hub's side of the bridge: `upscaler_bridge_server.h` (connection, no GPU), `upscaler_bridge.cpp/.h` (D3D12, DLSS, FSR), compiled into FusionFixHub.exe | NVIDIA NGX and AMD FidelityFX SDKs (the hub has them) |
| `tests/` | one test file per header, the test harness, a fake hub, a probe DLL | `tools/fetch-test-deps.ps1` |

Dear ImGui is the `imgui` submodule (it follows ImGui's master branch; `git submodule update --remote imgui` takes the
latest). The other third-party code the headers need (injector, Hooking.Patterns, the vendor SDKs) is not in here: each
fix keeps its own.

## Using it in a fix

```
git submodule add https://github.com/ThirteenAG/FusionFixUtils external/FusionFixUtils
```

premake5.lua:

```lua
includedirs { "external/FusionFixUtils/include", "external/FusionFixUtils" }
```

then `#include "callbacks.h"`, `#include "gameref.hpp"`, `#include <asi_settings/asi_settings.hpp>`,
`#include <overlay/OverlayD3D9.hpp>`, `#include <upscaler/upscaler_bridge_client.h>` and so on. The CI workflow checks out with `submodules: recursive`.

## The sync rule

Change a shared header **only here**, run the tests, commit, then bump the submodule in each fix that should get the
change (`git -C external/FusionFixUtils pull`, commit the new submodule commit). Never edit
`external/FusionFixUtils` inside a fix's repository on its own, and never copy a header back into a fix.

## Tests

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch-test-deps.ps1
premake5 vs2026
msbuild build\FusionFixUtils.slnx -p:Configuration=Release -p:Platform=Win32
msbuild build\FusionFixUtils.slnx -p:Configuration=Release -p:Platform=x64
tools\run-tests.bat Release Win32
tools\run-tests.bat Release x64
```

`fetch-test-deps.ps1` puts injector (with safetyhook) and Hooking.Patterns at pinned commits into
`tests\external` (not tracked). `run-tests.bat` runs `FusionFixUtilsTests.exe` (all suites, or `FusionFixUtilsTests.exe
callbacks upscaler` for some), `AsiSettingsTests.exe` and the validator on the test fixture. Build both platforms first: the
x86 tests also talk to the x64 fake hub and the other way round. `premake5 vs2026 --upscaler-sdks=<folder>` also
compiles `upscaler/bridge/upscaler_bridge.cpp` (x64) against the NVIDIA NGX and AMD FidelityFX SDK headers in
`<folder>` (`dlss/include`, `fidelityfx/api/include`, `fidelityfx/upscalers/include`).

| Suite | Tests |
|---|---|
| callbacks | immediate and module callbacks, the one-callback-per-module rule, unload and any-module callbacks, reloads, deferred callbacks in order, the hook removed afterwards, a nested `GetSystemTimeAsFileTime`, registering from a callback and from several threads, patterns, a clean process exit |
| gameref | reads, writes, every operator, `ProtectedGameRef` on a read-only page (and a plain one faulting), deferred and failed resolution, `SetAddress`, assignment between references |
| modulelist | enumeration, names without extension in any case, local and system modules, `ReEnumerate` |
| filewatch | added, modified, renamed and removed files in a folder next to the exe, one watched file, a filter, the watch ending with its object |
| asi_settings | the 208 checks of `asi_settings/tests` (own exe with the live-link plugins), the validator on the example |
| overlay_model | the settings model of the overlay |
| overlay | the overlay's frames in a hidden window without a renderer: the Supporter panel without a valid token, the settings with one, closing |
| supporter | the supporter token: tokens of the site's signing code verified, changed and expired ones refused, the token file, the panel's texts |
| automation | the frame-export contract: the export found by name, stages, contexts, unregistering |
| aa | matrices, Halton jitter, `JitterProjection`, the TAA fallback without the hub |
| upscaler | protocol layout (same in x86 and x64), values through the shared memory, version mismatch, missing objects, the plugin's client against a fake hub (start, Configure, Evaluate, late answers, shutdown, the job), runtime discovery |
| standalone headers | every header compiled on its own as the first include (generated by premake5.lua) |

CI (`.github/workflows/ci.yml`) builds Release and Debug for Win32 and x64, compiles every header standalone, runs
all tests and checks the compiled shaders.
