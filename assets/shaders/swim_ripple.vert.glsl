#version 450

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
};

// Half the height of the target in pixels. A drop's size is a size in the world:
// a fixed number of pixels was a fleck on a headset's eye, which is three times
// the height of a monitor and sees the drop from a yard away rather than eight.
layout(push_constant) uniform Push {
    float halfHeight;
} push;

// aSize keeps its old meaning - pixels on a 1080-line screen with the camera's
// 45 degrees, at the third-person camera's eight yards - turned into yards:
// 8 / (540 / tan(22.5)) per unit, a drop of 3 to 7 units being 2 to 4 cm.
const float kYardsPerUnit = 8.0 / (540.0 * 2.41421);

layout(location = 0) in vec3 aPos;
layout(location = 1) in float aSize;
layout(location = 2) in float aAlpha;

layout(location = 0) out float vAlpha;

void main() {
    vAlpha = aAlpha;
    gl_Position = projection * view * vec4(aPos, 1.0);
    float pixels = aSize * kYardsPerUnit * projection[1][1] * push.halfHeight / max(gl_Position.w, 0.05);
    gl_PointSize = clamp(pixels, 1.0, 64.0);
}
