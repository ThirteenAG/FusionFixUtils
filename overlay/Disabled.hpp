#pragma once
// The overlay in builds without ASI_OVERLAY (release builds): the same API as empty inline functions. No ImGui, no
// window or pad input, no renderer, no supporter check. Draw functions are taken by reference and dropped, so their
// bodies are never called or emitted; they still have to compile (OverlayUI's rows below). The includes are the
// overlay's own minus ImGui, so code that compiles against the overlay compiles against this.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include "../asi_settings/asi_settings.hpp"
#include "Extensions.hpp"

namespace asi_overlay
{
    class OverlayUI
    {
    public:
        bool Toggle(const char*, bool&, const char* = nullptr) { return false; }
        bool Slider(const char*, float&, float, float, const char* = nullptr, const char* = nullptr) { return false; }
        bool SliderInt(const char*, int&, int, int, const char* = nullptr, const char* = nullptr) { return false; }
        template<typename Items>
        bool Combo(const char*, int&, const Items&, const char* = nullptr) { return false; }
        bool Button(const char*, const char* = nullptr) { return false; }
        void Value(const char*, const std::string&, const char* = nullptr) {}
        void Text(const char*, ...) {}
        bool Section(const char*) { return false; }
        void Separator() {}
        void Hint(const char*) {}
        float LabelWidth() const { return 0; }
        float ControlWidth() const { return 0; }
    };

    class Overlay
    {
    public:
        static constexpr void* context = nullptr;
        static constexpr HWND window = nullptr;
        static constexpr HMODULE host = nullptr;
        int hotkey = 0;
        bool hotkeyCtrl = true;
        std::string fallbackLanguage;
        bool dark = true;

        bool Initialize(HWND, HMODULE) { return false; }
        void Shutdown() {}
        constexpr bool IsOpen() const { return false; }
        constexpr bool ShowsSupporterPanel() const { return false; }
        constexpr bool BlocksInput() const { return false; }
        void SetTheme(bool) {}
        void SelectPlugin(std::string_view, std::string_view = {}) {}
        void SetOpen(bool) {}

        template<typename... Args> static int AddPage(Args&&...) { return 0; }
        template<typename... Args> static int AddRows(Args&&...) { return 0; }
        template<typename... Args> static int AddMenu(Args&&...) { return 0; }
        static bool Remove(int) { return false; }

        bool Message(HWND, UINT, WPARAM, LPARAM, LRESULT&) { return false; }
        void RawMouse(LPARAM) {}
        void MouseButton(int, bool) {}
        void MouseWheel(float) {}
        void Gamepad(uint16_t, int8_t = 0, int8_t = 0) {}
        void NewFrame(float = 0, float = 0, bool = false) {}
    };

    class D3D9Overlay : public Overlay
    {
    public:
        bool pretransformed = false;
        template<typename... Args> void Present(Args&&...) {}
        void BeforeReset() {}
        void ShutdownRenderer() {}
    };

    class D3D11Overlay : public Overlay
    {
    public:
        template<typename... Args> void Present(Args&&...) {}
        void ShutdownRenderer() {}
    };
}
