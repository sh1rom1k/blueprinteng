#version 450 core

in vec2 textureCoord;

out vec4 fragmentColor;

uniform sampler2D Texture0;

void main() {
    fragmentColor = vec4(texture(Texture0, textureCoord).rgb, 1.0);
}
