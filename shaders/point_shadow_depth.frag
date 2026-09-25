#version 450 core

in vec4 FragPos;

uniform vec3 LightPos;
uniform float FarPlane;

void main() {
    float lightDistance = length(FragPos.xyz - LightPos);
    gl_FragDepth = lightDistance / FarPlane;
}
