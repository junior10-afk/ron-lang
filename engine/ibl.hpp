#pragma once

// Image-based lighting, CPU side: Radiance .hdr (RGBE) decoding and the
// precomputation of an equirectangular environment into
//   - a mip chain where level i is GGX-prefiltered for roughness i / (levels - 1)
//     (level 0 is the sharp environment, also used as the background),
//   - 9 spherical-harmonics coefficients of the diffuse irradiance.
// The engine uploads the result; mesh.frag samples it with the same mapping.

#include <cstdint>
#include <string>
#include <vector>

namespace ron::ibl {

struct FloatImage {
    int width = 0;
    int height = 0;
    std::vector<float> rgb;   // 3 floats per pixel, linear
};

bool decodeHdr(const uint8_t* data, size_t size, FloatImage& out, std::string& error);

// Loads a .hdr file, or a PNG/JPEG (converted from sRGB to linear).
bool loadEnvironmentImage(const std::string& path, FloatImage& out, std::string& error);

struct Environment {
    std::vector<FloatImage> levels;   // equirect mip chain, width = 2 * height, powers of two
    float sh[9][3] = {};              // irradiance SH (already convolved with the cosine lobe)
};

// `baseWidth` caps the size of level 0 (rounded down to a power of two).
void build(const FloatImage& source, Environment& out, int baseWidth = 2048, int maxLevels = 7);

// Direction <-> equirect UV, shared with the shaders:
//   u = atan2(d.z, d.x) / (2 pi) + 0.5,  v = acos(d.y) / pi
void directionToUv(const float* d, float& u, float& v);

// Irradiance / pi in direction n from SH coefficients (what the shader computes).
void evalIrradiance(const float sh[9][3], const float* n, float* rgb);

} // namespace ron::ibl
