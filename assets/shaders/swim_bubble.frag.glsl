#version 450

layout(location = 0) in float vAlpha;

layout(location = 0) out vec4 outColor;

// The first two matrices of the per-frame block, for the depth's distance.
layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
};
// The scene's depth for soft edges (the eyes' copy, TASK_12), or a white
// stand-in - depth 1, the far plane - that leaves every drop as it was.
layout(set = 1, binding = 0) uniform sampler2D uSceneDepth;

float softFade() {
    float scene = texelFetch(uSceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    float sceneDist = projection[3][2] / (scene + projection[2][2]);
    float dropDist = projection[3][2] / (gl_FragCoord.z + projection[2][2]);
    return clamp((sceneDist - dropDist) / 0.2, 0.0, 1.0);
}

void main() {
    vec2 p = gl_PointCoord - vec2(0.5);
    float dist = length(p);
    if (dist > 0.5) discard;
    float ring = smoothstep(0.5, 0.4, dist) - smoothstep(0.38, 0.28, dist);
    float highlight = smoothstep(0.3, 0.1, length(p - vec2(-0.15, 0.15))) * 0.5;
    float alpha = (ring + highlight) * vAlpha * softFade();
    outColor = vec4(0.8, 0.9, 1.0, alpha);
}
