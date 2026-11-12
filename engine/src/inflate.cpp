// inflate.cpp - see inflate.h: gzip and deflate, decoded the way RFC 1951 and 1952 describe them.

#include "inflate.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sessionscope {

namespace {

[[noreturn]] void damaged(const char *what) { throw std::runtime_error(std::string("Damaged compressed data: ") + what); }

// Reads the compressed data bit by bit. Deflate packs bits starting from the lowest bit of each byte.
struct Bits {
    const std::string &in;
    std::size_t at = 0;          // the next byte to read
    std::uint32_t buffer = 0;    // bits read but not used yet, lowest first
    int count = 0;               // how many

    int need(int n) {            // the next n bits as a number (n <= 24)
        while (count < n) {
            if (at >= in.size()) damaged("it ends too early");
            buffer |= static_cast<std::uint32_t>(static_cast<unsigned char>(in[at++])) << count;
            count += 8;
        }
        const int value = static_cast<int>(buffer & ((1u << n) - 1));
        buffer >>= n;
        count -= n;
        return value;
    }
    void to_byte_boundary() {    // stored blocks start at a whole byte
        buffer = 0;
        count = 0;
    }
};

// A Huffman code, given by the code length of every symbol (RFC 1951, 3.2.2): `counts[len]` is
// how many symbols have codes of that length, `symbols` lists them ordered by code.
struct Huffman {
    std::array<int, 16> counts{};
    std::vector<int> symbols;

    explicit Huffman(const std::vector<int> &lengths) {
        for (const int length : lengths) ++counts[static_cast<std::size_t>(length)];
        counts[0] = 0;
        std::array<int, 16> offsets{};
        for (std::size_t len = 1; len < 16; ++len) offsets[len] = offsets[len - 1] + counts[len - 1];
        symbols.assign(lengths.size(), 0);
        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol)
            if (lengths[symbol] != 0) symbols[static_cast<std::size_t>(offsets[static_cast<std::size_t>(lengths[symbol])]++)] = static_cast<int>(symbol);
    }

    // Reads one symbol: codes are read one bit at a time, most significant bit first, and the
    // codes of each length are consecutive numbers.
    int decode(Bits &bits) const {
        int code = 0, first = 0, index = 0;
        for (std::size_t len = 1; len < 16; ++len) {
            code |= bits.need(1);
            const int count = counts[len];
            if (code - first < count) return symbols[static_cast<std::size_t>(index + (code - first))];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        damaged("a code that doesn't exist");
    }
};

// Lengths 3..258 and distances 1..32768: a base value plus some extra bits (RFC 1951, 3.2.5).
constexpr int LENGTH_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int LENGTH_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int DISTANCE_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr int DISTANCE_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// One packed block: literal bytes, and (length, distance) pairs that copy earlier output.
void packed_block(Bits &bits, std::string &out, const Huffman &literals, const Huffman &distances) {
    for (;;) {
        const int symbol = literals.decode(bits);
        if (symbol < 256) {
            out += static_cast<char>(symbol);
        } else if (symbol == 256) {
            return;   // end of the block
        } else {
            const int l = symbol - 257;
            if (l >= 29) damaged("a length code that doesn't exist");
            const int length = LENGTH_BASE[l] + bits.need(LENGTH_EXTRA[l]);
            const int d = distances.decode(bits);
            if (d >= 30) damaged("a distance code that doesn't exist");
            const auto distance = static_cast<std::size_t>(DISTANCE_BASE[d] + bits.need(DISTANCE_EXTRA[d]));
            if (distance > out.size()) damaged("a distance before the start");
            const std::size_t from = out.size() - distance;
            for (int i = 0; i < length; ++i) out += out[from + static_cast<std::size_t>(i)];   // may overlap itself: byte by byte
        }
    }
}

const Huffman &fixed_literals() {   // the fixed code of RFC 1951, 3.2.6
    static const Huffman code([] {
        std::vector<int> lengths(288);
        for (int i = 0; i < 288; ++i) lengths[static_cast<std::size_t>(i)] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        return lengths;
    }());
    return code;
}

const Huffman &fixed_distances() {
    static const Huffman code(std::vector<int>(30, 5));
    return code;
}

// A block with its own codes: first the code lengths, themselves packed (RFC 1951, 3.2.7).
void dynamic_block(Bits &bits, std::string &out) {
    const int literal_count = bits.need(5) + 257, distance_count = bits.need(5) + 1, length_count = bits.need(4) + 4;
    if (literal_count > 286 || distance_count > 30) damaged("too many codes");
    static constexpr int ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    std::vector<int> length_lengths(19, 0);
    for (int i = 0; i < length_count; ++i) length_lengths[static_cast<std::size_t>(ORDER[i])] = bits.need(3);
    const Huffman length_code(length_lengths);

    std::vector<int> lengths;
    while (static_cast<int>(lengths.size()) < literal_count + distance_count) {
        const int symbol = length_code.decode(bits);
        if (symbol < 16) {
            lengths.push_back(symbol);
        } else {
            int repeat = 0, value = 0;
            if (symbol == 16) {   // the previous length, 3-6 times
                if (lengths.empty()) damaged("a repeat with nothing before it");
                value = lengths.back();
                repeat = 3 + bits.need(2);
            } else if (symbol == 17) {
                repeat = 3 + bits.need(3);   // zero, 3-10 times
            } else {
                repeat = 11 + bits.need(7);  // zero, 11-138 times
            }
            lengths.insert(lengths.end(), static_cast<std::size_t>(repeat), value);
        }
    }
    if (static_cast<int>(lengths.size()) != literal_count + distance_count) damaged("code lengths run over");
    if (lengths[256] == 0) damaged("no end-of-block code");
    const Huffman literals(std::vector<int>(lengths.begin(), lengths.begin() + literal_count));
    const Huffman distances(std::vector<int>(lengths.begin() + literal_count, lengths.end()));
    packed_block(bits, out, literals, distances);
}

std::uint32_t crc32(const std::string &data) {   // the checksum gzip stores (RFC 1952, 8)
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (const char byte : data) c = table[(c ^ static_cast<unsigned char>(byte)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::uint32_t le32(const std::string &bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (int b = 3; b >= 0; --b) value = (value << 8) | static_cast<unsigned char>(bytes[at + static_cast<std::size_t>(b)]);
    return value;
}

}  // namespace

std::string inflate_raw(const std::string &deflate_bytes) {
    Bits bits{deflate_bytes};
    std::string out;
    bool last = false;
    while (!last) {
        last = bits.need(1) == 1;
        const int type = bits.need(2);
        if (type == 0) {   // stored: a length, its complement, then the bytes as they are
            bits.to_byte_boundary();
            if (bits.at + 4 > deflate_bytes.size()) damaged("a stored block ends too early");
            const auto length = static_cast<std::size_t>(static_cast<unsigned char>(deflate_bytes[bits.at]) |
                                                          static_cast<unsigned char>(deflate_bytes[bits.at + 1]) << 8);
            const auto check = static_cast<std::size_t>(static_cast<unsigned char>(deflate_bytes[bits.at + 2]) |
                                                         static_cast<unsigned char>(deflate_bytes[bits.at + 3]) << 8);
            if ((length ^ 0xFFFF) != check) damaged("a stored block's length");
            bits.at += 4;
            if (bits.at + length > deflate_bytes.size()) damaged("a stored block ends too early");
            out.append(deflate_bytes, bits.at, length);
            bits.at += length;
        } else if (type == 1) {
            packed_block(bits, out, fixed_literals(), fixed_distances());
        } else if (type == 2) {
            dynamic_block(bits, out);
        } else {
            damaged("a block type that doesn't exist");
        }
    }
    return out;
}

std::string gunzip(const std::string &gzip_bytes) {
    // The header (RFC 1952, 2.3): magic 1F 8B, method 8 (deflate), flags, then optional fields.
    if (gzip_bytes.size() < 18 || static_cast<unsigned char>(gzip_bytes[0]) != 0x1F ||
        static_cast<unsigned char>(gzip_bytes[1]) != 0x8B || gzip_bytes[2] != 8)
        throw std::runtime_error("Not gzip data.");
    const int flags = static_cast<unsigned char>(gzip_bytes[3]);
    std::size_t at = 10;
    if (flags & 4) at += 2 + (static_cast<unsigned char>(gzip_bytes[at]) | static_cast<unsigned char>(gzip_bytes[at + 1]) << 8);   // extra field
    if (flags & 8) at = gzip_bytes.find('\0', at) + 1;    // file name
    if (flags & 16) at = gzip_bytes.find('\0', at) + 1;   // comment
    if (flags & 2) at += 2;                               // header checksum
    if (at == 0 || at + 8 > gzip_bytes.size()) damaged("the header");

    // The deflate data, then CRC-32 and length of the unpacked data. The decoder stops at the
    // last block by itself, so the whole rest is passed and the last 8 bytes checked.
    const std::string out = inflate_raw(gzip_bytes.substr(at, gzip_bytes.size() - at - 8));
    const std::size_t tail = gzip_bytes.size() - 8;
    if (le32(gzip_bytes, tail) != crc32(out)) damaged("the checksum doesn't match");
    if (le32(gzip_bytes, tail + 4) != static_cast<std::uint32_t>(out.size())) damaged("the length doesn't match");
    return out;
}

}  // namespace sessionscope
