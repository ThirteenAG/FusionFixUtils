// include/WindowStyle.hpp: where windows go (as big as the screen: the client area covers the monitor, the frame
// outside; smaller: centered), on a primary and a secondary monitor, the frame at 100 % and 300 % scaling, and a real
// window placed with it.

#include "../include/WindowStyle.hpp"
#include "check.h"
#include <cstdlib>

namespace
{
    const WindowStyle::Frame frame96{ -8, -31, 8, 8 };     // WS_OVERLAPPEDWINDOW at 96 DPI on Windows 10/11

    bool Same(const RECT& a, const RECT& b)
    {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }
}

TEST_CASE("windowstyle: a window as big as the screen has its client area on the monitor, the frame outside")
{
    RECT primary{ 0, 0, 1920, 1080 };
    auto r = WindowStyle::WindowRect(primary, 1920, 1080, frame96);
    CHECK(Same(r, RECT{ -8, -31, 1928, 1088 }));
    // The client area is the monitor
    CHECK(r.left - frame96.left == 0);
    CHECK(r.top - frame96.top == 0);
    CHECK(r.right - frame96.right == 1920);
    CHECK(r.bottom - frame96.bottom == 1080);

    // A secondary monitor right of the primary one, and one above it
    RECT right{ 1920, 0, 4480, 1440 };
    CHECK(Same(WindowStyle::WindowRect(right, 2560, 1440, frame96), RECT{ 1912, -31, 4488, 1448 }));
    RECT above{ -200, -2160, 3640, 0 };
    CHECK(Same(WindowStyle::WindowRect(above, 3840, 2160, frame96), RECT{ -208, -2191, 3648, 8 }));
}

TEST_CASE("windowstyle: smaller windows are centered on their monitor")
{
    RECT right{ 1920, 0, 4480, 1440 };
    auto r = WindowStyle::WindowRect(right, 1280, 720, frame96);
    CHECK(r.right - r.left == 1280 + frame96.Width());
    CHECK(r.bottom - r.top == 720 + frame96.Height());
    CHECK(r.left - right.left == right.right - r.right);           // centered horizontally
    CHECK(std::abs((r.top - right.top) - (right.bottom - r.bottom)) <= 1);   // and vertically (odd sizes: a pixel)
    CHECK(r.left >= right.left);

    // The client fits, the frame doesn't: the client area is centered
    RECT primary{ 0, 0, 1920, 1080 };
    r = WindowStyle::WindowRect(primary, 1920, 1060, frame96);
    CHECK(r.top - frame96.top == 10);
    CHECK(r.left - frame96.left == 0);

    // No frame (borderless): the client area is the window
    CHECK(Same(WindowStyle::WindowRect(primary, 1920, 1080, {}), primary));
    CHECK(Same(WindowStyle::WindowRect(primary, 1280, 720, {}), RECT{ 320, 180, 1600, 900 }));
}

TEST_CASE("windowstyle: the frame of a style at a DPI")
{
    auto frame = WindowStyle::FrameOf(WS_OVERLAPPEDWINDOW, 0, 96);
    CHECK(frame.left < 0);
    CHECK(frame.top < frame.left);          // the title bar
    CHECK(frame.right > 0);
    CHECK(frame.bottom > 0);
    auto popup = WindowStyle::FrameOf(WS_POPUP, 0, 96);
    CHECK(popup.Width() == 0);
    CHECK(popup.Height() == 0);
    // 300 %: a bigger frame where Windows can tell (AdjustWindowRectExForDpi, Windows 10 1607 and later)
    if (GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"))
        CHECK(WindowStyle::FrameOf(WS_OVERLAPPEDWINDOW, 0, 288).Height() > frame.Height());
}

TEST_CASE("windowstyle: a real window gets the client size it asks for")
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WindowStyleTest";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"WindowStyleTest", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    REQUIRE(window != nullptr);
    auto placed = WindowStyle::Place(window, 640, 360, FALSE);
    RECT client{};
    GetClientRect(window, &client);
    CHECK(client.right - client.left == 640);
    CHECK(client.bottom - client.top == 360);
    RECT actual{};
    GetWindowRect(window, &actual);
    CHECK(Same(actual, placed));
    // As big as its monitor: Windows would clamp it to the screen; the client area is the monitor
    auto monitor = WindowStyle::MonitorRect(window);
    placed = WindowStyle::Place(window, monitor.right - monitor.left, monitor.bottom - monitor.top, FALSE);
    GetClientRect(window, &client);
    CHECK(client.right == monitor.right - monitor.left);
    CHECK(client.bottom == monitor.bottom - monitor.top);
    POINT origin{};
    ClientToScreen(window, &origin);
    CHECK(origin.x == monitor.left);
    CHECK(origin.y == monitor.top);
    // A mode switch may temporarily move the HWND to a different display. Explicit placement
    // must restore the display selected before that move, and stale monitor handles must fall back.
    auto preferred = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    RECT selected = WindowStyle::MonitorRect(window, preferred);
    MoveWindow(window, selected.right + 200, selected.top + 200, 640, 360, FALSE);
    WindowStyle::Place(window, 640, 360, FALSE, preferred);
    CHECK(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) == preferred);
    MONITORINFO stale{ sizeof(stale) };
    auto staleHandle = reinterpret_cast<HMONITOR>(1);
    if (!GetMonitorInfoW(staleHandle, &stale))
        CHECK(Same(WindowStyle::MonitorRect(window, staleHandle), WindowStyle::MonitorRect(window)));
    WindowStyle::Unplace(window);
    WindowStyle::SquareCorners(window);     // true on Windows 11, false before: both fine
    CHECK(WindowStyle::DpiOf(window) >= 96);
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}
