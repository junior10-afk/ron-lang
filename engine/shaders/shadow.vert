#version 450
#extension GL_GOOGLE_include_directive : require

// Depth-only pass from the light's point of view (no fragment shader needed).

#include "common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;

void main() {
    mat3 normalMatrix;
    vec3 world = worldPosition(inPosition, inJoints, inWeights, normalMatrix);
    gl_Position = scene.lightViewProj * vec4(world, 1.0);
}
