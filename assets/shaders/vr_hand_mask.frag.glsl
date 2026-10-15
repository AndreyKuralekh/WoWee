#version 450

// Clears the alpha of a headset eye before the hands are drawn into it, with
// only alpha written: what the hands then leave at 1 is where they are seen,
// and the eye goes to the headset a second time, over the interface panel,
// showing only those pixels. See XrSystem::endFrame.

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(0.0);
}
