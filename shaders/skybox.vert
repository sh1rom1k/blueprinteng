#version 450 core

layout (location = 0) in vec3 aPosition;
layout (location = 2) in vec2 aTexCoord;

uniform mat4 View;
uniform mat4 Projection;

out vec2 textureCoord;

void main() {
    textureCoord = aTexCoord;
    gl_Position = Projection * View * vec4(aPosition, 1.0);
}
