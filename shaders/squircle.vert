#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inRectSize;
layout(location = 3) in vec3 inParams; // x: radius, y: shadow_blur, z: shadow_alpha

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragPos; // Local position within the quad
layout(location = 2) out vec2 rectSize;
layout(location = 3) out vec3 params;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
    // We assume the vertex positions define a quad larger than the rect to accommodate the shadow.
    // For simplicity, let's say the quad is passed with correct local coordinates.
    // We pass local coords directly via uniform or instancing.
    // In a real system, we'd expand the vertex bounds based on shadow_blur.
    fragPos = inPosition; 
    rectSize = inRectSize;
    params = inParams;
}
