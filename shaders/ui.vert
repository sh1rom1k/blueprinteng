#version 450 core

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;

uniform vec2 uDisplaySize;

out vec4 vColor;
out vec2 vUV;

void main() {
    // Top-left origin (0, 0) to bottom-right (w, h) mapped to NDC [-1, 1]
    vec2 ndc;
    ndc.x = (aPos.x / uDisplaySize.x) * 2.0 - 1.0;
    ndc.y = 1.0 - (aPos.y / uDisplaySize.y) * 2.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vColor = aColor;
    vUV = aUV;
}
