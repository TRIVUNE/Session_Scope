#ifndef SESSIONSCOPE_TEXT_FORMAT_H
#define SESSIONSCOPE_TEXT_FORMAT_H

// Text formatting that matches Python's output exactly, for character-by-character
// comparison between the C++ and Python report outputs.
//
// - fixed(value, decimals)       like f"{value:.2f}"
// - general(value)               like f"{value:g}"
// - with_commas(value)           like f"{value:,}"
// - padded_left/right(text, width) like str.ljust/rjust, width counts Unicode code points
// - display_length(utf8)         number of Unicode code points in a UTF-8 string
//
// NaN -> "nan", +inf -> "inf", -inf -> "-inf" (same as Python).

#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sessionscope {

// Formats a double with fixed-point notation and the given number of decimals, rounding the exact
// binary value correctly, as Python does. NaN -> "nan", +inf -> "inf", -inf -> "-inf".
std::string fixed(double value, int decimals);

// Formats a double with general format (like Python's f"{value:g}").
// Uses std::to_chars with std::chars_format::general.
// NaN -> "nan", +inf -> "inf", -inf -> "-inf".
std::string general(double value);

// Formats a signed integer with thousands separators (commas).
// Like Python's f"{value:,}".
std::string with_commas(long long value);

// Returns the number of Unicode code points in a UTF-8 string.
// Invalid UTF-8 sequences are counted as one code point each (replacement character).
int display_length(std::string_view utf8);

// Pads text on the right to reach the given display width (in code points).
// Like Python's str.ljust(width). If text is already >= width, returns text.
std::string padded_right(std::string_view text, int width);

// Pads text on the left to reach the given display width (in code points).
// Like Python's str.rjust(width). If text is already >= width, returns text.
std::string padded_left(std::string_view text, int width);

// Python's round() for doubles: rounds halves to even.
// Returns the nearest integer as a double (to match Python's float return type).
double py_round(double value);

// Like f"{value:+.2f}": always shows a sign, including "+nan" and "-0.00".
std::string signed_fixed(double value, int decimals);

// Like Python's round(value, decimals): decides on the exact binary value, as fixed() does, so
// 2.675 rounded to 2 decimals is 2.67. Scaling by 100 first would get such cases wrong.
double rounded_to_decimals(double value, int decimals);

// Like Python's str(float) for ordinary values: the shortest form that reads back exactly, with
// at least one decimal, such as "0.25" and "1.0".
std::string float_text(double value);

// A number from JSON in its shortest exact form, with no ".0" on whole numbers: 48000 prints as 48000.
std::string json_number_text(double value);

}  // namespace sessionscope

#endif  // SESSIONSCOPE_TEXT_FORMAT_H
