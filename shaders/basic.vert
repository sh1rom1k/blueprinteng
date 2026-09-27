#version 450 core

layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aColor;
layout (location = 2) in vec2 aTexCoord;
layout (location = 3) in vec2 aLightmapCoord;
layout (location = 4) in float aLightStyle;

uniform mat4 Model;
uniform mat4 View;
uniform mat4 Projection;
uniform mat4 FlashlightSpaceMatrix;
uniform bool DecalPass;
uniform float Time;
uniform bool WarpSurface;

out vec3 vertexColor;
out vec2 textureCoord;
out vec2 lightmapCoord;
out float lightStyle;
out vec3 FragPos;
out vec4 FragPosLightSpace;

void main() {
    vec4 worldPos = Model * vec4(aPosition, 1.0);
    FragPos = worldPos.xyz;
    vec4 clipPos = Projection * View * worldPos;
    if (DecalPass) {
        // Shift depth slightly towards camera in clip space to prevent z-fighting
        // and avoid clipping issues when player is touching the wall
        clipPos.z -= 0.0002 * clipPos.w;
    }
    gl_Position = clipPos;
    vertexColor = aColor;
    textureCoord = aTexCoord;
    if (WarpSurface) {
        float wave = sin(aPosition.x * 4.0 + Time * 1.7) * 0.015 + cos(aPosition.z * 3.0 + Time) * 0.01;
        textureCoord += vec2(Time * 0.04 + wave, Time * 0.02);
    }
    lightmapCoord = aLightmapCoord;
    lightStyle = aLightStyle;
    FragPosLightSpace = FlashlightSpaceMatrix * worldPos;
}
