#pragma once

// Window style helpers for windowed and borderless game modes.
//
// - SquareCorners(window): no rounded corners on Windows 11 (a no-op before); call again after the game restyles.
// - Place(window, clientWidth, clientHeight): centers the window on its monitor; at desktop size the client area covers
//   the monitor exactly with the frame outside the screen (looks like borderless fullscreen). Unplace undoes it.
// - WindowRect(monitor, clientWidth, clientHeight, frame): the placement computation alone.

#include <windows.h>
#include <shobjidl.h>
#include <algorithm>

namespace WindowStyle
{
    // The frame around a client area: AdjustWindowRectEx of an empty rect (left/top negative, right/bottom positive)
    struct Frame
    {
        LONG left = 0, top = 0, right = 0, bottom = 0;
        LONG Width() const { return right - left; }
        LONG Height() const { return bottom - top; }
    };

    namespace detail
    {
        inline HMODULE System(const wchar_t* name)
        {
            return LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }

        inline HRESULT SetDwmAttribute(HWND window, DWORD attribute, const void* value, DWORD size)
        {
            using SetAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
            static auto setAttribute = []() -> SetAttributeFn
            {
                auto dwm = System(L"dwmapi.dll");
                return dwm ? reinterpret_cast<SetAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
            }();
            return setAttribute && window ? setAttribute(window, attribute, value, size) : E_NOTIMPL;
        }

        // The window size each placed window needs (Windows clamps windows to the screen otherwise)
        struct Allowed
        {
            HWND window = nullptr;
            LONG width = 0, height = 0;
        };

        inline Allowed* AllowedFor(HWND window, bool add)
        {
            static Allowed allowed[8];
            for (auto& a : allowed)
                if (a.window == window)
                    return &a;
            if (add)
                for (auto& a : allowed)
                    if (!a.window || !IsWindow(a.window))
                    {
                        a = { window, 0, 0 };
                        return &a;
                    }
            return nullptr;
        }

        inline LRESULT CALLBACK Subclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
        {
            using DefSubclassProcFn = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
            static auto defSubclassProc = reinterpret_cast<DefSubclassProcFn>(GetProcAddress(System(L"comctl32.dll"), "DefSubclassProc"));
            LRESULT result = defSubclassProc(window, message, wParam, lParam);
            if (message == WM_GETMINMAXINFO)
            {
                if (auto a = AllowedFor(window, false))
                {
                    auto info = reinterpret_cast<MINMAXINFO*>(lParam);
                    info->ptMaxTrackSize.x = (std::max)(info->ptMaxTrackSize.x, a->width);
                    info->ptMaxTrackSize.y = (std::max)(info->ptMaxTrackSize.y, a->height);
                }
            }
            return result;
        }

        inline void AllowSize(HWND window, LONG width, LONG height)
        {
            using SetWindowSubclassFn = BOOL(WINAPI*)(HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR);
            static auto setWindowSubclass = reinterpret_cast<SetWindowSubclassFn>(GetProcAddress(System(L"comctl32.dll"), "SetWindowSubclass"));
            auto a = AllowedFor(window, true);
            if (!a || !setWindowSubclass)
                return;
            a->width = width;
            a->height = height;
            setWindowSubclass(window, &Subclass, 0x46465753, 0);     // the same id again only updates it
        }

        // Tells the shell the window is fullscreen (the taskbar hides) or not
        inline void MarkFullscreen(HWND window, bool fullscreen)
        {
            using CoInitializeExFn = HRESULT(WINAPI*)(LPVOID, DWORD);
            using CoCreateInstanceFn = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
            using CoUninitializeFn = void(WINAPI*)();
            static auto ole = System(L"ole32.dll");
            static auto coInitializeEx = ole ? reinterpret_cast<CoInitializeExFn>(GetProcAddress(ole, "CoInitializeEx")) : nullptr;
            static auto coCreateInstance = ole ? reinterpret_cast<CoCreateInstanceFn>(GetProcAddress(ole, "CoCreateInstance")) : nullptr;
            static auto coUninitialize = ole ? reinterpret_cast<CoUninitializeFn>(GetProcAddress(ole, "CoUninitialize")) : nullptr;
            if (!coInitializeEx || !coCreateInstance || !coUninitialize)
                return;
            HRESULT init = coInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            ITaskbarList2* taskbar = nullptr;
            if (SUCCEEDED(coCreateInstance(__uuidof(TaskbarList), nullptr, CLSCTX_INPROC_SERVER, __uuidof(ITaskbarList2), reinterpret_cast<void**>(&taskbar))) && taskbar)
            {
                if (SUCCEEDED(taskbar->HrInit()))
                    taskbar->MarkFullscreenWindow(window, fullscreen ? TRUE : FALSE);
                taskbar->Release();
            }
            if (SUCCEEDED(init))
                coUninitialize();
        }

        // Windows 11's 1-pixel window border: no colour (none) or the system's
        inline void BorderColor(HWND window, bool none)
        {
            constexpr DWORD DWMWA_BORDER_COLOR_ = 34;
            COLORREF color = none ? 0xFFFFFFFE : 0xFFFFFFFF;    // DWMWA_COLOR_NONE, DWMWA_COLOR_DEFAULT
            SetDwmAttribute(window, DWMWA_BORDER_COLOR_, &color, sizeof(color));
        }
    }

    // The window rect for a client area of clientWidth x clientHeight on a monitor (its full rect, rcMonitor)
    inline RECT WindowRect(const RECT& monitor, LONG clientWidth, LONG clientHeight, const Frame& frame)
    {
        LONG monitorWidth = monitor.right - monitor.left;
        LONG monitorHeight = monitor.bottom - monitor.top;
        RECT client{};
        if (clientWidth == monitorWidth && clientHeight == monitorHeight)
        {
            // As big as the screen: the client area is the monitor, the frame lies outside it
            client = monitor;
        }
        else
        {
            LONG width = clientWidth + frame.Width();
            LONG height = clientHeight + frame.Height();
            // Centered; a window taller or wider than the monitor keeps its client area centered instead
            LONG x = width <= monitorWidth ? monitor.left + (monitorWidth - width) / 2 - frame.left : monitor.left + (monitorWidth - clientWidth) / 2;
            LONG y = height <= monitorHeight ? monitor.top + (monitorHeight - height) / 2 - frame.top : monitor.top + (monitorHeight - clientHeight) / 2;
            client = { x, y, x + clientWidth, y + clientHeight };
        }
        return { client.left + frame.left, client.top + frame.top, client.right + frame.right, client.bottom + frame.bottom };
    }

    // The DPI the window's coordinates are in (the system's for a system-aware process)
    inline UINT DpiOf(HWND window)
    {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        static auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        if (getDpiForWindow && window)
            if (auto dpi = getDpiForWindow(window))
                return dpi;
        HDC dc = GetDC(nullptr);
        UINT dpi = dc ? UINT(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
        if (dc)
            ReleaseDC(nullptr, dc);
        return dpi ? dpi : 96;
    }

    // The frame of a window style at a DPI
    inline Frame FrameOf(DWORD style, DWORD exStyle, UINT dpi)
    {
        using AdjustForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        static auto adjustForDpi = reinterpret_cast<AdjustForDpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
        RECT rect{};
        // WS_OVERLAPPED is 0: a window without a caption gets no frame from it
        style &= ~(WS_MAXIMIZE | WS_MINIMIZE);
        if (!(adjustForDpi && adjustForDpi(&rect, style, FALSE, exStyle, dpi)))
            AdjustWindowRectEx(&rect, style, FALSE, exStyle);
        return { rect.left, rect.top, rect.right, rect.bottom };
    }

    inline Frame FrameOf(HWND window)
    {
        return FrameOf(DWORD(GetWindowLongW(window, GWL_STYLE)), DWORD(GetWindowLongW(window, GWL_EXSTYLE)), DpiOf(window));
    }

    inline RECT MonitorRect(HWND window, HMONITOR preferred = nullptr)
    {
        MONITORINFO info{ sizeof(info) };
        if (!preferred || !GetMonitorInfoW(preferred, &info))
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);
        return info.rcMonitor;
    }

    // No rounded corners (Windows 11); true when DWM took it
    inline bool SquareCorners(HWND window)
    {
        constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCE_ = 33;
        int preference = 1;     // DWMWCP_DONOTROUND
        return SUCCEEDED(detail::SetDwmAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE_, &preference, sizeof(preference)));
    }

    // Whether a window rect is the desktop-size placement: its client area is the whole monitor
    inline bool CoversMonitor(const RECT& monitor, LONG clientWidth, LONG clientHeight)
    {
        return clientWidth == monitor.right - monitor.left && clientHeight == monitor.bottom - monitor.top;
    }

    // Places a window with its current style for a client area of clientWidth x clientHeight on its monitor; returns the
    // window rect it was given. A preferred monitor preserves the display selected before a mode switch temporarily
    // moved the window; an invalid handle falls back to the window's monitor. Call it on the window's thread, outside
    // the loader lock (it initializes COM there when needed).
    inline RECT Place(HWND window, LONG clientWidth, LONG clientHeight, BOOL repaint = TRUE, HMONITOR preferred = nullptr)
    {
        auto monitor = MonitorRect(window, preferred);
        auto frame = FrameOf(window);
        auto rect = WindowRect(monitor, clientWidth, clientHeight, frame);
        bool desktopSize = CoversMonitor(monitor, clientWidth, clientHeight);
        detail::AllowSize(window, rect.right - rect.left, rect.bottom - rect.top);
        MoveWindow(window, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, repaint);
        SquareCorners(window);
        // Bordered at desktop size: the taskbar hides as for borderless, and Windows 11's 1-pixel border gets no colour
        // so no line shows at the screen's edges
        bool covering = desktopSize && frame.Height() > 0;
        detail::BorderColor(window, covering);
        detail::MarkFullscreen(window, desktopSize);
        return rect;
    }

    // The window isn't placed any more (exclusive fullscreen): the shell's and DWM's defaults again
    inline void Unplace(HWND window)
    {
        detail::BorderColor(window, false);
        detail::MarkFullscreen(window, false);
    }
}
