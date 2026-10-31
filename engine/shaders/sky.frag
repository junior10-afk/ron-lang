#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

layout(location = 0) in vec2 vNdc;
layout(set = 0, binding = 4) uniform sampler2D environment;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265;
const float EXPOSURE = 0.6;   // must match mesh.frag

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec4 far = scene.invViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - scene.cameraPos.xyz);
    vec2 uv = vec2(atan(dir.z, dir.x) / (2.0 * PI) + 0.5, acos(clamp(dir.y, -1.0, 1.0)) / PI);
    vec3 c = textureLod(environment, uv, 0.0).rgb * scene.env.y;
    outColor = vec4(aces(c * EXPOSURE), 1.0);
}
