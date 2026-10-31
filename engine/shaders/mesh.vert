#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;
layout(location = 5) in vec4 inTangent;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec4 vTangent;

void main() {
    vec3 position = inPosition;
    vec3 normal = inNormal;
    applyMorphs(gl_VertexIndex, position, normal);
    mat3 normalMatrix;
    vWorldPos = worldPosition(position, inJoints, inWeights, normalMatrix);
    vNormal = normalize(normalMatrix * normal);
    vTangent = vec4(normalMatrix * inTangent.xyz, inTangent.w);
    vUv = inUv;
    gl_Position = scene.viewProj * vec4(vWorldPos, 1.0);
}
