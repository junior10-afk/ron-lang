#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;

void main() {
    mat3 normalMatrix;
    vWorldPos = worldPosition(inPosition, inJoints, inWeights, normalMatrix);
    vNormal = normalize(normalMatrix * inNormal);
    vUv = inUv;
    gl_Position = scene.viewProj * vec4(vWorldPos, 1.0);
}
