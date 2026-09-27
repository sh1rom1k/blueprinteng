#version 450 core

in vec3 vertexColor;
in vec2 textureCoord;
in vec2 lightmapCoord;
in float lightStyle;
in vec3 FragPos;
in vec4 FragPosLightSpace;

out vec4 fragmentColor;

uniform sampler2D Texture0;
uniform bool UseTexture;
uniform sampler2DArray Lightmap0;
uniform bool UseLightmap;
uniform float Opacity;
uniform bool DecalPass;
uniform int AlphaMode;

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

#define MAX_MAP_SPOTS 8
uniform int NumMapSpots;
uniform SpotLight MapSpots[MAX_MAP_SPOTS];

uniform vec3 SunDirection;
uniform vec3 SunColor;
uniform float SunIntensity;
uniform vec3 AmbientColor;
uniform float LightStyleValues[64];
uniform bool FogEnabled;
uniform vec3 FogColor;
uniform float FogStart;
uniform float FogEnd;
uniform float FogMaxDensity;
uniform vec3 CameraPos;
uniform bool UseSpriteTint;
uniform vec3 SpriteTint;
uniform vec3 PropTint;
uniform bool UseEnvmap;
uniform vec3 EnvmapTint;
uniform samplerCube Envmap0;
uniform bool WarpSurface;
uniform float WaterSurfaceY;
uniform bool CameraUnderwater;

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
    if (AlphaMode == 4 && UseTexture) {
        fragmentColor = vec4(texColor.rgb, 1.0);
        return;
    }
    if (AlphaMode == 3) {
        vec3 glow = UseTexture ? texColor.rgb : vertexColor;
        if (UseSpriteTint) {
            glow *= SpriteTint;
        }
        float glowAlpha = UseTexture ? texColor.a : 1.0;
        fragmentColor = vec4(glow, glowAlpha * Opacity);
        return;
    }
    float alpha = Opacity;
    if (UseTexture && AlphaMode == 1) {
        alpha = texColor.a > 0.97 ? Opacity : texColor.a * Opacity;
    } else if (UseTexture && AlphaMode == 2 && texColor.a < 0.5) {
        discard;
    }
    vec4 baseColor = UseTexture
        ? vec4(texColor.rgb, alpha)
        : vec4(vertexColor, Opacity);
    if (UseSpriteTint) {
        baseColor.rgb *= SpriteTint;
    }

    if (DecalPass) {
        if (baseColor.a < 0.02) {
            discard;
        }
    }

    vec3 bakedLight = vec3(0.0);
    if (lightStyle < -0.5) {
        bakedLight = vertexColor;
    } else if (UseLightmap) {
        bakedLight = texture(Lightmap0, vec3(lightmapCoord, 0.0)).rgb;
        int styleId = int(lightStyle + 0.5);
        if (styleId > 0 && styleId < 64) {
            bakedLight += texture(Lightmap0, vec3(lightmapCoord, 1.0)).rgb * LightStyleValues[styleId];
        }
        bakedLight = min(bakedLight, vec3(1.0));
    }
    // Props and other unlit models have no lightmap. The sun is already baked
    // into the world, so a full light_environment here blows them out.
    vec3 propAmbient = dot(PropTint, PropTint) > 1.0e-6 ? PropTint : vec3(0.35);
    vec3 ambient = (UseLightmap || lightStyle < -0.5) ? vec3(0.0) : propAmbient;

    vec3 dynamicLight = vec3(0.0);
    int count = clamp(NumPointLights, 0, MAX_POINT_LIGHTS);
    int spotCount = clamp(NumMapSpots, 0, MAX_MAP_SPOTS);

    // Compute normal only if dynamic lights are active, avoiding screen-space derivatives when unnecessary
    vec3 shadedNormal = vec3(0.0, 1.0, 0.0);
    bool hasNormal = false;
    if (count > 0 || Flashlight.enabled || spotCount > 0 || UseEnvmap) {
        vec3 dX = dFdx(FragPos);
        vec3 dY = dFdy(FragPos);
        vec3 normal = cross(dX, dY);
        float normalLen = length(normal);
        normal = (normalLen > 1.0e-6) ? (normal / normalLen) : vec3(0.0, 1.0, 0.0);
        shadedNormal = normal;
        hasNormal = true;

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

        const float kMaxMapSpotDistSq = 128.0 * 128.0;
        for (int s = 0; s < spotCount; ++s) {
            if (!MapSpots[s].enabled) {
                continue;
            }
            vec3 spotDir = MapSpots[s].position - FragPos;
            float distSq = dot(spotDir, spotDir);
            if (distSq >= kMaxMapSpotDistSq || distSq <= 1.0e-8) {
                continue;
            }
            float distance = sqrt(distSq);
            spotDir /= distance;
            float theta = dot(-spotDir, MapSpots[s].direction);
            float epsilon = MapSpots[s].innerCutOff - MapSpots[s].outerCutOff;
            float spotIntensity = clamp((theta - MapSpots[s].outerCutOff) / max(epsilon, 1.0e-4), 0.0, 1.0);
            if (spotIntensity <= 0.0) {
                continue;
            }
            float diff = abs(dot(normal, spotDir));
            if (diff <= 0.001) {
                continue;
            }
            float attenuation = 1.0 / (MapSpots[s].constant +
                                      MapSpots[s].linear * distance +
                                      MapSpots[s].quadratic * distSq);
            if (attenuation * MapSpots[s].intensity < 0.001) {
                continue;
            }
            dynamicLight += MapSpots[s].color * (MapSpots[s].intensity * diff * spotIntensity * attenuation);
        }
    }

    vec3 color = baseColor.rgb * (ambient + bakedLight + dynamicLight);
    if (UseEnvmap && hasNormal) {
        vec3 viewDir = normalize(CameraPos - FragPos);
        vec3 reflection = reflect(-viewDir, shadedNormal);
        vec3 cubeDir = vec3(reflection.x, reflection.y, -reflection.z);
        float fresnel = pow(1.0 - clamp(dot(shadedNormal, viewDir), 0.0, 1.0), 4.0);
        color += texture(Envmap0, cubeDir).rgb * EnvmapTint * mix(0.08, 1.0, fresnel);
    }
    if (WarpSurface) {
        color *= vec3(0.62, 0.82, 0.86);
        if (CameraUnderwater && FragPos.y < WaterSurfaceY) {
            color = mix(color, vec3(0.12, 0.32, 0.38), 0.45);
        }
    }
    if (FogEnabled && FogEnd > FogStart) {
        float dist = distance(FragPos, CameraPos);
        float reach = max(FogEnd - FogStart, 1.0e-4);
        float fog = clamp((dist - FogStart) / reach, 0.0, 1.0) * clamp(FogMaxDensity, 0.0, 1.0);
        color = mix(color, FogColor, fog);
    }
    fragmentColor = vec4(color, baseColor.a);
}
