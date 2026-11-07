// Tests for the JSON parser.

#include "json.h"
#include "test_support.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using sessionscope::JsonValue;
using sessionscope::parse_json;
using sessionscope::json_escaped;

static void check_null(const std::optional<JsonValue> &v) {
    CHECK(v.has_value());
    CHECK(v->is_null());
}

static void check_bool(const std::optional<JsonValue> &v, bool expected) {
    CHECK(v.has_value());
    CHECK(v->is_bool());
    CHECK(v->boolean() == expected);
}

static void check_number(const std::optional<JsonValue> &v, double expected) {
    CHECK(v.has_value());
    CHECK(v->is_number());
    CHECK_NEAR(v->number(), expected, 1e-12);
}

static void check_string(const std::optional<JsonValue> &v, const std::string &expected) {
    CHECK(v.has_value());
    CHECK(v->is_string());
    CHECK(v->text() == expected);
}

static void check_array(const std::optional<JsonValue> &v, std::size_t expected_size) {
    CHECK(v.has_value());
    CHECK(v->is_array());
    CHECK(v->items().size() == expected_size);
}

static void check_object(const std::optional<JsonValue> &v, std::size_t expected_keys) {
    CHECK(v.has_value());
    CHECK(v->is_object());
    CHECK(v->keys().size() == expected_keys);
}

int main() {
    // ------------------------------------------------------------
    // Basic value types
    // ------------------------------------------------------------
    {
        check_null(parse_json("null"));
        check_bool(parse_json("true"), true);
        check_bool(parse_json("false"), false);
        check_number(parse_json("0"), 0.0);
        check_number(parse_json("-42"), -42.0);
        check_number(parse_json("3.14"), 3.14);
        check_number(parse_json("1e6"), 1e6);
        check_number(parse_json("-2.5e-3"), -2.5e-3);
        check_string(parse_json("\"hello\""), "hello");
        check_string(parse_json("\"\""), "");
        check_array(parse_json("[]"), 0);
        check_array(parse_json("[1, 2, 3]"), 3);
        check_object(parse_json("{}"), 0);
        check_object(parse_json("{\"a\": 1}"), 1);
    }

    // ------------------------------------------------------------
    // Nested access through missing keys gives null safely
    // ------------------------------------------------------------
    {
        auto v = parse_json("{\"a\": {\"b\": 5}}");
        CHECK(v.has_value());
        CHECK(v->is_object());

        // Existing path
        CHECK(v->operator[]("a").is_object());
        CHECK(v->operator[]("a").operator[]("b").is_number());
        CHECK_NEAR(v->operator[]("a").operator[]("b").number(), 5.0, 1e-12);

        // Missing key at any level returns null safely
        CHECK(v->operator[]("missing").is_null());
        CHECK(v->operator[]("a").operator[]("missing").is_null());
        CHECK(v->operator[]("missing").operator[]("also_missing").is_null());

        // Non-object returns null
        CHECK(v->operator[]("a").operator[]("b").operator[]("c").is_null());

        // Chained access like meta["summary"]["sensor_records"].number() is always safe
        auto meta = parse_json("{\"summary\": {\"sensor_records\": 42}}");
        CHECK(meta.has_value());
        CHECK_NEAR(meta->operator[]("summary").operator[]("sensor_records").number(), 42.0, 1e-12);
        CHECK_NEAR(meta->operator[]("summary").operator[]("missing").number(0.0), 0.0, 1e-12);
        CHECK_NEAR(meta->operator[]("missing").operator[]("also_missing").number(std::numeric_limits<double>::quiet_NaN()),
                   std::numeric_limits<double>::quiet_NaN(), 1e-12);
    }

    // ------------------------------------------------------------
    // String escapes
    // ------------------------------------------------------------
    {
        // Basic escapes
        check_string(parse_json("\"hello\\nworld\""), "hello\nworld");
        check_string(parse_json("\"hello\\rworld\""), "hello\rworld");
        check_string(parse_json("\"hello\\tworld\""), "hello\tworld");
        check_string(parse_json("\"quote\\\"here\""), "quote\"here");
        check_string(parse_json("\"backslash\\\\here\""), "backslash\\here");
        check_string(parse_json("\"slash\\/here\""), "slash/here");
        check_string(parse_json("\"backspace\\bhere\""), "backspace\bhere");
        check_string(parse_json("\"formfeed\\fhere\""), "formfeed\fhere");

        // Unicode escapes
        check_string(parse_json("\"\\u00E9\""), "\xC3\xA9");  // é
        check_string(parse_json("\"\\u0041\""), "A");

        // Surrogate pair (U+1F600 GRINNING FACE)
        check_string(parse_json("\"\\uD83D\\uDE00\""), "\xF0\x9F\x98\x80");

        // Mixed
        check_string(parse_json("\"line1\\nline2\\u00E9\""), "line1\nline2\xC3\xA9");
    }

    // ------------------------------------------------------------
    // Rejections
    // ------------------------------------------------------------
    {
        // Empty text
        CHECK(!parse_json("").has_value());
        CHECK(!parse_json("   ").has_value());

        // Incomplete structures
        CHECK(!parse_json("{").has_value());
        CHECK(!parse_json("[").has_value());
        CHECK(!parse_json("{\"a\":}").has_value());
        CHECK(!parse_json("[1,]").has_value());
        CHECK(!parse_json("{\"a\":1,}").has_value());

        // Invalid numbers (strict JSON grammar)
        CHECK(!parse_json("01").has_value());      // leading zero
        CHECK(!parse_json("1.").has_value());      // trailing dot
        CHECK(!parse_json("+1").has_value());      // leading plus
        CHECK(!parse_json(".5").has_value());      // leading dot
        CHECK(!parse_json("1e").has_value());      // incomplete exponent
        CHECK(!parse_json("1e+").has_value());     // incomplete exponent
        CHECK(!parse_json("-").has_value());       // just minus

        // Invalid literals
        CHECK(!parse_json("nul").has_value());
        CHECK(!parse_json("tru").has_value());
        CHECK(!parse_json("fals").has_value());
        CHECK(!parse_json("undefined").has_value());

        // Trailing garbage (whitespace is fine)
        CHECK(!parse_json("1 2").has_value());
        CHECK(!parse_json("{} extra").has_value());
        CHECK(!parse_json("[1] [2]").has_value());
        CHECK(parse_json("1 ").has_value());       // trailing whitespace OK
        CHECK(parse_json("1\n\t").has_value());    // trailing whitespace OK

        // Unterminated strings
        CHECK(!parse_json("\"hello").has_value());
        CHECK(!parse_json("\"hello\\").has_value());

        // Bad unicode escape
        CHECK(!parse_json("\"\\u\"").has_value());
        CHECK(!parse_json("\"\\u123\"").has_value());
        CHECK(!parse_json("\"\\uXXXX\"").has_value());
        CHECK(!parse_json("\"\\uD800\"").has_value());  // lone high surrogate
        CHECK(!parse_json("\"\\uDC00\"").has_value());  // lone low surrogate
        CHECK(!parse_json("\"\\uD800\\uXXXX\"").has_value());  // bad low surrogate
    }

    // ------------------------------------------------------------
    // Nesting depth limit (64 OK, 65 rejected)
    // ------------------------------------------------------------
    {
        // 64 levels of array nesting: [[[...]]] (valid JSON)
        std::string deep_arr64;
        deep_arr64.reserve(128);
        for (int i = 0; i < 64; ++i) deep_arr64 += "[";
        for (int i = 0; i < 64; ++i) deep_arr64 += "]";
        auto a64 = parse_json(deep_arr64);
        CHECK(a64.has_value());

        // 65 levels: should be rejected
        std::string deep_arr65 = "[" + deep_arr64 + "]";
        auto a65 = parse_json(deep_arr65);
        CHECK(!a65.has_value());

        // 64 levels of object nesting: {"a": {"b": ...}} (valid JSON)
        // 63 nested objects + innermost value = 64 max depth
        std::string deep_obj64;
        for (int i = 0; i < 63; ++i) deep_obj64 += "{\"a\":";
        deep_obj64 += "1";
        for (int i = 0; i < 63; ++i) deep_obj64 += "}";
        auto o64 = parse_json(deep_obj64);
        CHECK(o64.has_value());

        // 65 levels of object nesting: should be rejected (64 nested + value = 65)
        std::string deep_obj65 = "{\"a\":" + deep_obj64 + "}";
        auto o65 = parse_json(deep_obj65);
        CHECK(!o65.has_value());
    }

    // ------------------------------------------------------------
    // Byte order mark
    // ------------------------------------------------------------
    {
        std::string with_bom = "\xEF\xBB\xBF{ \"a\": 1 }";
        auto v = parse_json(with_bom);
        CHECK(v.has_value());
        CHECK(v->is_object());
        CHECK(v->keys().size() == 1);
        CHECK(v->operator[]("a").is_number());
        CHECK_NEAR(v->operator[]("a").number(), 1.0, 1e-12);
    }

    // ------------------------------------------------------------
    // json_escaped round-trips
    // ------------------------------------------------------------
    {
        std::string test_text = "hello \"world\"\nwith \\ backslash\tand \x01 control";
        std::string escaped = json_escaped(test_text);
        // The escaped string should be valid JSON and parse back to the original
        auto parsed = parse_json(escaped);
        CHECK(parsed.has_value());
        CHECK(parsed->is_string());
        CHECK(parsed->text() == test_text);

        // Also check the escaped format looks right
        CHECK(escaped.front() == '"');
        CHECK(escaped.back() == '"');
        CHECK(escaped.find("\\\"") != std::string::npos);   // escaped quote
        CHECK(escaped.find("\\\\") != std::string::npos);   // escaped backslash
        CHECK(escaped.find("\\n") != std::string::npos);    // escaped newline
        CHECK(escaped.find("\\t") != std::string::npos);    // escaped tab
        CHECK(escaped.find("\\u0001") != std::string::npos); // control char
    }

    // ------------------------------------------------------------
    // Object keys in file order
    // ------------------------------------------------------------
    {
        auto v = parse_json("{\"z\": 1, \"a\": 2, \"m\": 3}");
        CHECK(v.has_value());
        auto keys = v->keys();
        CHECK(keys.size() == 3);
        CHECK(keys[0] == "z");
        CHECK(keys[1] == "a");
        CHECK(keys[2] == "m");
    }

    // ------------------------------------------------------------
    // Array items access
    // ------------------------------------------------------------
    {
        auto v = parse_json("[10, 20, 30]");
        CHECK(v.has_value());
        auto items = v->items();
        CHECK(items.size() == 3);
        CHECK_NEAR(items[0].number(), 10.0, 1e-12);
        CHECK_NEAR(items[1].number(), 20.0, 1e-12);
        CHECK_NEAR(items[2].number(), 30.0, 1e-12);
    }

    // ------------------------------------------------------------
    // Type query methods
    // ------------------------------------------------------------
    {
        auto v_null = parse_json("null");
        CHECK(v_null->is_null());
        CHECK(!v_null->is_bool());
        CHECK(!v_null->is_number());
        CHECK(!v_null->is_string());
        CHECK(!v_null->is_array());
        CHECK(!v_null->is_object());

        auto v_bool = parse_json("true");
        CHECK(v_bool->is_bool());
        CHECK(v_bool->boolean());

        auto v_num = parse_json("42");
        CHECK(v_num->is_number());
        CHECK_NEAR(v_num->number(), 42.0, 1e-12);

        auto v_str = parse_json("\"hello\"");
        CHECK(v_str->is_string());
        CHECK(v_str->text() == "hello");

        auto v_arr = parse_json("[]");
        CHECK(v_arr->is_array());
        CHECK(v_arr->items().empty());

        auto v_obj = parse_json("{}");
        CHECK(v_obj->is_object());
        CHECK(v_obj->keys().empty());
    }

    // ------------------------------------------------------------
    // Fallback values
    // ------------------------------------------------------------
    {
        auto v = parse_json("{\"a\": 5}");
        CHECK(v.has_value());

        // number with fallback
        CHECK_NEAR(v->operator[]("a").number(), 5.0, 1e-12);
        CHECK_NEAR(v->operator[]("missing").number(42.0), 42.0, 1e-12);
        CHECK(std::isnan(v->operator[]("missing").number(std::numeric_limits<double>::quiet_NaN())));

        // boolean with fallback
        auto v2 = parse_json("{\"flag\": true}");
        CHECK(v2->operator[]("flag").boolean());                    // actual value: true
        CHECK(v2->operator[]("flag").boolean(false) == true);       // actual value: true (fallback ignored)
        CHECK(v2->operator[]("missing").boolean() == false);        // missing -> default fallback false
        CHECK(v2->operator[]("missing").boolean(false) == false);   // missing -> explicit fallback false
        CHECK(v2->operator[]("missing").boolean(true) == true);     // missing -> explicit fallback true

        // text fallback (empty string)
        CHECK(v->operator[]("a").text().empty());
        CHECK(v->operator[]("missing").text().empty());
    }

    return finish_tests();
}
