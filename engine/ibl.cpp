#include "ibl.hpp"

#include "jpeg.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace ron::ibl {

namespace {

constexpr float kPi = 3.14159265358979f;

// ───────────────────────────── Radiance HDR ─────────────────────────────

struct Reader {
    const uint8_t* p;
    const uint8_t* end;

    bool line(std::string& out) {
        out.clear();
        while (p < end && *p != '\n') out += static_cast<char>(*p++);
        if (p >= end) return false;
        ++p;
        return true;
    }
};

void rgbeToFloat(const uint8_t* rgbe, float* out) {
    if (rgbe[3] == 0) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    float f = std::ldexp(1.0f, static_cast<int>(rgbe[3]) - (128 + 8));
    for (int c = 0; c < 3; ++c) out[c] = (static_cast<float>(rgbe[c]) + 0.5f) * f;
}

float srgbToLinear(uint8_t v) {
    float c = static_cast<float>(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// ───────────────────────────── Sampling helpers ─────────────────────────────

void uvToDirection(float u, float v, float* d) {
    float phi = (u - 0.5f) * 2.0f * kPi;
    float theta = v * kPi;
    float s = std::sin(theta);
    d[0] = s * std::cos(phi);
    d[1] = std::cos(theta);
    d[2] = s * std::sin(phi);
}

// Bilinear lookup, wrapping horizontally and clamping vertically.
void bilinear(const FloatImage& img, float u, float v, float* out) {
    float x = u * static_cast<float>(img.width) - 0.5f;
    float y = std::clamp(v * static_cast<float>(img.height) - 0.5f, 0.0f, static_cast<float>(img.height - 1));
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(y);
    float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    int y1 = std::min(y0 + 1, img.height - 1);
    auto wrap = [&](int xi) { return ((xi % img.width) + img.width) % img.width; };
    int xa = wrap(x0), xb = wrap(x0 + 1);
    const float* p00 = &img.rgb[(static_cast<size_t>(y0) * img.width + xa) * 3];
    const float* p10 = &img.rgb[(static_cast<size_t>(y0) * img.width + xb) * 3];
    const float* p01 = &img.rgb[(static_cast<size_t>(y1) * img.width + xa) * 3];
    const float* p11 = &img.rgb[(static_cast<size_t>(y1) * img.width + xb) * 3];
    for (int c = 0; c < 3; ++c) {
        float top = p00[c] + (p10[c] - p00[c]) * fx;
        float bottom = p01[c] + (p11[c] - p01[c]) * fx;
        out[c] = top + (bottom - top) * fy;
    }
}

FloatImage downsample(const FloatImage& src) {
    FloatImage dst;
    dst.width = std::max(1, src.width / 2);
    dst.height = std::max(1, src.height / 2);
    dst.rgb.assign(static_cast<size_t>(dst.width) * dst.height * 3, 0.0f);
    for (int y = 0; y < dst.height; ++y)
        for (int x = 0; x < dst.width; ++x)
            for (int c = 0; c < 3; ++c) {
                float s = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        int sx = std::min(x * 2 + dx, src.width - 1), sy = std::min(y * 2 + dy, src.height - 1);
                        s += src.rgb[(static_cast<size_t>(sy) * src.width + sx) * 3 + c];
                    }
                dst.rgb[(static_cast<size_t>(y) * dst.width + x) * 3 + c] = s * 0.25f;
            }
    return dst;
}

// Trilinear lookup into a box-filtered pyramid.
void sampleLod(const std::vector<FloatImage>& pyramid, const float* dir, float lod, float* out) {
    float u, v;
    directionToUv(dir, u, v);
    lod = std::clamp(lod, 0.0f, static_cast<float>(pyramid.size() - 1));
    int l0 = static_cast<int>(lod);
    int l1 = std::min(l0 + 1, static_cast<int>(pyramid.size()) - 1);
    float t = lod - static_cast<float>(l0);
    float a[3], b[3];
    bilinear(pyramid[l0], u, v, a);
    if (t <= 0.0f || l1 == l0) {
        std::memcpy(out, a, sizeof(a));
        return;
    }
    bilinear(pyramid[l1], u, v, b);
    for (int c = 0; c < 3; ++c) out[c] = a[c] + (b[c] - a[c]) * t;
}

float radicalInverse(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

// GGX prefiltering with "filtered importance sampling" (GPU Gems 3, ch. 20):
// each sample reads a blurrier pyramid level when its pdf is small, which keeps
// bright spots (the sun) from turning into noise with few samples.
void prefilterRows(const std::vector<FloatImage>& pyramid, FloatImage& dst, float roughness, int y0, int y1) {
    const int samples = 64;
    float a = std::max(roughness * roughness, 1e-3f);
    float a2 = a * a;
    const FloatImage& base = pyramid[0];
    float texelSolidAngle = 4.0f * kPi / (static_cast<float>(base.width) * static_cast<float>(base.height));
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < dst.width; ++x) {
            float n[3];
            uvToDirection((static_cast<float>(x) + 0.5f) / static_cast<float>(dst.width),
                          (static_cast<float>(y) + 0.5f) / static_cast<float>(dst.height), n);
            // Tangent frame around n
            float up[3] = {0, 1, 0};
            if (std::fabs(n[1]) > 0.999f) { up[1] = 0; up[2] = 1; }
            float tx[3] = {up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2], up[0] * n[1] - up[1] * n[0]};
            float tl = std::sqrt(tx[0] * tx[0] + tx[1] * tx[1] + tx[2] * tx[2]);
            for (float& c : tx) c /= tl;
            float ty[3] = {n[1] * tx[2] - n[2] * tx[1], n[2] * tx[0] - n[0] * tx[2], n[0] * tx[1] - n[1] * tx[0]};

            float sum[3] = {0, 0, 0};
            float weight = 0;
            for (int s = 0; s < samples; ++s) {
                float xi0 = (static_cast<float>(s) + 0.5f) / samples;
                float xi1 = radicalInverse(static_cast<uint32_t>(s));
                float phi = 2.0f * kPi * xi0;
                float cosT = std::sqrt((1.0f - xi1) / (1.0f + (a2 - 1.0f) * xi1));
                float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
                float hl[3] = {sinT * std::cos(phi), sinT * std::sin(phi), cosT};
                float h[3];
                for (int c = 0; c < 3; ++c) h[c] = tx[c] * hl[0] + ty[c] * hl[1] + n[c] * hl[2];
                // V = N, so L = reflect(-N, H)
                float ndh = cosT;
                float l[3];
                for (int c = 0; c < 3; ++c) l[c] = 2.0f * ndh * h[c] - n[c];
                float ndl = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
                if (ndl <= 0.0f) continue;
                float d = ndh * ndh * (a2 - 1.0f) + 1.0f;
                float D = a2 / (kPi * d * d);
                float pdf = D * 0.25f;   // D * ndh / (4 * vdh) with v = n
                float sampleSolidAngle = 1.0f / (static_cast<float>(samples) * pdf + 1e-6f);
                float lod = 0.5f * std::log2(sampleSolidAngle / texelSolidAngle) + 1.0f;
                float c3[3];
                sampleLod(pyramid, l, lod, c3);
                for (int c = 0; c < 3; ++c) sum[c] += c3[c] * ndl;
                weight += ndl;
            }
            float* out = &dst.rgb[(static_cast<size_t>(y) * dst.width + x) * 3];
            for (int c = 0; c < 3; ++c) out[c] = weight > 0 ? sum[c] / weight : 0.0f;
        }
    }
}

void shBasis(const float* d, float* y) {
    float x = d[0], yy = d[1], z = d[2];
    y[0] = 0.282095f;
    y[1] = 0.488603f * yy;
    y[2] = 0.488603f * z;
    y[3] = 0.488603f * x;
    y[4] = 1.092548f * x * yy;
    y[5] = 1.092548f * yy * z;
    y[6] = 0.315392f * (3.0f * z * z - 1.0f);
    y[7] = 1.092548f * x * z;
    y[8] = 0.546274f * (x * x - yy * yy);
}

} // namespace

bool decodeHdr(const uint8_t* data, size_t size, FloatImage& out, std::string& error) {
    Reader r{data, data + size};
    std::string line;
    if (!r.line(line) || line.rfind("#?", 0) != 0) {
        error = "not a Radiance HDR file";
        return false;
    }
    bool rgbe = true;
    while (r.line(line) && !line.empty())
        if (line.rfind("FORMAT=", 0) == 0) rgbe = line == "FORMAT=32-bit_rle_rgbe";
    if (!rgbe) {
        error = "unsupported HDR format (only 32-bit_rle_rgbe)";
        return false;
    }
    if (!r.line(line)) {
        error = "missing HDR resolution line";
        return false;
    }
    char ya[3] = {}, xa[3] = {};
    int h = 0, w = 0;
    if (std::sscanf(line.c_str(), "%2s %d %2s %d", ya, &h, xa, &w) != 4 || w <= 0 || h <= 0 || w > 32768 ||
        h > 32768 || std::strcmp(xa, "+X") != 0 || (std::strcmp(ya, "-Y") != 0 && std::strcmp(ya, "+Y") != 0)) {
        error = "unsupported HDR orientation '" + line + "'";
        return false;
    }
    bool flip = std::strcmp(ya, "+Y") == 0;   // bottom-up
    out.width = w;
    out.height = h;
    out.rgb.assign(static_cast<size_t>(w) * h * 3, 0.0f);
    std::vector<uint8_t> row(static_cast<size_t>(w) * 4);
    for (int y = 0; y < h; ++y) {
        if (r.end - r.p < 4) {
            error = "truncated HDR data";
            return false;
        }
        if (w >= 8 && w < 32768 && r.p[0] == 2 && r.p[1] == 2 && ((r.p[2] << 8) | r.p[3]) == w) {
            // New run-length encoding: each channel separately.
            r.p += 4;
            for (int c = 0; c < 4; ++c) {
                int x = 0;
                while (x < w) {
                    if (r.p >= r.end) {
                        error = "truncated HDR scanline";
                        return false;
                    }
                    int count = *r.p++;
                    if (count > 128) {
                        count -= 128;
                        if (r.p >= r.end || x + count > w) {
                            error = "bad HDR run";
                            return false;
                        }
                        uint8_t v = *r.p++;
                        for (int k = 0; k < count; ++k) row[(x++) * 4 + c] = v;
                    } else {
                        if (count == 0 || r.end - r.p < count || x + count > w) {
                            error = "bad HDR literal run";
                            return false;
                        }
                        for (int k = 0; k < count; ++k) row[(x++) * 4 + c] = *r.p++;
                    }
                }
            }
        } else {
            // Flat RGBE pixels
            if (r.end - r.p < static_cast<ptrdiff_t>(row.size())) {
                error = "truncated HDR data";
                return false;
            }
            std::memcpy(row.data(), r.p, row.size());
            r.p += row.size();
        }
        int dy = flip ? h - 1 - y : y;
        for (int x = 0; x < w; ++x) rgbeToFloat(&row[x * 4], &out.rgb[(static_cast<size_t>(dy) * w + x) * 3]);
    }
    return true;
}

bool loadEnvironmentImage(const std::string& path, FloatImage& out, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open '" + path + "'";
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    if (bytes.size() >= 2 && bytes[0] == '#' && bytes[1] == '?') return decodeHdr(bytes.data(), bytes.size(), out, error);
    png::Image img;
    if (!decodeImage(bytes.data(), bytes.size(), img, error)) return false;
    out.width = static_cast<int>(img.width);
    out.height = static_cast<int>(img.height);
    out.rgb.resize(static_cast<size_t>(out.width) * out.height * 3);
    float lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = srgbToLinear(static_cast<uint8_t>(i));
    for (size_t i = 0; i < static_cast<size_t>(out.width) * out.height; ++i)
        for (int c = 0; c < 3; ++c) out.rgb[i * 3 + c] = lut[img.rgba[i * 4 + c]];
    return true;
}

void directionToUv(const float* d, float& u, float& v) {
    u = std::atan2(d[2], d[0]) / (2.0f * kPi) + 0.5f;
    v = std::acos(std::clamp(d[1], -1.0f, 1.0f)) / kPi;
}

void evalIrradiance(const float sh[9][3], const float* n, float* rgb) {
    float y[9];
    shBasis(n, y);
    for (int c = 0; c < 3; ++c) {
        float s = 0;
        for (int i = 0; i < 9; ++i) s += sh[i][c] * y[i];
        rgb[c] = std::max(s, 0.0f);
    }
}

void build(const FloatImage& source, Environment& out, int baseWidth, int maxLevels) {
    out.levels.clear();
    if (source.width <= 0 || source.height <= 0) return;

    // Level 0: the source resampled to a power-of-two 2:1 equirect.
    int w = 32;
    while (w * 2 <= std::min(baseWidth, std::max(source.width, 32))) w *= 2;
    FloatImage base;
    base.width = w;
    base.height = w / 2;
    base.rgb.resize(static_cast<size_t>(base.width) * base.height * 3);
    for (int y = 0; y < base.height; ++y)
        for (int x = 0; x < base.width; ++x)
            bilinear(source, (static_cast<float>(x) + 0.5f) / static_cast<float>(base.width),
                     (static_cast<float>(y) + 0.5f) / static_cast<float>(base.height),
                     &base.rgb[(static_cast<size_t>(y) * base.width + x) * 3]);

    // Box pyramid used as the prefiltering source.
    std::vector<FloatImage> pyramid;
    pyramid.push_back(base);
    while (pyramid.back().width > 8) pyramid.push_back(downsample(pyramid.back()));

    int levels = 1;
    while (levels < maxLevels && (base.width >> levels) >= 16) ++levels;
    out.levels.push_back(base);
    unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    for (int level = 1; level < levels; ++level) {
        FloatImage dst;
        dst.width = base.width >> level;
        dst.height = base.height >> level;
        dst.rgb.assign(static_cast<size_t>(dst.width) * dst.height * 3, 0.0f);
        float roughness = static_cast<float>(level) / static_cast<float>(levels - 1);
        std::vector<std::thread> pool;
        int rowsPer = (dst.height + static_cast<int>(threads) - 1) / static_cast<int>(threads);
        for (unsigned t = 0; t < threads; ++t) {
            int y0 = static_cast<int>(t) * rowsPer, y1 = std::min(dst.height, y0 + rowsPer);
            if (y0 >= y1) break;
            pool.emplace_back(prefilterRows, std::cref(pyramid), std::ref(dst), roughness, y0, y1);
        }
        for (auto& th : pool) th.join();
        out.levels.push_back(std::move(dst));
    }

    // Irradiance SH from a small pyramid level (32 x 16 or the closest).
    const FloatImage* small = &pyramid.back();
    for (const FloatImage& p : pyramid)
        if (p.width <= 64) { small = &p; break; }
    double acc[9][3] = {};
    for (int y = 0; y < small->height; ++y) {
        float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(small->height);
        float dOmega = (2.0f * kPi / static_cast<float>(small->width)) * (kPi / static_cast<float>(small->height)) *
                       std::sin(v * kPi);
        for (int x = 0; x < small->width; ++x) {
            float d[3], basis[9];
            uvToDirection((static_cast<float>(x) + 0.5f) / static_cast<float>(small->width), v, d);
            shBasis(d, basis);
            const float* c = &small->rgb[(static_cast<size_t>(y) * small->width + x) * 3];
            for (int i = 0; i < 9; ++i)
                for (int k = 0; k < 3; ++k) acc[i][k] += static_cast<double>(c[k]) * basis[i] * dOmega;
        }
    }
    // Cosine-lobe convolution (Ramamoorthi & Hanrahan), divided by pi so the
    // shader gets irradiance / pi, ready to multiply the albedo.
    const float band[9] = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
    for (int i = 0; i < 9; ++i)
        for (int k = 0; k < 3; ++k) out.sh[i][k] = static_cast<float>(acc[i][k]) * band[i];
}

} // namespace ron::ibl
