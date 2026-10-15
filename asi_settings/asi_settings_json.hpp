/*
    ASI Settings 1.0
    A small JSON reader for descriptions and translations, used by asi_settings.hpp. No dependencies.
*/
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asi_settings::json
{
    struct Value
    {
        enum class Kind { Null, Bool, Number, String, Array, Object };

        Kind kind = Kind::Null;
        bool boolean = false;
        double number = 0.0;
        std::string string;
        std::vector<Value> items;                               // Array
        std::vector<std::pair<std::string, Value>> members;     // Object, in file order

        bool IsNull() const { return kind == Kind::Null; }
        bool IsBool() const { return kind == Kind::Bool; }
        bool IsNumber() const { return kind == Kind::Number; }
        bool IsString() const { return kind == Kind::String; }
        bool IsArray() const { return kind == Kind::Array; }
        bool IsObject() const { return kind == Kind::Object; }

        // The member of an object, or nullptr
        const Value* Find(std::string_view name) const
        {
            if (kind != Kind::Object)
                return nullptr;
            for (auto& [key, value] : members)
            {
                if (key == name)
                    return &value;
            }
            return nullptr;
        }

        std::string_view Str(std::string_view name, std::string_view fallback = {}) const
        {
            auto v = Find(name);
            return v && v->IsString() ? std::string_view(v->string) : fallback;
        }

        double Num(std::string_view name, double fallback = 0.0) const
        {
            auto v = Find(name);
            return v && v->IsNumber() ? v->number : fallback;
        }

        bool Bool(std::string_view name, bool fallback = false) const
        {
            auto v = Find(name);
            return v && v->IsBool() ? v->boolean : fallback;
        }
    };

    struct Error
    {
        size_t offset = 0;
        std::string message;
    };

    namespace detail
    {
        struct Parser
        {
            std::string_view text;
            size_t pos = 0;
            Error* error = nullptr;
            int depth = 0;

            bool Fail(const char* message)
            {
                if (error && error->message.empty())
                {
                    error->offset = pos;
                    error->message = message;
                }
                return false;
            }

            void SkipSpace()
            {
                while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n'))
                    ++pos;
            }

            bool Literal(std::string_view word)
            {
                if (text.substr(pos, word.size()) != word)
                    return Fail("unexpected text");
                pos += word.size();
                return true;
            }

            static void AppendUtf8(std::string& out, uint32_t cp)
            {
                if (cp < 0x80)
                    out += char(cp);
                else if (cp < 0x800)
                {
                    out += char(0xC0 | (cp >> 6));
                    out += char(0x80 | (cp & 0x3F));
                }
                else if (cp < 0x10000)
                {
                    out += char(0xE0 | (cp >> 12));
                    out += char(0x80 | ((cp >> 6) & 0x3F));
                    out += char(0x80 | (cp & 0x3F));
                }
                else
                {
                    out += char(0xF0 | (cp >> 18));
                    out += char(0x80 | ((cp >> 12) & 0x3F));
                    out += char(0x80 | ((cp >> 6) & 0x3F));
                    out += char(0x80 | (cp & 0x3F));
                }
            }

            bool Hex4(uint32_t& out)
            {
                if (pos + 4 > text.size())
                    return Fail("incomplete \\u escape");
                out = 0;
                for (int i = 0; i < 4; ++i)
                {
                    char c = text[pos++];
                    out <<= 4;
                    if (c >= '0' && c <= '9') out |= uint32_t(c - '0');
                    else if (c >= 'a' && c <= 'f') out |= uint32_t(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') out |= uint32_t(c - 'A' + 10);
                    else return Fail("invalid \\u escape");
                }
                return true;
            }

            bool String(std::string& out)
            {
                ++pos; // "
                while (pos < text.size())
                {
                    char c = text[pos++];
                    if (c == '"')
                        return true;
                    if (uint8_t(c) < 0x20)
                        return Fail("control character in string");
                    if (c != '\\')
                    {
                        out += c;
                        continue;
                    }
                    if (pos >= text.size())
                        break;
                    char e = text[pos++];
                    switch (e)
                    {
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        case '/': out += '/'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case 'n': out += '\n'; break;
                        case 'r': out += '\r'; break;
                        case 't': out += '\t'; break;
                        case 'u':
                        {
                            uint32_t cp;
                            if (!Hex4(cp))
                                return false;
                            if (cp >= 0xD800 && cp <= 0xDBFF)
                            {
                                uint32_t low;
                                if (text.substr(pos, 2) != "\\u")
                                    return Fail("unpaired surrogate");
                                pos += 2;
                                if (!Hex4(low) || low < 0xDC00 || low > 0xDFFF)
                                    return Fail("unpaired surrogate");
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            }
                            AppendUtf8(out, cp);
                            break;
                        }
                        default:
                            return Fail("invalid escape");
                    }
                }
                return Fail("unterminated string");
            }

            bool Number(double& out)
            {
                size_t start = pos;
                if (pos < text.size() && text[pos] == '-')
                    ++pos;
                while (pos < text.size() && ((text[pos] >= '0' && text[pos] <= '9') || text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E' || text[pos] == '+' || text[pos] == '-'))
                    ++pos;
                std::string number(text.substr(start, pos - start));
                char* end = nullptr;
                out = std::strtod(number.c_str(), &end);
                if (number.empty() || end != number.c_str() + number.size())
                    return Fail("invalid number");
                return true;
            }

            bool Parse(Value& value)
            {
                if (++depth > 64)
                    return Fail("nested too deeply");
                SkipSpace();
                if (pos >= text.size())
                    return Fail("unexpected end");
                bool ok = true;
                char c = text[pos];
                if (c == '{')
                {
                    value.kind = Value::Kind::Object;
                    ++pos;
                    SkipSpace();
                    if (pos < text.size() && text[pos] == '}')
                        ++pos;
                    else
                    {
                        while (ok)
                        {
                            SkipSpace();
                            if (pos >= text.size() || text[pos] != '"')
                                return Fail("expected a member name");
                            std::string key;
                            if (!String(key))
                                return false;
                            SkipSpace();
                            if (pos >= text.size() || text[pos] != ':')
                                return Fail("expected ':'");
                            ++pos;
                            Value member;
                            if (!Parse(member))
                                return false;
                            value.members.emplace_back(std::move(key), std::move(member));
                            SkipSpace();
                            if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
                            if (pos < text.size() && text[pos] == '}') { ++pos; break; }
                            return Fail("expected ',' or '}'");
                        }
                    }
                }
                else if (c == '[')
                {
                    value.kind = Value::Kind::Array;
                    ++pos;
                    SkipSpace();
                    if (pos < text.size() && text[pos] == ']')
                        ++pos;
                    else
                    {
                        while (ok)
                        {
                            Value item;
                            if (!Parse(item))
                                return false;
                            value.items.push_back(std::move(item));
                            SkipSpace();
                            if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
                            if (pos < text.size() && text[pos] == ']') { ++pos; break; }
                            return Fail("expected ',' or ']'");
                        }
                    }
                }
                else if (c == '"')
                {
                    value.kind = Value::Kind::String;
                    ok = String(value.string);
                }
                else if (c == 't')
                {
                    value.kind = Value::Kind::Bool;
                    value.boolean = true;
                    ok = Literal("true");
                }
                else if (c == 'f')
                {
                    value.kind = Value::Kind::Bool;
                    ok = Literal("false");
                }
                else if (c == 'n')
                    ok = Literal("null");
                else
                {
                    value.kind = Value::Kind::Number;
                    ok = Number(value.number);
                }
                --depth;
                return ok;
            }
        };
    }

    // Parses a whole document; a UTF-8 byte order mark is skipped
    inline bool Parse(std::string_view text, Value& out, Error* error = nullptr)
    {
        if (text.substr(0, 3) == "\xEF\xBB\xBF")
            text.remove_prefix(3);
        detail::Parser parser{ text, 0, error };
        out = {};
        if (!parser.Parse(out))
            return false;
        parser.SkipSpace();
        if (parser.pos != text.size())
            return parser.Fail("text after the document");
        return true;
    }
}
