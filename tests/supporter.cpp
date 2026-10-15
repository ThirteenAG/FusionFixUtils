// overlay/Supporter.hpp: the supporter token of the in-game overlay. The tokens below were made by the site's own
// signing code (FusionFixHub-Site worker/api/supporter.js) with a test key; the public key is that key's.

#include <windows.h>
#include "../overlay/Supporter.hpp"
#include "check.h"

namespace
{
    using asi_overlay::supporter::Status;
    namespace supporter = asi_overlay::supporter;

    constexpr std::string_view TestKey = "ffca6bc45401a0dc4c67ce8e1c83e1c37292030173bada69beb9b9e6e57b9579bcddfa528698c4b41b2705993f4b03519a7bb38290b088a72903ba5bf0123c91";
    constexpr std::string_view TestKeys[] = { TestKey };
    // Account "7" and "8", issued 1790000000, expiring 35 days later (1793024000)
    constexpr std::string_view Good = "ffsupporter1.5d4e22987f1334411c9e5fd2146e3de3.1790000000.1793024000.imOUpd2pwAa6g2uW1skF98GFFt84AMESXz_9T8NdQntwDScgJlOaWK0FJcLbYpYs5uNIbc-uKy4TiWiWWdvYhg";
    constexpr std::string_view Other = "ffsupporter1.75d86eb3b528d64034b0884214bd10ff.1790000000.1793024000.c6nxwB-03f8L6X-jE1CqDaU_dkdrV-xEU3FZu4HiHXWuKV_gXFv66WGQjVoXb4uHCVE3OX8UQtj8IWb_h9S75Q";
    constexpr int64_t Issued = 1790000000, Expires = 1793024000;

    std::string Replace(std::string_view text, std::string_view from, std::string_view to)
    {
        std::string s(text);
        auto at = s.find(from);
        if (at != std::string::npos) s.replace(at, from.size(), to);
        return s;
    }
}

TEST_CASE("supporter: a token signed by the site verifies until it expires")
{
    supporter::Token token;
    REQUIRE(supporter::Parse(Good, token));
    CHECK(token.subject == "5d4e22987f1334411c9e5fd2146e3de3");
    CHECK(token.issued == Issued && token.expires == Expires);
    CHECK(token.signature.size() == 64);
    CHECK(supporter::Check(Good, Issued + 86400, TestKeys) == Status::Valid);
    CHECK(supporter::Check(Other, Issued + 86400, TestKeys) == Status::Valid);
    CHECK(supporter::Check(Good, Expires - 1, TestKeys) == Status::Valid);
    CHECK(supporter::Check(Good, Expires, TestKeys) == Status::Expired);
    CHECK(supporter::Check(Good, Expires + 86400 * 400, TestKeys) == Status::Expired);
    // As FusionFixHub writes it, and as a text editor may save it
    CHECK(supporter::Check(std::string(Good) + "\r\n", Issued, TestKeys) == Status::Valid);
    CHECK(supporter::Check("\xEF\xBB\xBF" + std::string(Good) + "\n", Issued, TestKeys) == Status::Valid);
}

TEST_CASE("supporter: changed tokens and other keys are refused")
{
    // Another account's subject, a later expiry, the signature of another token: the signature no longer matches
    CHECK(supporter::Check(Replace(Good, "5d4e22987f1334411c9e5fd2146e3de3", "5d4e22987f1334411c9e5fd2146e3de4"), Issued, TestKeys) == Status::BadSignature);
    CHECK(supporter::Check(Replace(Good, ".1793024000.", ".1793024001."), Issued, TestKeys) == Status::BadSignature);
    auto mixed = std::string(Good.substr(0, Good.rfind('.'))) + std::string(Other.substr(Other.rfind('.')));
    CHECK(supporter::Check(mixed, Issued, TestKeys) == Status::BadSignature);
    // The test key isn't the site's: the overlay's own keys refuse a token signed with it
    CHECK(supporter::Check(Good, Issued, supporter::Keys) == Status::BadSignature);
    // A signature byte flipped
    auto flipped = std::string(Good);
    flipped[flipped.size() - 5] = flipped[flipped.size() - 5] == 'A' ? 'B' : 'A';
    CHECK(supporter::Check(flipped, Issued, TestKeys) == Status::BadSignature);
}

TEST_CASE("supporter: what isn't a token")
{
    CHECK(supporter::Check("", Issued, TestKeys) == Status::Missing);
    CHECK(supporter::Check(" \r\n", Issued, TestKeys) == Status::Missing);
    CHECK(supporter::Check("supporter = 1", Issued, TestKeys) == Status::Malformed);
    CHECK(supporter::Check(Replace(Good, "ffsupporter1", "ffsupporter2"), Issued, TestKeys) == Status::Malformed);
    CHECK(supporter::Check(std::string(Good) + ".x", Issued, TestKeys) == Status::Malformed);
    CHECK(supporter::Check(Good.substr(0, Good.size() - 2), Issued, TestKeys) == Status::Malformed);     // 63 bytes
    CHECK(supporter::Check(Replace(Good, "5d4e2298", "5d4e229g"), Issued, TestKeys) == Status::Malformed);
    CHECK(supporter::Check(Replace(Good, ".1793024000.", ".1789999999."), Issued, TestKeys) == Status::Malformed);  // expires before issued
    CHECK(supporter::Check(Replace(Good, ".1793024000.", ".1799000000."), Issued, TestKeys) == Status::Malformed);  // longer than 40 days
    CHECK(supporter::Check(Replace(Good, ".1790000000.", ".-1790000000."), Issued, TestKeys) == Status::Malformed);
    CHECK(supporter::Check(std::string(600, 'a'), Issued, TestKeys) == Status::Malformed);
}

TEST_CASE("supporter: the token file")
{
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    auto folder = std::filesystem::path(temp) / (L"ffu-supporter-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    auto file = folder / L"supporter.token";
    CHECK(supporter::CheckFile(file, Issued, TestKeys) == Status::Missing);
    CHECK(supporter::CheckFile({}, Issued, TestKeys) == Status::Missing);
    {
        std::ofstream out(file, std::ios::binary);
        out << Good << "\r\n";
    }
    CHECK(supporter::CheckFile(file, Issued + 10, TestKeys) == Status::Valid);
    CHECK(supporter::CheckFile(file, Expires, TestKeys) == Status::Expired);
    {
        std::ofstream out(file, std::ios::binary);
        out << std::string(4096, 'x');
    }
    CHECK(supporter::CheckFile(file, Issued, TestKeys) == Status::Malformed);
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    // Where FusionFixHub keeps it
    auto standard = supporter::DefaultFile();
    CHECK(standard.filename() == L"supporter.token" && standard.parent_path().filename() == L"FusionFixHub");
}

TEST_CASE("supporter: the panel's texts")
{
    using namespace supporter::text;
    CHECK(std::string_view(Heading).find("Supporter") != std::string_view::npos);
    CHECK(*StatusLine(Status::Missing) && *StatusLine(Status::Expired) && *StatusLine(Status::BadSignature) && *StatusLine(Status::Malformed));
    CHECK(!*StatusLine(Status::Valid));
    // Plain wording: the marker is "Supporter"
    for (std::string_view text : { std::string_view(Heading), std::string_view(About), std::string_view(Editing), std::string_view(PageButton),
                                   std::string_view(StatusLine(Status::Missing)), std::string_view(StatusLine(Status::Expired)), std::string_view(StatusLine(Status::Malformed)) })
    {
        std::string lower(text);
        for (auto& c : lower) c = char(tolower(static_cast<unsigned char>(c)));
        for (auto word : { "free", "paid", "unlock", "premium", "pro " })
            CHECK(lower.find(word) == std::string::npos);
    }
}
