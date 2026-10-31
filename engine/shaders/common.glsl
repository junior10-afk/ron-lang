// Shared declarations for all Ron engine shaders.

layout(push_constant) uniform Push {
    vec4 model0;     // rows of the model matrix (3x4)
    vec4 model1;
    vec4 model2;
    vec4 color;      // base color (linear rgb), a = alpha (unused)
    vec4 material;   // x = roughness, y = metallic, z = emissive, w = world-space UV scale (0 = mesh UVs)
    ivec4 info;      // x = texture index, y = flags, z = first joint matrix
} pc;

const int FLAG_CHECKER = 1;
const int FLAG_SKINNED = 2;

layout(set = 0, binding = 0) uniform Scene {
    mat4 viewProj;
    mat4 lightViewProj;
    vec4 lightDir;     // xyz = direction the light travels, w = intensity
    vec4 lightColor;
    vec4 skyColor;     // ambient from above
    vec4 groundColor;  // ambient from below
    vec4 cameraPos;
    vec4 fog;          // rgb = color, w = density
} scene;

layout(std430, set = 0, binding = 3) readonly buffer Joints {
    mat4 joints[];
};

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
