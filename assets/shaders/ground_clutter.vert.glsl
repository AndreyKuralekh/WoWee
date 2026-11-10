#version 450

// A ground clutter doodad: one instance of a small M2, placed, turned and
// scaled, swayed by the wind above its root and sunk into the ground over
// the last part of the draw range so the field fades out instead of ending.

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
    vec4 fogParams;   // z = wind time
    vec4 shadowParams;
    vec4 playerPos;
};

// Matches InstanceGPU in ground_clutter_renderer.cpp (std430).
struct Instance {
    vec4 positionScale;
    float yaw;
    uint model;
    uint pad0;
    uint pad1;
};
layout(std430, set = 1, binding = 0) readonly buffer Instances { Instance instances[]; };
// Written by the cull; the draw's firstInstance is the model's base, so
// gl_InstanceIndex already points into the model's own range.
layout(std430, set = 1, binding = 1) readonly buffer Visible { uint visible[]; };

layout(push_constant) uniform Fade {
    float fadeStart;
    float fadeEnd;
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out float vHeight;

void main() {
    Instance inst = instances[visible[gl_InstanceIndex]];
    vec3 root = inst.positionScale.xyz;
    float c = cos(inst.yaw);
    float s = sin(inst.yaw);

    float scale = inst.positionScale.w;
    // Sink toward the edge of the range, measured from the player as the
    // window is: measured from the camera, the edge would slide as it orbits.
    float dist = distance(root.xy, playerPos.xy);
    scale *= 1.0 - smoothstep(fadeStart, fadeEnd, dist);

    vec3 local = aPos * scale;
    vec3 rotated = vec3(c * local.x - s * local.y, s * local.x + c * local.y, local.z);

    // Wind: the higher above the root, the further it leans. Phase from the
    // world position, so neighbours sway together in a wave and the same
    // doodad sways the same way on every frame.
    float t = fogParams.z;
    float phase = dot(root.xy, vec2(0.11, 0.07));
    float sway = sin(t * 1.7 + phase) * 0.6 + sin(t * 3.1 + phase * 1.9) * 0.25;
    float lean = max(local.z, 0.0) * 0.08 * sway;
    rotated.xy += vec2(0.8, 0.6) * lean;

    vec3 world = root + rotated;
    vWorldPos = world;
    vUV = aUV;
    vec3 n = vec3(c * aNormal.x - s * aNormal.y, s * aNormal.x + c * aNormal.y, aNormal.z);
    vNormal = n;
    vHeight = clamp(aPos.z, 0.0, 1.0);
    gl_Position = projection * view * vec4(world, 1.0);
}
