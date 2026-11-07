// JSON parser implementation. Bad input never crashes or throws; it returns nullopt.

#include "json.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace sessionscope {

namespace {

// Maximum nesting depth to avoid stack overflow. 64 levels allowed, 65 rejected.
constexpr int MAX_NESTING = 64;

// --------------------------------------------------------------------------------------------
// Unicode / UTF-8 helpers
// --------------------------------------------------------------------------------------------

// Appends a Unicode code point to a UTF-8 string.
inline void append_utf8(std::string &out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Parses a 4-hex-digit Unicode escape (\uXXXX). Advances pos past the 4 digits on success.
bool parse_unicode_escape(std::string_view text, std::size_t &pos, uint32_t &out_cp) {
    if (pos + 4 > text.size()) return false;
    uint32_t cp = 0;
    for (int i = 0; i < 4; ++i) {
        char c = text[pos + i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        cp = (cp << 4) | digit;
    }
    pos += 4;
    out_cp = cp;
    return true;
}

// --------------------------------------------------------------------------------------------
// JSON number validation (strict JSON grammar, no leading +, no leading zeros, no trailing .)
// --------------------------------------------------------------------------------------------

// Validates that text matches the JSON number grammar exactly and parses it with from_chars.
// Returns true on success, setting out_value.
bool parse_json_number(std::string_view text, double &out_value) {
    if (text.empty()) return false;

    std::size_t i = 0;
    const std::size_t n = text.size();

    // Optional minus sign
    if (text[i] == '-') {
        ++i;
        if (i == n) return false;  // just "-"
    }
    // Plus sign is NOT allowed in JSON
    else if (text[i] == '+') {
        return false;
    }

    // Integer part: either "0" or non-zero digit followed by digits
    if (i >= n) return false;
    if (text[i] == '0') {
        ++i;
    } else if (text[i] >= '1' && text[i] <= '9') {
        ++i;
        while (i < n && text[i] >= '0' && text[i] <= '9') ++i;
    } else {
        return false;  // not a digit
    }

    // Fractional part
    if (i < n && text[i] == '.') {
        ++i;
        if (i >= n) return false;  // "1." is invalid
        if (text[i] < '0' || text[i] > '9') return false;
        while (i < n && text[i] >= '0' && text[i] <= '9') ++i;
    }

    // Exponent part
    if (i < n && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i >= n) return false;
        if (text[i] == '+' || text[i] == '-') ++i;
        if (i >= n) return false;
        if (text[i] < '0' || text[i] > '9') return false;
        while (i < n && text[i] >= '0' && text[i] <= '9') ++i;
    }

    // Must consume entire string
    if (i != n) return false;

    // Parse with from_chars (fast, no locale, no exceptions)
    const char *begin = text.data();
    const char *end = begin + n;
    double value = 0;
    auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) return false;

    out_value = value;
    return true;
}

// --------------------------------------------------------------------------------------------
// Parser state
// --------------------------------------------------------------------------------------------

struct Parser {
    std::string_view text;
    std::size_t pos = 0;
    int depth = 0;

    // Skip whitespace. Returns false if end of input.
    bool skip_ws() {
        while (pos < text.size()) {
            char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos;
            } else {
                return true;
            }
        }
        return false;
    }

    // Parse a JSON value. Returns nullopt on error.
    std::optional<JsonValue> parse_value() {
        if (!skip_ws()) return std::nullopt;

        ++depth;
        if (depth > MAX_NESTING) {
            --depth;
            return std::nullopt;
        }
        auto result = parse_value_impl();
        --depth;
        return result;
    }

    std::optional<JsonValue> parse_value_impl() {
        if (pos >= text.size()) return std::nullopt;

        char c = text[pos];

        if (c == 'n') return parse_null();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == '"') return parse_string();
        if (c == '[') return parse_array();
        if (c == '{') return parse_object();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();

        return std::nullopt;
    }

    std::optional<JsonValue> parse_null() {
        if (pos + 4 <= text.size() && text.substr(pos, 4) == "null") {
            pos += 4;
            return JsonValue(nullptr);
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parse_bool() {
        if (pos + 4 <= text.size() && text.substr(pos, 4) == "true") {
            pos += 4;
            return JsonValue(true);
        }
        if (pos + 5 <= text.size() && text.substr(pos, 5) == "false") {
            pos += 5;
            return JsonValue(false);
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parse_number() {
        // Take everything that could belong to a number; parse_json_number then checks the grammar.
        const std::size_t start = pos;
        while (pos < text.size() && std::string_view("+-.eE0123456789").find(text[pos]) != std::string_view::npos) {
            ++pos;
        }

        std::string_view num_text = text.substr(start, pos - start);
        double value = 0;
        if (!parse_json_number(num_text, value)) return std::nullopt;
        return JsonValue(value);
    }

    std::optional<JsonValue> parse_string() {
        if (pos >= text.size() || text[pos] != '"') return std::nullopt;
        ++pos;  // skip opening quote

        std::string result;
        result.reserve(32);

        while (pos < text.size()) {
            char c = text[pos++];
            if (c == '"') {
                return JsonValue(std::move(result));
            }
            if (c == '\\') {
                if (pos >= text.size()) return std::nullopt;
                char esc = text[pos++];
                switch (esc) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp;
                        if (!parse_unicode_escape(text, pos, cp)) return std::nullopt;
                        // Handle surrogate pairs
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // High surrogate: expect low surrogate next
                            if (pos + 2 > text.size() || text[pos] != '\\' || text[pos + 1] != 'u') {
                                return std::nullopt;
                            }
                            pos += 2;  // skip \u
                            uint32_t low;
                            if (!parse_unicode_escape(text, pos, low)) return std::nullopt;
                            if (low < 0xDC00 || low > 0xDFFF) return std::nullopt;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            // Lone low surrogate is invalid
                            return std::nullopt;
                        }
                        append_utf8(result, cp);
                        break;
                    }
                    default: return std::nullopt;
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                // Control characters not allowed in JSON strings (unescaped)
                return std::nullopt;
            } else {
                result.push_back(c);
            }
        }
        return std::nullopt;  // unterminated string
    }

    std::optional<JsonValue> parse_array() {
        if (pos >= text.size() || text[pos] != '[') return std::nullopt;
        ++pos;  // skip '['

        std::vector<JsonValue> elements;

        if (!skip_ws()) return std::nullopt;
        if (pos < text.size() && text[pos] == ']') {
            ++pos;
            return JsonValue(std::move(elements));
        }

        while (true) {
            auto value = parse_value();
            if (!value) return std::nullopt;
            elements.push_back(std::move(*value));

            if (!skip_ws()) return std::nullopt;
            if (pos >= text.size()) return std::nullopt;

            if (text[pos] == ']') {
                ++pos;
                break;
            }
            if (text[pos] != ',') return std::nullopt;
            ++pos;  // skip ','
        }

        return JsonValue(std::move(elements));
    }

    std::optional<JsonValue> parse_object() {
        if (pos >= text.size() || text[pos] != '{') return std::nullopt;
        ++pos;  // skip '{'

        std::vector<std::pair<std::string, JsonValue>> members;

        if (!skip_ws()) return std::nullopt;
        if (pos < text.size() && text[pos] == '}') {
            ++pos;
            return JsonValue(std::move(members));
        }

        while (true) {
            if (!skip_ws()) return std::nullopt;
            if (pos >= text.size() || text[pos] != '"') return std::nullopt;

            auto key_val = parse_string();
            if (!key_val) return std::nullopt;
            std::string key = key_val->text();  // move the string out

            if (!skip_ws()) return std::nullopt;
            if (pos >= text.size() || text[pos] != ':') return std::nullopt;
            ++pos;  // skip ':'

            auto value = parse_value();
            if (!value) return std::nullopt;

            members.emplace_back(std::move(key), std::move(*value));

            if (!skip_ws()) return std::nullopt;
            if (pos >= text.size()) return std::nullopt;

            if (text[pos] == '}') {
                ++pos;
                break;
            }
            if (text[pos] != ',') return std::nullopt;
            ++pos;  // skip ','
        }

        return JsonValue(std::move(members));
    }
};

}  // namespace

std::optional<JsonValue> parse_json(std::string_view text) {
    // Skip UTF-8 BOM if present
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }

    Parser parser{text, 0, 0};
    auto result = parser.parse_value();
    if (!result) return std::nullopt;

    // Allow trailing whitespace, but nothing else
    if (!parser.skip_ws()) return result;
    return std::nullopt;  // trailing garbage
}

std::string json_escaped(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 4);
    result.push_back('"');

    for (unsigned char c : text) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c < 0x20) {
                    // Control character: \u00XX
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    result += buf;
                } else {
                    result.push_back(static_cast<char>(c));
                }
                break;
        }
    }

    result.push_back('"');
    return result;
}

namespace {

void json_write(const JsonValue &value, std::string &out, int indent, int level) {
    switch (value.type()) {
        case JsonType::Null:
            out += "null";
            break;
        case JsonType::Bool:
            out += value.boolean() ? "true" : "false";
            break;
        case JsonType::Number: {
            double n = value.number();
            if (std::isnan(n) || std::isinf(n)) {
                out += "null";
            } else {
                // Use std::to_chars for shortest representation that reads back exactly
                char buffer[64];
                auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), n);
                if (ec == std::errc()) {
                    out.append(buffer, ptr);
                } else {
                    out += "null";
                }
            }
            break;
        }
        case JsonType::String:
            out += json_escaped(value.text());
            break;
        case JsonType::Array: {
            out += '[';
            const auto &items = value.items();
            if (!items.empty()) {
                out += '\n';
                for (size_t i = 0; i < items.size(); ++i) {
                    out += std::string(indent * (level + 1), ' ');
                    json_write(items[i], out, indent, level + 1);
                    if (i + 1 < items.size()) out += ',';
                    out += '\n';
                }
                out += std::string(indent * level, ' ');
            }
            out += ']';
            break;
        }
        case JsonType::Object: {
            out += '{';
            const auto &object = value.object_members();
            if (!object.empty()) {
                out += '\n';
                for (size_t i = 0; i < object.size(); ++i) {
                    out += std::string(indent * (level + 1), ' ');
                    out += json_escaped(object[i].first);
                    out += ": ";
                    json_write(object[i].second, out, indent, level + 1);
                    if (i + 1 < object.size()) out += ',';
                    out += '\n';
                }
                out += std::string(indent * level, ' ');
            }
            out += '}';
            break;
        }
    }
}

}  // namespace

std::string json_text(const JsonValue &value, int indent) {
    std::string out;
    json_write(value, out, indent, 0);
    return out;
}

}  // namespace sessionscope
