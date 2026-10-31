#version 450

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 model0;
    vec4 model1;
    vec4 model2;
    vec4 color;    // rgb, w = material flags (1 = checkerboard)
} pc;

layout(set = 0, binding = 0) uniform Scene {
    mat4 lightViewProj;
    vec4 lightDir;     // xyz = direction the light travels, w = intensity
    vec4 lightColor;
    vec4 skyColor;     // ambient from above
    vec4 groundColor;  // ambient from below
    vec4 cameraPos;
    vec4 fog;          // rgb = color, w = density
} scene;

layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;

layout(location = 0) out vec4 outColor;

const float EXPOSURE = 0.6;   // must match kExposure in engine.cpp

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// 3x3 PCF on top of the hardware 2x2 comparison filter.
float shadow(vec3 worldPos, vec3 n, vec3 L) {
    vec3 offsetPos = worldPos + n * 0.04;
    vec4 lp = scene.lightViewProj * vec4(offsetPos, 1.0);
    vec3 p = lp.xyz / lp.w;
    vec2 uv = p.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || p.z > 1.0) return 1.0;
    float bias = max(0.0015 * (1.0 - dot(n, L)), 0.0004);
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(shadowMap, vec3(uv + vec2(x, y) * texel * 1.5, p.z - bias));
    return sum / 9.0;
}

void main() {
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;
    vec3 albedo = pc.color.rgb;

    // Procedural checkerboard for ground-like surfaces
    if (pc.color.w > 0.5) {
        vec2 cell = floor(vWorldPos.xz);
        float c = mod(cell.x + cell.y, 2.0);
        albedo *= mix(0.72, 1.0, c);
    }

    vec3 L = normalize(-scene.lightDir.xyz);
    vec3 V = normalize(scene.cameraPos.xyz - vWorldPos);
    vec3 H = normalize(L + V);

    float ndl = max(dot(n, L), 0.0);
    float sh = ndl > 0.0 ? shadow(vWorldPos, n, L) : 0.0;
    float spec = pow(max(dot(n, H), 0.0), 64.0) * 0.4 * step(0.0, ndl);
    float hemi = n.y * 0.5 + 0.5;
    vec3 ambient = mix(scene.groundColor.rgb, scene.skyColor.rgb, hemi);

    vec3 direct = scene.lightColor.rgb * scene.lightDir.w * sh;
    vec3 lit = albedo * (ambient + direct * ndl) + direct * spec;

    float dist = length(scene.cameraPos.xyz - vWorldPos);
    float fogAmount = 1.0 - exp(-pow(dist * scene.fog.w, 2.0));
    lit = mix(lit, scene.fog.rgb, fogAmount);

    outColor = vec4(aces(lit * EXPOSURE), 1.0);
}
