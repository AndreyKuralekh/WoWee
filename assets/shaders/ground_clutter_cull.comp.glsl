#version 450

// Ground clutter cull: one thread per doodad. A doodad inside the draw range
// and the view frustum takes the next slot of its model's range in the visible
// list, and every part of its model counts one more instance.
//
// The visible slot comes from the model's first part; the other parts only
// count. They all end on the same number, which is all their draws need: each
// starts at the model's base and reads that many slots.

layout(local_size_x = 64) in;

// Matches CullUniformsGPU in ground_clutter_renderer.cpp (std140).
layout(std140, set = 0, binding = 0) uniform CullUniforms {
    vec4 frustumPlanes[6];
    vec4 center;          // xyz = range centre (the player), w = range squared
    uint instanceCount;
};

// Matches InstanceGPU (std430).
struct Instance {
    vec4 positionScale;
    float yaw;
    uint model;
    uint groundNormalXY;
    float shade;
};
layout(std430, set = 0, binding = 1) readonly buffer Instances { Instance instances[]; };
layout(std430, set = 0, binding = 2) writeonly buffer Visible { uint visible[]; };

// VkDrawIndexedIndirectCommand, five words.
struct DrawCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
layout(std430, set = 0, binding = 3) buffer Commands { DrawCommand commands[]; };

// Matches ModelInfoGPU.
struct ModelInfo {
    uint base;
    uint firstPart;
    uint partCount;
    float radius;
};
layout(std430, set = 0, binding = 4) readonly buffer Models { ModelInfo models[]; };

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= instanceCount) return;
    Instance inst = instances[i];
    vec3 p = inst.positionScale.xyz;

    vec2 d = p.xy - center.xy;
    if (dot(d, d) > center.w) return;

    ModelInfo m = models[inst.model];
    float r = m.radius * inst.positionScale.w;
    vec3 c = p + vec3(0.0, 0.0, r * 0.5);
    for (int k = 0; k < 6; ++k) {
        if (dot(frustumPlanes[k].xyz, c) + frustumPlanes[k].w < -r) return;
    }

    uint slot = atomicAdd(commands[m.firstPart].instanceCount, 1u);
    for (uint k = 1u; k < m.partCount; ++k) {
        atomicAdd(commands[m.firstPart + k].instanceCount, 1u);
    }
    visible[m.base + slot] = i;
}
