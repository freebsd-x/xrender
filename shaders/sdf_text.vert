#version 450

layout(location = 0) in vec2 inPosition; // NDC [-1..1]
layout(location = 1) in vec2 inUV;       // Normalised atlas coords [0..1]
layout(location = 2) in vec4 inColor;    // RGBA [0..1]

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    fragUV    = inUV;
    fragColor = inColor;
}
