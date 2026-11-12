// inflate.h - unpacking gzip files (an Ableton Live set, .als, is gzip-compressed XML)
//
// gzip (RFC 1952) is a small header around "deflate" data (RFC 1951): blocks that are stored
// as they are, or packed with Huffman codes (fixed ones, or ones described at the start of the
// block) and back-references to text seen earlier. This is a plain, readable decoder of that
// format, written from the two RFCs (www.rfc-editor.org/rfc/rfc1951, rfc1952), in the style of
// Mark Adler's "puff" (zlib's teaching decoder): correctness and clarity first. It reads a whole
// file into memory, which suits project files of a few megabytes.
#pragma once

#include <string>

namespace sessionscope {

// The unpacked contents of gzip data. Throws std::runtime_error if the data is not gzip or is
// damaged (a wrong length or checksum, a code that doesn't exist).
std::string gunzip(const std::string &gzip_bytes);

// Raw deflate data (no gzip header) to its unpacked contents. Throws on damaged data.
std::string inflate_raw(const std::string &deflate_bytes);

}  // namespace sessionscope
