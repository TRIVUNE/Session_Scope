#include "text_format.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>

namespace sessionscope {

namespace {

// Counts Unicode code points in a UTF-8 string.
// Invalid sequences are counted as one code point each.
int count_code_points(std::string_view utf8) {
    int count = 0;
    for (std::size_t i = 0; i < utf8.size();) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            // ASCII
            ++count;
            ++i;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte sequence
            ++count;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte sequence
            ++count;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte sequence
            ++count;
            i += 4;
        } else {
            // Invalid start byte, count as one and advance by 1
            ++count;
            ++i;
        }
    }
    return count;
}

}  // namespace

std::string fixed(double value, int decimals) {
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value > 0 ? "inf" : "-inf";
    }

    // Use std::to_chars with fixed format directly.
    // This matches Python's f"{value:.Nf}" formatting (round-half-even on exact decimal).
    char buffer[64];
    auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                   std::chars_format::fixed, decimals);
    if (ec == std::errc()) {
        return std::string(buffer, ptr);
    }
    // Fallback (should not happen with our buffer size)
    return std::to_string(value);
}

std::string general(double value) {
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value > 0 ? "inf" : "-inf";
    }

    // Python's f"{value:g}" uses 6 significant digits by default.
    char buffer[64];
    auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                   std::chars_format::general, 6);
    if (ec == std::errc()) {
        return std::string(buffer, ptr);
    }
    return std::to_string(value);
}

std::string with_commas(long long value) {
    bool negative = value < 0;
    unsigned long long uval = negative ? static_cast<unsigned long long>(-value)
                                       : static_cast<unsigned long long>(value);

    std::string digits = std::to_string(uval);
    std::string result;
    result.reserve(digits.size() + digits.size() / 3 + 1);

    int pos = static_cast<int>(digits.size());
    while (pos > 3) {
        pos -= 3;
        if (!result.empty()) {
            result.insert(0, ",");
        }
        result.insert(0, digits.substr(pos, 3));
    }
    if (pos > 0) {
        if (!result.empty()) {
            result.insert(0, ",");
        }
        result.insert(0, digits.substr(0, pos));
    }

    if (negative) {
        result.insert(0, "-");
    }
    return result;
}

int display_length(std::string_view utf8) {
    return count_code_points(utf8);
}

std::string padded_right(std::string_view text, int width) {
    int len = display_length(text);
    if (len >= width) {
        return std::string(text);
    }
    return std::string(text) + std::string(width - len, ' ');
}

std::string padded_left(std::string_view text, int width) {
    int len = display_length(text);
    if (len >= width) {
        return std::string(text);
    }
    return std::string(width - len, ' ') + std::string(text);
}

double py_round(double value) {
    return std::nearbyint(value);
}

std::string signed_fixed(double value, int decimals) {
    const std::string text = fixed(value, decimals);
    return std::signbit(value) ? text : "+" + text;
}

double rounded_to_decimals(double value, int decimals) {
    if (!std::isfinite(value)) return value;
    const std::string text = fixed(value, decimals);
    double rounded = value;
    std::from_chars(text.data(), text.data() + text.size(), rounded);
    return rounded;
}

std::string float_text(double value) {
    if (!std::isfinite(value)) return fixed(value, 0);
    char buffer[64];
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
    std::string text(buffer, written.ptr);
    if (text.find_first_of(".en") == std::string::npos) text += ".0";
    return text;
}

std::string json_number_text(double value) {
    if (!std::isfinite(value)) return float_text(value);
    char buffer[64];
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, written.ptr);
}

}  // namespace sessionscope
