#pragma once

// glTF 2.0 loader (.glb binary and .gltf with external or base64 buffers),
// written from scratch: JSON parser, accessors, node hierarchy, skins,
// materials with embedded PNG textures and keyframe animations.
// CPU side only; the engine uploads the result to the GPU.

#include "png.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ron::gltf {

// Matches the engine's GPU vertex layout (52 bytes).
struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    uint8_t joints[4];
    float weights[4];
};
static_assert(sizeof(Vertex) == 52, "vertex layout must match the GPU pipeline");

using Mat4 = std::array<float, 16>;   // column-major

struct Primitive {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    int material = -1;
    bool skinned = false;
};

struct Mesh {
    std::vector<Primitive> primitives;
};

struct Node {
    int parent = -1;
    std::vector<int> children;
    float translation[3] = {0, 0, 0};
    float rotation[4] = {0, 0, 0, 1};   // quaternion x, y, z, w
    float scale[3] = {1, 1, 1};
    bool hasMatrix = false;
    Mat4 matrix{};
    int mesh = -1;
    int skin = -1;
    std::string name;
};

struct Skin {
    std::vector<int> joints;          // node indices
    std::vector<Mat4> inverseBind;
};

struct Material {
    float baseColor[4] = {1, 1, 1, 1};
    int image = -1;
    float roughness = 1.0f;
    float metallic = 0.0f;
};

enum class Path { Translation, Rotation, Scale };

struct Channel {
    int node = -1;
    Path path = Path::Translation;
    bool step = false;
    std::vector<float> times;
    std::vector<float> values;   // 3 or 4 floats per key
};

struct Animation {
    std::string name;
    float duration = 0;
    std::vector<Channel> channels;
};

struct Model {
    std::vector<Mesh> meshes;
    std::vector<Node> nodes;
    std::vector<Skin> skins;
    std::vector<Material> materials;
    std::vector<png::Image> images;
    std::vector<Animation> animations;
    std::vector<int> roots;
};

bool load(const std::string& path, Model& out, std::string& error);
bool loadFromMemory(const uint8_t* data, size_t size, const std::string& baseDir, Model& out, std::string& error);

// Local transforms of every node at `time` (seconds, looping) of animation `anim`
// (-1 = bind pose), blended with animation `animB` at `timeB` by `weight` (0..1).
void samplePose(const Model& m, int anim, float time, int animB, float timeB, float weight,
                std::vector<Mat4>& localOut);
// Global node transforms from local ones.
void globalTransforms(const Model& m, const std::vector<Mat4>& local, std::vector<Mat4>& globalOut);

Mat4 multiply(const Mat4& a, const Mat4& b);

} // namespace ron::gltf
