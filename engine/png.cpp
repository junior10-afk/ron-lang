#include "png.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ron::png {

namespace {

// ───────────────────────────── Inflate ─────────────────────────────

struct BitReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    uint32_t buffer = 0;
    int count = 0;
    bool error = false;

    // Reads `n` bits, least significant first (deflate bit order).
    uint32_t bits(int n) {
        while (count < n) {
            if (pos >= size) {
                error = true;
                return 0;
            }
            buffer |= static_cast<uint32_t>(data[pos++]) << count;
            count += 8;
        }
        uint32_t v = buffer & ((1u << n) - 1);
        buffer >>= n;
        count -= n;
        return v;
    }

    void alignToByte() {
        buffer = 0;
        count = 0;
    }
};

// Canonical Huffman table: number of codes per length + symbols ordered by code.
struct Huffman {
    uint16_t counts[16] = {};
    uint16_t symbols[320] = {};
};

bool build(Huffman& h, const uint8_t* lengths, int n) {
    std::memset(h.counts, 0, sizeof(h.counts));
    for (int i = 0; i < n; ++i) ++h.counts[lengths[i]];
    h.counts[0] = 0;
    uint16_t offsets[16] = {};
    for (int len = 1; len < 16; ++len) offsets[len] = offsets[len - 1] + h.counts[len - 1];
    for (int i = 0; i < n; ++i)
        if (lengths[i]) h.symbols[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
    return true;
}

// Decodes one symbol by walking code lengths (codes are read MSB-first).
int decodeSymbol(BitReader& br, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= static_cast<int>(br.bits(1));
        int count = h.counts[len];
        if (code - count < first) return h.symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (br.error) return -1;
    }
    return -1;
}

const uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflateBlock(BitReader& br, std::vector<uint8_t>& out, const Huffman& lit, const Huffman& dist) {
    while (true) {
        int sym = decodeSymbol(br, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            size_t len = kLengthBase[sym] + br.bits(kLengthExtra[sym]);
            int d = decodeSymbol(br, dist);
            if (d < 0 || d >= 30) return false;
            size_t distance = kDistBase[d] + br.bits(kDistExtra[d]);
            if (distance > out.size() || br.error) return false;
            size_t from = out.size() - distance;
            for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]);   // may overlap: byte by byte
        }
    }
}

// ───────────────────────────── Checksums ─────────────────────────────

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t adler32(const uint8_t* data, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint8_t paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    if (pb <= pc) return static_cast<uint8_t>(b);
    return static_cast<uint8_t>(c);
}

} // namespace

bool inflateZlib(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    if (size < 2 || (data[0] & 0x0F) != 8 || ((data[0] << 8) | data[1]) % 31 != 0) return false;
    BitReader br{data + 2, size - 2};
    bool last = false;
    while (!last) {
        last = br.bits(1) != 0;
        uint32_t type = br.bits(2);
        if (br.error) return false;
        if (type == 0) {
            // Stored block: LEN, NLEN, raw bytes
            br.alignToByte();
            if (br.pos + 4 > br.size) return false;
            uint16_t len = static_cast<uint16_t>(br.data[br.pos] | (br.data[br.pos + 1] << 8));
            uint16_t nlen = static_cast<uint16_t>(br.data[br.pos + 2] | (br.data[br.pos + 3] << 8));
            if (len != static_cast<uint16_t>(~nlen)) return false;
            br.pos += 4;
            if (br.pos + len > br.size) return false;
            out.insert(out.end(), br.data + br.pos, br.data + br.pos + len);
            br.pos += len;
        } else if (type == 1) {
            static Huffman fixedLit, fixedDist;
            static bool built = false;
            if (!built) {
                uint8_t lengths[288];
                for (int i = 0; i < 144; ++i) lengths[i] = 8;
                for (int i = 144; i < 256; ++i) lengths[i] = 9;
                for (int i = 256; i < 280; ++i) lengths[i] = 7;
                for (int i = 280; i < 288; ++i) lengths[i] = 8;
                build(fixedLit, lengths, 288);
                uint8_t d[30];
                std::memset(d, 5, sizeof(d));
                build(fixedDist, d, 30);
                built = true;
            }
            if (!inflateBlock(br, out, fixedLit, fixedDist)) return false;
        } else if (type == 2) {
            int hlit = static_cast<int>(br.bits(5)) + 257;
            int hdist = static_cast<int>(br.bits(5)) + 1;
            int hclen = static_cast<int>(br.bits(4)) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t clen[19] = {};
            for (int i = 0; i < hclen; ++i) clen[order[i]] = static_cast<uint8_t>(br.bits(3));
            Huffman codeLen;
            build(codeLen, clen, 19);
            uint8_t lengths[320] = {};
            int n = 0;
            while (n < hlit + hdist) {
                int sym = decodeSymbol(br, codeLen);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[n++] = static_cast<uint8_t>(sym);
                } else {
                    int repeat = 0;
                    uint8_t value = 0;
                    if (sym == 16) {
                        if (n == 0) return false;
                        value = lengths[n - 1];
                        repeat = 3 + static_cast<int>(br.bits(2));
                    } else if (sym == 17) {
                        repeat = 3 + static_cast<int>(br.bits(3));
                    } else {
                        repeat = 11 + static_cast<int>(br.bits(7));
                    }
                    if (n + repeat > hlit + hdist) return false;
                    while (repeat--) lengths[n++] = value;
                }
            }
            Huffman lit, dist;
            build(lit, lengths, hlit);
            build(dist, lengths + hlit, hdist);
            if (!inflateBlock(br, out, lit, dist)) return false;
        } else {
            return false;
        }
        if (br.error) return false;
    }
    return true;
}

bool decode(const uint8_t* data, size_t size, Image& out, std::string& error) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (size < 8 || std::memcmp(data, sig, 8) != 0) {
        error = "not a PNG file";
        return false;
    }
    uint32_t width = 0, height = 0;
    int depth = 0, colorType = 0, interlace = 0;
    std::vector<uint8_t> idat, palette, trns;
    size_t pos = 8;
    while (pos + 8 <= size) {
        uint32_t len = be32(data + pos);
        const uint8_t* type = data + pos + 4;
        const uint8_t* body = data + pos + 8;
        if (pos + 12 + len > size) break;
        if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            width = be32(body);
            height = be32(body + 4);
            depth = body[8];
            colorType = body[9];
            interlace = body[12];
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            palette.assign(body, body + len);
        } else if (std::memcmp(type, "tRNS", 4) == 0) {
            trns.assign(body, body + len);
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), body, body + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + len;
    }
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        error = "invalid PNG dimensions";
        return false;
    }
    if (interlace != 0) {
        error = "interlaced PNG files are not supported";
        return false;
    }
    int channels = colorType == 0 ? 1 : colorType == 2 ? 3 : colorType == 3 ? 1 : colorType == 4 ? 2 : colorType == 6 ? 4 : 0;
    if (channels == 0 || !(depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) {
        error = "unsupported PNG color type or bit depth";
        return false;
    }

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(width) * height * channels * (depth / 8 + 1) + height);
    if (!inflateZlib(idat.data(), idat.size(), raw)) {
        error = "corrupt PNG image data";
        return false;
    }

    size_t bitsPerPixel = static_cast<size_t>(channels) * depth;
    size_t stride = (width * bitsPerPixel + 7) / 8;
    size_t bpp = std::max<size_t>(1, bitsPerPixel / 8);
    if (raw.size() < height * (stride + 1)) {
        error = "truncated PNG image data";
        return false;
    }

    // Undo the per-scanline filters in place.
    std::vector<uint8_t> pixels(height * stride);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t filter = raw[y * (stride + 1)];
        const uint8_t* src = &raw[y * (stride + 1) + 1];
        uint8_t* row = &pixels[y * stride];
        const uint8_t* prev = y ? &pixels[(y - 1) * stride] : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            int a = x >= bpp ? row[x - bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= bpp) ? prev[x - bpp] : 0;
            int v = src[x];
            switch (filter) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: v += paeth(a, b, c); break;
            default: break;
            }
            row[x] = static_cast<uint8_t>(v);
        }
    }

    // Expand to RGBA8.
    out.width = width;
    out.height = height;
    out.rgba.assign(static_cast<size_t>(width) * height * 4, 255);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = &pixels[y * stride];
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* d = &out.rgba[(static_cast<size_t>(y) * width + x) * 4];
            auto sample = [&](int channel) -> uint8_t {
                if (depth == 8) return row[x * channels + channel];
                if (depth == 16) return row[(x * channels + channel) * 2];   // high byte
                // 1/2/4-bit (gray or palette, single channel)
                size_t bit = static_cast<size_t>(x) * depth;
                int shift = 8 - depth - static_cast<int>(bit % 8);
                return static_cast<uint8_t>((row[bit / 8] >> shift) & ((1 << depth) - 1));
            };
            switch (colorType) {
            case 0: {
                uint8_t g = sample(0);
                if (depth < 8) g = static_cast<uint8_t>(g * 255 / ((1 << depth) - 1));
                d[0] = d[1] = d[2] = g;
                break;
            }
            case 2:
                d[0] = sample(0);
                d[1] = sample(1);
                d[2] = sample(2);
                break;
            case 3: {
                uint8_t i = sample(0);
                if (static_cast<size_t>(i) * 3 + 2 < palette.size()) {
                    d[0] = palette[i * 3];
                    d[1] = palette[i * 3 + 1];
                    d[2] = palette[i * 3 + 2];
                }
                if (i < trns.size()) d[3] = trns[i];
                break;
            }
            case 4:
                d[0] = d[1] = d[2] = sample(0);
                d[3] = sample(1);
                break;
            default:
                d[0] = sample(0);
                d[1] = sample(1);
                d[2] = sample(2);
                d[3] = sample(3);
                break;
            }
        }
    }
    return true;
}

bool load(const std::string& path, Image& out, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    return decode(bytes.data(), bytes.size(), out, error);
}

std::vector<uint8_t> encode(const Image& image) {
    std::vector<uint8_t> raw;
    size_t stride = static_cast<size_t>(image.width) * 4;
    for (uint32_t y = 0; y < image.height; ++y) {
        raw.push_back(0);   // filter: none
        raw.insert(raw.end(), image.rgba.begin() + y * stride, image.rgba.begin() + (y + 1) * stride);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t off = 0; off < raw.size() || off == 0; off += 65535) {
        size_t len = std::min<size_t>(65535, raw.size() - off);
        bool last = off + len >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(len));
        z.push_back(static_cast<uint8_t>(len >> 8));
        z.push_back(static_cast<uint8_t>(~len));
        z.push_back(static_cast<uint8_t>(~len >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + len);
        if (last) break;
    }
    uint32_t ad = adler32(raw.data(), raw.size());
    for (int i = 3; i >= 0; --i) z.push_back(static_cast<uint8_t>(ad >> (8 * i)));

    std::vector<uint8_t> out = {137, 80, 78, 71, 13, 10, 26, 10};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& body) {
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(body.size() >> (8 * i)));
        size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), body.begin(), body.end());
        uint32_t crc = crc32(&out[start], out.size() - start);
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(crc >> (8 * i)));
    };
    std::vector<uint8_t> ihdr(13, 0);
    for (int i = 0; i < 4; ++i) {
        ihdr[i] = static_cast<uint8_t>(image.width >> (24 - 8 * i));
        ihdr[4 + i] = static_cast<uint8_t>(image.height >> (24 - 8 * i));
    }
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // RGBA
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return out;
}

} // namespace ron::png
