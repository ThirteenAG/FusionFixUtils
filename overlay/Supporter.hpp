#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

// The in-game overlay is a Supporter feature. FusionFixHub keeps a token for the signed-in supporter in
// %LOCALAPPDATA%\FusionFixHub\supporter.token (fusionfix.io issues it, signed; renewed whenever the hub runs, removed on
// sign-out or when the support ends). The overlay checks it offline when it opens: no network, nothing at startup.
//
//   token     = "ffsupporter1.<subject>.<issued>.<expires>.<signature>"
//   subject   = 32 hex digits (an account hash), issued/expires = Unix seconds
//   signature = ECDSA P-256 / SHA-256, r || s (64 bytes), base64url without padding, over
//               "fusionfix.io supporter token v1\0" + everything before the last "."
namespace asi_overlay::supporter
{
    // The public keys tokens are signed with: X and Y of a P-256 key, hex. Rotation: add the next key in a release
    // the current key still covers; sign with it after that release is out; drop the old key later.
    inline constexpr std::string_view Keys[] = {
        "bbe7c27f801d3c53a831c08c472ed04beff52a79595c3144302f102db8c29daf60b169f695b612fc1387bc0bfe562f9813e585925054dc4683b00a788a98b27f",
    };
    inline constexpr std::string_view Prefix = "ffsupporter1";
    inline constexpr char Context[] = "fusionfix.io supporter token v1";    // and its terminating NUL
    inline constexpr int64_t MaxLifetime = 40 * 86400;                      // the site issues 35 days

    enum class Status { Valid, Missing, Malformed, BadSignature, Expired };

    struct Token
    {
        std::string subject;
        int64_t issued = 0, expires = 0;
        std::string body;           // what is signed after the context: everything before the last "."
        std::string signature;      // r || s, 64 bytes
    };

    namespace detail
    {
        inline bool Number(std::string_view text, int64_t& out)
        {
            if (text.empty() || text.size() > 12) return false;
            out = 0;
            for (char c : text) { if (c < '0' || c > '9') return false; out = out * 10 + (c - '0'); }
            return true;
        }

        inline bool Base64Url(std::string_view text, std::string& out)
        {
            out.clear();
            uint32_t buffer = 0;
            int bits = 0;
            for (char c : text)
            {
                int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '-' ? 62 : c == '_' ? 63 : -1;
                if (v < 0) return false;
                buffer = (buffer << 6) | uint32_t(v);
                bits += 6;
                if (bits >= 8) { bits -= 8; out += char((buffer >> bits) & 0xFF); }
            }
            return bits < 6 && (buffer & ((1u << bits) - 1)) == 0;
        }

        inline bool Hex(std::string_view text, unsigned char* out, size_t size)
        {
            if (text.size() != size * 2) return false;
            auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
            for (size_t i = 0; i < size; ++i)
            {
                int hi = digit(text[i * 2]), lo = digit(text[i * 2 + 1]);
                if (hi < 0 || lo < 0) return false;
                out[i] = static_cast<unsigned char>(hi << 4 | lo);
            }
            return true;
        }

        // CNG from System32, loaded the first time a token is checked (the overlay's first open), never before
        struct Cng
        {
            decltype(&BCryptOpenAlgorithmProvider) open = nullptr;
            decltype(&BCryptCloseAlgorithmProvider) close = nullptr;
            decltype(&BCryptCreateHash) createHash = nullptr;
            decltype(&BCryptHashData) hashData = nullptr;
            decltype(&BCryptFinishHash) finishHash = nullptr;
            decltype(&BCryptDestroyHash) destroyHash = nullptr;
            decltype(&BCryptImportKeyPair) importKey = nullptr;
            decltype(&BCryptVerifySignature) verify = nullptr;
            decltype(&BCryptDestroyKey) destroyKey = nullptr;
            bool ok = false;

            static const Cng& Get()
            {
                static const Cng cng = []()
                {
                    Cng c;
                    auto module = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                    if (!module) return c;
                    auto get = [&](auto& f, const char* name) { f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(GetProcAddress(module, name)); return f != nullptr; };
                    c.ok = get(c.open, "BCryptOpenAlgorithmProvider") && get(c.close, "BCryptCloseAlgorithmProvider") &&
                        get(c.createHash, "BCryptCreateHash") && get(c.hashData, "BCryptHashData") && get(c.finishHash, "BCryptFinishHash") &&
                        get(c.destroyHash, "BCryptDestroyHash") && get(c.importKey, "BCryptImportKeyPair") &&
                        get(c.verify, "BCryptVerifySignature") && get(c.destroyKey, "BCryptDestroyKey");
                    return c;
                }();
                return cng;
            }
        };

        inline bool Sha256(const Cng& cng, std::string_view data, unsigned char digest[32])
        {
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            bool ok = BCRYPT_SUCCESS(cng.open(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
                BCRYPT_SUCCESS(cng.createHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0)) &&
                BCRYPT_SUCCESS(cng.hashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), ULONG(data.size()), 0)) &&
                BCRYPT_SUCCESS(cng.finishHash(hash, digest, 32, 0));
            if (hash) cng.destroyHash(hash);
            if (algorithm) cng.close(algorithm, 0);
            return ok;
        }
    }

    // The token's parts; false when it isn't one (the signature isn't checked here)
    inline bool Parse(std::string_view text, Token& out)
    {
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
        if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        if (text.size() > 512) return false;
        std::string_view parts[5];
        size_t count = 0, start = 0;
        for (size_t i = 0; i <= text.size(); ++i)
        {
            if (i < text.size() && text[i] != '.') continue;
            if (count == 5) return false;
            parts[count++] = text.substr(start, i - start);
            start = i + 1;
        }
        unsigned char subject[16];
        Token t;
        if (count != 5 || parts[0] != Prefix || !detail::Hex(parts[1], subject, sizeof(subject)) ||
            !detail::Number(parts[2], t.issued) || !detail::Number(parts[3], t.expires) || !detail::Base64Url(parts[4], t.signature) ||
            t.signature.size() != 64 || t.expires <= t.issued || t.expires - t.issued > MaxLifetime)
            return false;
        t.subject = std::string(parts[1]);
        t.body = std::string(text.substr(0, size_t(parts[4].data() - text.data()) - 1));
        out = std::move(t);
        return true;
    }

    // The signature with one public key (hex X || Y)
    inline bool Verify(const Token& token, std::string_view publicKey)
    {
        auto& cng = detail::Cng::Get();
        if (!cng.ok) return false;
        struct { BCRYPT_ECCKEY_BLOB header; unsigned char xy[64]; } blob{ { BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32 }, {} };
        if (!detail::Hex(publicKey, blob.xy, sizeof(blob.xy))) return false;
        unsigned char digest[32];
        std::string message(Context, sizeof(Context));
        message += token.body;
        if (!detail::Sha256(cng, message, digest)) return false;
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_KEY_HANDLE key = nullptr;
        bool ok = BCRYPT_SUCCESS(cng.open(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)) &&
            BCRYPT_SUCCESS(cng.importKey(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, reinterpret_cast<PUCHAR>(&blob), ULONG(sizeof(blob)), 0)) &&
            BCRYPT_SUCCESS(cng.verify(key, nullptr, digest, sizeof(digest), reinterpret_cast<PUCHAR>(const_cast<char*>(token.signature.data())), ULONG(token.signature.size()), 0));
        if (key) cng.destroyKey(key);
        if (algorithm) cng.close(algorithm, 0);
        return ok;
    }

    // A token's text at `now` (Unix seconds), against these keys
    inline Status Check(std::string_view text, int64_t now, std::span<const std::string_view> keys = Keys)
    {
        if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) return Status::Missing;
        Token token;
        if (!Parse(text, token)) return Status::Malformed;
        bool signedByKey = false;
        for (auto key : keys) if (Verify(token, key)) { signedByKey = true; break; }
        if (!signedByKey) return Status::BadSignature;
        return now >= token.expires ? Status::Expired : Status::Valid;
    }

    // %LOCALAPPDATA%\FusionFixHub\supporter.token (empty when there is no such folder)
    inline std::filesystem::path DefaultFile()
    {
        wchar_t local[MAX_PATH * 2] = {};
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, DWORD(std::size(local)));
        if (!n || n >= std::size(local)) return {};
        return std::filesystem::path(local) / L"FusionFixHub" / L"supporter.token";
    }

    // What the overlay shows instead of the settings without a valid token
    namespace text
    {
        inline constexpr char Heading[] = "The in-game settings overlay is a Supporter feature";
        inline constexpr char About[] = "It is available to supporters of Fusion Fix development. To use it on this computer, sign in to "
            "FusionFixHub with your fusionfix.io account; the hub renews the access each time it runs.";
        inline constexpr char Editing[] = "Settings can still be changed without it: edit the settings file in any text editor, or use "
            "FusionFixHub's text editor. Saved changes are picked up while the game runs; some apply after a restart.";
        inline constexpr char SettingsFile[] = "Settings file: ";
        inline constexpr char PageButton[] = "Become a supporter";
        inline constexpr char PageUrl[] = "https://fusionfix.io/credits/";
        inline constexpr char Close[] = "Close";

        inline const char* StatusLine(Status status)
        {
            switch (status)
            {
                case Status::Valid: return "";
                case Status::Expired: return "The supporter access on this computer has expired. Open FusionFixHub to renew it.";
                case Status::Malformed:
                case Status::BadSignature: return "The supporter access on this computer can't be verified. Open FusionFixHub to renew it.";
                default: return "No supporter access was found on this computer.";
            }
        }
    }

    inline Status CheckFile(const std::filesystem::path& file, int64_t now = int64_t(std::time(nullptr)), std::span<const std::string_view> keys = Keys)
    {
        if (file.empty()) return Status::Missing;
        std::ifstream in(file, std::ios::binary);
        if (!in) return Status::Missing;
        std::string text(1024, '\0');
        in.read(text.data(), std::streamsize(text.size()));
        text.resize(size_t(in.gcount()));
        return Check(text, now, keys);
    }
}
