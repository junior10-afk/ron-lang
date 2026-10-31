#version 450
#extension GL_GOOGLE_include_directive : require

// Fullscreen triangle at the far plane; the fragment shader turns the pixel
// into a view direction and looks up the environment map.

#include "common.glsl"

layout(location = 0) out vec2 vNdc;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vNdc = p * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 1.0, 1.0);
}
