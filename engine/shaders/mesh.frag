#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;

layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;
layout(set = 0, binding = 2) uniform sampler2D textures[256];

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265;
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

// Cook-Torrance microfacet BRDF: GGX distribution, Smith-Schlick geometry, Schlick Fresnel.
float distributionGGX(float ndh, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float geometrySmith(float ndv, float ndl, float roughness) {
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return (ndv / (ndv * (1.0 - k) + k)) * (ndl / (ndl * (1.0 - k) + k));
}

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;

    // World-space (box-projected) UVs keep texel density constant on stretched boxes.
    vec2 uv = vUv;
    if (pc.material.w > 0.0) {
        vec3 an = abs(n);
        vec3 wp = vWorldPos * pc.material.w;
        uv = an.x > an.y && an.x > an.z ? wp.zy : (an.y > an.z ? wp.xz : wp.xy);
    }
    vec4 tex = texture(textures[pc.info.x], uv);
    vec3 albedo = pc.color.rgb * tex.rgb;
    if ((pc.info.y & FLAG_CHECKER) != 0) {
        vec2 cell = floor(vWorldPos.xz);
        albedo *= mix(0.72, 1.0, mod(cell.x + cell.y, 2.0));
    }
    float roughness = clamp(pc.material.x, 0.04, 1.0);
    float metallic = clamp(pc.material.y, 0.0, 1.0);

    vec3 L = normalize(-scene.lightDir.xyz);
    vec3 V = normalize(scene.cameraPos.xyz - vWorldPos);
    vec3 H = normalize(L + V);
    float ndl = max(dot(n, L), 0.0);
    float ndv = max(dot(n, V), 1e-4);
    float ndh = max(dot(n, H), 0.0);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), f0);
    float D = distributionGGX(ndh, roughness);
    float G = geometrySmith(ndv, ndl, roughness);
    vec3 specular = D * G * F / max(4.0 * ndv * ndl, 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metallic);

    float sh = ndl > 0.0 ? shadow(vWorldPos, n, L) : 0.0;
    vec3 radiance = scene.lightColor.rgb * scene.lightDir.w;
    vec3 direct = (kd * albedo + specular * PI) * radiance * ndl * sh;

    // Image-free ambient: hemispheric diffuse + a sky/ground "environment" reflection.
    float hemi = n.y * 0.5 + 0.5;
    vec3 ambientDiffuse = mix(scene.groundColor.rgb, scene.skyColor.rgb, hemi);
    // Reflections see a bright horizon/sky (the fog color) above and the lit ground below.
    vec3 R = reflect(-V, n);
    vec3 skyReflect = mix(scene.fog.rgb * 1.6, scene.skyColor.rgb * 2.5, clamp(R.y, 0.0, 1.0));
    vec3 groundReflect = scene.groundColor.rgb * 2.5 + scene.lightColor.rgb * scene.lightDir.w * 0.08;
    vec3 envColor = mix(groundReflect, skyReflect, smoothstep(-0.15, 0.15, R.y));
    vec3 Fa = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - ndv, 5.0);
    vec3 ambient = (1.0 - Fa) * (1.0 - metallic) * albedo * ambientDiffuse + Fa * envColor * (1.0 - roughness * 0.7);

    vec3 lit = direct + ambient + albedo * pc.material.z;

    float dist = length(scene.cameraPos.xyz - vWorldPos);
    float fogAmount = 1.0 - exp(-pow(dist * scene.fog.w, 2.0));
    lit = mix(lit, scene.fog.rgb, fogAmount);

    outColor = vec4(aces(lit * EXPOSURE), 1.0);
}
