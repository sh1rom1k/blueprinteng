#version 450 core

in vec4 vColor;
in vec2 vUV;

uniform sampler2D uFontTexture;
uniform int uUseTexture;

out vec4 FragColor;

void main() {
    if (uUseTexture != 0) {
        float alpha = texture(uFontTexture, vUV).r;
        FragColor = vec4(vColor.rgb, vColor.a * alpha);
    } else {
        FragColor = vColor;
    }
}
