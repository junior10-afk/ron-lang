#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec4 vTangent;

layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;
layout(set = 0, binding = 2) uniform sampler2D textures[256];
layout(set = 0, binding = 4) uniform sampler2D environment;   // prefiltered equirect mip chain

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265;
const float EXPOSURE = 0.6;   // must match kExposure in engine.cpp

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// 3x3 PCF on top of the hardware 2x2 comparison filter.
float shadow(vec3 worldPos, vec3 n, vec3 L, float spread) {
    vec3 offsetPos = worldPos + n * 0.04;
    vec4 lp = scene.lightViewProj * vec4(offsetPos, 1.0);
    vec3 p = lp.xyz / lp.w;
    vec2 uv = p.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || p.z > 1.0) return 1.0;
    float bias = max(0.0015 * (1.0 - abs(dot(n, L))), 0.0004);
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(shadowMap, vec3(uv + vec2(x, y) * texel * spread, p.z - bias));
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

// Analytic fit of the split-sum environment BRDF (Karis, "Physically Based Shading on Mobile").
vec3 envBRDF(vec3 f0, float roughness, float ndv) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

vec2 equirect(vec3 d) {
    return vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / PI);
}

vec3 irradianceSH(vec3 n) {
    vec3 r = scene.sh[0].rgb * 0.282095
           + scene.sh[1].rgb * (0.488603 * n.y) + scene.sh[2].rgb * (0.488603 * n.z) + scene.sh[3].rgb * (0.488603 * n.x)
           + scene.sh[4].rgb * (1.092548 * n.x * n.y) + scene.sh[5].rgb * (1.092548 * n.y * n.z)
           + scene.sh[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0)) + scene.sh[7].rgb * (1.092548 * n.x * n.z)
           + scene.sh[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
    return max(r, vec3(0.0));
}

void main() {
    vec3 geomN = normalize(vNormal);
    if (!gl_FrontFacing) geomN = -geomN;
    int flags = pc.info.y;

    // World-space (box-projected) UVs keep texel density constant on stretched boxes.
    vec2 uv = vUv;
    if (pc.material.w > 0.0) {
        vec3 an = abs(geomN);
        vec3 wp = vWorldPos * pc.material.w;
        uv = an.x > an.y && an.x > an.z ? wp.zy : (an.y > an.z ? wp.xz : wp.xy);
    }
    vec4 base = texture(textures[pc.info.x], uv) * pc.color;
    float alpha = base.a;
    if ((flags & FLAG_ALPHA_MASK) != 0) {
        // Sharpened alpha for alpha-to-coverage: crisp edges, antialiased by MSAA.
        alpha = (alpha - pc.material.z) / max(fwidth(alpha), 1e-4) + 0.5;
        if (alpha <= 0.0) discard;
    }
    vec3 albedo = base.rgb;
    if ((flags & FLAG_CHECKER) != 0) {
        vec2 cell = floor(vWorldPos.xz);
        albedo *= mix(0.72, 1.0, mod(cell.x + cell.y, 2.0));
    }

    // Tangent-space normal mapping
    vec3 n = geomN;
    vec3 T = vTangent.xyz;
    float tlen = length(T);
    vec3 B = vec3(0.0);
    if (tlen > 1e-5) {
        T = normalize(T - geomN * dot(geomN, T));
        float handed = vTangent.w < 0.0 ? -1.0 : 1.0;
        B = cross(geomN, T) * handed;
        if (pc.maps.x >= 0) {
            vec3 tn = texture(textures[pc.maps.x], uv).xyz * 2.0 - 1.0;
            tn.xy *= pc.emissive.w;
            n = normalize(T * tn.x + B * tn.y + geomN * max(tn.z, 1e-3));
        }
    }

    vec4 mr = texture(textures[pc.maps.y], uv);
    float roughness = clamp(pc.material.x * mr.g, 0.04, 1.0);
    float metallic = clamp(pc.material.y * mr.b, 0.0, 1.0);
    float ao = texture(textures[pc.maps.z], uv).r;
    vec3 emissive = pc.emissive.rgb * texture(textures[pc.maps.w], uv).rgb;

    vec3 L = normalize(-scene.lightDir.xyz);
    vec3 V = normalize(scene.cameraPos.xyz - vWorldPos);
    vec3 H = normalize(L + V);
    float ndlRaw = dot(n, L);
    float ndl = max(ndlRaw, 0.0);
    float ndv = max(dot(n, V), 1e-4);
    float ndh = max(dot(n, H), 0.0);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), f0);
    float D = distributionGGX(ndh, roughness);
    float G = geometrySmith(ndv, ndl, roughness);
    vec3 specular = D * G * F / max(4.0 * ndv * ndl, 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    vec3 radiance = scene.lightColor.rgb * scene.lightDir.w;

    bool skin = (flags & FLAG_SKIN) != 0;
    float sh = shadow(vWorldPos, geomN, L, skin ? 2.5 : 1.5);
    vec3 diffuseLight;
    if (skin) {
        // Subsurface approximation: light wraps further around the terminator
        // for red than for green and blue, and the red channel sees a smoother
        // (geometric) normal, like light diffusing under the skin.
        vec3 wrap = vec3(0.5, 0.22, 0.14);
        vec3 ndlC = vec3(dot(normalize(mix(n, geomN, 0.6)), L), ndlRaw, ndlRaw);
        vec3 d = clamp((ndlC + wrap) / (1.0 + wrap), 0.0, 1.0);
        // Shadow edges bleed red as well.
        vec3 shC = mix(vec3(sh), vec3(sqrt(sh)), vec3(0.6, 0.15, 0.05));
        diffuseLight = d * shC;
    } else {
        diffuseLight = vec3(ndl * sh);
    }
    vec3 direct = kd * albedo * diffuseLight * radiance + specular * PI * radiance * ndl * sh;

    if ((flags & FLAG_HAIR) != 0 && tlen > 1e-5) {
        // Kajiya-Kay: strands run along the bitangent; two shifted lobes
        // (white primary, tinted secondary).
        vec3 strand = normalize(B);
        vec3 t1 = normalize(strand + n * 0.08);
        vec3 t2 = normalize(strand - n * 0.12);
        float s1 = pow(sqrt(max(1.0 - pow(dot(t1, H), 2.0), 0.0)), 80.0);
        float s2 = pow(sqrt(max(1.0 - pow(dot(t2, H), 2.0), 0.0)), 16.0);
        float atten = smoothstep(-1.0, 0.0, dot(t1, L));
        direct += (vec3(0.12) * s1 + albedo * 0.25 * s2) * atten * radiance * ndl * sh;
    }

    vec3 ambient;
    vec3 R = reflect(-V, n);
    if (scene.env.x > 0.5) {
        // Image-based lighting: SH irradiance + prefiltered environment (split sum).
        vec3 irradiance = irradianceSH(n) * scene.env.y;
        vec3 prefiltered = textureLod(environment, equirect(R), roughness * scene.env.z).rgb * scene.env.y;
        vec3 Fa = envBRDF(f0, roughness, ndv);
        vec3 kdA = (1.0 - Fa) * (1.0 - metallic);
        float specAo = clamp(pow(ndv + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
        vec3 diffuseAmbient = kdA * albedo * irradiance;
        if (skin) diffuseAmbient *= mix(vec3(1.0), vec3(1.08, 0.95, 0.92), 0.5);
        ambient = diffuseAmbient * ao + prefiltered * Fa * specAo;
    } else {
        // Image-free ambient: hemispheric diffuse + a sky/ground "environment" reflection.
        float hemi = n.y * 0.5 + 0.5;
        vec3 ambientDiffuse = mix(scene.groundColor.rgb, scene.skyColor.rgb, hemi);
        // Reflections see a bright horizon/sky (the fog color) above and the lit ground below.
        vec3 skyReflect = mix(scene.fog.rgb * 1.6, scene.skyColor.rgb * 2.5, clamp(R.y, 0.0, 1.0));
        vec3 groundReflect = scene.groundColor.rgb * 2.5 + scene.lightColor.rgb * scene.lightDir.w * 0.08;
        vec3 envColor = mix(groundReflect, skyReflect, smoothstep(-0.15, 0.15, R.y));
        vec3 Fa = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - ndv, 5.0);
        ambient = ((1.0 - Fa) * (1.0 - metallic) * albedo * ambientDiffuse + Fa * envColor * (1.0 - roughness * 0.7)) * ao;
    }

    vec3 lit = direct + ambient + emissive;

    float dist = length(scene.cameraPos.xyz - vWorldPos);
    float fogAmount = 1.0 - exp(-pow(dist * scene.fog.w, 2.0));
    lit = mix(lit, scene.fog.rgb, fogAmount);

    float outAlpha = (flags & (FLAG_ALPHA_MASK | FLAG_ALPHA_BLEND)) != 0 ? clamp(alpha, 0.0, 1.0) : 1.0;
    outColor = vec4(aces(lit * EXPOSURE), outAlpha);
}
