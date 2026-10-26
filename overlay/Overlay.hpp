#pragma once
#include "SettingsModel.hpp"
#include "Extensions.hpp"
#include "fonts/fonts.hpp"
#include "MessageGate.hpp"
#include "Supporter.hpp"
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_win32.h"
#include <mutex>
#include <cstdio>
#include <atomic>
#include <cstdarg>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace asi_overlay
{
    // What extension pages draw with: rows in the overlay's idiom (label left, control right, hint in the status line).
    // Plain ImGui calls work too: the overlay's context is current while a page draws. Nothing is saved by the overlay:
    // extensions own their state (keep it in statics or your own objects).
    class OverlayUI
    {
    public:
        OverlayUI(float labelWidth, float controlWidth, std::string& status, std::string& hoverStatus)
            : labelWidth(labelWidth), controlWidth(controlWidth), status(status), hoverStatus(hoverStatus) {}

        bool Toggle(const char* label, bool& value, const char* hint = nullptr) { Label(label, hint); bool c = ImGui::Checkbox("##v", &value); return End(hint, c); }
        bool Slider(const char* label, float& value, float min, float max, const char* format = "%.2f", const char* hint = nullptr)
        {
            Label(label, hint); ImGui::SetNextItemWidth(controlWidth);
            return End(hint, ImGui::SliderFloat("##v", &value, min, max, format));
        }
        bool SliderInt(const char* label, int& value, int min, int max, const char* format = "%d", const char* hint = nullptr)
        {
            Label(label, hint); ImGui::SetNextItemWidth(controlWidth);
            return End(hint, ImGui::SliderInt("##v", &value, min, max, format));
        }
        bool Combo(const char* label, int& index, const std::vector<std::string>& items, const char* hint = nullptr)
        {
            Label(label, hint); ImGui::SetNextItemWidth(controlWidth);
            bool changed = false;
            if (ImGui::BeginCombo("##v", index >= 0 && index < int(items.size()) ? items[size_t(index)].c_str() : ""))
            {
                for (int i = 0; i < int(items.size()); ++i)
                    if (ImGui::Selectable(items[size_t(i)].c_str(), i == index)) { index = i; changed = true; }
                ImGui::EndCombo();
            }
            return End(hint, changed);
        }
        // A button in the control column; true when pressed
        bool Button(const char* label, const char* hint = nullptr)
        {
            ImGui::PushID(label);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelWidth);
            bool pressed = ImGui::Button(label, ImVec2(controlWidth, 0));
            Hint(hint);
            ImGui::PopID();
            return pressed;
        }
        // A read-only row: label left, value right
        void Value(const char* label, const std::string& value, const char* hint = nullptr)
        {
            ImGui::PushID(label);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            Hint(hint);
            ImGui::SameLine(labelWidth);
            ImGui::TextUnformatted(value.c_str());
            Hint(hint);
            ImGui::PopID();
        }
        void Text(const char* format, ...) IM_FMTARGS(2)
        {
            va_list args;
            va_start(args, format);
            ImGui::TextWrappedV(format, args);
            va_end(args);
        }
        // A collapsible section (open by default); draw its rows only when it returns true
        bool Section(const char* title) { return ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen); }
        void Separator() { ImGui::Separator(); }
        // The status line text for the last item while it is hovered or focused
        void Hint(const char* text)
        {
            if (!text || !*text) return;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) hoverStatus = text;
            else if (ImGui::IsItemFocused()) status = text;
        }
        float LabelWidth() const { return labelWidth; }
        float ControlWidth() const { return controlWidth; }

    private:
        void Label(const char* label, const char* hint)
        {
            ImGui::PushID(label);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            Hint(hint);
            ImGui::SameLine(labelWidth);
        }
        bool End(const char* hint, bool result)
        {
            Hint(hint);
            ImGui::PopID();
            return result;
        }

        float labelWidth, controlWidth;
        std::string& status;
        std::string& hoverStatus;
    };

    // Each host owns a context. Every public entry point restores the previous context
    // so other mods using ImGui do not accidentally draw into ours.
    class Overlay
    {
    public:
        struct Scope
        {
            std::unique_lock<std::recursive_mutex> lock;
            ImGuiContext* previous;
            Scope(Overlay& o) : lock(o.mutex), previous(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(o.context); }
            // With a lock taken already (the window procedure's try, MessageGate)
            Scope(Overlay& o, std::unique_lock<std::recursive_mutex>&& taken) : lock(std::move(taken)), previous(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(o.context); }
            ~Scope() { ImGui::SetCurrentContext(previous); }
        };
        SettingsModel model;
        ImGuiContext* context = nullptr;
        HWND window = nullptr;
        HMODULE host = nullptr;
        int hotkey = VK_F10;
        bool hotkeyCtrl = true;     // the hotkey toggles only with Ctrl held
        std::string fallbackLanguage = "en";
        bool dark = true;
        // The overlay is a Supporter feature (Supporter.hpp): the token FusionFixHub keeps, checked at each open.
        // Builds with ASI_OVERLAY_NO_SUPPORTER_CHECK (development, automation) skip the check.
        std::filesystem::path supporterFile = supporter::DefaultFile();
        std::span<const std::string_view> supporterKeys = supporter::Keys;

        bool Initialize(HWND hwnd, HMODULE module)
        {
            if (context) return window == hwnd;
            auto previous = ImGui::GetCurrentContext();
            context = ImGui::CreateContext();
            Scope scope(*this);
            scope.previous = previous;
            window = hwnd;
            host = module;
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            io.ConfigDebugHighlightIdConflicts = false; // never show ImGui's programmer popups to players
            // Noto Sans JP covers the common European and Japanese translations in addition to symbols. ImGui 1.92 loads glyphs on demand.
            io.Fonts->AddFontFromMemoryCompressedTTF((void*)NotoSansJP_compressed_data, NotoSansJP_compressed_size, 16.0f);
            // A bad extension page must not stop the game: no asserts or tooltips, the overlay recovers the stack.
            io.ConfigErrorRecoveryEnableAssert = false;
            io.ConfigErrorRecoveryEnableTooltip = false;
            io.ConfigErrorRecoveryEnableDebugLog = true;   // ImGui wants one report: its own log, never shown to players
            if (!ImGui_ImplWin32_Init(hwnd)) { ImGui::DestroyContext(context); context = nullptr; return false; }
            return true;
        }

        void Shutdown()
        {
            if (!context) return;
            Scope scope(*this);
            SetOpen(false);
            ImGui_ImplWin32_Shutdown();
            if (scope.previous == context) scope.previous = nullptr;
            ImGui::DestroyContext(context);
            context = nullptr;
            window = nullptr;
        }

        bool IsOpen() const { return open; }
        // What the last open found: anything but Valid shows the Supporter panel instead of the settings
        supporter::Status SupporterStatus() const { return supporterStatus; }
        bool ShowsSupporterPanel() const { return open && supporterStatus != supporter::Status::Valid; }
        bool BlocksInput() const { return open || waitPadRelease || GetTickCount64() < releaseUntil; }
        // The theme follows the hub, which follows Windows' app theme; a host may set it (session only)
        void SetTheme(bool useDark) { Scope scope(*this); dark = useDark; themeSet = true; }
        // Opens a plugin's first page (or a page by id)
        void SelectPlugin(std::string_view id, std::string_view page = {})
        {
            Scope scope(*this);
            selected = "p:" + std::string(id) + ":" + std::string(page);
            selectFirstOfPlugin = page.empty();
        }

        // ---- Extensions (overlay/readme.md, "Extending the overlay"): thread-safe, any time, this module's registry
        static int AddPage(std::string parent, std::string title, DrawFunction draw, double order = 0) { return Registry().AddPage(std::move(parent), std::move(title), std::move(draw), order); }
        static int AddRows(std::string plugin, std::string page, DrawFunction draw, Where where = Where::Bottom, std::string group = {}, double order = 0) { return Registry().AddRows(std::move(plugin), std::move(page), std::move(draw), where, std::move(group), order); }
        static int AddMenu(std::string menu, DrawFunction draw, double order = 0) { return Registry().AddMenu(std::move(menu), std::move(draw), order); }
        static bool Remove(int id) { return Registry().Remove(id); }

        void SetOpen(bool value)
        {
            if (!context || value == open) return;
            Scope scope(*this);
            open = value;
            edits.clear();
            if (open)
            {
                waitPadRelease = false;
#ifdef ASI_OVERLAY_NO_SUPPORTER_CHECK
                supporterStatus = supporter::Status::Valid;
#else
                supporterStatus = supporter::CheckFile(supporterFile, int64_t(std::time(nullptr)), supporterKeys);
#endif
                model.Discover(host, fallbackLanguage);
                layouts.clear();
                for (size_t i = 0; i < model.plugins.size(); ++i) layouts.push_back(model.Layout(i));
                if (!themeSet) dark = !WindowsUsesLight();
                model.message.clear();
                GetClipCursor(&oldClip);
                GetCursorPos(&oldCursor);
                oldCapture = GetCapture();
                ReleaseCapture();
                ClipCursor(nullptr);
                // Do not modify ShowCursor's counter; ImGui draws the cursor itself.
                oldCursorShape = GetCursor();
                ImGui::GetIO().MouseDrawCursor = true;
                SetCursor(nullptr);
                firstFrame = true;
            }
            else
            {
                pending.clear();
                ImGui::GetIO().MouseDrawCursor = false;
                ImGui::GetIO().ClearInputKeys();
                ImGui::GetIO().ClearInputMouse();
                ReleaseCapture();
                if (GetForegroundWindow() == window)
                {
                    if (oldCapture && IsWindow(oldCapture)) SetCapture(oldCapture);
                    ClipCursor(&oldClip);
                    SetCursorPos(oldCursor.x, oldCursor.y);
                }
                else ClipCursor(nullptr);
                SetCursor(oldCursorShape);
                // Consume opening/closing button edges rather than leaking them into the game.
                releaseUntil = GetTickCount64() + 200;
                waitPadRelease = padButtons != 0 || padCombo;
            }
        }

        // Call before the game's WndProc/raw-input handler. Never swallow window lifecycle messages. Never waits for
        // the overlay's lock (MessageGate.hpp): the D3D9 runtime calls the window procedure from the render thread's
        // Present on a fullscreen focus change, while that thread holds the lock in Present.
        bool Message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result)
        {
            if (!context || hwnd != window) return false;
            std::unique_lock<std::recursive_mutex> taken;
            if (!gate.Enter(taken, msg, wp)) return false;
            Scope scope(*this, std::move(taken));
            if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == WPARAM(hotkey) && !(lp & (1LL << 30)) && (!hotkeyCtrl || GetKeyState(VK_CONTROL) < 0))
            {
                SetOpen(!open);
                hotkeyConsumed = true;
                result = 0;
                return true;
            }
            // The hotkey's key up is swallowed only after its key down toggled the overlay
            bool hotkeyUp = (msg == WM_KEYUP || msg == WM_SYSKEYUP) && wp == WPARAM(hotkey) && std::exchange(hotkeyConsumed, false);
            if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATEAPP && !wp)) SetOpen(false);
            if (!open)
            {
                if (hotkeyUp) { result = 0; return true; }
                return false;
            }
            // Ctrl went down before the overlay opened: the game also sees it released
            bool ctrlUp = (msg == WM_KEYUP || msg == WM_SYSKEYUP) && (wp == VK_CONTROL || wp == VK_LCONTROL || wp == VK_RCONTROL);
            // A lone Alt would toggle ImGui's menu layer (title bar close button), so a following
            // Enter/A closes the overlay; Alt+Tab returns trigger it too. The UI has no Alt shortcuts.
            bool alt = (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_KEYDOWN || msg == WM_KEYUP) && (wp == VK_MENU || wp == VK_LMENU || wp == VK_RMENU);
            if (!alt) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) legacyWheel = true;
            if (msg == WM_INPUT) RawMouse(lp);
            if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); result = TRUE; return true; }
            if (((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT) && !ctrlUp)
            {
                result = msg == WM_INPUT ? DefWindowProcW(hwnd, msg, wp, lp) : 0;
                return true;
            }
            return false;
        }

        // Games registering raw mouse input with RIDEV_NOLEGACY (Max Payne 3) never receive
        // WM_xBUTTON messages. Read buttons/wheel from the raw packet; ImGui ignores duplicate
        // button states, and the wheel is only used when the window gets no legacy wheel messages.
        void RawMouse(LPARAM lp)
        {
            RAWINPUT raw{};
            UINT size = sizeof(raw);
            if (GetRawInputData(HRAWINPUT(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) || raw.header.dwType != RIM_TYPEMOUSE) return;
            auto& io = ImGui::GetIO();
            auto flags = raw.data.mouse.usButtonFlags;
            const USHORT down[] = { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN };
            const USHORT up[] = { RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_UP };
            for (int i = 0; i < 3; ++i)
            {
                if (flags & down[i]) io.AddMouseButtonEvent(i, true);
                if (flags & up[i]) io.AddMouseButtonEvent(i, false);
            }
            if ((flags & RI_MOUSE_WHEEL) && !legacyWheel) io.AddMouseWheelEvent(0, float(SHORT(raw.data.mouse.usButtonData)) / WHEEL_DELTA);
        }

        // Hosts whose game owns the mouse through DirectInput (exclusive/foreground cooperative levels
        // suppress window mouse messages) feed buttons and wheel steps from the game's device reads.
        void MouseButton(int button, bool down)
        {
            if (!context || !open) return;
            Scope scope(*this);
            ImGui::GetIO().AddMouseButtonEvent(button, down);
        }
        void MouseWheel(float steps)
        {
            if (!context || !open || steps == 0) return;
            Scope scope(*this);
            ImGui::GetIO().AddMouseWheelEvent(0, steps);
        }

        // PS2-style mapped pad after the game's DirectInput/XInput mapper, before game consumers.
        // For other games call Gamepad with their normalized button state.
        void Gamepad(uint16_t buttons, int8_t x = 0, int8_t y = 0)
        {
            if (!context) return;
            Scope scope(*this);
            bool combo = (buttons & 0x9) == 0x9; // Select + Start
            if (combo && !padCombo && GetForegroundWindow() == window) SetOpen(!open);
            padCombo = combo;
            if (!open && buttons == 0 && std::abs(int(x)) < 24 && std::abs(int(y)) < 24) waitPadRelease = false;
            padButtons = combo ? 0 : buttons;
            padX = x; padY = y;
        }

        void NewFrame(float framebufferWidth = 0, float framebufferHeight = 0, bool scaledBackend = false)
        {
            // A focus loss the window procedure couldn't act on (the lock was busy here)
            if (gate.TakePendingClose()) SetOpen(false);
            for (auto& p : model.plugins) if (p.module == host)
            {
                std::string language;
                if (SettingsModel::Read(p.api, "MAIN.TextLanguage", language))
                {
                    if (language == "auto" || language.empty()) language = fallbackLanguage;
                    if (language != model.language) model.SetLanguage(language);
                }
            }
            ImGui_ImplWin32_NewFrame();
            auto& frameIO = ImGui::GetIO();
            frameIO.DisplayFramebufferScale = scaledBackend && frameIO.DisplaySize.x > 0 && frameIO.DisplaySize.y > 0
                ? ImVec2(framebufferWidth / frameIO.DisplaySize.x, framebufferHeight / frameIO.DisplaySize.y) : ImVec2(1, 1);
            // Only supply the game's pad when it has activity; the Win32 backend handles ordinary XInput otherwise.
            if (padButtons || padX || padY || hadMappedPad)
            {
                auto& io = ImGui::GetIO();
                io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
                auto button = [&](ImGuiKey key, uint16_t mask) { io.AddKeyEvent(key, (padButtons & mask) != 0); };
                button(ImGuiKey_GamepadDpadUp, 0x10); button(ImGuiKey_GamepadDpadRight, 0x20);
                button(ImGuiKey_GamepadDpadDown, 0x40); button(ImGuiKey_GamepadDpadLeft, 0x80);
                button(ImGuiKey_GamepadFaceUp, 0x1000); button(ImGuiKey_GamepadFaceRight, 0x2000);
                button(ImGuiKey_GamepadFaceDown, 0x4000); button(ImGuiKey_GamepadFaceLeft, 0x8000);
                button(ImGuiKey_GamepadL1, 0x400); button(ImGuiKey_GamepadR1, 0x800);
                auto axis = [&](ImGuiKey key, int value) { float f = std::clamp((value - 24) / 103.0f, 0.0f, 1.0f); io.AddKeyAnalogEvent(key, f > 0, f); };
                axis(ImGuiKey_GamepadLStickLeft, -int(padX)); axis(ImGuiKey_GamepadLStickRight, padX);
                axis(ImGuiKey_GamepadLStickUp, -int(padY)); axis(ImGuiKey_GamepadLStickDown, padY);
                hadMappedPad = padButtons || padX || padY;
            }
            auto scale = std::clamp(ImGui_ImplWin32_GetDpiScaleForHwnd(window), 1.0f, 3.0f);
            scale = std::min(scale, std::max(0.65f, ImGui::GetIO().DisplaySize.y / 560.0f));
            if (scale != uiScale || styleDark != dark)
            {
                // ImGui's own look, compact: a developer menu
                ImGui::GetStyle() = ImGuiStyle();
                if (dark) ImGui::StyleColorsDark(); else ImGui::StyleColorsLight();
                auto& style = ImGui::GetStyle();
                style.WindowRounding = 4; style.FrameRounding = 2; style.ChildRounding = 2; style.PopupRounding = 2;
                style.FramePadding = ImVec2(6, 2); style.ItemSpacing = ImVec2(8, 3); style.IndentSpacing = 14;
                style.Colors[ImGuiCol_WindowBg].w = 0.96f;
                style.Colors[ImGuiCol_PopupBg].w = 1.0f;
                style.ScaleAllSizes(scale);
                style.FontScaleDpi = scale;
                uiScale = scale; styleDark = dark;
            }
            ImGui::NewFrame();
            Draw();
            ImGui::Render();
        }

        // The official DX9 backend has no framebuffer-scale support. Scale this frame's
        // geometry before handing it to the unmodified backend, keeping input in client coordinates.
        static void ScaleD3D9DrawData(ImDrawData* data, float width, float height)
        {
            if (!data || data->DisplaySize.x <= 0 || data->DisplaySize.y <= 0) return;
            ImVec2 factor(width / data->DisplaySize.x, height / data->DisplaySize.y);
            if (factor.x == 1 && factor.y == 1) return;
            for (auto list : data->CmdLists)
            {
                for (auto& vertex : list->VtxBuffer) { vertex.pos.x *= factor.x; vertex.pos.y *= factor.y; }
                for (auto& command : list->CmdBuffer)
                {
                    command.ClipRect.x *= factor.x; command.ClipRect.z *= factor.x;
                    command.ClipRect.y *= factor.y; command.ClipRect.w *= factor.y;
                }
            }
            data->DisplaySize = ImVec2(width, height);
            data->DisplayPos.x *= factor.x; data->DisplayPos.y *= factor.y;
        }

    private:
        std::recursive_mutex mutex;
        MessageGate gate{ mutex };
        std::atomic<bool> open = false;
        std::atomic<bool> waitPadRelease = false;
        std::atomic<supporter::Status> supporterStatus = supporter::Status::Missing;
        bool hotkeyConsumed = false;
        bool firstFrame = false, legacyWheel = false, padCombo = false, hadMappedPad = false, styleDark = true, themeSet = false;
        uint16_t padButtons = 0;
        int8_t padX = 0, padY = 0;
        std::atomic<ULONGLONG> releaseUntil = 0;
        float uiScale = 0, fittedScale = 0;
        ImVec2 fittedDisplay{};
        RECT oldClip{};
        POINT oldCursor{};
        HWND oldCapture = nullptr;
        HCURSOR oldCursorShape = nullptr;
        std::vector<Change> pending;
        std::map<std::string, std::string> edits;
        std::vector<std::vector<PageView>> layouts;    // per plugin, made when the overlay opens

        // The page shown: "p:<plugin id>:<page id>" or "x:<extension id>"
        std::string selected;
        bool selectFirstOfPlugin = false, showIds = false, focusSearch = false, collapseAll = false, expandAll = false;
        char search[128] = {};
        std::string status, hoverStatus;               // the status line: the focused or hovered item's text
        struct Focused { int plugin = -1; const asi_settings::Setting* setting = nullptr; } focused;
        std::string presetName;
        bool openConfirmation = false;
        static constexpr ImGuiID KeyOwner = 0x46464F56; // the overlay's own pad buttons (X reset, LB/RB pages)

        static bool WindowsUsesLight()
        {
            DWORD value = 0, size = sizeof(value);
            return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value != 0;
        }

        // ---- Text

        const asi_settings::LoadedPlugin& Plugin(int i) const { return model.plugins[size_t(i)]; }
        std::string Name(int i) const { return Plugin(i).file.texts.Resolve(Plugin(i).file.description.name); }
        std::string Text(int i, const std::string& text) const { return Plugin(i).file.texts.Resolve(text); }

        // "Fusion Fix — Grand Theft Auto IV (Ctrl+F10)": the host plugin, the game of the running exe, the hotkey
        std::string Title() const
        {
            std::string name = "Settings", game;
            for (size_t i = 0; i < model.plugins.size(); ++i)
            {
                if (model.plugins[i].module != host) continue;
                name = Name(int(i));
                wchar_t exe[MAX_PATH * 4] = {};
                GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
                auto file = asi_settings::text::Narrow(std::filesystem::path(exe).filename().wstring());
                for (auto& g : model.plugins[i].file.description.games)
                {
                    if (game.empty()) game = g.name;
                    for (auto& e : g.exe)
                        if (asi_settings::text::IEquals(asi_settings::text::Narrow(std::filesystem::path(asi_settings::text::Widen(e.path)).filename().wstring()), file))
                            game = g.name;
                }
            }
            return name + (game.empty() ? "" : " \xE2\x80\x94 " + game) + " (" + KeyName() + ")";
        }

        std::string KeyName() const
        {
            std::string prefix = hotkeyCtrl ? "Ctrl+" : "";
            wchar_t key[64] = {};
            UINT scan = MapVirtualKeyW(UINT(hotkey), MAPVK_VK_TO_VSC);
            if (!scan || !GetKeyNameTextW(LONG(scan << 16), key, int(std::size(key))))
                return prefix + "key " + std::to_string(hotkey);
            return prefix + asi_settings::text::Narrow(key);
        }

        // ---- Status line: the hovered item's text, else the focused one's (pad and keyboard)

        void Hint(const std::string& text)
        {
            if (text.empty()) return;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) hoverStatus = text;
            else if (ImGui::IsItemFocused()) status = text;
        }

        float LabelWidth() const { return std::max(ImGui::GetContentRegionAvail().x - ControlWidth(), 120.0f * uiScale); }
        float ControlWidth() const { return std::clamp(ImGui::GetContentRegionAvail().x * 0.42f, 160.0f * uiScale, 340.0f * uiScale); }

        // ---- Presets: one combo per plugin, a confirmation lists the changes (SPEC.md, Presets)

        void PresetRow(int plugin)
        {
            // One preset only: the first that matches, as the hub and the game menus show it (identical presets can match together)
            auto preset = model.CurrentPreset(size_t(plugin));
            if (!preset) return;
            int matched = int(*preset);
            static const char* names[] = { "Vanilla", "Recommended", "Enhanced" };
            std::string current = matched >= 0 ? names[matched] : "";
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Preset");
            Hint("Sets the options this preset covers, after listing the changes; others keep their values. Custom: the values match no preset.");
            ImGui::SameLine(LabelWidth());
            ImGui::SetNextItemWidth(ControlWidth());
            if (ImGui::BeginCombo("##preset", current.empty() ? "Custom" : current.c_str()))
            {
                for (int p = 0; p < 3; ++p)
                {
                    if (ImGui::Selectable(names[p], p == matched)) { pending = model.PresetChanges(p, plugin); presetName = names[p]; openConfirmation = true; }
                    // Pad and keyboard navigation start on the current preset, not on the first row
                    if (p == matched) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            Hint("Sets the options this preset covers, after listing the changes; others keep their values. Custom: the values match no preset.");
            ImGui::Separator();
        }

        void Confirmation()
        {
            if (openConfirmation) { ImGui::OpenPopup("Apply preset?"); openConfirmation = false; }
            ImGui::SetNextWindowSize(ImVec2(std::min(620.0f * uiScale, ImGui::GetIO().DisplaySize.x - 32), 0), ImGuiCond_Appearing);
            if (!ImGui::BeginPopupModal("Apply preset?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
            ImGui::Text("%s: %zu change%s", presetName.c_str(), pending.size(), pending.size() == 1 ? "" : "s");
            if (!pending.empty())
            {
                float height = std::min(ImGui::GetTextLineHeightWithSpacing() * float(pending.size() + 1) + 4, ImGui::GetIO().DisplaySize.y * 0.5f);
                if (ImGui::BeginTable("changes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, height)))
                {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Setting"); ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("To");
                    ImGui::TableHeadersRow();
                    for (auto& change : pending)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(change.label.c_str());
                        if (ImGui::IsItemHovered() && !change.description.empty()) ImGui::SetTooltip("%s", change.description.c_str());
                        if (change.restart) { ImGui::SameLine(); ImGui::TextDisabled("R"); }
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(change.beforeText.c_str());
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(change.afterText.c_str());
                    }
                    ImGui::EndTable();
                }
            }
            else ImGui::TextDisabled("These values are already selected.");
            if (ImGui::Button("Apply")) { model.Apply(pending); pending.clear(); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) { pending.clear(); ImGui::CloseCurrentPopup(); }
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }

        // ---- One setting, one line: label and markers left, control right

        void Reset(int index, const asi_settings::Setting& s)
        {
            if (s.HasValue()) model.Set(size_t(index), s, asi_settings::detail::JsonToText(s.defaultValue));
        }

        void Row(int index, const asi_settings::Setting& s, const std::vector<asi_settings::Slot>& values, bool tierTag)
        {
            auto& p = Plugin(index);
            using Type = asi_settings::Type;
            std::string value, stored;
            bool hasValue = s.HasValue() && SettingsModel::Read(p.api, s.id, value);
            if (hasValue && !model.Stored(size_t(index), s, stored)) stored = value;
            bool hidden = !SettingsModel::Condition(s.visibleIf, values) || SettingsModel::Hidden(p.api, s);
            // File-only settings are shown read-only, as in the hub: they're changed in the settings file
            bool fileOnly = s.visibility == "file";
            bool enabled = !hidden && !fileOnly && SettingsModel::Condition(s.enabledIf, values) && (!s.HasValue() || hasValue);
            auto label = Text(index, s.label.empty() ? s.id : s.label);
            auto key = p.file.id + ":" + s.id;
            bool restartPending = model.restartChanges.count(key) != 0;
            auto display = [&](const std::string& v) { return model.Display(size_t(index), s, v); };

            // What the status line says for this row
            std::string info = Text(index, s.description);
            if (hasValue && display(stored) != display(value))
                info = "Saved: " + display(stored) + ", running: " + display(value) + " (the saved choice can't be used now). " + info;
            if (hidden) info = "Not used now. " + info;
            if (s.apply == "restart") info += restartPending ? "  [restart to apply]" : "  [after restart]";
            info += "  (" + s.id + ")";

            ImGui::PushID(s.id.c_str());
            auto rowStart = ImGui::GetCursorScreenPos();
            ImGui::AlignTextToFramePadding();
            if (hidden) ImGui::TextDisabled("%s", label.c_str()); else ImGui::TextUnformatted(label.c_str());
            Hint(info);
            // Markers: ● not the default, R after a restart, a tag for file-only settings (and the tier in search results)
            if (hasValue && !SettingsModel::IsDefault(s, stored))
            {
                ImGui::SameLine(0, 4 * uiScale);
                ImGui::TextColored(ImVec4(0.35f, 0.65f, 1, 1), "\xE2\x97\x8F");
                Hint("Not the default (" + display(asi_settings::detail::JsonToText(s.defaultValue)) + ")");
            }
            if (s.apply == "restart")
            {
                ImGui::SameLine(0, 4 * uiScale);
                if (restartPending) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "R"); else ImGui::TextDisabled("R");
                Hint(restartPending ? "Restart the game to apply this change" : "Applies after a restart");
            }
            std::string tag = fileOnly ? "file" : !tierTag ? "" : s.compatibility ? "compat" : !s.InMenu() ? "adv" : "";
            if (!tag.empty()) { ImGui::SameLine(0, 6 * uiScale); ImGui::TextDisabled("%s", tag.c_str()); if (fileOnly) Hint("Changed in the settings file only"); }
            if (showIds) { ImGui::SameLine(0, 6 * uiScale); ImGui::TextDisabled("%s", s.id.c_str()); }

            ImGui::SameLine(LabelWidth());
            ImGui::SetNextItemWidth(ControlWidth());
            ImGui::BeginDisabled(!enabled);
            auto commit = [&](const std::string& v) { model.Set(size_t(index), s, v); };
            if (s.type == Type::Bool)
            {
                int b = 0; asi_settings::detail::ParseBool(value, b); bool checked = b != 0;
                if (ImGui::Checkbox("##value", &checked)) commit(checked ? "1" : "0");
            }
            else if (s.type == Type::Choice || s.dynamic)
            {
                auto choices = SettingsModel::Choices(p.api, s);
                auto preview = display(stored);
                if (display(stored) != display(value)) preview += " (running: " + display(value) + ")";
                if (ImGui::BeginCombo("##value", preview.c_str()))
                {
                    for (auto& c : choices)
                    {
                        auto v = asi_settings::detail::JsonToText(c.value);
                        auto text = c.label.empty() ? v : Text(index, c.label);
                        if (ImGui::Selectable(text.c_str(), asi_settings::detail::JsonEquals(c.value, stored, !s.StringValued()))) commit(v);
                    }
                    if (choices.empty()) ImGui::TextDisabled("No value can be used now");
                    ImGui::EndCombo();
                }
            }
            else if (s.type == Type::Int || s.type == Type::Float)
            {
                auto& edit = edits[key];
                double number = std::strtod(edit.empty() ? value.c_str() : edit.c_str(), nullptr);
                // Decimals: the description's, else as many as the step needs (1 -> "25", 0.05 -> "0.25"), else 2
                int decimals = s.decimals;
                if (decimals < 0 && s.step && *s.step > 0) { decimals = 0; for (double f = *s.step; decimals < 4 && std::abs(f - std::round(f)) > 1e-6; f *= 10) ++decimals; }
                std::string unit;
                for (char c : s.unit) { unit += c; if (c == '%') unit += '%'; }
                auto format = (s.type == Type::Int ? std::string("%.0f") : "%." + std::to_string(decimals < 0 ? 2 : decimals) + "f") + (unit.empty() ? "" : " " + unit);
                bool changed;
                if (s.min && s.max) changed = ImGui::SliderScalar("##value", ImGuiDataType_Double, &number, &*s.min, &*s.max, format.c_str());
                else changed = ImGui::DragScalar("##value", ImGuiDataType_Double, &number, float(s.step.value_or(s.type == Type::Int ? 1.0 : 0.01)), s.min ? &*s.min : nullptr, s.max ? &*s.max : nullptr, format.c_str());
                if (changed)
                {
                    if (s.step && *s.step > 0) number = s.min.value_or(0) + std::round((number - s.min.value_or(0)) / *s.step) * *s.step;
                    if (s.min) number = std::max(number, *s.min);
                    if (s.max) number = std::min(number, *s.max);
                    if (s.type == Type::Int) number = std::round(number);
                    edit = asi_settings::text::FormatNumber(number);
                }
                // Saved when the edit ends, not once per drag frame
                if (ImGui::IsItemDeactivatedAfterEdit() && !edit.empty()) { commit(edit); edit.clear(); }
                else if (!ImGui::IsItemActive()) edit.clear();
            }
            else if (s.type == Type::Action)
            {
                if (ImGui::Button("Run", ImVec2(ControlWidth(), 0)) && OVERLAY_HAS(p.api, Invoke))
                    if (p.api->Invoke(s.id.c_str()) < 0) model.message = label + ": refused.";
            }
            else if (s.type == Type::Info)
            {
                ImGui::TextDisabled("%s", value.c_str());
            }
            else
            {
                // Text-like values: kept as typed while editing, saved on Enter or when the field is left
                if (!edits.count(key)) edits[key] = value;
                auto& edit = edits[key];
                std::vector<char> buffer(4097, 0);
                std::memcpy(buffer.data(), edit.data(), std::min(edit.size(), buffer.size() - 1));
                bool enter = ImGui::InputText("##value", buffer.data(), s.maxLength > 0 ? std::min<size_t>(s.maxLength + 1, buffer.size()) : buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
                edit = buffer.data();
                if (enter || ImGui::IsItemDeactivatedAfterEdit()) { commit(edit); edits.erase(key); }
                else if (!ImGui::IsItemActive()) edits.erase(key);
            }
            ImGui::EndDisabled();
            Hint(info);
            bool controlFocused = ImGui::IsItemFocused();
            if (controlFocused) focused = { index, &s };
            // Delete resets the focused setting (not while typing into it); the pad's X is handled in Draw
            if (controlFocused && enabled && !ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) Reset(index, s);

            // The context menu: right-click anywhere on the row, or the pad's Y on its control (ImGui's own)
            ImVec2 rowEnd(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x, ImGui::GetItemRectMax().y);
            if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(rowStart, rowEnd) && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) ImGui::OpenPopup("row");
            if (ImGui::BeginPopupContextItem("row"))
            {
                ImGui::TextDisabled("%s", s.id.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Reset to default", "Del / X", false, s.HasValue() && enabled)) Reset(index, s);
                if (ImGui::MenuItem("Copy id")) ImGui::SetClipboardText(s.id.c_str());
                if (hasValue && ImGui::MenuItem("Copy value")) ImGui::SetClipboardText(stored.c_str());
                // Special and suggested values of free-form settings
                if ((!s.special.empty() || !s.suggestions.empty()) && s.type != Type::Choice && !s.dynamic && enabled)
                {
                    ImGui::Separator();
                    for (auto list : { &s.special, &s.suggestions })
                        for (auto& c : *list)
                        {
                            auto v = asi_settings::detail::JsonToText(c.value);
                            if (ImGui::MenuItem((c.label.empty() ? v : Text(index, c.label)).c_str(), nullptr, v == stored)) commit(v);
                        }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        // ---- Pages

        bool Visible(int index, const asi_settings::Setting& s) const { return model.Matches(size_t(index), s, search); }

        void Rows(const std::vector<Extension>& rows, int plugin, const PageView& page, Where where, const std::string& group = {})
        {
            for (auto& e : rows)
                if (e.plugin == Plugin(plugin).file.id && e.page == page.id && e.where == where && (group.empty() || asi_settings::text::IEquals(e.group, group)))
                    RunExtension(e);
        }

        void Page(int index, const PageView& page)
        {
            auto values = model.Snapshot(size_t(index));
            auto rows = Registry().List(Extension::Kind::Rows);
            Rows(rows, index, page, Where::Top);
            for (auto& group : page.groups)
            {
                size_t count = 0;
                for (auto s : group.settings) count += Visible(index, *s);
                if (!count) continue;
                Rows(rows, index, page, Where::BeforeGroup, group.title);
                bool openGroup = true;
                if (!group.title.empty())
                {
                    if (collapseAll) ImGui::SetNextItemOpen(false); else if (expandAll) ImGui::SetNextItemOpen(true);
                    openGroup = ImGui::CollapsingHeader((group.title + "  (" + std::to_string(count) + ")###" + group.title).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                }
                if (openGroup)
                {
                    ImGui::PushID(group.title.c_str());
                    for (auto s : group.settings) if (Visible(index, *s)) Row(index, *s, values, false);
                    ImGui::PopID();
                }
                Rows(rows, index, page, Where::AfterGroup, group.title);
            }
            Rows(rows, index, page, Where::Bottom);
        }

        // Search results: every plugin, every tier
        void Results()
        {
            size_t found = 0;
            for (int i = 0; i < int(model.plugins.size()); ++i)
            {
                std::vector<const asi_settings::Setting*> hits;
                for (auto& page : layouts[size_t(i)]) for (auto& g : page.groups) for (auto s : g.settings) if (Visible(i, *s)) hits.push_back(s);
                if (hits.empty()) continue;
                found += hits.size();
                ImGui::PushID(i);
                if (ImGui::CollapsingHeader((Name(i) + "  (" + std::to_string(hits.size()) + ")").c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                {
                    auto values = model.Snapshot(size_t(i));
                    for (auto s : hits) Row(i, *s, values, true);
                }
                ImGui::PopID();
            }
            if (!found) ImGui::TextDisabled("Nothing matches \"%s\".", search);
        }

        // ---- Extensions: run inside the overlay's frame; a throwing (or crashing) one is switched off with a note

        struct Call { const DrawFunction* draw; OverlayUI* ui; std::string error; };
        static void Invoke(void* context)
        {
            auto call = static_cast<Call*>(context);
            try { (*call->draw)(*call->ui); }
            catch (const std::exception& e) { call->error = *e.what() ? e.what() : "exception"; }
            catch (...) { call->error = "exception"; }
        }
        static bool Guarded(void (*function)(void*), void* context)
        {
            __try { function(context); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        void RunExtension(const Extension& e)
        {
            ImGui::PushID(e.id);
            if (!e.error.empty())
                ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "Switched off after an error: %s", e.error.c_str());
            else if (e.draw)
            {
                OverlayUI ui(LabelWidth(), ControlWidth(), status, hoverStatus);
                Call call{ &e.draw, &ui, {} };
                ImGuiErrorRecoveryState state;
                ImGui::ErrorRecoveryStoreState(&state);
                if (!Guarded(Invoke, &call)) call.error = "crashed";
                ImGui::ErrorRecoveryTryToRecoverState(&state);
                if (!call.error.empty()) Registry().Fail(e.id, call.error);
            }
            ImGui::PopID();
        }

        // ---- The tree: plugins and their pages in the hub's order, then the extension nodes (Debug, Tools, ...)

        // Every page in tree order, for LB/RB
        std::vector<std::string> PageKeys(const std::vector<Extension>& pages, const std::vector<std::string>& nodes) const
        {
            std::vector<std::string> keys;
            for (size_t i = 0; i < model.plugins.size(); ++i)
            {
                for (auto& page : layouts[i]) keys.push_back("p:" + model.plugins[i].file.id + ":" + page.id);
                for (auto& e : pages) if (e.parent == model.plugins[i].file.id) keys.push_back("x:" + std::to_string(e.id));
            }
            for (auto& node : nodes) for (auto& e : pages) if (e.parent == node) keys.push_back("x:" + std::to_string(e.id));
            return keys;
        }

        void Leaf(const std::string& key, const std::string& text, const std::string& hint = {})
        {
            bool isSelected = selected == key;
            ImGui::PushID(key.c_str());
            ImGui::Indent(ImGui::GetStyle().IndentSpacing * 0.5f);
            if (ImGui::Selectable(text.c_str(), isSelected)) selected = key;
            ImGui::Unindent(ImGui::GetStyle().IndentSpacing * 0.5f);
            if (isSelected && firstFrame) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
            Hint(hint);
        }

        void Tree(const std::vector<Extension>& pages, const std::vector<std::string>& nodes)
        {
            for (int i = 0; i < int(model.plugins.size()); ++i)
            {
                auto& id = Plugin(i).file.id;
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                if (ImGui::TreeNodeEx(id.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth, "%s", Name(i).c_str()))
                {
                    for (auto& page : layouts[size_t(i)])
                        Leaf("p:" + id + ":" + page.id, page.title + "  (" + std::to_string(page.Count()) + ")",
                            page.tier == Tier::Advanced ? "Settings that aren't in the game's menu: editor and file-only ones, by section of the file." :
                            page.tier == Tier::Compatibility ? "Workarounds for problem setups, by section of the file." : "");
                    for (auto& e : pages) if (e.parent == id) Leaf("x:" + std::to_string(e.id), e.title);
                    ImGui::TreePop();
                }
            }
            for (auto& node : nodes)
            {
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                if (ImGui::TreeNodeEx(("node:" + node).c_str(), ImGuiTreeNodeFlags_SpanAvailWidth, "%s", node.c_str()))
                {
                    for (auto& e : pages) if (e.parent == node) Leaf("x:" + std::to_string(e.id), e.title);
                    ImGui::TreePop();
                }
            }
        }

        void Content(const std::vector<Extension>& pages)
        {
            if (search[0]) { Results(); return; }
            if (selected.rfind("x:", 0) == 0)
            {
                int id = std::atoi(selected.c_str() + 2);
                for (auto& e : pages) if (e.id == id) { RunExtension(e); return; }
            }
            for (int i = 0; i < int(model.plugins.size()); ++i)
            {
                auto prefix = "p:" + Plugin(i).file.id + ":";
                if (selected.rfind(prefix, 0) != 0) continue;
                auto pageId = selected.substr(prefix.size());
                for (auto& page : layouts[size_t(i)])
                {
                    if (page.id != pageId) continue;
                    PresetRow(i);
                    Page(i, page);
                    return;
                }
            }
            ImGui::TextDisabled("%s", model.plugins.empty() ? "No plugin with settings was found." : "Choose a page on the left.");
        }

        void MenuBar()
        {
            if (!ImGui::BeginMenuBar()) return;
            if (ImGui::BeginMenu("View"))
            {
                ImGui::MenuItem("Show ids", nullptr, &showIds);
                if (ImGui::MenuItem("Expand all groups")) expandAll = true;
                if (ImGui::MenuItem("Collapse all groups")) collapseAll = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Dark", nullptr, dark)) { dark = true; themeSet = true; }
                if (ImGui::MenuItem("Light", nullptr, !dark)) { dark = false; themeSet = true; }
                ImGui::EndMenu();
            }
            // Menus of extensions, merged by name, in their order
            auto menus = Registry().List(Extension::Kind::Menu);
            std::vector<std::string> names;
            for (auto& m : menus) if (std::find(names.begin(), names.end(), m.title) == names.end()) names.push_back(m.title);
            for (auto& name : names)
            {
                if (!ImGui::BeginMenu(name.c_str())) continue;
                for (auto& m : menus) if (m.title == name) RunExtension(m);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // Without a valid supporter token: what the overlay is, how to get it, and how to change settings without it
        void SupporterPanel()
        {
            auto& io = ImGui::GetIO();
            auto display = io.DisplaySize;
            float width = std::min(540.0f * uiScale, display.x - 32);
            ImGui::SetNextWindowPos(ImVec2(display.x / 2, display.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, display.y - 32));
            bool keepOpen = true;
            ImGui::Begin((Title() + "###FusionFixSupporter").c_str(), &keepOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
            if (firstFrame) ImGui::SetWindowFocus();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(supporter::text::Heading);
            ImGui::Separator();
            ImGui::TextUnformatted(supporter::text::About);
            ImGui::TextDisabled("%s", supporter::text::StatusLine(supporterStatus));
            ImGui::Spacing();
            ImGui::TextUnformatted(supporter::text::Editing);
            for (size_t i = 0; i < model.plugins.size(); ++i)
            {
                if (model.plugins[i].module != host) continue;
                auto file = model.SettingsFile(i);
                if (!file.empty()) ImGui::TextDisabled("%s%s", supporter::text::SettingsFile, asi_settings::text::Narrow(file.wstring()).c_str());
            }
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button(supporter::text::PageButton)) ShellExecuteA(nullptr, "open", supporter::text::PageUrl, nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::SameLine();
            if (ImGui::Button(supporter::text::Close)) keepOpen = false;
            if (firstFrame) ImGui::SetItemDefaultFocus();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) keepOpen = false;
            ImGui::Separator();
            ImGui::TextDisabled("%s", (KeyName() + " / Back+Start close").c_str());
            ImGui::End();
            firstFrame = false;
            if (!keepOpen) SetOpen(false);
        }

        void Draw()
        {
            if (!open) return;
            ClipCursor(nullptr);
            if (supporterStatus != supporter::Status::Valid) { SupporterPanel(); return; }
            auto& io = ImGui::GetIO();
            auto display = io.DisplaySize;
            // Re-fit after a resolution/window size change; otherwise keep the user's placement
            auto fit = display.x != fittedDisplay.x || display.y != fittedDisplay.y || uiScale != fittedScale ? ImGuiCond_Always : ImGuiCond_Appearing;
            fittedDisplay = display; fittedScale = uiScale;
            ImGui::SetNextWindowSize(ImVec2(std::min(980.0f * uiScale, display.x - 32), std::min(640.0f * uiScale, display.y - 32)), fit);
            ImGui::SetNextWindowPos(ImVec2(display.x / 2, display.y / 2), fit, ImVec2(0.5f, 0.5f));
            bool keepOpen = true;
            ImGui::Begin((Title() + "###FusionFixOverlay").c_str(), &keepOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar);
            if (firstFrame) ImGui::SetWindowFocus();

            // The overlay's own pad buttons: X resets (instead of ImGui's menu toggle), LB/RB change pages
            for (auto k : { ImGuiKey_GamepadFaceLeft, ImGuiKey_GamepadL1, ImGuiKey_GamepadR1 }) ImGui::SetKeyOwner(k, KeyOwner);
            auto pages = Registry().List(Extension::Kind::Page);
            std::vector<std::string> pluginIds;
            for (auto& p : model.plugins) pluginIds.push_back(p.file.id);
            auto nodes = Registry().Nodes(pluginIds);
            auto keys = PageKeys(pages, nodes);
            if (selectFirstOfPlugin)
            {
                for (auto& k : keys) if (k.rfind(selected, 0) == 0) { selected = k; break; }
                selectFirstOfPlugin = false;
            }
            if (std::find(keys.begin(), keys.end(), selected) == keys.end()) selected = keys.empty() ? std::string() : keys.front();
            if (!keys.empty() && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
            {
                int step = int(ImGui::IsKeyPressed(ImGuiKey_GamepadR1, ImGuiInputFlags_Repeat, KeyOwner) || (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_PageDown))) -
                           int(ImGui::IsKeyPressed(ImGuiKey_GamepadL1, ImGuiInputFlags_Repeat, KeyOwner) || (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_PageUp)));
                if (step)
                {
                    auto at = std::find(keys.begin(), keys.end(), selected) - keys.begin();
                    selected = keys[size_t((at + step + ptrdiff_t(keys.size())) % ptrdiff_t(keys.size()))];
                }
            }
            if (focused.setting && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, ImGuiInputFlags_None, KeyOwner)) Reset(focused.plugin, *focused.setting);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) focusSearch = true;
            focused = {};
            status.clear(); hoverStatus.clear();

            MenuBar();
            float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
            // Left: search and the tree
            ImGui::BeginChild("tree", ImVec2(230 * uiScale, -footer), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (focusSearch) { ImGui::SetKeyboardFocusHere(); focusSearch = false; }
            ImGui::InputTextWithHint("##search", "Search name or id (Ctrl+F)", search, sizeof(search));
            Hint("Filters the settings of all plugins by name or id.");
            ImGui::Separator();
            Tree(pages, nodes);
            ImGui::EndChild();
            ImGui::SameLine();
            // Right: the page
            ImGui::BeginChild("page", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
            Content(pages);
            if (!model.message.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", model.message.c_str());
            Confirmation();
            ImGui::EndChild();
            collapseAll = expandAll = false;

            // Footer: the hovered or focused item's text, the keys
            ImGui::Separator();
            auto keysText = KeyName() + " / Back+Start close   LB/RB page   Y menu   X/Del reset";
            float keysWidth = ImGui::CalcTextSize(keysText.c_str()).x;
            auto line = hoverStatus.empty() ? status : hoverStatus;
            auto start = ImGui::GetCursorScreenPos();
            ImGui::PushClipRect(start, ImVec2(start.x + std::max(0.0f, ImGui::GetContentRegionAvail().x - keysWidth - 16 * uiScale), start.y + ImGui::GetFrameHeight()), true);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopClipRect();
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - keysWidth));
            ImGui::TextDisabled("%s", keysText.c_str());
            ImGui::End();
            firstFrame = false;
            if (!keepOpen) SetOpen(false);
        }
    };
}
