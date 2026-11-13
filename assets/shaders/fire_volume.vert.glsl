#version 450

// A fire's volume (TASK_10, step 3): the box round a hearth's flames, drawn
// by its back faces so that every pixel it covers is shaded once, from
// outside it or from within. The marching is the fragment shader's.

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
};

layout(push_constant) uniform Push {
    vec4 boxMin;     // xyz; w = seconds
    vec4 boxMax;     // xyz; w = brightness (flicker and fade in)
    vec4 params;     // x = steps, y = seed, z, w unused
} push;

layout(location = 0) out vec3 vWorldPos;

// The 36 corners of a box's twelve triangles, as bits x, y, z of a corner.
const int kCorners[36] = int[36](
    0, 2, 1,  1, 2, 3,   // -z
    4, 5, 6,  5, 7, 6,   // +z
    0, 1, 4,  1, 5, 4,   // -y
    2, 6, 3,  3, 6, 7,   // +y
    0, 4, 2,  2, 4, 6,   // -x
    1, 3, 5,  3, 7, 5    // +x
);

void main() {
    int c = kCorners[gl_VertexIndex];
    vec3 t = vec3(float(c & 1), float((c >> 1) & 1), float((c >> 2) & 1));
    vWorldPos = mix(push.boxMin.xyz, push.boxMax.xyz, t);
    gl_Position = projection * view * vec4(vWorldPos, 1.0);
}
