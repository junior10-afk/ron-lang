// Shared declarations for all Ron engine shaders.

layout(push_constant) uniform Push {
    vec4 model0;     // rows of the model matrix (3x4)
    vec4 model1;
    vec4 model2;
    vec4 color;      // base color (linear rgb), a = alpha
    vec4 material;   // x = roughness, y = metallic, z = alpha cutoff, w = world-space UV scale (0 = mesh UVs)
    vec4 emissive;   // rgb = emissive color, w = normal map scale
    ivec4 info;      // x = base color texture, y = flags, z = first joint matrix, w = morph header (-1 = none)
    ivec4 maps;      // x = normal map (-1 = none), y = metallic-roughness, z = occlusion, w = emissive texture
} pc;

const int FLAG_CHECKER = 1;
const int FLAG_SKINNED = 2;
const int FLAG_ALPHA_MASK = 4;
const int FLAG_ALPHA_BLEND = 8;
const int FLAG_SKIN = 16;    // subsurface scattering approximation
const int FLAG_HAIR = 64;    // anisotropic (Kajiya-Kay) highlight

layout(set = 0, binding = 0) uniform Scene {
    mat4 viewProj;
    mat4 lightViewProj;
    vec4 lightDir;     // xyz = direction the light travels, w = intensity
    vec4 lightColor;
    vec4 skyColor;     // ambient from above
    vec4 groundColor;  // ambient from below
    vec4 cameraPos;
    vec4 fog;          // rgb = color, w = density
    mat4 invViewProj;
    vec4 env;          // x = 1 if an environment map is loaded, y = intensity, z = max mip level, w = background
    vec4 sh[9];        // irradiance / pi as spherical harmonics
} scene;

layout(std430, set = 0, binding = 3) readonly buffer Joints {
    mat4 joints[];
};

// Morph targets: deltas of every loaded model (static), and this frame's
// per-draw headers + weights.
layout(std430, set = 0, binding = 5) readonly buffer MorphDeltas {
    vec4 morphDeltas[];
};
layout(std430, set = 0, binding = 6) readonly buffer MorphFrame {
    // At header h: [first delta, target count, vertex count, has normals], then the weights.
    float morphFrame[];
};

// Adds the weighted morph target offsets of this vertex.
void applyMorphs(int vertexIndex, inout vec3 position, inout vec3 normal) {
    int h = pc.info.w;
    if (h < 0) return;
    int first = floatBitsToInt(morphFrame[h]);
    int count = floatBitsToInt(morphFrame[h + 1]);
    int vertexCount = floatBitsToInt(morphFrame[h + 2]);
    int hasNormals = floatBitsToInt(morphFrame[h + 3]);
    int stride = vertexCount * (hasNormals + 1);
    for (int t = 0; t < count; ++t) {
        float w = morphFrame[h + 4 + t];
        if (w == 0.0) continue;
        int base = first + t * stride + vertexIndex;
        position += w * morphDeltas[base].xyz;
        if (hasNormals != 0) normal += w * morphDeltas[base + vertexCount].xyz;
    }
}

// Applies skinning (if enabled) then the model matrix.
vec3 worldPosition(vec3 position, uvec4 jointIds, vec4 weights, out mat3 normalMatrix) {
    vec4 p = vec4(position, 1.0);
    mat4 skin = mat4(1.0);
    if ((pc.info.y & FLAG_SKINNED) != 0) {
        int base = pc.info.z;
        skin = weights.x * joints[base + int(jointIds.x)] + weights.y * joints[base + int(jointIds.y)] +
               weights.z * joints[base + int(jointIds.z)] + weights.w * joints[base + int(jointIds.w)];
        p = skin * p;
    }
    mat3 model = transpose(mat3(pc.model0.xyz, pc.model1.xyz, pc.model2.xyz));
    normalMatrix = model * mat3(skin);
    return vec3(dot(pc.model0, p), dot(pc.model1, p), dot(pc.model2, p));
}
