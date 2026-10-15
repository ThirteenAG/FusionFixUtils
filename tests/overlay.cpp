// overlay/Overlay.hpp: the overlay's frame without a renderer, in a hidden window: which window it draws when it opens
// with and without a valid supporter token. (The renderers, OverlayD3D9.hpp and OverlayD3D11.hpp, need a device: the
// standalone header check compiles them.)

#define NOMINMAX
#include <windows.h>
#include "../overlay/Overlay.hpp"
#include "check.h"
#include <fstream>
#include <ctime>

namespace
{
    constexpr std::string_view TestKeys[] = { "ffca6bc45401a0dc4c67ce8e1c83e1c37292030173bada69beb9b9e6e57b9579bcddfa528698c4b41b2705993f4b03519a7bb38290b088a72903ba5bf0123c91" };
    constexpr char TestPrivate[] = "93ffea49e791f8e73934b494c0e0820782ea6b2499e3338c98360d54cb3452e1";

    // A token as the site makes it, signed now with the tests' key
    std::string Sign(int64_t issued, int64_t expires)
    {
        namespace s = asi_overlay::supporter;
        std::string body = std::string(s::Prefix) + ".00112233445566778899aabbccddeeff." + std::to_string(issued) + "." + std::to_string(expires);
        struct { BCRYPT_ECCKEY_BLOB header; unsigned char xyd[96]; } blob{ { BCRYPT_ECDSA_PRIVATE_P256_MAGIC, 32 }, {} };
        s::detail::Hex(TestKeys[0], blob.xyd, 64);
        s::detail::Hex(TestPrivate, blob.xyd + 64, 32);
        unsigned char digest[32], signature[64];
        std::string message(s::Context, sizeof(s::Context));
        message += body;
        s::detail::Sha256(s::detail::Cng::Get(), message, digest);
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_KEY_HANDLE key = nullptr;
        ULONG size = 0;
        BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
        BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key, reinterpret_cast<PUCHAR>(&blob), sizeof(blob), 0);
        BCryptSignHash(key, nullptr, digest, sizeof(digest), signature, sizeof(signature), &size, 0);
        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string text;
        uint32_t buffer = 0;
        int bits = 0;
        for (auto byte : signature)
        {
            buffer = (buffer << 8) | byte;
            bits += 8;
            while (bits >= 6) { bits -= 6; text += table[(buffer >> bits) & 63]; }
        }
        if (bits) text += table[(buffer << (6 - bits)) & 63];
        return body + "." + text;
    }

    void Write(const std::filesystem::path& file, const std::string& text)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    }

    struct Window
    {
        HWND hwnd = nullptr;
        Window()
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"FusionFixUtilsOverlayTest";
            RegisterClassW(&wc);
            hwnd = CreateWindowExW(0, wc.lpszClassName, L"overlay test", WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
        }
        ~Window() { if (hwnd) DestroyWindow(hwnd); }
    };

    // One frame; true when the window of this name was drawn in it
    bool Drew(asi_overlay::Overlay& overlay, const char* name)
    {
        asi_overlay::Overlay::Scope scope(overlay);
        // No renderer here: the font atlas is used as it is
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        overlay.NewFrame();
        auto window = ImGui::FindWindowByName(name);
        return window && window->Active;
    }
}

TEST_CASE("overlay: without a valid supporter token the panel opens instead of the settings")
{
    Window window;
    REQUIRE(window.hwnd != nullptr);
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    auto folder = std::filesystem::path(temp) / (L"ffu-overlay-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);

    asi_overlay::Overlay overlay;
    REQUIRE(overlay.Initialize(window.hwnd, GetModuleHandleW(nullptr)));
    overlay.supporterFile = folder / L"supporter.token";

    // No token
    overlay.SetOpen(true);
    CHECK(overlay.IsOpen());
    CHECK(overlay.SupporterStatus() == asi_overlay::supporter::Status::Missing);
    CHECK(overlay.ShowsSupporterPanel());
    CHECK(Drew(overlay, "###FusionFixSupporter"));
    CHECK(!Drew(overlay, "###FusionFixOverlay"));
    overlay.SetOpen(false);
    CHECK(!overlay.ShowsSupporterPanel());

    // A text file saying so is no token
    Write(overlay.supporterFile, "supporter = 1\r\n");
    overlay.SetOpen(true);
    CHECK(overlay.SupporterStatus() == asi_overlay::supporter::Status::Malformed);
    CHECK(Drew(overlay, "###FusionFixSupporter"));
    overlay.SetOpen(false);

    // A token the overlay's own keys don't know (signed with the tests' key)
    auto now = int64_t(std::time(nullptr));
    Write(overlay.supporterFile, Sign(now - 86400, now + 34 * 86400));
    overlay.SetOpen(true);
    CHECK(overlay.SupporterStatus() == asi_overlay::supporter::Status::BadSignature);
    CHECK(Drew(overlay, "###FusionFixSupporter"));
    CHECK(!Drew(overlay, "###FusionFixOverlay"));
    overlay.SetOpen(false);

    // Valid: the settings window
    overlay.supporterKeys = TestKeys;
    overlay.SetOpen(true);
    CHECK(overlay.SupporterStatus() == asi_overlay::supporter::Status::Valid);
    CHECK(!overlay.ShowsSupporterPanel());
    CHECK(Drew(overlay, "###FusionFixOverlay"));
    CHECK(!Drew(overlay, "###FusionFixSupporter"));
    overlay.SetOpen(false);

    // Expired: the panel again, checked at each open
    Write(overlay.supporterFile, Sign(now - 35 * 86400, now - 60));
    overlay.SetOpen(true);
    CHECK(overlay.SupporterStatus() == asi_overlay::supporter::Status::Expired);
    CHECK(Drew(overlay, "###FusionFixSupporter"));
    overlay.SetOpen(false);

    // Removed (signed out): the panel
    std::filesystem::remove(overlay.supporterFile);
    overlay.SetOpen(true);
    CHECK(overlay.SupporterStatus() == asi_overlay::supporter::Status::Missing);
    overlay.SetOpen(false);

    // Escape closes the panel (the game doesn't see the key); the hotkey toggles it like the overlay
    overlay.SetOpen(true);
    LRESULT result = 0;
    CHECK(overlay.Message(window.hwnd, WM_KEYDOWN, VK_ESCAPE, 0, result));
    Drew(overlay, "###FusionFixSupporter");
    CHECK(!overlay.IsOpen());
    overlay.hotkeyCtrl = false;
    CHECK(overlay.Message(window.hwnd, WM_KEYDOWN, VK_F10, 0, result));
    CHECK(overlay.ShowsSupporterPanel());
    CHECK(overlay.Message(window.hwnd, WM_KEYDOWN, VK_F10, 0, result));
    CHECK(!overlay.IsOpen());

    overlay.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}
