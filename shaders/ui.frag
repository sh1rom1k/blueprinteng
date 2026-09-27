#version 450 core

in vec4 vColor;
in vec2 vUV;

uniform sampler2D uFontTexture;
uniform int uUseTexture;

out vec4 FragColor;

void main() {
    if (uUseTexture == 1) {
        float alpha = texture(uFontTexture, vUV).r;
        FragColor = vec4(vColor.rgb, vColor.a * alpha);
    } else if (uUseTexture == 2) {
        vec4 texel = texture(uFontTexture, vUV);
        FragColor = vec4(texel.rgb, texel.a * vColor.a);
    } else {
        FragColor = vColor;
    }
}
