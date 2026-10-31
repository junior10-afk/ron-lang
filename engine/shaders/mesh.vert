#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 model0;   // rows of the model matrix (3x4)
    vec4 model1;
    vec4 model2;
    vec4 color;    // rgb, w = material flags (1 = checkerboard)
} pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;

void main() {
    vec4 p = vec4(inPosition, 1.0);
    vWorldPos = vec3(dot(pc.model0, p), dot(pc.model1, p), dot(pc.model2, p));
    vec3 n = vec3(dot(pc.model0.xyz, inNormal), dot(pc.model1.xyz, inNormal), dot(pc.model2.xyz, inNormal));
    vNormal = normalize(n);
    gl_Position = pc.mvp * p;
}
