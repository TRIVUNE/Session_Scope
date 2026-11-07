#ifndef SESSIONSCOPE_JSON_H
#define SESSIONSCOPE_JSON_H

// A small reading-only JSON parser for trace meta.json files. A meta.json may be in the middle of
// being rewritten, so bad input must never crash or throw out of parse_json.
//
// Uses only the C++17 standard library. All numbers are stored as double. Object keys are kept in
// file order. Nesting deeper than 64 levels is rejected to avoid stack overflow.

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sessionscope {

enum class JsonType {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
};

class JsonValue {
public:
    JsonValue() = default;
    JsonValue(std::nullptr_t) : type_(JsonType::Null) {}
    JsonValue(bool b) : type_(JsonType::Bool), bool_(b) {}
    JsonValue(double n) : type_(JsonType::Number), number_(n) {}
    JsonValue(std::string s) : type_(JsonType::String), string_(std::move(s)) {}
    JsonValue(std::vector<JsonValue> a) : type_(JsonType::Array), array_(std::move(a)) {}
    JsonValue(std::vector<std::pair<std::string, JsonValue>> o)
        : type_(JsonType::Object), object_(std::move(o)) {}
    // Construct from vector of strings -> JSON array of strings.
    JsonValue(const std::vector<std::string> &strings) : type_(JsonType::Array) {
        array_.reserve(strings.size());
        for (const auto &s : strings) array_.emplace_back(s);
    }

    JsonType type() const { return type_; }

    bool is_null() const { return type_ == JsonType::Null; }
    bool is_bool() const { return type_ == JsonType::Bool; }
    bool is_number() const { return type_ == JsonType::Number; }
    bool is_string() const { return type_ == JsonType::String; }
    bool is_array() const { return type_ == JsonType::Array; }
    bool is_object() const { return type_ == JsonType::Object; }

    // Returns the number value, or fallback if not a number.
    double number(double fallback = std::numeric_limits<double>::quiet_NaN()) const {
        return type_ == JsonType::Number ? number_ : fallback;
    }

    // Returns the string value, or an empty string if not a string.
    const std::string &text() const {
        static const std::string empty;
        return type_ == JsonType::String ? string_ : empty;
    }

    // Returns the boolean value, or fallback if not a bool.
    bool boolean(bool fallback = false) const {
        return type_ == JsonType::Bool ? bool_ : fallback;
    }

    // Returns the array items, or an empty vector if not an array.
    const std::vector<JsonValue> &items() const {
        static const std::vector<JsonValue> empty;
        return type_ == JsonType::Array ? array_ : empty;
    }

    // Object access by key. Returns a shared null value when the key is missing or this is not an
    // object, so meta["summary"]["sensor_records"].number() is always safe.
    const JsonValue &operator[](std::string_view key) const {
        static const JsonValue null_value;
        if (type_ != JsonType::Object) return null_value;
        for (const auto &pair : object_) {
            if (pair.first == key) return pair.second;
        }
        return null_value;
    }

    // Returns all object keys in file order.
    std::vector<std::string> keys() const {
        std::vector<std::string> result;
        if (type_ == JsonType::Object) {
            result.reserve(object_.size());
            for (const auto &pair : object_) {
                result.push_back(pair.first);
            }
        }
        return result;
    }

    // Returns all object members in file order (for JSON writing).
    const std::vector<std::pair<std::string, JsonValue>> &object_members() const {
        return object_;
    }

private:
    JsonType type_ = JsonType::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> array_;
    std::vector<std::pair<std::string, JsonValue>> object_;
};

// Parses a JSON document from text. Returns nullopt on any malformed input, including trailing
// garbage (whitespace after the document is fine). A UTF-8 byte order mark at the start is skipped.
// Nesting deeper than 64 levels gives nullopt rather than a stack overflow.
// Numbers are validated against the JSON number grammar and parsed with std::from_chars, so "01",
// "1.", "+1", and ".5" are rejected.
std::optional<JsonValue> parse_json(std::string_view text);

// Escapes text for JSON output: wraps in quotes and applies JSON escapes. Control characters below
// 0x20 become \u00XX. Used by the code that writes meta.json.
std::string json_escaped(std::string_view text);

// Writes a JsonValue as JSON text with the given indentation (spaces per level). NaN and infinity
// are written as null, since JSON has no NaN. Object keys are kept in their stored order.
std::string json_text(const JsonValue &value, int indent = 2);

}  // namespace sessionscope

#endif  // SESSIONSCOPE_JSON_H
