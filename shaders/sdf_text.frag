#version 450

layout(binding = 0) uniform sampler2D sdfTexture;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    float dist = texture(sdfTexture, fragUV).r;

    // Width of the SDF transition band.  0.1 gives crisp edges at typical sizes;
    // scale fwidth(dist) for screen-space-adaptive smoothing if needed later.
    float alpha = smoothstep(0.45, 0.55, dist);

    outColor = vec4(fragColor.rgb, fragColor.a * alpha);
}
