#version 450

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragPos;
layout(location = 2) in vec2 rectSize;
layout(location = 3) in vec3 params; // radius, shadow_blur, shadow_alpha

layout(location = 0) out vec4 outColor;

// SDF for a rounded rectangle (squircle-like)
float roundedBoxSDF(vec2 CenterPosition, vec2 Size, float Radius) {
    vec2 q = abs(CenterPosition) - Size + vec2(Radius);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - Radius;
}

void main() {
    float radius = params.x;
    float blur = params.y;
    float shadowAlpha = params.z;

    vec2 center = rectSize * 0.5;
    float distance = roundedBoxSDF(fragPos - center, center, radius);

    // Smoothstep for anti-aliasing the edges of the box
    float edgeSmoothing = 1.0; 
    float alpha = 1.0 - smoothstep(-edgeSmoothing, edgeSmoothing, distance);

    vec4 boxColor = vec4(fragColor.rgb, fragColor.a * alpha);

    // Simple Gaussian shadow approximation using the SDF distance
    float shadow = 0.0;
    if (distance > 0.0 && blur > 0.0) {
        shadow = exp(-distance * distance / (blur * blur)) * shadowAlpha;
    }

    vec4 shadowColor = vec4(0.0, 0.0, 0.0, shadow);

    // Alpha blend box over shadow
    outColor = mix(shadowColor, boxColor, alpha);
}
