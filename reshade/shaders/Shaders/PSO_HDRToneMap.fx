/*
 * PSO_HDRToneMap.fx  -  ReShade FX port of the anzz1 PSOBB-widescreen HDR tonemap.
 *
 * HDR Scene Tonemapping from GSFx Shader Suite by Asmodean.
 * PSOBB DX9 .fx packaging: anzz1 (psobb_patches / psobb_widescreen).
 * ReShade FX port: this repo. Color-only (no depth); curve + defaults preserved
 *   1:1 from anzz1's HDRToneMap.fx.
 *
 * NOTE: Asmodean's GSFx suite has no formal OSS license. Bundling this port is
 *   gated on owner go/no-go; the MIT fallback is SweetFX Tonemap + LevelsPlus.
 */

#include "ReShade.fxh"

uniform int TonemapType <
    ui_type = "combo"; ui_items = "LDR\0HDR (original)\0Filmic HDR A\0Filmic HDR B\0";
    ui_label = "Tonemap Type";
> = 1;

uniform int TonemapMask <
    ui_type = "combo"; ui_items = "Off\0On\0"; ui_label = "Tone Masking";
> = 0;

uniform float MaskStrength <
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.001; ui_label = "Mask Strength";
> = 0.30;

uniform float ToneAmount <
    ui_type = "slider"; ui_min = 0.050; ui_max = 1.0; ui_step = 0.001; ui_label = "Tone Amount";
> = 0.300;

uniform float BlackLevels <
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.001; ui_label = "Black Levels";
> = 0.060;

uniform float Exposure <
    ui_type = "slider"; ui_min = 0.100; ui_max = 2.0; ui_step = 0.001; ui_label = "Exposure";
> = 1.000;

uniform float Luminance <
    ui_type = "slider"; ui_min = 0.100; ui_max = 2.0; ui_step = 0.001; ui_label = "Luminance";
> = 1.000;

uniform float WhitePoint <
    ui_type = "slider"; ui_min = 0.100; ui_max = 2.0; ui_step = 0.001; ui_label = "White Point";
> = 0.940;

static const float Epsilon = 1e-10;
static const float3 lumCoeff = float3(0.2126729, 0.7151522, 0.0721750);

float AvgLuminance(float3 color)
{
    return sqrt(color.x * color.x * lumCoeff.x +
                color.y * color.y * lumCoeff.y +
                color.z * color.z * lumCoeff.z);
}

float3 EncodeGamma(float3 color, float gamma)
{
    color = saturate(color);
    color.r = (color.r <= 0.0404482362771082) ? color.r / 12.92 : pow((color.r + 0.055) / 1.055, gamma);
    color.g = (color.g <= 0.0404482362771082) ? color.g / 12.92 : pow((color.g + 0.055) / 1.055, gamma);
    color.b = (color.b <= 0.0404482362771082) ? color.b / 12.92 : pow((color.b + 0.055) / 1.055, gamma);
    return color;
}

float3 ScaleLuminance(float3 x)
{
    const float W = 1.02;
    const float L = 0.06;
    const float C = 1.02;
    const float N = clamp(0.76 + ToneAmount, 1.0, 2.0);
    const float K = (N - L * C) / C;
    float3 tone = L * C + (1.0 - L * C) * (1.0 + K * (x - L) / ((W - L) * (W - L))) * (x - L) / (x - L + K);
    return (x > L) ? tone : C * x;
}

float3 TmMask(float3 color)
{
    float3 tone = color;
    float highTone = 6.2; float greyTone = 0.5;
    float midTone = 1.620; float lowTone = 0.066;
    tone.r = (tone.r * (highTone * tone.r + greyTone)) / (tone.r * (highTone * tone.r + midTone) + lowTone);
    tone.g = (tone.g * (highTone * tone.g + greyTone)) / (tone.g * (highTone * tone.g + midTone) + lowTone);
    tone.b = (tone.b * (highTone * tone.b + greyTone)) / (tone.b * (highTone * tone.b + midTone) + lowTone);
    tone = EncodeGamma(tone, 2.42);
    return lerp(color, tone, MaskStrength);
}

float3 TmCurve(float3 color)
{
    float3 T = color;
    float blevel = length(T);
    float bmask = pow(blevel, 0.02);
    const float A = 0.100; const float B = 0.300; const float C = 0.100;
    const float D = ToneAmount; const float E = 0.020; const float F = 0.300;
    const float W = 1.000;
    T.r = ((T.r * (A * T.r + C * B) + D * E) / (T.r * (A * T.r + B) + D * F)) - E / F;
    T.g = ((T.g * (A * T.g + C * B) + D * E) / (T.g * (A * T.g + B) + D * F)) - E / F;
    T.b = ((T.b * (A * T.b + C * B) + D * E) / (T.b * (A * T.b + B) + D * F)) - E / F;
    float denom = ((W * (A * W + C * B) + D * E) / (W * (A * W + B) + D * F)) - E / F;
    T = (T / denom) * bmask;
    return saturate(T);
}

float4 PS_Tonemap(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float3 color = tex2D(ReShade::BackBuffer, uv).rgb;
    float3 tonemap = color;

    float blackLevel = length(tonemap);
    tonemap = ScaleLuminance(tonemap);

    float luminanceAverage = AvgLuminance(Luminance.xxx);

    if (TonemapMask == 1) tonemap = TmMask(tonemap);
    if (TonemapType == 1) tonemap = TmCurve(tonemap);

    const float3x3 RGB2XYZ = float3x3(0.4124564, 0.3575761, 0.1804375,
                                      0.2126729, 0.7151522, 0.0721750,
                                      0.0193339, 0.1191920, 0.9503041);
    float3 XYZ = mul(RGB2XYZ, tonemap);

    float3 Yxy;
    Yxy.r = XYZ.g;
    Yxy.g = XYZ.r / (XYZ.r + XYZ.g + XYZ.b);
    Yxy.b = XYZ.g / (XYZ.r + XYZ.g + XYZ.b);

    float Wt = saturate(Yxy.r / AvgLuminance(XYZ));
    if (TonemapType == 2) Yxy.r = TmCurve(Yxy).r;

    float Lp = Yxy.r * Exposure / (luminanceAverage + Epsilon);
    float Wp = dot(abs(Wt), WhitePoint);
    Yxy.r = (Lp * (1.0 + Lp / (Wp * Wp))) / (1.0 + Lp);

    XYZ.r = Yxy.r * Yxy.g / Yxy.b;
    XYZ.g = Yxy.r;
    XYZ.b = Yxy.r * (1.0 - Yxy.g - Yxy.b) / Yxy.b;

    if (TonemapType == 3) XYZ = TmCurve(XYZ);

    const float3x3 XYZ2RGB = float3x3( 3.2404542,-1.5371385,-0.4985314,
                                      -0.9692660, 1.8760108, 0.0415560,
                                       0.0556434,-0.2040259, 1.0572252);
    tonemap = mul(XYZ2RGB, XYZ);

    float shadowmask = pow(saturate(blackLevel), BlackLevels);
    tonemap *= shadowmask;

    return float4(tonemap, 1.0);
}

technique PSO_HDRToneMap <
    ui_label = "PSO HDR Tone Map (anzz1 / Asmodean)";
    ui_tooltip = "Scene tonemapping for deeper blacks / vivid color. Color-only (no depth).";
>
{
    pass Tonemap { VertexShader = PostProcessVS; PixelShader = PS_Tonemap; }
}
