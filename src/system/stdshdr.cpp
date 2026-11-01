
#include "gfx.h"

namespace GFX
{

const std::string GFXEngine:: _stdPShaderText =
"\
#version 410 core\n\
layout(std140) uniform Parameters\
{\
    mat4 MProj;\
    mat4 MView;\
    vec3 Fog;\
    vec3 AlphaFog;\
    bool Textured;\
    bool Flat;\
    bool ATest;\
};\
uniform sampler2D texSample;\
in vec4 smoothColor;\
flat in vec4 flatColor;\
in vec2 texCoords;\
out vec4 fragColor;\
void main()\
{\
    vec4 clr;\
    if (Flat) clr = flatColor; else clr = smoothColor;\
    if (Textured)\
        clr = texture(texSample, texCoords) * clr;\
    if (ATest && clr.w <= 0.0)\
        discard;\
    fragColor = clr;\
}";


const std::string GFXEngine:: _stdVShaderText =
"\
#version 410 core\n\
layout(std140) uniform Parameters\
{\
    mat4 MProj;\
    mat4 MView;\
    vec3 Fog;\
    vec3 AlphaFog;\
    bool Textured;\
    bool Flat;\
    bool ATest;\
};\
in vec3 vPos;\
in vec4 vColor;\
in vec2 vUV;\
\
out vec4 smoothColor;\
flat out vec4 flatColor;\
out vec2 texCoords;\
void main()\
{\
    vec4 tformed = MView * vec4(vPos, 1.0);\
    gl_Position = MProj * tformed;\
\
    vec4 clr;\
    if (Fog.x != 0.0)\
    {\
        float fg = clamp((tformed.z - Fog.y) / Fog.z, 0.0, 1.0);\
        clr = vec4(mix(vColor.xyz, vec3(0.0), fg), vColor.w);\
    }\
    else\
    {\
        clr = vColor;\
    }\
    \
    if (AlphaFog.x != 0.0)\
    {\
        clr.w *= 1.0 - clamp((tformed.z - AlphaFog.y) / AlphaFog.z, 0.0, 1.0);\
    }\
    flatColor = clr;\
    smoothColor = clr;\
    if (Textured) texCoords = vUV;\
}";


// ---------------------------------------------------------------------------
// Reforge renderer, UA_RENDER_V2=1. Used only for meshes carrying a
// TPbrMaterial (modern OBJ assets); original meshes never reach it.
// Lighting in linear space: albedo decoded from sRGB, one directional sun
// (+ flat ambient) calibrated against the game's baked polygon shade values
// (least-squares fit), GGX specular from the ORM map,
// Khronos PBR Neutral tonemap (identity below ~0.76, filmic shoulder above, so
// UA's palette is not shifted), sRGB re-encode, then the game's depth cue
// (mix to black) exactly as the legacy shader applies it.
const std::string GFXEngine:: _v2VShaderText = R"GLSL(#version 410 core
layout(std140) uniform Parameters
{
    mat4 MProj;
    mat4 MView;
    vec3 Fog;
    vec3 AlphaFog;
    bool Textured;
    bool Flat;
    bool ATest;
};
in vec3 vPos;
in vec4 vColor;
in vec2 vUV;
in vec3 vNormal;
in vec4 vTangent;
out vec3 vsPos;
out vec3 vsNorm;
out vec4 vsTan;
out vec2 texCoords;
out vec4 baseColor;
out float fogMul;
void main()
{
    vec4 tformed = MView * vec4(vPos, 1.0);
    gl_Position = MProj * tformed;
    mat3 m = mat3(MView);
    mat3 nm = transpose(inverse(m));
    vsPos = tformed.xyz;
    vsNorm = nm * vNormal;
    // A mirroring model-view flips cross(N,T); keep the bitangent on the
    // side the UVs say it is.
    vsTan = vec4(m * vTangent.xyz, vTangent.w * sign(determinant(m)));
    fogMul = 1.0;
    if (Fog.x != 0.0)
        fogMul = 1.0 - clamp((tformed.z - Fog.y) / Fog.z, 0.0, 1.0);
    baseColor = vColor;
    if (AlphaFog.x != 0.0)
        baseColor.w *= 1.0 - clamp((tformed.z - AlphaFog.y) / AlphaFog.z, 0.0, 1.0);
    texCoords = vUV;
}
)GLSL";

const std::string GFXEngine:: _v2PShaderText = R"GLSL(#version 410 core
layout(std140) uniform Parameters
{
    mat4 MProj;
    mat4 MView;
    vec3 Fog;
    vec3 AlphaFog;
    bool Textured;
    bool Flat;
    bool ATest;
};
uniform sampler2D texSample;
uniform sampler2D normSample;
uniform sampler2D ormSample;
uniform sampler2D emiSample;
uniform vec3 SunDir;     // view space, unit, pointing TOWARDS the sun
uniform vec3 SunColor;   // linear, "albedo units" (pi * irradiance)
uniform vec3 Ambient;    // linear
uniform int HasNormal;
uniform int HasOrm;
uniform int HasEmissive;
uniform vec2 RoughMetal;
uniform vec3 Emission;
uniform float Exposure;  // 1 = off
uniform vec3 HemiUp;     // view space world up
uniform vec3 HemiDelta;  // (sky - ground) / 2; 0 = flat ambient
uniform int ToneMap;     // 1 = Khronos PBR Neutral, 2 = Neutral without the toe, 0 = clamp only
uniform int UseAo;       // 1 = ORM.r occludes the ambient term
uniform int AlbedoLinear;// 1 = texSample is a GL sRGB texture (returns linear)
uniform float NormalScale; // mtl `norm -bm s`: tangent-space xy scale
uniform int NormalRG;      // 1 = BC5 (RG) normal map, z rebuilt
uniform int Debug;       // 0 off; 1 albedo (sRGB), 2 normal, 3 orm, 4 uv, 5 baseColor, 6 lit before tonemap,
                         // 7 geometric (vertex) normal, 8 geometric N.V (grey)
uniform int Terms;       // lighting terms, bit mask (31 = all, the default): 1 sun diffuse, 2 sun specular,
                         // 4 ambient diffuse, 8 ambient Fresnel, 16 emissive (far-v2 attribution only)
uniform vec4 FarLod;     // far-v2, all x far fade (0 = off; close-ups always 0): x silhouette darkening strength,
                         // y geometric N.V where it starts, z N.V where it is full, w specular + ambient Fresnel cut
uniform int NormalMip;   // 0 renormalise the filtered normal (as before), 1 + Toksvig roughness from its length,
                         // 2 + fade the map to flat by the sampled normal-map LOD (FarNormLod x..y)
uniform vec2 FarNormLod;
uniform float FarGain;   // far-v2: gain on the diffuse terms that replaces the energy of the cut (1 = off)
in vec3 vsPos;
in vec3 vsNorm;
in vec4 vsTan;
in vec2 texCoords;
in vec4 baseColor;
in float fogMul;
out vec4 fragColor;
const float PI = 3.14159265;
vec3 toLin(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}
vec3 toSrgb(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
// Khronos PBR Neutral tone mapper (2024 reference implementation).
vec3 neutralTonemap(vec3 color, bool toe)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    if (toe)
    {
        float x = min(color.r, min(color.g, color.b));
        float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
        color -= offset;
    }
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, newPeak * vec3(1.0), g);
}
void main()
{
    vec4 alb = vec4(baseColor.rgb, 1.0);
    vec3 albedo;
    if (AlbedoLinear != 0)
    {
        vec4 t = Textured ? texture(texSample, texCoords) : vec4(1.0);
        alb.w = t.w;
        albedo = toLin(baseColor.rgb) * t.rgb;
    }
    else
    {
        if (Textured)
            alb *= texture(texSample, texCoords);
        albedo = toLin(alb.rgb);
    }
    if (ATest && alb.w <= 0.0)
        discard;

    vec3 N = normalize(vsNorm);
    vec3 Ng = N;
    float nlen = 1.0;
    if (HasNormal != 0)
    {
        vec3 T = normalize(vsTan.xyz - N * dot(N, vsTan.xyz));
        vec3 B = cross(N, T) * vsTan.w;
        vec3 m = texture(normSample, texCoords).xyz * 2.0 - 1.0;
        if (NormalRG != 0) // two-channel BC5 map: rebuild z from xy
            m.z = sqrt(max(0.0, 1.0 - dot(m.xy, m.xy)));
        if (NormalMip != 0)
        {
            nlen = clamp(length(m), 1e-3, 1.0);
            if (NormalMip == 2)
                m.xy *= 1.0 - smoothstep(FarNormLod.x, FarNormLod.y, textureQueryLod(normSample, texCoords).y);
        }
        m.xy *= NormalScale;
        N = normalize(T * m.x + B * m.y + N * m.z);
    }

    float ao = 1.0;
    float rough = RoughMetal.x;
    float metal = RoughMetal.y;
    if (HasOrm != 0)
    {
        vec3 o = texture(ormSample, texCoords).rgb;
        if (UseAo != 0)
            ao = o.r;
        rough *= o.g;
        metal *= o.b;
    }
    rough = clamp(rough, 0.045, 1.0);
    if (NormalMip == 1 && nlen < 1.0)
    {
        // Toksvig: the filtered normal's shortening is its variance; widen the
        // GGX lobe by it (alpha^2 += (1 - |n|) / |n|), so far mips do not sparkle.
        float at = rough * rough;
        rough = clamp(sqrt(sqrt(min(1.0, at * at + (1.0 - nlen) / nlen))), 0.045, 1.0);
    }

    vec3 V = normalize(-vsPos);
    vec3 L = SunDir;
    vec3 H = normalize(L + V);
    float NdL = max(dot(N, L), 0.0);
    float NdV = max(dot(N, V), 1e-4);
    float NdH = max(dot(N, H), 0.0);
    float VdH = max(dot(V, H), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metal);
    float a = rough * rough;
    float a2 = a * a;
    float dd = NdH * NdH * (a2 - 1.0) + 1.0;
    float D = a2 / (PI * dd * dd);
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    float G = (NdL / (NdL * (1.0 - k) + k)) * (NdV / (NdV * (1.0 - k) + k));
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdH, 5.0);
    vec3 specBrdf = D * G * F / max(4.0 * NdL * NdV, 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metal);

    vec3 lit;
    // Flat ambient: diffuse + a roughness-aware Fresnel term so metals are
    // not black without an environment map.
    vec3 Fa = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - NdV, 5.0);
    if (Terms == 31 && FarLod.w == 0.0 && FarGain == 1.0)
    {
        lit = (kd * albedo + specBrdf * PI) * SunColor * NdL;
        lit += (albedo * (1.0 - metal) + Fa) * (Ambient + HemiDelta * dot(N, HemiUp)) * ao;
        if (HasEmissive != 0)
            lit += toLin(texture(emiSample, texCoords).rgb) * Emission;
    }
    else
    {
        // Same terms, separable: attribution (Terms) and the far-v2 cut of the
        // achromatic specular + ambient-Fresnel floor (FarLod.w).
        vec3 amb = (Ambient + HemiDelta * dot(N, HemiUp)) * ao;
        float keep = mix(1.0 - FarLod.w, 1.0, metal); // metals keep theirs: it is their colour
        lit = vec3(0.0);
        if ((Terms & 1) != 0) lit += kd * albedo * SunColor * NdL;
        if ((Terms & 4) != 0) lit += albedo * (1.0 - metal) * amb;
        lit *= FarGain; // multiplicative, so black stays black and the hue stays the albedo's
        if ((Terms & 2) != 0) lit += specBrdf * PI * SunColor * NdL * keep;
        if ((Terms & 8) != 0) lit += Fa * amb * keep;
        if ((Terms & 16) != 0 && HasEmissive != 0)
            lit += toLin(texture(emiSample, texCoords).rgb) * Emission;
    }
    float ndvG = max(dot(Ng, V), 0.0);
    if (FarLod.x > 0.0)
    {
        // far-v2 silhouette darkening: grazing geometric faces (hull sides
        // turning away, bevels at the outline) go dark, like the original's
        // dark rim. Scaled by the far fade, so 0 at close range.
        lit *= 1.0 - FarLod.x * (1.0 - smoothstep(FarLod.z, FarLod.y, ndvG));
    }
    lit *= Exposure;

    if (Debug != 0)
    {
        vec3 dbg = toSrgb(lit);
        if (Debug == 1) dbg = toSrgb(albedo);
        else if (Debug == 2) dbg = N * 0.5 + 0.5;
        else if (Debug == 3) dbg = HasOrm != 0 ? texture(ormSample, texCoords).rgb : vec3(1.0, RoughMetal);
        else if (Debug == 4) dbg = vec3(fract(texCoords), 0.0);
        else if (Debug == 5) dbg = baseColor.rgb;
        else if (Debug == 7) dbg = Ng * 0.5 + 0.5;
        else if (Debug == 8) dbg = vec3(ndvG);
        fragColor = vec4(dbg, 1.0);
        return;
    }
    vec3 outc = toSrgb(ToneMap != 0 ? neutralTonemap(lit, ToneMap == 1) : lit) * fogMul;
    fragColor = vec4(outc, alb.w * baseColor.w);
}
)GLSL";

}
