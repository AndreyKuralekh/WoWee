#version 450

// The player's hands in a headset eye: a skinned mesh, each vertex moved by
// up to four of the joints the headset reports. The skin matrices take the
// mesh from where it was bound straight to the world.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in uvec4 inJoints;
layout(location = 3) in vec4 inWeights;

layout(set = 0, binding = 0) uniform HandData {
    mat4 skin[50];      // the left hand's 25 joints, then the right's
    vec4 lightDir;      // towards the ground
    vec4 lightColor;
    vec4 ambientColor;
    vec4 skinColor;
} hand;

layout(push_constant) uniform Push {
    mat4 viewProjection;
    vec4 eye;           // xyz the eye, w the hand's first skin matrix
} push;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vWorld;

void main() {
    const int base = int(push.eye.w);
    mat4 m = hand.skin[base + int(inJoints.x)] * inWeights.x +
             hand.skin[base + int(inJoints.y)] * inWeights.y +
             hand.skin[base + int(inJoints.z)] * inWeights.z +
             hand.skin[base + int(inJoints.w)] * inWeights.w;
    vec4 world = m * vec4(inPosition, 1.0);
    vWorld = world.xyz;
    // The scale in m is uniform, so this is the normal up to its length.
    vNormal = mat3(m) * inNormal;
    gl_Position = push.viewProjection * world;
}
