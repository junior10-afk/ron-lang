#pragma once

// JPEG decoder written from scratch: baseline and progressive Huffman JPEGs,
// 1 or 3 components, any chroma subsampling (4:4:4, 4:2:2, 4:2:0, ...),
// restart intervals. Output is RGBA8.

#include "png.hpp"

#include <string>

namespace ron::jpeg {

bool decode(const uint8_t* data, size_t size, png::Image& out, std::string& error);

} // namespace ron::jpeg

namespace ron {
// PNG or JPEG, detected from the file signature.
bool decodeImage(const uint8_t* data, size_t size, png::Image& out, std::string& error);
bool loadImage(const std::string& path, png::Image& out, std::string& error);
} // namespace ron
