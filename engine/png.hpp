#pragma once

// PNG decoding and (uncompressed) encoding, written from scratch: zlib inflate
// (stored, fixed and dynamic Huffman blocks), scanline unfiltering and all
// non-interlaced color types.

#include <cstdint>
#include <string>
#include <vector>

namespace ron::png {

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;   // width * height * 4 bytes
};

// zlib stream (RFC 1950 / 1951) -> raw bytes. Returns false on corrupt data.
bool inflateZlib(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

bool decode(const uint8_t* data, size_t size, Image& out, std::string& error);
bool load(const std::string& path, Image& out, std::string& error);

// Writes an RGBA PNG using stored (uncompressed) deflate blocks.
std::vector<uint8_t> encode(const Image& image);

} // namespace ron::png
