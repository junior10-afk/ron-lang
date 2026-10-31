// Tests for the engine's asset loaders (PNG decoder, glTF loader, animation sampling).

#include "test.hpp"

#include "gltf.hpp"
#include "png.hpp"

#include <cmath>
#include <string>

using namespace ron;

namespace {

bool pixelIs(const png::Image& img, uint32_t x, uint32_t y, int r, int g, int b) {
    const uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
    return p[0] == r && p[1] == g && p[2] == b && p[3] == 255;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

} // namespace

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
