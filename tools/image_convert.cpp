// Decodes an image with Ron's own PNG/JPEG decoders and writes it back as PNG.
// Usage: image_convert <input.jpg|png> <output.png>

#include "jpeg.hpp"
#include "png.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: image_convert <input> <output.png>\n");
        return 2;
    }
    ron::png::Image img;
    std::string error;
    if (!ron::loadImage(argv[1], img, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::vector<uint8_t> bytes = ron::png::encode(img);
    std::FILE* f = std::fopen(argv[2], "wb");
    if (!f) return 1;
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    std::printf("%ux%u\n", img.width, img.height);
    return 0;
}
