#version 450
#extension GL_GOOGLE_include_directive : require

// Depth pass from the light's point of view.

#include "common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inUv;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;

layout(location = 0) out vec2 vUv;

void main() {
    vec3 position = inPosition;
    vec3 normal = vec3(0.0);
    applyMorphs(gl_VertexIndex, position, normal);
    mat3 normalMatrix;
    vec3 world = worldPosition(position, inJoints, inWeights, normalMatrix);
    vUv = inUv;
    gl_Position = scene.lightViewProj * vec4(world, 1.0);
}
