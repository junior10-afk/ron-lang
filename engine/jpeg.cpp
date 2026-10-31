#include "jpeg.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ron::jpeg {

namespace {

// Zigzag position -> natural (row-major) index in an 8x8 block.
const uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                             12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                             35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                             58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct Huffman {
    // Canonical decoding tables (JPEG spec F.2.2.3)
    int maxCode[18] = {};
    int valPtr[17] = {};
    int minCode[17] = {};
    uint8_t values[256] = {};
    bool defined = false;
};

struct Component {
    int id = 0;
    int h = 1, v = 1;            // sampling factors
    int quant = 0;               // quantization table index
    int dcTable = 0, acTable = 0;
    int blocksW = 0, blocksH = 0;   // blocks stored (padded to whole MCUs)
    std::vector<int16_t> coefs;  // blocksW * blocksH * 64, natural order
    int dcPred = 0;
};

struct Decoder {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    std::string error;

    uint16_t quant[4][64] = {};
    Huffman dc[4], ac[4];
    std::vector<Component> comps;
    int width = 0, height = 0;
    int hmax = 1, vmax = 1;
    int mcusX = 0, mcusY = 0;
    bool progressive = false;
    int restartInterval = 0;

    // Bit reader state (entropy-coded segment)
    uint32_t bitBuffer = 0;
    int bitCount = 0;
    bool hitMarker = false;
    int eobRun = 0;

    bool fail(const char* message) {
        if (error.empty()) error = message;
        return false;
    }

    int u8() { return pos < size ? data[pos++] : 0; }
    int u16() {
        int hi = u8();
        return (hi << 8) | u8();
    }

    void resetBits() {
        bitBuffer = 0;
        bitCount = 0;
        hitMarker = false;
    }

    // Fills the bit buffer, handling 0xFF00 byte stuffing; stops at markers.
    void fill() {
        while (bitCount <= 24) {
            int b = 0;
            if (!hitMarker && pos < size) {
                b = data[pos];
                if (b == 0xFF) {
                    int next = pos + 1 < size ? data[pos + 1] : 0;
                    if (next == 0x00) {
                        pos += 2;
                    } else {
                        hitMarker = true;   // RSTn or another marker: feed zeros
                        b = 0;
                    }
                } else {
                    ++pos;
                }
            }
            bitBuffer |= static_cast<uint32_t>(b) << (24 - bitCount);
            bitCount += 8;
        }
    }

    int bits(int n) {
        if (n == 0) return 0;
        fill();
        int v = static_cast<int>(bitBuffer >> (32 - n));
        bitBuffer <<= n;
        bitCount -= n;
        return v;
    }

    int bit() { return bits(1); }

    // Value of `s` additional bits, sign-extended (JPEG F.2.2.1 EXTEND)
    int receiveExtend(int s) {
        if (s == 0) return 0;
        int v = bits(s);
        return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
    }

    int decodeHuffman(const Huffman& h) {
        int code = 0;
        for (int len = 1; len <= 16; ++len) {
            code = (code << 1) | bit();
            if (h.maxCode[len] >= 0 && code <= h.maxCode[len] && code >= h.minCode[len])
                return h.values[h.valPtr[len] + code - h.minCode[len]];
        }
        return 0;   // corrupt data: decode as zero
    }

    // ───────────── Marker segments ─────────────

    bool readDQT() {
        int len = u16() - 2;
        while (len > 0) {
            int pq = u8();
            int precision = pq >> 4, id = pq & 3;
            for (int i = 0; i < 64; ++i) quant[id][kZigzag[i]] = static_cast<uint16_t>(precision ? u16() : u8());
            len -= 1 + 64 * (precision ? 2 : 1);
        }
        return true;
    }

    bool readDHT() {
        int len = u16() - 2;
        while (len > 16) {
            int tc = u8();
            Huffman& h = (tc >> 4) ? ac[tc & 3] : dc[tc & 3];
            int counts[17] = {};
            int total = 0;
            for (int i = 1; i <= 16; ++i) total += counts[i] = u8();
            if (total > 256) return fail("bad Huffman table");
            for (int i = 0; i < total; ++i) h.values[i] = static_cast<uint8_t>(u8());
            int code = 0, k = 0;
            for (int l = 1; l <= 16; ++l) {
                h.valPtr[l] = k;
                h.minCode[l] = code;
                code += counts[l];
                k += counts[l];
                h.maxCode[l] = counts[l] ? code - 1 : -1;
                code <<= 1;
            }
            h.defined = true;
            len -= 17 + total;
        }
        return true;
    }

    bool readSOF() {
        u16();
        if (u8() != 8) return fail("only 8-bit JPEG images are supported");
        height = u16();
        width = u16();
        int n = u8();
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return fail("invalid JPEG dimensions");
        if (n != 1 && n != 3) return fail("only grayscale and YCbCr JPEG images are supported");
        comps.resize(n);
        for (Component& c : comps) {
            c.id = u8();
            int s = u8();
            c.h = std::max(1, s >> 4);
            c.v = std::max(1, s & 15);
            c.quant = u8() & 3;
            hmax = std::max(hmax, c.h);
            vmax = std::max(vmax, c.v);
        }
        mcusX = (width + 8 * hmax - 1) / (8 * hmax);
        mcusY = (height + 8 * vmax - 1) / (8 * vmax);
        for (Component& c : comps) {
            c.blocksW = mcusX * c.h;
            c.blocksH = mcusY * c.v;
            c.coefs.assign(static_cast<size_t>(c.blocksW) * c.blocksH * 64, 0);
        }
        return true;
    }

    // ───────────── Entropy-coded data ─────────────

    void decodeBlockBaseline(Component& c, int16_t* blk) {
        int t = decodeHuffman(dc[c.dcTable]);
        c.dcPred += receiveExtend(t);
        blk[0] = static_cast<int16_t>(c.dcPred);
        for (int k = 1; k < 64;) {
            int rs = decodeHuffman(ac[c.acTable]);
            int r = rs >> 4, s = rs & 15;
            if (s == 0) {
                if (r != 15) break;   // end of block
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) break;
            blk[kZigzag[k]] = static_cast<int16_t>(receiveExtend(s));
            ++k;
        }
    }

    void decodeDCFirst(Component& c, int16_t* blk, int al) {
        int t = decodeHuffman(dc[c.dcTable]);
        c.dcPred += receiveExtend(t);
        blk[0] = static_cast<int16_t>(c.dcPred * (1 << al));
    }

    void decodeDCRefine(int16_t* blk, int al) {
        if (bit()) blk[0] = static_cast<int16_t>(blk[0] | (1 << al));
    }

    void decodeACFirst(Component& c, int16_t* blk, int ss, int se, int al) {
        if (eobRun > 0) {
            --eobRun;
            return;
        }
        for (int k = ss; k <= se;) {
            int rs = decodeHuffman(ac[c.acTable]);
            int r = rs >> 4, s = rs & 15;
            if (s == 0) {
                if (r < 15) {
                    eobRun = (1 << r) - 1;
                    if (r) eobRun += bits(r);
                    break;
                }
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) break;
            blk[kZigzag[k]] = static_cast<int16_t>(receiveExtend(s) * (1 << al));
            ++k;
        }
    }

    // Successive approximation refinement of AC coefficients (JPEG G.1.2.3)
    void decodeACRefine(Component& c, int16_t* blk, int ss, int se, int al) {
        int p1 = 1 << al, m1 = -1 * (1 << al);
        int k = ss;
        if (eobRun <= 0) {
            for (; k <= se;) {
                int rs = decodeHuffman(ac[c.acTable]);
                int r = rs >> 4, s = rs & 15;
                int value = 0;
                if (s == 0) {
                    if (r < 15) {
                        eobRun = (1 << r);
                        if (r) eobRun += bits(r);
                        break;
                    }
                    // r == 15: skip 16 zero coefficients (refining nonzero ones on the way)
                } else {
                    value = bit() ? p1 : m1;
                }
                while (k <= se) {
                    int16_t& coef = blk[kZigzag[k]];
                    if (coef != 0) {
                        if (bit() && (coef & p1) == 0) coef = static_cast<int16_t>(coef >= 0 ? coef + p1 : coef + m1);
                    } else {
                        if (r == 0) {
                            if (value) coef = static_cast<int16_t>(value);
                            ++k;
                            break;
                        }
                        --r;
                    }
                    ++k;
                }
            }
        }
        if (eobRun > 0) {
            // Remaining coefficients of this band only receive correction bits.
            for (; k <= se; ++k) {
                int16_t& coef = blk[kZigzag[k]];
                if (coef != 0 && bit() && (coef & p1) == 0) coef = static_cast<int16_t>(coef >= 0 ? coef + p1 : coef + m1);
            }
            --eobRun;
        }
    }

    bool handleRestart() {
        // Expect an RSTn marker at the current position (after the bit reader stopped there).
        resetBits();
        while (pos + 1 < size && !(data[pos] == 0xFF && data[pos + 1] >= 0xD0 && data[pos + 1] <= 0xD7)) ++pos;
        if (pos + 1 < size) pos += 2;
        for (Component& c : comps) c.dcPred = 0;
        eobRun = 0;
        return true;
    }

    bool readSOS() {
        u16();
        int n = u8();
        std::vector<Component*> scan;
        for (int i = 0; i < n; ++i) {
            int id = u8(), tables = u8();
            Component* found = nullptr;
            for (Component& c : comps)
                if (c.id == id) found = &c;
            if (!found) return fail("JPEG scan references an unknown component");
            found->dcTable = tables >> 4 & 3;
            found->acTable = tables & 3;
            scan.push_back(found);
        }
        int ss = u8(), se = u8(), a = u8();
        int ah = a >> 4, al = a & 15;
        resetBits();
        eobRun = 0;
        for (Component* c : scan) c->dcPred = 0;

        auto block = [&](Component& c, int bx, int by) -> int16_t* {
            return &c.coefs[(static_cast<size_t>(by) * c.blocksW + bx) * 64];
        };
        auto decode = [&](Component& c, int16_t* blk) {
            if (!progressive) decodeBlockBaseline(c, blk);
            else if (ss == 0) ah == 0 ? decodeDCFirst(c, blk, al) : decodeDCRefine(blk, al);
            else if (ah == 0) decodeACFirst(c, blk, ss, se, al);
            else decodeACRefine(c, blk, ss, se, al);
        };

        int restartsLeft = restartInterval;
        if (scan.size() == 1) {
            // Non-interleaved: blocks of one component in raster order, covering its real size.
            Component& c = *scan[0];
            int cw = (width * c.h + hmax - 1) / hmax;
            int ch = (height * c.v + vmax - 1) / vmax;
            int bw = (cw + 7) / 8, bh = (ch + 7) / 8;
            for (int by = 0; by < bh; ++by)
                for (int bx = 0; bx < bw; ++bx) {
                    if (restartInterval && restartsLeft == 0) {
                        handleRestart();
                        restartsLeft = restartInterval;
                    }
                    decode(c, block(c, bx, by));
                    --restartsLeft;
                }
        } else {
            // Interleaved: MCU by MCU, each component contributing h x v blocks.
            for (int my = 0; my < mcusY; ++my)
                for (int mx = 0; mx < mcusX; ++mx) {
                    if (restartInterval && restartsLeft == 0) {
                        handleRestart();
                        restartsLeft = restartInterval;
                    }
                    for (Component* c : scan)
                        for (int y = 0; y < c->v; ++y)
                            for (int x = 0; x < c->h; ++x) decode(*c, block(*c, mx * c->h + x, my * c->v + y));
                    --restartsLeft;
                }
        }
        // Move to the next marker
        resetBits();
        while (pos + 1 < size && !(data[pos] == 0xFF && data[pos + 1] != 0x00 &&
                                   !(data[pos + 1] >= 0xD0 && data[pos + 1] <= 0xD7)))
            ++pos;
        return true;
    }

    // ───────────── Reconstruction ─────────────

    // 8x8 inverse DCT (separable, float) of dequantized coefficients → samples 0..255
    static void idct(const int16_t* in, const uint16_t* q, uint8_t* out, int stride) {
        static float cosTable[8][8];
        static bool ready = false;
        if (!ready) {
            for (int x = 0; x < 8; ++x)
                for (int u = 0; u < 8; ++u) {
                    float cu = u == 0 ? std::sqrt(0.5f) : 1.0f;
                    cosTable[x][u] = cu * std::cos((2 * x + 1) * u * 3.14159265f / 16.0f);
                }
            ready = true;
        }
        float tmp[64];
        float coef[64];
        for (int i = 0; i < 64; ++i) coef[i] = static_cast<float>(in[i]) * q[i];
        for (int y = 0; y < 8; ++y)        // rows: frequency u -> position x
            for (int x = 0; x < 8; ++x) {
                float s = 0;
                for (int u = 0; u < 8; ++u) s += cosTable[x][u] * coef[y * 8 + u];
                tmp[y * 8 + x] = s * 0.5f;
            }
        for (int x = 0; x < 8; ++x)        // columns
            for (int y = 0; y < 8; ++y) {
                float s = 0;
                for (int v = 0; v < 8; ++v) s += cosTable[y][v] * tmp[v * 8 + x];
                int value = static_cast<int>(std::lround(s * 0.5f + 128.0f));
                out[y * stride + x] = static_cast<uint8_t>(std::clamp(value, 0, 255));
            }
    }

    bool finish(png::Image& img) {
        // Each component to a full plane at its own resolution
        std::vector<std::vector<uint8_t>> planes(comps.size());
        for (size_t ci = 0; ci < comps.size(); ++ci) {
            Component& c = comps[ci];
            int pw = c.blocksW * 8, ph = c.blocksH * 8;
            planes[ci].assign(static_cast<size_t>(pw) * ph, 0);
            for (int by = 0; by < c.blocksH; ++by)
                for (int bx = 0; bx < c.blocksW; ++bx)
                    idct(&c.coefs[(static_cast<size_t>(by) * c.blocksW + bx) * 64], quant[c.quant],
                         &planes[ci][static_cast<size_t>(by) * 8 * pw + bx * 8], pw);
        }
        img.width = static_cast<uint32_t>(width);
        img.height = static_cast<uint32_t>(height);
        img.rgba.assign(static_cast<size_t>(width) * height * 4, 255);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                auto sample = [&](size_t ci) -> float {
                    const Component& c = comps[ci];
                    // Upsample chroma by sampling the matching position (bilinear between samples)
                    float sx = (x + 0.5f) * c.h / hmax - 0.5f;
                    float sy = (y + 0.5f) * c.v / vmax - 0.5f;
                    int pw = c.blocksW * 8, ph = c.blocksH * 8;
                    int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, pw - 1);
                    int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, ph - 1);
                    int x1 = std::min(x0 + 1, pw - 1), y1 = std::min(y0 + 1, ph - 1);
                    float fx = std::clamp(sx - x0, 0.0f, 1.0f), fy = std::clamp(sy - y0, 0.0f, 1.0f);
                    const uint8_t* p = planes[ci].data();
                    float a = p[y0 * pw + x0] + (p[y0 * pw + x1] - p[y0 * pw + x0]) * fx;
                    float b = p[y1 * pw + x0] + (p[y1 * pw + x1] - p[y1 * pw + x0]) * fx;
                    return a + (b - a) * fy;
                };
                uint8_t* d = &img.rgba[(static_cast<size_t>(y) * width + x) * 4];
                if (comps.size() == 1) {
                    d[0] = d[1] = d[2] = static_cast<uint8_t>(std::clamp(sample(0), 0.0f, 255.0f));
                } else {
                    float Y = sample(0), cb = sample(1) - 128.0f, cr = sample(2) - 128.0f;
                    d[0] = static_cast<uint8_t>(std::clamp(Y + 1.402f * cr, 0.0f, 255.0f));
                    d[1] = static_cast<uint8_t>(std::clamp(Y - 0.344136f * cb - 0.714136f * cr, 0.0f, 255.0f));
                    d[2] = static_cast<uint8_t>(std::clamp(Y + 1.772f * cb, 0.0f, 255.0f));
                }
            }
        return true;
    }

    bool run(png::Image& img) {
        if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return fail("not a JPEG file");
        pos = 2;
        bool frame = false, scanned = false;
        while (pos + 1 < size) {
            if (data[pos] != 0xFF) {
                ++pos;
                continue;
            }
            int marker = data[pos + 1];
            pos += 2;
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0xFF) {
                if (marker == 0xFF) --pos;   // fill byte
                continue;
            }
            if (marker == 0xD9) break;   // EOI
            if (pos + 2 > size || pos + ((static_cast<size_t>(data[pos]) << 8) | data[pos + 1]) > size)
                return fail("truncated JPEG segment");
            switch (marker) {
            case 0xDB: readDQT(); break;
            case 0xC4: if (!readDHT()) return false; break;
            case 0xDD: u16(); restartInterval = u16(); break;
            case 0xC0: case 0xC1: case 0xC2:
                progressive = marker == 0xC2;
                if (!readSOF()) return false;
                frame = true;
                break;
            case 0xDA:
                if (!frame) return fail("JPEG scan before frame header");
                if (!readSOS()) return false;
                scanned = true;
                break;
            default:
                if ((marker >= 0xC3 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
                    return fail("unsupported JPEG type (lossless or arithmetic coding)");
                pos += static_cast<size_t>(u16()) - 2;   // skip APPn, COM, ...
                break;
            }
        }
        if (!frame || !scanned) return fail("JPEG has no image data");
        return finish(img);
    }
};

} // namespace

bool decode(const uint8_t* data, size_t size, png::Image& out, std::string& error) {
    Decoder d{data, size};
    bool ok = d.run(out);
    if (!ok) error = d.error;
    return ok;
}

} // namespace ron::jpeg

namespace ron {

bool decodeImage(const uint8_t* data, size_t size, png::Image& out, std::string& error) {
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) return jpeg::decode(data, size, out, error);
    return png::decode(data, size, out, error);
}

bool loadImage(const std::string& path, png::Image& out, std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    return decodeImage(bytes.data(), bytes.size(), out, error);
}

} // namespace ron