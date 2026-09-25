#version 450 core

in vec3 vertexColor;
in vec2 textureCoord;
in vec2 lightmapCoord;
in vec3 FragPos;
in vec4 FragPosLightSpace;

out vec4 fragmentColor;

uniform sampler2D Texture0;
uniform bool UseTexture;
uniform sampler2D Lightmap0;
uniform bool UseLightmap;
uniform float Opacity;
uniform bool DecalPass;

struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float constant;
    float linear;
    float quadratic;
};

#define MAX_POINT_LIGHTS 32
uniform int NumPointLights;
uniform PointLight PointLights[MAX_POINT_LIGHTS];

struct SpotLight {
    vec3 position;
    vec3 direction;
    vec3 color;
    float intensity;
    float innerCutOff;
    float outerCutOff;
    float constant;
    float linear;
    float quadratic;
    bool enabled;
};

uniform SpotLight Flashlight;

uniform sampler2DShadow FlashlightShadowMap;
uniform bool FlashlightCastShadow;

#define MAX_SHADOW_POINT_LIGHTS 4
uniform samplerCube PointShadowMaps[MAX_SHADOW_POINT_LIGHTS];
uniform int NumShadowPointLights;
uniform float PointShadowFarPlane;
uniform float ShadowRenderDistance;
uniform bool PointShadowsEnabled;

float SamplePointShadowMap(int index, vec3 dir) {
    switch (index) {
        case 0: return texture(PointShadowMaps[0], dir).r;
        case 1: return texture(PointShadowMaps[1], dir).r;
        case 2: return texture(PointShadowMaps[2], dir).r;
        case 3: return texture(PointShadowMaps[3], dir).r;
        default: return 1.0;
    }
}

float CalculateFlashlightShadow(vec4 fragPosLightSpace, float diff, float lightDistance) {
    if (!FlashlightCastShadow || lightDistance >= ShadowRenderDistance) {
        return 0.0;
    }

    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0) {
        return 0.0;
    }

    float bias = max(0.003 * (1.0 - diff), 0.0005);
    float shadowLit = texture(FlashlightShadowMap, vec3(projCoords.xy, projCoords.z - bias));
    return 1.0 - shadowLit;
}

float CalculatePointShadow(int lightIndex, float diff, vec3 fragToLight, float distance) {
    if (!PointShadowsEnabled || lightIndex >= NumShadowPointLights || distance >= PointShadowFarPlane) {
        return 0.0;
    }

    float currentDepth = distance;
    float bias = max(0.1 * (1.0 - diff), 0.03);
    float diskRadius = (1.0 + (distance / PointShadowFarPlane)) / 40.0;

    // Fast-path center tap: if well in front of occluders, fragment is fully lit.
    // This avoids 4 additional cubemap lookups on unoccluded fragments without reducing shadow quality.
    float centerDepth = SamplePointShadowMap(lightIndex, fragToLight) * PointShadowFarPlane;
    if (currentDepth - bias <= centerDepth - (diskRadius * PointShadowFarPlane * 0.75)) {
        return 0.0;
    }

    const vec3 offsets[4] = vec3[](
        vec3( 1.0,  1.0,  0.0),
        vec3(-1.0,  1.0,  0.0),
        vec3( 1.0, -1.0,  0.0),
        vec3(-1.0, -1.0,  0.0)
    );

    float shadow = (currentDepth - bias > centerDepth) ? 1.0 : 0.0;
    for (int s = 0; s < 4; ++s) {
        float sampleDepth = SamplePointShadowMap(lightIndex, fragToLight + offsets[s] * diskRadius) * PointShadowFarPlane;
        if (currentDepth - bias > sampleDepth) {
            shadow += 1.0;
        }
    }
    return shadow * 0.2;
}

void main() {
    vec4 texColor = texture(Texture0, textureCoord);
    vec4 baseColor = UseTexture
        ? vec4(texColor.rgb, texColor.a * Opacity)
        : vec4(vertexColor, Opacity);

    if (DecalPass) {
        if (baseColor.a < 0.02) {
            discard;
        }
    }

    vec3 ambient = UseLightmap ? vec3(0.0) : vec3(0.35);
    vec3 bakedLight = UseLightmap ? texture(Lightmap0, lightmapCoord).rgb * 2.0 : vec3(0.0);

    vec3 dynamicLight = vec3(0.0);
    int count = clamp(NumPointLights, 0, MAX_POINT_LIGHTS);

    // Compute normal only if dynamic lights are active, avoiding screen-space derivatives when unnecessary
    if (count > 0 || Flashlight.enabled) {
        vec3 dX = dFdx(FragPos);
        vec3 dY = dFdy(FragPos);
        vec3 normal = cross(dX, dY);
        float normalLen = length(normal);
        normal = (normalLen > 1.0e-6) ? (normal / normalLen) : vec3(0.0, 1.0, 0.0);

        const float kMaxPointLightDistSq = PointShadowFarPlane * PointShadowFarPlane;

        for (int i = 0; i < count; ++i) {
            vec3 toLight = PointLights[i].position - FragPos;
            float distSq = dot(toLight, toLight);
            if (distSq >= kMaxPointLightDistSq) {
                continue;
            }

            float distance = sqrt(distSq);
            if (distance > 1.0e-4) {
                vec3 lightDir = toLight / distance;
                float diff = abs(dot(normal, lightDir));
                if (diff < 0.001) {
                    continue;
                }

                float attenuation = 1.0 / (PointLights[i].constant +
                                          PointLights[i].linear * distance +
                                          PointLights[i].quadratic * distSq);

                if (attenuation * PointLights[i].intensity < 0.001) {
                    continue;
                }

                float shadow = 0.0;
                if (i < NumShadowPointLights) {
                    shadow = CalculatePointShadow(i, diff, -toLight, distance);
                }

                dynamicLight += PointLights[i].color * (PointLights[i].intensity * diff * attenuation * (1.0 - shadow));
            }
        }

        if (Flashlight.enabled) {
            vec3 spotDir = Flashlight.position - FragPos;
            float distSq = dot(spotDir, spotDir);
            const float kMaxSpotDistSq = ShadowRenderDistance * ShadowRenderDistance;
            if (distSq < kMaxSpotDistSq) {
                float distance = sqrt(distSq);
                if (distance > 1.0e-4) {
                    spotDir /= distance;
                    // Flashlight.direction is already normalized on CPU
                    float theta = dot(-spotDir, Flashlight.direction);
                    float epsilon = Flashlight.innerCutOff - Flashlight.outerCutOff;
                    float spotIntensity = clamp((theta - Flashlight.outerCutOff) / max(epsilon, 1.0e-4), 0.0, 1.0);

                    if (spotIntensity > 0.0) {
                        float diff = abs(dot(normal, spotDir));
                        if (diff > 0.001) {
                            float attenuation = 1.0 / (Flashlight.constant +
                                                      Flashlight.linear * distance +
                                                      Flashlight.quadratic * distSq);
                            float shadow = CalculateFlashlightShadow(FragPosLightSpace, diff, distance);
                            dynamicLight += Flashlight.color * (Flashlight.intensity * diff * spotIntensity * attenuation * (1.0 - shadow));
                        }
                    }
                }
            }
        }
    }

    vec3 totalLight = ambient + bakedLight + dynamicLight;
    fragmentColor = vec4(baseColor.rgb * totalLight, baseColor.a);
}
