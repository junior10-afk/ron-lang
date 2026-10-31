#version 450

// Depth-only pass from the light's point of view (no fragment shader needed).

layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform Push {
    mat4 mvp;   // lightViewProj * model
} pc;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
}
