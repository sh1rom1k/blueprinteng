#version 450 core

layout (location = 0) in vec3 aPosition;

uniform mat4 LightSpaceMatrix;
uniform mat4 Model;

void main() {
    gl_Position = LightSpaceMatrix * Model * vec4(aPosition, 1.0);
}
