// Generates assets/character.glb: a rigged, textured humanoid with "idle",
// "walk" and "run" animations, written as a standard glTF 2.0 binary file.
// It exercises the same path as a model exported from Blender: skin, inverse
// bind matrices, node hierarchy, embedded PNG texture and keyframe channels.
//
// Usage: make_character <output.glb>

#include "png.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace png = ron::png;

namespace {

struct Bone {
    const char* name;
    int parent;
    float offset[3];   // translation relative to the parent
};

// Skeleton (node index = joint index + 1; node 0 is the scene root)
const Bone kBones[] = {
    {"hips", -1, {0.0f, 0.95f, 0.0f}},
    {"spine", 0, {0.0f, 0.12f, 0.0f}},
    {"chest", 1, {0.0f, 0.22f, 0.0f}},
    {"head", 2, {0.0f, 0.26f, 0.0f}},
    {"upper_arm.L", 2, {0.24f, 0.17f, 0.0f}},
    {"lower_arm.L", 4, {0.0f, -0.28f, 0.0f}},
    {"upper_arm.R", 2, {-0.24f, 0.17f, 0.0f}},
    {"lower_arm.R", 6, {0.0f, -0.28f, 0.0f}},
    {"upper_leg.L", 0, {0.11f, -0.05f, 0.0f}},
    {"lower_leg.L", 8, {0.0f, -0.43f, 0.0f}},
    {"upper_leg.R", 0, {-0.11f, -0.05f, 0.0f}},
    {"lower_leg.R", 10, {0.0f, -0.43f, 0.0f}},
};
constexpr int kJoints = sizeof(kBones) / sizeof(kBones[0]);

enum Region { Skin, Shirt, Pants, Shoes, Face, Belt };

struct Vertex {
    float p[3], n[3], uv[2];
    uint8_t j[4];
    float w[4];
};

std::vector<Vertex> vertices;
std::vector<uint32_t> indices;
float bindPos[kJoints][3];

// Texture atlas: 4x2 regions of 32x32 pixels.
void regionUv(Region r, float& u0, float& v0, float& u1, float& v1) {
    int x = r % 4, y = r / 4;
    u0 = (x * 32 + 1) / 128.0f;
    v0 = (y * 32 + 1) / 64.0f;
    u1 = (x * 32 + 31) / 128.0f;
    v1 = (y * 32 + 31) / 64.0f;
}

// Adds a box (in bind-pose model space) rigidly bound to `joint`. The front face
// (+Z) can use a different region (for the face).
void addBox(float cx, float cy, float cz, float sx, float sy, float sz, int joint, Region region, Region front) {
    static const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; ++f) {
        float nx = n[f][0], ny = n[f][1], nz = n[f][2];
        float ux = ny, uy = nz, uz = nx;
        float vx = ny * uz - nz * uy, vy = nz * ux - nx * uz, vz = nx * uy - ny * ux;
        if (f == 4) {   // keep the face texture upright: u = +X, v = -Y
            ux = 1; uy = 0; uz = 0;
            vx = 0; vy = -1; vz = 0;
        }
        float u0, v0, u1, v1;
        regionUv(f == 4 ? front : region, u0, v0, u1, v1);
        auto base = static_cast<uint32_t>(vertices.size());
        const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (auto& c : corners) {
            Vertex v{};
            v.p[0] = cx + 0.5f * sx * (nx + c[0] * ux + c[1] * vx);
            v.p[1] = cy + 0.5f * sy * (ny + c[0] * uy + c[1] * vy);
            v.p[2] = cz + 0.5f * sz * (nz + c[0] * uz + c[1] * vz);
            v.n[0] = nx; v.n[1] = ny; v.n[2] = nz;
            v.uv[0] = u0 + (u1 - u0) * (c[0] + 1) * 0.5f;
            v.uv[1] = v0 + (v1 - v0) * (c[1] + 1) * 0.5f;
            v.j[0] = static_cast<uint8_t>(joint);
            v.w[0] = 1.0f;
            vertices.push_back(v);
        }
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

void buildMesh() {
    for (int i = 0; i < kJoints; ++i) {
        const Bone& b = kBones[i];
        for (int c = 0; c < 3; ++c) bindPos[i][c] = b.offset[c] + (b.parent >= 0 ? bindPos[b.parent][c] : 0.0f);
    }
    auto at = [](int j, int c) { return bindPos[j][c]; };
    addBox(at(0, 0), at(0, 1) + 0.02f, 0, 0.34f, 0.16f, 0.2f, 0, Belt, Belt);               // hips
    addBox(at(1, 0), at(1, 1) + 0.1f, 0, 0.36f, 0.22f, 0.21f, 1, Shirt, Shirt);             // belly
    addBox(at(2, 0), at(2, 1) + 0.08f, 0, 0.42f, 0.24f, 0.24f, 2, Shirt, Shirt);            // chest
    addBox(at(3, 0), at(3, 1) + 0.13f, 0, 0.24f, 0.26f, 0.24f, 3, Skin, Face);              // head
    for (int side = 0; side < 2; ++side) {
        int ua = side == 0 ? 4 : 6, la = ua + 1, ul = side == 0 ? 8 : 10, ll = ul + 1;
        addBox(at(ua, 0), at(ua, 1) - 0.13f, 0, 0.11f, 0.3f, 0.11f, ua, Shirt, Shirt);      // upper arm
        addBox(at(la, 0), at(la, 1) - 0.12f, 0, 0.1f, 0.26f, 0.1f, la, Skin, Skin);         // forearm
        addBox(at(ul, 0), at(ul, 1) - 0.21f, 0, 0.15f, 0.44f, 0.15f, ul, Pants, Pants);     // thigh
        addBox(at(ll, 0), at(ll, 1) - 0.2f, 0, 0.13f, 0.4f, 0.13f, ll, Pants, Pants);       // shin
        addBox(at(ll, 0), at(ll, 1) - 0.42f, 0.04f, 0.14f, 0.08f, 0.24f, ll, Shoes, Shoes); // shoe
    }
}

png::Image buildTexture() {
    png::Image img;
    img.width = 128;
    img.height = 64;
    img.rgba.resize(128 * 64 * 4);
    const uint8_t colors[6][3] = {{232, 190, 160}, {40, 110, 200}, {50, 50, 70}, {30, 25, 25}, {232, 190, 160}, {120, 80, 40}};
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 128; ++x) {
            int region = static_cast<int>((y / 32) * 4 + x / 32);
            const uint8_t* c = colors[region < 6 ? region : 0];
            int lx = static_cast<int>(x % 32), ly = static_cast<int>(y % 32);
            // Subtle fabric noise
            int noise = static_cast<int>((x * 7 + y * 13) % 9) - 4;
            uint8_t r = static_cast<uint8_t>(std::clamp(c[0] + noise, 0, 255));
            uint8_t gch = static_cast<uint8_t>(std::clamp(c[1] + noise, 0, 255));
            uint8_t b = static_cast<uint8_t>(std::clamp(c[2] + noise, 0, 255));
            if (region == Face) {
                bool eye = (ly >= 10 && ly <= 14) && ((lx >= 8 && lx <= 12) || (lx >= 19 && lx <= 23));
                bool mouth = ly >= 21 && ly <= 22 && lx >= 11 && lx <= 20;
                if (eye || mouth) r = gch = b = 30;
            }
            if (region == Shirt && ly % 8 == 0) { r = static_cast<uint8_t>(r * 0.8); gch = static_cast<uint8_t>(gch * 0.8); b = static_cast<uint8_t>(b * 0.8); }
            uint8_t* p = &img.rgba[(y * 128 + x) * 4];
            p[0] = r; p[1] = gch; p[2] = b; p[3] = 255;
        }
    return img;
}

// ───────────── Animations ─────────────

struct Track {
    int joint;
    bool rotation;           // rotation (quaternion) or translation
    std::vector<float> values;
};

struct Clip {
    const char* name;
    float duration;
    int keys;
    std::vector<Track> tracks;
};

void quatX(float angle, float* q) {
    q[0] = std::sin(angle * 0.5f);
    q[1] = 0;
    q[2] = 0;
    q[3] = std::cos(angle * 0.5f);
}

void quatXY(float ax, float ay, float* q) {
    // rotation about Y then X: q = qy * qx
    float sx = std::sin(ax * 0.5f), cx = std::cos(ax * 0.5f), sy = std::sin(ay * 0.5f), cy = std::cos(ay * 0.5f);
    q[0] = cy * sx;
    q[1] = sy * cx;
    q[2] = -sy * sx;
    q[3] = cy * cx;
}

Clip makeLocomotion(const char* name, float duration, float legSwing, float armSwing, float knee, float lean, float bob) {
    Clip clip{name, duration, 17, {}};
    const float tau = 6.2831853f;
    auto rot = [&](int joint, auto angleAt) {
        Track t{joint, true, {}};
        for (int k = 0; k < clip.keys; ++k) {
            float phase = static_cast<float>(k) / (clip.keys - 1) * tau;
            float q[4];
            quatX(angleAt(phase), q);
            t.values.insert(t.values.end(), q, q + 4);
        }
        clip.tracks.push_back(t);
    };
    rot(8, [&](float p) { return std::sin(p) * legSwing; });                                   // upper_leg.L
    rot(10, [&](float p) { return -std::sin(p) * legSwing; });                                 // upper_leg.R
    rot(9, [&](float p) { return -std::max(0.0f, -std::cos(p)) * knee - 0.05f; });             // lower_leg.L
    rot(11, [&](float p) { return -std::max(0.0f, std::cos(p)) * knee - 0.05f; });             // lower_leg.R
    rot(4, [&](float p) { return -std::sin(p) * armSwing; });                                  // upper_arm.L
    rot(6, [&](float p) { return std::sin(p) * armSwing; });                                   // upper_arm.R
    rot(5, [&](float p) { return -0.3f - 0.3f * armSwing * (1 + std::sin(p)); });              // lower_arm.L
    rot(7, [&](float p) { return -0.3f - 0.3f * armSwing * (1 - std::sin(p)); });              // lower_arm.R
    rot(1, [&](float) { return lean; });                                                        // spine lean
    // Hips bob twice per cycle
    Track hips{0, false, {}};
    for (int k = 0; k < clip.keys; ++k) {
        float phase = static_cast<float>(k) / (clip.keys - 1) * tau;
        hips.values.insert(hips.values.end(), {0.0f, kBones[0].offset[1] - bob * std::fabs(std::cos(phase)), 0.0f});
    }
    clip.tracks.push_back(hips);
    return clip;
}

Clip makeIdle() {
    Clip clip{"idle", 3.0f, 13, {}};
    const float tau = 6.2831853f;
    auto add = [&](int joint, auto fn) {
        Track t{joint, true, {}};
        for (int k = 0; k < clip.keys; ++k) {
            float phase = static_cast<float>(k) / (clip.keys - 1) * tau;
            float q[4];
            fn(phase, q);
            t.values.insert(t.values.end(), q, q + 4);
        }
        clip.tracks.push_back(t);
    };
    add(1, [](float p, float* q) { quatX(0.03f * std::sin(p), q); });                           // breathing
    add(3, [](float p, float* q) { quatXY(0.05f * std::sin(p * 2), 0.35f * std::sin(p), q); }); // look around
    add(4, [](float p, float* q) { quatXY(0.06f * std::sin(p), 0, q); });
    add(6, [](float p, float* q) { quatXY(-0.06f * std::sin(p), 0, q); });
    add(5, [](float, float* q) { quatX(-0.15f, q); });
    add(7, [](float, float* q) { quatX(-0.15f, q); });
    return clip;
}

// ───────────── GLB writer ─────────────

struct Bin {
    std::vector<uint8_t> data;
    std::string views;
    std::string accessors;
    int viewCount = 0;
    int accessorCount = 0;

    int view(const void* bytes, size_t size) {
        while (data.size() % 4) data.push_back(0);
        size_t off = data.size();
        data.insert(data.end(), static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + size);
        if (viewCount) views += ",";
        views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) + ",\"byteLength\":" + std::to_string(size) + "}";
        return viewCount++;
    }

    int accessor(int viewIndex, int componentType, size_t count, const char* type, const std::string& extra = "") {
        if (accessorCount) accessors += ",";
        accessors += "{\"bufferView\":" + std::to_string(viewIndex) + ",\"componentType\":" + std::to_string(componentType) +
                     ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"" + extra + "}";
        return accessorCount++;
    }
};

std::string floats(const float* v, int n) {
    std::string s = "[";
    for (int i = 0; i < n; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%.6g", i ? "," : "", v[i]);
        s += buf;
    }
    return s + "]";
}

} // namespace

int main(int argc, char** argv) {
    const char* outPath = argc > 1 ? argv[1] : "character.glb";
    buildMesh();
    Bin bin;

    // Vertex attributes, split into separate arrays
    size_t n = vertices.size();
    std::vector<float> pos(n * 3), nrm(n * 3), uv(n * 2), wts(n * 4);
    std::vector<uint8_t> jts(n * 4);
    float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            pos[i * 3 + c] = vertices[i].p[c];
            nrm[i * 3 + c] = vertices[i].n[c];
            mn[c] = std::min(mn[c], vertices[i].p[c]);
            mx[c] = std::max(mx[c], vertices[i].p[c]);
        }
        uv[i * 2] = vertices[i].uv[0];
        uv[i * 2 + 1] = vertices[i].uv[1];
        for (int c = 0; c < 4; ++c) {
            jts[i * 4 + c] = vertices[i].j[c];
            wts[i * 4 + c] = vertices[i].w[c];
        }
    }
    int aPos = bin.accessor(bin.view(pos.data(), pos.size() * 4), 5126, n, "VEC3",
                            ",\"min\":" + floats(mn, 3) + ",\"max\":" + floats(mx, 3));
    int aNrm = bin.accessor(bin.view(nrm.data(), nrm.size() * 4), 5126, n, "VEC3");
    int aUv = bin.accessor(bin.view(uv.data(), uv.size() * 4), 5126, n, "VEC2");
    int aJts = bin.accessor(bin.view(jts.data(), jts.size()), 5121, n, "VEC4");
    int aWts = bin.accessor(bin.view(wts.data(), wts.size() * 4), 5126, n, "VEC4");
    int aIdx = bin.accessor(bin.view(indices.data(), indices.size() * 4), 5125, indices.size(), "SCALAR");

    // Inverse bind matrices: the bind pose has no rotation, so each one is a translation.
    std::vector<float> ibm(kJoints * 16, 0.0f);
    for (int j = 0; j < kJoints; ++j) {
        float* m = &ibm[j * 16];
        m[0] = m[5] = m[10] = m[15] = 1.0f;
        m[12] = -bindPos[j][0];
        m[13] = -bindPos[j][1];
        m[14] = -bindPos[j][2];
    }
    int aIbm = bin.accessor(bin.view(ibm.data(), ibm.size() * 4), 5126, kJoints, "MAT4");

    // Texture
    std::vector<uint8_t> pngBytes = png::encode(buildTexture());
    int imageView = bin.view(pngBytes.data(), pngBytes.size());

    // Animations
    std::vector<Clip> clips = {makeIdle(), makeLocomotion("walk", 1.0f, 0.55f, 0.45f, 0.6f, 0.05f, 0.03f),
                               makeLocomotion("run", 0.62f, 0.95f, 0.9f, 1.3f, 0.25f, 0.06f)};
    std::string animations;
    for (size_t c = 0; c < clips.size(); ++c) {
        const Clip& clip = clips[c];
        std::vector<float> times(clip.keys);
        for (int k = 0; k < clip.keys; ++k) times[k] = clip.duration * k / (clip.keys - 1);
        float tmin = 0, tmax = clip.duration;
        int aTime = bin.accessor(bin.view(times.data(), times.size() * 4), 5126, times.size(), "SCALAR",
                                 ",\"min\":[" + std::to_string(tmin) + "],\"max\":[" + std::to_string(tmax) + "]");
        std::string samplers, channels;
        for (size_t t = 0; t < clip.tracks.size(); ++t) {
            const Track& tr = clip.tracks[t];
            int aOut = bin.accessor(bin.view(tr.values.data(), tr.values.size() * 4), 5126, clip.keys,
                                    tr.rotation ? "VEC4" : "VEC3");
            if (t) {
                samplers += ",";
                channels += ",";
            }
            samplers += "{\"input\":" + std::to_string(aTime) + ",\"output\":" + std::to_string(aOut) +
                        ",\"interpolation\":\"LINEAR\"}";
            channels += "{\"sampler\":" + std::to_string(t) + ",\"target\":{\"node\":" + std::to_string(tr.joint + 1) +
                        ",\"path\":\"" + (tr.rotation ? "rotation" : "translation") + "\"}}";
        }
        if (c) animations += ",";
        animations += "{\"name\":\"" + std::string(clip.name) + "\",\"samplers\":[" + samplers + "],\"channels\":[" +
                      channels + "]}";
    }

    // Nodes: 0 = root, 1..kJoints = bones, kJoints+1 = skinned mesh
    std::string nodes = "{\"name\":\"character\",\"children\":[1," + std::to_string(kJoints + 1) + "]}";
    for (int j = 0; j < kJoints; ++j) {
        std::string children;
        for (int k = 0; k < kJoints; ++k)
            if (kBones[k].parent == j) children += (children.empty() ? "" : ",") + std::to_string(k + 1);
        nodes += ",{\"name\":\"" + std::string(kBones[j].name) + "\",\"translation\":" + floats(kBones[j].offset, 3);
        if (!children.empty()) nodes += ",\"children\":[" + children + "]";
        nodes += "}";
    }
    nodes += ",{\"name\":\"body\",\"mesh\":0,\"skin\":0}";
    std::string jointList;
    for (int j = 0; j < kJoints; ++j) jointList += (j ? "," : "") + std::to_string(j + 1);

    while (bin.data.size() % 4) bin.data.push_back(0);
    std::string json =
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Ron make_character\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[" + nodes + "],"
        "\"meshes\":[{\"name\":\"body\",\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(aPos) +
        ",\"NORMAL\":" + std::to_string(aNrm) + ",\"TEXCOORD_0\":" + std::to_string(aUv) + ",\"JOINTS_0\":" +
        std::to_string(aJts) + ",\"WEIGHTS_0\":" + std::to_string(aWts) + "},\"indices\":" + std::to_string(aIdx) +
        ",\"material\":0}]}],"
        "\"skins\":[{\"joints\":[" + jointList + "],\"inverseBindMatrices\":" + std::to_string(aIbm) +
        ",\"skeleton\":1}],"
        "\"materials\":[{\"name\":\"body\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},"
        "\"metallicFactor\":0.0,\"roughnessFactor\":0.75}}],"
        "\"textures\":[{\"source\":0}],\"images\":[{\"bufferView\":" + std::to_string(imageView) +
        ",\"mimeType\":\"image/png\"}],"
        "\"animations\":[" + animations + "],"
        "\"accessors\":[" + bin.accessors + "],\"bufferViews\":[" + bin.views + "],"
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.data.size()) + "}]}";
    while (json.size() % 4) json += ' ';

    std::vector<uint8_t> glb;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) glb.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    uint32_t total = static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin.data.size());
    u32(0x46546C67);   // "glTF"
    u32(2);
    u32(total);
    u32(static_cast<uint32_t>(json.size()));
    u32(0x4E4F534A);   // JSON
    glb.insert(glb.end(), json.begin(), json.end());
    u32(static_cast<uint32_t>(bin.data.size()));
    u32(0x004E4942);   // BIN
    glb.insert(glb.end(), bin.data.begin(), bin.data.end());

    FILE* f = std::fopen(outPath, "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", outPath);
        return 1;
    }
    std::fwrite(glb.data(), 1, glb.size(), f);
    std::fclose(f);
    std::printf("wrote %s: %zu vertices, %d joints, %zu animations, %zu bytes\n", outPath, n, kJoints, clips.size(), glb.size());
    return 0;
}
