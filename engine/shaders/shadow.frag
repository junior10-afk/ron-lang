#version 450
#extension GL_GOOGLE_include_directive : require

// Alpha-tested materials (hair cards, leaves) cut holes in their shadows.

#include "common.glsl"

layout(location = 0) in vec2 vUv;

layout(set = 0, binding = 2) uniform sampler2D textures[256];

void main() {
    if ((pc.info.y & (FLAG_ALPHA_MASK | FLAG_ALPHA_BLEND)) != 0) {
        float alpha = pc.color.a * texture(textures[pc.info.x], vUv).a;
        if (alpha < max(pc.material.z, 0.01)) discard;
    }
}
