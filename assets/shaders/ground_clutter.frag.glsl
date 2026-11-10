#version 450

// Ground clutter: an alpha-cut card in the sun and the zone's fog.

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
    vec4 fogParams;
    vec4 shadowParams;
    vec4 playerPos;
    vec4 playerWake;
    vec4 localLightPosRadius[64];
    vec4 localLightColorIntensity[64];
    ivec4 localLightMeta;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
};

layout(set = 0, binding = 2) uniform sampler3D uFogVolume;
layout(set = 2, binding = 0) uniform sampler2D uTexture;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vWorldPos;
layout(location = 3) in float vHeight;

layout(location = 0) out vec4 outColor;

vec4 fogVolumeAt(vec3 worldPos) {
    vec4 clip = projection * view * vec4(worldPos, 1.0);
    float depth = max(clip.w, 1e-4);
    vec2 uv = clip.xy / depth * 0.5 + 0.5;
    float slice = log(max(depth, volumetricParams.y) / volumetricParams.y) * volumetricParams.z;
    return textureLod(uFogVolume, vec3(uv, slice - 0.5 / volumetricParams.w), 0.0);
}

vec3 applyFog(vec3 color, vec3 worldPos, float dist) {
    float fogFactor = clamp((fogParams.y - dist) / (fogParams.y - fogParams.x), 0.0, 1.0);
    color = mix(fogColor.rgb, color, fogFactor);
    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeAt(worldPos);
        color = color * air.a + air.rgb;
    }
    return color;
}

void main() {
    vec4 tex = texture(uTexture, vUV);
    if (tex.a < 0.5) discard;

    // Cards are seen from both sides and their normals are rarely more than
    // a hint; light them wrapped, as the grass is, and darker at the root.
    vec3 n = normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0);
    float wrapped = clamp(dot(n, normalize(-lightDir.xyz)) * 0.5 + 0.5, 0.0, 1.0);
    float ao = mix(0.7, 1.0, vHeight);
    vec3 lit = tex.rgb * (ambientColor.rgb + lightColor.rgb * wrapped) * ao;
    lit = applyFog(lit, vWorldPos, length(viewPos.xyz - vWorldPos));
    outColor = vec4(lit, 1.0);
}
