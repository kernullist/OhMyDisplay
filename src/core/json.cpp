// OhMyDisplay - minimal JSON serializer/parser implementation.
#include "core/json.h"

#include <cstdio>
#include <cmath>
#include <cstring>

namespace omd::json
{
    const std::string Value::empty_string_;
    const Value Value::null_sentinel_;

    static void EscapeString(std::string& out, const std::string& s)
    {
        out.push_back('"');
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    // UTF-8 bytes pass through unescaped.
                    out.push_back(static_cast<char>(c));
                }
                break;
            }
        }
        out.push_back('"');
    }

    static void AppendNumber(std::string& out, double n)
    {
        if (std::isnan(n) || std::isinf(n))
        {
            out += "0";
            return;
        }
        // Integers print without a decimal point; otherwise use a compact form.
        double rounded = std::floor(n);
        if (rounded == n && std::fabs(n) < 1e15)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n));
            out += buf;
        }
        else
        {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.10g", n);
            out += buf;
        }
    }

    void Value::DumpTo(std::string& out, bool pretty, int depth) const
    {
        const char* nl = pretty ? "\n" : "";
        auto indent = [&](int d)
        {
            if (pretty)
            {
                out.append(static_cast<size_t>(d) * 2, ' ');
            }
        };

        switch (type_)
        {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += bool_ ? "true" : "false";
            break;
        case Type::Number:
            AppendNumber(out, num_);
            break;
        case Type::String:
            EscapeString(out, str_);
            break;
        case Type::Array:
        {
            if (arr_.empty())
            {
                out += "[]";
                break;
            }
            out += "[";
            out += nl;
            for (size_t i = 0; i < arr_.size(); ++i)
            {
                indent(depth + 1);
                arr_[i].DumpTo(out, pretty, depth + 1);
                if (i + 1 < arr_.size())
                {
                    out += ",";
                }
                out += nl;
            }
            indent(depth);
            out += "]";
            break;
        }
        case Type::Object:
        {
            if (obj_.empty())
            {
                out += "{}";
                break;
            }
            out += "{";
            out += nl;
            for (size_t i = 0; i < obj_.size(); ++i)
            {
                indent(depth + 1);
                EscapeString(out, obj_[i].first);
                out += pretty ? ": " : ":";
                obj_[i].second.DumpTo(out, pretty, depth + 1);
                if (i + 1 < obj_.size())
                {
                    out += ",";
                }
                out += nl;
            }
            indent(depth);
            out += "}";
            break;
        }
        }
    }

    std::string Value::Dump(bool pretty) const
    {
        std::string out;
        DumpTo(out, pretty, 0);
        return out;
    }

    // ---- Parser ----
    namespace
    {
        struct Parser
        {
            std::string_view s;
            size_t i = 0;
            std::string* err = nullptr;

            bool Fail(const char* msg)
            {
                if (err && err->empty())
                {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), " at byte %zu", i);
                    *err = std::string(msg) + buf;
                }
                return false;
            }

            void SkipWs()
            {
                while (i < s.size())
                {
                    char c = s[i];
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                    {
                        ++i;
                    }
                    else
                    {
                        break;
                    }
                }
            }

            bool ParseValue(Value& out)
            {
                bool ok = false;
                do
                {
                    SkipWs();
                    if (i >= s.size())
                    {
                        Fail("unexpected end");
                        break;
                    }
                    char c = s[i];
                    if (c == '{')
                    {
                        ok = ParseObject(out);
                        break;
                    }
                    if (c == '[')
                    {
                        ok = ParseArray(out);
                        break;
                    }
                    if (c == '"')
                    {
                        std::string str;
                        if (!ParseString(str))
                        {
                            break;
                        }
                        out = Value(std::move(str));
                        ok = true;
                        break;
                    }
                    if (c == 't' || c == 'f')
                    {
                        ok = ParseBool(out);
                        break;
                    }
                    if (c == 'n')
                    {
                        ok = ParseNull(out);
                        break;
                    }
                    ok = ParseNumber(out);
                } while (false);
                return ok;
            }

            bool ParseLiteral(const char* lit)
            {
                size_t n = std::strlen(lit);
                if (i + n > s.size() || s.compare(i, n, lit) != 0)
                {
                    return Fail("invalid literal");
                }
                i += n;
                return true;
            }

            bool ParseNull(Value& out)
            {
                if (!ParseLiteral("null"))
                {
                    return false;
                }
                out = Value(nullptr);
                return true;
            }

            bool ParseBool(Value& out)
            {
                if (s[i] == 't')
                {
                    if (!ParseLiteral("true"))
                    {
                        return false;
                    }
                    out = Value(true);
                }
                else
                {
                    if (!ParseLiteral("false"))
                    {
                        return false;
                    }
                    out = Value(false);
                }
                return true;
            }

            bool ParseNumber(Value& out)
            {
                size_t start = i;
                if (i < s.size() && (s[i] == '-' || s[i] == '+'))
                {
                    ++i;
                }
                bool any = false;
                while (i < s.size())
                {
                    char c = s[i];
                    if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')
                    {
                        ++i;
                        any = true;
                    }
                    else
                    {
                        break;
                    }
                }
                if (!any)
                {
                    return Fail("invalid number");
                }
                std::string tok(s.substr(start, i - start));
                out = Value(std::strtod(tok.c_str(), nullptr));
                return true;
            }

            void AppendCodepoint(std::string& out, unsigned cp)
            {
                if (cp <= 0x7F)
                {
                    out.push_back(static_cast<char>(cp));
                }
                else if (cp <= 0x7FF)
                {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                else if (cp <= 0xFFFF)
                {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                else
                {
                    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            }

            bool ParseHex4(unsigned& out)
            {
                if (i + 4 > s.size())
                {
                    return Fail("bad unicode escape");
                }
                unsigned v = 0;
                for (int k = 0; k < 4; ++k)
                {
                    char c = s[i + k];
                    v <<= 4;
                    if (c >= '0' && c <= '9')
                    {
                        v |= static_cast<unsigned>(c - '0');
                    }
                    else if (c >= 'a' && c <= 'f')
                    {
                        v |= static_cast<unsigned>(c - 'a' + 10);
                    }
                    else if (c >= 'A' && c <= 'F')
                    {
                        v |= static_cast<unsigned>(c - 'A' + 10);
                    }
                    else
                    {
                        return Fail("bad unicode escape");
                    }
                }
                i += 4;
                out = v;
                return true;
            }

            bool ParseString(std::string& out)
            {
                bool ok = false;
                do
                {
                    if (s[i] != '"')
                    {
                        Fail("expected string");
                        break;
                    }
                    ++i;
                    bool closed = false;
                    while (i < s.size())
                    {
                        char c = s[i++];
                        if (c == '"')
                        {
                            closed = true;
                            break;
                        }
                        if (c == '\\')
                        {
                            if (i >= s.size())
                            {
                                break;
                            }
                            char e = s[i++];
                            switch (e)
                            {
                            case '"':
                                out.push_back('"');
                                break;
                            case '\\':
                                out.push_back('\\');
                                break;
                            case '/':
                                out.push_back('/');
                                break;
                            case 'b':
                                out.push_back('\b');
                                break;
                            case 'f':
                                out.push_back('\f');
                                break;
                            case 'n':
                                out.push_back('\n');
                                break;
                            case 'r':
                                out.push_back('\r');
                                break;
                            case 't':
                                out.push_back('\t');
                                break;
                            case 'u':
                            {
                                unsigned cp = 0;
                                if (!ParseHex4(cp))
                                {
                                    return false;
                                }
                                if (cp >= 0xD800 && cp <= 0xDBFF)
                                {
                                    // High surrogate; expect a low surrogate next.
                                    if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u')
                                    {
                                        i += 2;
                                        unsigned lo = 0;
                                        if (!ParseHex4(lo))
                                        {
                                            return false;
                                        }
                                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                    }
                                }
                                AppendCodepoint(out, cp);
                                break;
                            }
                            default:
                                Fail("bad escape");
                                return false;
                            }
                        }
                        else
                        {
                            out.push_back(c);
                        }
                    }
                    if (!closed)
                    {
                        Fail("unterminated string");
                        break;
                    }
                    ok = true;
                } while (false);
                return ok;
            }

            bool ParseArray(Value& out)
            {
                bool ok = false;
                do
                {
                    out = Value::MakeArray();
                    ++i; // consume '['
                    SkipWs();
                    if (i < s.size() && s[i] == ']')
                    {
                        ++i;
                        ok = true;
                        break;
                    }
                    bool good = true;
                    while (true)
                    {
                        Value elem;
                        if (!ParseValue(elem))
                        {
                            good = false;
                            break;
                        }
                        out.Push(std::move(elem));
                        SkipWs();
                        if (i >= s.size())
                        {
                            Fail("unterminated array");
                            good = false;
                            break;
                        }
                        if (s[i] == ',')
                        {
                            ++i;
                            continue;
                        }
                        if (s[i] == ']')
                        {
                            ++i;
                            break;
                        }
                        Fail("expected , or ]");
                        good = false;
                        break;
                    }
                    ok = good;
                } while (false);
                return ok;
            }

            bool ParseObject(Value& out)
            {
                bool ok = false;
                do
                {
                    out = Value::MakeObject();
                    ++i; // consume '{'
                    SkipWs();
                    if (i < s.size() && s[i] == '}')
                    {
                        ++i;
                        ok = true;
                        break;
                    }
                    bool good = true;
                    while (true)
                    {
                        SkipWs();
                        if (i >= s.size() || s[i] != '"')
                        {
                            Fail("expected object key");
                            good = false;
                            break;
                        }
                        std::string key;
                        if (!ParseString(key))
                        {
                            good = false;
                            break;
                        }
                        SkipWs();
                        if (i >= s.size() || s[i] != ':')
                        {
                            Fail("expected :");
                            good = false;
                            break;
                        }
                        ++i;
                        Value val;
                        if (!ParseValue(val))
                        {
                            good = false;
                            break;
                        }
                        out.Set(key, std::move(val));
                        SkipWs();
                        if (i >= s.size())
                        {
                            Fail("unterminated object");
                            good = false;
                            break;
                        }
                        if (s[i] == ',')
                        {
                            ++i;
                            continue;
                        }
                        if (s[i] == '}')
                        {
                            ++i;
                            break;
                        }
                        Fail("expected , or }");
                        good = false;
                        break;
                    }
                    ok = good;
                } while (false);
                return ok;
            }
        };
    }

    bool Value::Parse(std::string_view text, Value& out, std::string* error)
    {
        Parser p;
        p.s = text;
        p.err = error;
        if (error)
        {
            error->clear();
        }
        bool ok = false;
        do
        {
            if (!p.ParseValue(out))
            {
                break;
            }
            p.SkipWs();
            // Trailing non-whitespace is tolerated as a soft warning, not a hard fail.
            ok = true;
        } while (false);
        return ok;
    }
}
