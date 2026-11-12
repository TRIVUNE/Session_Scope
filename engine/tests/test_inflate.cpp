// Tests for the gzip / deflate decoder (inflate.h). The gzip samples were made with GNU gzip -n -9
// on 12 Nov 2026: a short text (one block with the fixed codes) and a longer one (a block with
// its own, "dynamic", codes). A stored block is built by hand. Damaged data must throw.

#include "inflate.h"
#include "test_support.h"

#include <stdexcept>
#include <string>

using sessionscope::gunzip;
using sessionscope::inflate_raw;

namespace {

std::string from_hex(const std::string &hex) {
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
    return out;
}

// The text the long sample was made from (the same loop, in C++).
std::string long_text() {
    std::string out;
    for (int i = 1; i <= 60; ++i)
        out += "<PluginDevice Id=\"" + std::to_string(i) + "\"><Name Value=\"Track " + std::to_string(i * 7 % 13) + "\" /></PluginDevice>\n";
    return out;
}

bool throws(const std::string &data) {
    try {
        gunzip(data);
    } catch (const std::runtime_error &) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    const std::string short_gz = from_hex("1f8b0800000000000203f348cdc9c9d751084e2d2ececccf0b4ece2f4855040056fe923314000000");
    CHECK(gunzip(short_gz) == "Hello, SessionScope!");

    const std::string long_gz = from_hex(
        "1f8b0800000000000203a5d7bb4ac4401480e1dea7087981cdb9cd05b25bd9d88885d88735c8e26a21accfafa5e014"
        "07fe3e3f0933e72333ebd3f5f676f9bcdfbf2fe77d7a783dce329fd6c7ed639f5eb6eb6d3fcecf5fdbf97daaf37438"
        "ad87bf4f9feed67fb10e63c9c5368c5b2ef661acb9388671cfc565185b2eaee3055b72751bd69e8bfbf8d5c9cdfafd"
        "c4511ec97a3c6592dc2f19cf5949d6e3414baeb93801224184482144a41223d20812e944892e88890a71a28aa0a811"
        "28ea088a0681a28540d18afe248d40d14ea0d842a0981028a6048a1982624ea0582028560814ab088a3502c53a81e2"
        "0b81e242a0b8a243971128ee048a0781e20541f14aa0784350bc1328b1202821044a28811246a084132811044a1474"
        "3da9044a3402253a8252963c941fec40f07e8c0e0000");
    CHECK(gunzip(long_gz) == long_text());

    // A stored block: final bit 1, type 0, then length 5, its complement, and the bytes.
    CHECK(inflate_raw(std::string("\x01\x05\x00\xfa\xff", 5) + "Hello") == "Hello");

    // Damaged data: not gzip, a wrong checksum, cut short.
    CHECK(throws("not gzip at all, just text"));
    std::string bad_crc = short_gz;
    bad_crc[bad_crc.size() - 8] ^= 0x01;
    CHECK(throws(bad_crc));
    CHECK(throws(long_gz.substr(0, long_gz.size() / 2)));
    return finish_tests();
}
