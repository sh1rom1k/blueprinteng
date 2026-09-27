#version 450 core

out vec4 fragmentColor;

uniform vec3 Color;

void main() {
    fragmentColor = vec4(Color, 1.0);
}
