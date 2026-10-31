// Tests for the engine's asset loaders (PNG / JPEG / HDR decoders, glTF loader,
// animation and morph sampling, image-based lighting precomputation).

#include "test.hpp"

#include "gltf.hpp"
#include "ibl.hpp"
#include "jpeg.hpp"
#include "png.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace ron;

namespace {

bool pixelIs(const png::Image& img, uint32_t x, uint32_t y, int r, int g, int b) {
    const uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
    return p[0] == r && p[1] == g && p[2] == b && p[3] == 255;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// Compares a JPEG decoded by Ron with the same file decoded by Windows (GDI+).
// IDCT and upsampling implementations differ slightly, so allow small errors.
void checkJpegAgainstReference(const char* jpg, const char* reference) {
    std::string dir = std::string(RON_SOURCE_DIR) + "/tests/data/";
    png::Image ours, ref;
    std::string error;
    bool ok = loadImage(dir + jpg, ours, error);
    if (!ok) std::printf("    %s\n", error.c_str());
    CHECK(ok);
    CHECK(png::load(dir + reference, ref, error));
    if (!ok || ours.width != ref.width || ours.height != ref.height) {
        CHECK(false);
        return;
    }
    double sum = 0;
    int worst = 0;
    for (size_t i = 0; i < ours.rgba.size(); ++i) {
        if (i % 4 == 3) continue;
        int d = std::abs(static_cast<int>(ours.rgba[i]) - static_cast<int>(ref.rgba[i]));
        sum += d;
        worst = std::max(worst, d);
    }
    double mean = sum / (static_cast<double>(ours.rgba.size()) * 0.75);
    CHECK(mean < 1.5);
    CHECK(worst <= 24);
}

std::string base64(const std::vector<uint8_t>& data) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) v |= data[i + 2];
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += i + 1 < data.size() ? table[(v >> 6) & 63] : '=';
        out += i + 2 < data.size() ? table[v & 63] : '=';
    }
    return out;
}

template <typename T>
void append(std::vector<uint8_t>& buf, std::initializer_list<T> values) {
    for (T v : values) {
        uint8_t b[sizeof(T)];
        std::memcpy(b, &v, sizeof(T));
        buf.insert(buf.end(), b, b + sizeof(T));
    }
}

gltf::Vertex vertexAt(float x, float y, float z, float u, float v) {
    gltf::Vertex vx{};
    vx.position[0] = x;
    vx.position[1] = y;
    vx.position[2] = z;
    vx.uv[0] = u;
    vx.uv[1] = v;
    return vx;
}

} // namespace

TEST(jpeg_baseline_matches_windows_decoder) { checkJpegAgainstReference("baseline.jpg", "baseline_ref.png"); }

TEST(jpeg_progressive_matches_windows_decoder) { checkJpegAgainstReference("progressive.jpg", "progressive_ref.png"); }

TEST(jpeg_rejects_truncated_file) {
    std::FILE* f = std::fopen((std::string(RON_SOURCE_DIR) + "/tests/data/baseline.jpg").c_str(), "rb");
    CHECK(f != nullptr);
    if (!f) return;
    std::vector<uint8_t> bytes(400);
    size_t n = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    png::Image img;
    std::string error;
    CHECK(!jpeg::decode(bytes.data(), n, img, error));
    CHECK(!error.empty());
}

TEST(hdr_decodes_rle_and_flat_scanlines) {
    // 8x2 image: row 0 run-length encoded, row 1 flat RGBE pixels.
    std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 8\n";
    std::vector<uint8_t> bytes(header.begin(), header.end());
    bytes.insert(bytes.end(), {2, 2, 0, 8});
    bytes.insert(bytes.end(), {128 + 8, 128});                  // R: run of 8 x 128
    bytes.insert(bytes.end(), {8, 0, 16, 32, 48, 64, 80, 96, 112});   // G: literal
    bytes.insert(bytes.end(), {128 + 8, 0});                    // B: run of zeros
    bytes.insert(bytes.end(), {128 + 8, 129});                  // E: 2^(129-136) -> values * 2 / 256
    for (int x = 0; x < 8; ++x) bytes.insert(bytes.end(), {128, 128, 128, 128});   // 0.5 (+ rounding)
    ibl::FloatImage img;
    std::string error;
    CHECK(ibl::decodeHdr(bytes.data(), bytes.size(), img, error));
    CHECK_EQ(img.width, 8);
    CHECK_EQ(img.height, 2);
    if (img.rgb.size() != 8 * 2 * 3) return;
    CHECK(std::fabs(img.rgb[0] - (128.5f / 128.0f)) < 1e-4f);     // R of pixel 0: (128 + 0.5) * 2^-7
    CHECK(std::fabs(img.rgb[3 * 3 + 1] - (48.5f / 128.0f)) < 1e-4f);
    CHECK(std::fabs(img.rgb[8 * 3] - (128.5f / 256.0f)) < 1e-4f);
    const uint8_t junk[] = {'P', '6'};
    CHECK(!ibl::decodeHdr(junk, sizeof(junk), img, error));
}

TEST(ibl_constant_environment) {
    // A uniform environment of radiance 1: irradiance / pi is 1 everywhere and
    // every prefiltered level stays 1.
    ibl::FloatImage src;
    src.width = 64;
    src.height = 32;
    src.rgb.assign(64 * 32 * 3, 1.0f);
    ibl::Environment env;
    ibl::build(src, env, 64);
    CHECK(env.levels.size() >= 2);
    CHECK_EQ(env.levels[0].width, 64);
    CHECK_EQ(env.levels[0].height, 32);
    for (const auto& level : env.levels)
        for (float v : level.rgb) CHECK(std::fabs(v - 1.0f) < 1e-3f);
    const float dirs[4][3] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {0.6f, 0.0f, 0.8f}};
    for (const auto& d : dirs) {
        float rgb[3];
        ibl::evalIrradiance(env.sh, d, rgb);
        CHECK(std::fabs(rgb[0] - 1.0f) < 0.03f);
    }
}

TEST(ibl_sky_lights_from_above) {
    // Bright upper hemisphere, black lower one: up-facing normals receive far more light.
    ibl::FloatImage src;
    src.width = 64;
    src.height = 32;
    src.rgb.assign(64 * 32 * 3, 0.0f);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 64; ++x)
            for (int c = 0; c < 3; ++c) src.rgb[(y * 64 + x) * 3 + c] = 2.0f;
    ibl::Environment env;
    ibl::build(src, env, 64);
    float up[3] = {0, 1, 0}, down[3] = {0, -1, 0}, side[3] = {1, 0, 0}, rgbUp[3], rgbDown[3], rgbSide[3];
    ibl::evalIrradiance(env.sh, up, rgbUp);
    ibl::evalIrradiance(env.sh, down, rgbDown);
    ibl::evalIrradiance(env.sh, side, rgbSide);
    CHECK(rgbUp[0] > 1.7f);              // exact value: 2
    CHECK(rgbDown[0] < 0.3f);            // exact value: 0
    CHECK(std::fabs(rgbSide[0] - 1.0f) < 0.15f);   // half the sky
}

TEST(gltf_computes_normals_and_tangents) {
    // Quad in the XZ plane facing +Y, U along +X and V along +Z.
    std::vector<gltf::Vertex> v = {vertexAt(0, 0, 0, 0, 0), vertexAt(1, 0, 0, 1, 0), vertexAt(1, 0, 1, 1, 1),
                                   vertexAt(0, 0, 1, 0, 1)};
    std::vector<uint32_t> idx = {0, 2, 1, 0, 3, 2};
    gltf::computeNormals(v, idx);
    gltf::computeTangents(v, idx);
    for (const auto& vx : v) {
        CHECK(near(vx.normal[0], 0.0f) && near(vx.normal[1], 1.0f) && near(vx.normal[2], 0.0f));
        CHECK(near(vx.tangent[0], 1.0f) && near(vx.tangent[1], 0.0f) && near(vx.tangent[2], 0.0f));
        // bitangent = cross(n, t) * w = (0,1,0) x (1,0,0) * w = (0,0,-1) * w must follow +V (+Z)
        CHECK(near(vx.tangent[3], -1.0f));
    }
}

TEST(gltf_materials_morphs_and_weight_animation) {
    // One triangle with a morph target, a fully textured alpha-masked material
    // and an animation of the morph weight.
    std::vector<uint8_t> bin;
    append<float>(bin, {0, 0, 0, 1, 0, 0, 0, 1, 0});     // 0: positions (36 bytes)
    append<float>(bin, {0, 0, 1, 0, 0, 1, 0, 0, 1});     // 36: morph position deltas
    append<float>(bin, {0, 1});                          // 72: key times
    append<float>(bin, {0, 1});                          // 80: weight values
    append<uint16_t>(bin, {0, 1, 2, 0});                 // 88: indices (+ padding)
    // 96: a 1x1 PNG
    png::Image pixel;
    pixel.width = pixel.height = 1;
    pixel.rgba = {200, 100, 50, 128};
    std::vector<uint8_t> pngBytes = png::encode(pixel);
    size_t pngOffset = bin.size();
    bin.insert(bin.end(), pngBytes.begin(), pngBytes.end());

    std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + ",\"uri\":\"data:application/octet-stream;base64," +
        base64(bin) + "\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":8},{\"buffer\":0,\"byteOffset\":80,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":88,\"byteLength\":6},"
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(pngOffset) + ",\"byteLength\":" + std::to_string(pngBytes.size()) + "}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":4,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"images\":[{\"bufferView\":5,\"mimeType\":\"image/png\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"name\":\"hair_cards\",\"alphaMode\":\"MASK\",\"alphaCutoff\":0.3,\"doubleSided\":true,"
        "\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicRoughnessTexture\":{\"index\":0},"
        "\"roughnessFactor\":0.4},"
        "\"normalTexture\":{\"index\":0,\"scale\":0.5},\"occlusionTexture\":{\"index\":0},"
        "\"emissiveTexture\":{\"index\":0},\"emissiveFactor\":[1,0.5,0]}],"
        "\"meshes\":[{\"weights\":[0.25],\"extras\":{\"targetNames\":[\"smile\"]},"
        "\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":4,\"material\":0,\"targets\":[{\"POSITION\":1}]}]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"animations\":[{\"name\":\"talk\",\"samplers\":[{\"input\":2,\"output\":3}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"weights\"}}]}]}";

    gltf::Model m;
    std::string error;
    bool ok = gltf::loadFromMemory(reinterpret_cast<const uint8_t*>(json.data()), json.size(), ".", m, error);
    if (!ok) std::printf("    %s\n", error.c_str());
    CHECK(ok);
    if (!ok) return;

    const gltf::Material& mat = m.materials[0];
    CHECK_EQ(mat.name, "hair_cards");
    CHECK(mat.alphaMode == gltf::AlphaMode::Mask);
    CHECK(near(mat.alphaCutoff, 0.3f));
    CHECK(mat.doubleSided);
    CHECK_EQ(mat.image, 0);
    CHECK_EQ(mat.metallicRoughnessImage, 0);
    CHECK_EQ(mat.normalImage, 0);
    CHECK(near(mat.normalScale, 0.5f));
    CHECK_EQ(mat.occlusionImage, 0);
    CHECK_EQ(mat.emissiveImage, 0);
    CHECK(near(mat.emissive[1], 0.5f));
    CHECK(near(mat.roughness, 0.4f));
    CHECK(near(mat.metallic, 1.0f));   // glTF default
    CHECK_EQ(m.images[0].rgba[3], 128);

    const gltf::Primitive& prim = m.meshes[0].primitives[0];
    CHECK_EQ(prim.targets.size(), 1u);
    CHECK(near(prim.targets[0].positions[2], 1.0f));
    CHECK(prim.targets[0].normals.empty());
    CHECK_EQ(m.meshes[0].targetNames.size(), 1u);
    CHECK_EQ(m.meshes[0].targetNames[0], "smile");
    // No NORMAL attribute: computed from the triangle (+Z, counter-clockwise).
    CHECK(near(prim.vertices[0].normal[2], 1.0f));

    // Not animated: the mesh default; animated: interpolated keys; blended halfway.
    CHECK(near(gltf::sampleWeights(m, 0, -1, 0, -1, 0, 0)[0], 0.25f));
    CHECK(near(gltf::sampleWeights(m, 0, 0, 0.5f, -1, 0, 0)[0], 0.5f));
    CHECK(near(gltf::sampleWeights(m, 0, 0, 0.5f, -1, 0, 0.5f)[0], 0.5f));
    CHECK(near(gltf::sampleWeights(m, 0, -1, 0, 0, 0.75f, 0.5f)[0], 0.5f));   // (0.25 + 0.75) / 2
    // The weights channel must not disturb the node's transform.
    std::vector<gltf::Mat4> local;
    gltf::samplePose(m, 0, 0.5f, -1, 0, 0, local);
    CHECK(near(local[0][0], 1.0f) && near(local[0][12], 0.0f));
}

TEST(png_decodes_compressed_file) {
    // tests/data/reference.png was written by a real PNG encoder (dynamic Huffman blocks).
    png::Image img;
    std::string error;
    CHECK(png::load(std::string(RON_SOURCE_DIR) + "/tests/data/reference.png", img, error));
    CHECK_EQ(img.width, 1280u);
    CHECK_EQ(img.height, 720u);
    CHECK(pixelIs(img, 10, 10, 170, 183, 197));
    CHECK(pixelIs(img, 640, 600, 180, 175, 166));
    CHECK(pixelIs(img, 1000, 400, 217, 216, 212));
}

TEST(png_roundtrip) {
    png::Image src;
    src.width = 300;   // > 65535 bytes of scanlines: several stored deflate blocks
    src.height = 70;
    src.rgba.resize(static_cast<size_t>(src.width) * src.height * 4);
    for (size_t i = 0; i < src.rgba.size(); ++i) src.rgba[i] = static_cast<uint8_t>(i * 31 + i / 7);
    std::vector<uint8_t> bytes = png::encode(src);
    png::Image back;
    std::string error;
    CHECK(png::decode(bytes.data(), bytes.size(), back, error));
    CHECK_EQ(back.width, 300u);
    CHECK_EQ(back.height, 70u);
    CHECK(back.rgba == src.rgba);
}

TEST(png_rejects_garbage) {
    const uint8_t junk[16] = {1, 2, 3};
    png::Image img;
    std::string error;
    CHECK(!png::decode(junk, sizeof(junk), img, error));
    CHECK(!error.empty());
}

TEST(inflate_fixed_huffman) {
    // zlib.compress(b"hello hello hello hello"): fixed Huffman codes + a back-reference
    const uint8_t z[] = {0x78, 0x9C, 0xCB, 0x48, 0xCD, 0xC9, 0xC9, 0x57, 0xC8, 0x40, 0x27, 0x01, 0x68, 0x03, 0x08, 0xB1};
    std::vector<uint8_t> out;
    CHECK(png::inflateZlib(z, sizeof(z), out));
    CHECK_EQ(std::string(out.begin(), out.end()), "hello hello hello hello");
}

TEST(gltf_loads_character) {
    gltf::Model m;
    std::string error;
    bool ok = gltf::load(std::string(RON_SOURCE_DIR) + "/assets/character.glb", m, error);
    if (!ok) std::printf("    %s\n", error.c_str());
    CHECK(ok);
    if (!ok) return;
    CHECK_EQ(m.nodes.size(), 14u);
    CHECK_EQ(m.skins.size(), 1u);
    CHECK_EQ(m.skins[0].joints.size(), 12u);
    CHECK_EQ(m.meshes.size(), 1u);
    CHECK(m.meshes[0].primitives[0].skinned);
    CHECK_EQ(m.images.size(), 1u);
    CHECK_EQ(m.images[0].width, 128u);
    CHECK_EQ(m.animations.size(), 3u);
    CHECK_EQ(m.animations[1].name, "walk");
    CHECK(near(m.animations[1].duration, 1.0f));
    CHECK_EQ(m.roots.size(), 1u);
}

TEST(gltf_pose_and_skinning) {
    gltf::Model m;
    std::string error;
    if (!gltf::load(std::string(RON_SOURCE_DIR) + "/assets/character.glb", m, error)) {
        CHECK(false);
        return;
    }
    // Bind pose: every joint matrix (global * inverse bind) is the identity.
    std::vector<gltf::Mat4> local, global;
    gltf::samplePose(m, -1, 0, -1, 0, 0, local);
    gltf::globalTransforms(m, local, global);
    const gltf::Skin& skin = m.skins[0];
    for (size_t j = 0; j < skin.joints.size(); ++j) {
        gltf::Mat4 jm = gltf::multiply(global[skin.joints[j]], skin.inverseBind[j]);
        for (int i = 0; i < 16; ++i) CHECK(near(jm[i], (i % 5 == 0) ? 1.0f : 0.0f));
    }
    // Head (node 4) sits at hips + spine + chest + head offsets.
    CHECK(near(global[4][13], 0.95f + 0.12f + 0.22f + 0.26f));

    // Walking moves the left leg forward/backward over time.
    gltf::samplePose(m, 1, 0.25f, -1, 0, 0, local);
    gltf::Mat4 q1 = local[9];
    gltf::samplePose(m, 1, 0.75f, -1, 0, 0, local);
    gltf::Mat4 q2 = local[9];
    CHECK(std::fabs(q1[6] - q2[6]) > 0.5f);   // rotation about X differs by a large swing
    // Blending halfway between walk and run stays a valid (orthonormal) rotation.
    gltf::samplePose(m, 1, 0.25f, 2, 0.15f, 0.5f, local);
    float len = std::sqrt(local[9][4] * local[9][4] + local[9][5] * local[9][5] + local[9][6] * local[9][6]);
    CHECK(near(len, 1.0f));
}
