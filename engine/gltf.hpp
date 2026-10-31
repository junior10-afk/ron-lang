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

// Matches the engine's GPU vertex layout (68 bytes).
struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    uint8_t joints[4];
    float weights[4];
    float tangent[4];   // xyz + handedness (w = +1 / -1), for normal mapping
};
static_assert(sizeof(Vertex) == 68, "vertex layout must match the GPU pipeline");

// Computes smooth normals from triangles (for meshes exported without normals).
void computeNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
// Computes per-vertex tangents from positions, normals and UVs.
void computeTangents(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

using Mat4 = std::array<float, 16>;   // column-major

// Morph target: per-vertex offsets added with a weight (facial expressions...).
struct MorphTarget {
    std::vector<float> positions;   // 3 floats per vertex
    std::vector<float> normals;     // 3 floats per vertex (may be empty)
};

struct Primitive {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    int material = -1;
    bool skinned = false;
    std::vector<MorphTarget> targets;
};

struct Mesh {
    std::vector<Primitive> primitives;
    std::vector<float> weights;            // default morph weights
    std::vector<std::string> targetNames;  // extras.targetNames (Blender export)
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
    std::vector<float> weights;   // node-level default morph weights (override the mesh's)
};

struct Skin {
    std::vector<int> joints;          // node indices
    std::vector<Mat4> inverseBind;
};

enum class AlphaMode { Opaque, Mask, Blend };

struct Material {
    std::string name;
    float baseColor[4] = {1, 1, 1, 1};
    int image = -1;                 // base color (sRGB)
    float roughness = 1.0f;
    float metallic = 1.0f;
    int metallicRoughnessImage = -1;   // G = roughness, B = metallic (linear)
    int normalImage = -1;           // tangent-space normal map (linear)
    float normalScale = 1.0f;
    int occlusionImage = -1;        // R = ambient occlusion (linear)
    int emissiveImage = -1;         // sRGB
    float emissive[3] = {0, 0, 0};
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};

enum class Path { Translation, Rotation, Scale, Weights };

struct Channel {
    int node = -1;
    Path path = Path::Translation;
    bool step = false;
    std::vector<float> times;
    std::vector<float> values;   // 3 or 4 floats per key (weights: one per morph target)
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
// Morph weights of `node`'s mesh at the given pose (mesh defaults when not animated).
std::vector<float> sampleWeights(const Model& m, int node, int anim, float time, int animB, float timeB, float weight);
// Global node transforms from local ones.
void globalTransforms(const Model& m, const std::vector<Mat4>& local, std::vector<Mat4>& globalOut);

Mat4 multiply(const Mat4& a, const Mat4& b);

} // namespace ron::gltf
