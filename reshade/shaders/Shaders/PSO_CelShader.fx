/*
 * PSO_CelShader.fx  -  ReShade FX port of the anzz1 PSOBB-widescreen Cel shader.
 *
 * Cel shading from GSFx Shader Suite by Asmodean.
 * PSOBB DX9 .fx packaging: anzz1 (psobb_patches / psobb_widescreen).
 * ReShade FX port: this repo. Color-only (no depth); math + defaults preserved
 *   1:1 from anzz1's CelShader.fx.
 *
 * NOTE: Asmodean's GSFx suite has no formal OSS license. Bundling this port is
 *   gated on owner go/no-go; the MIT fallback is SweetFX Cartoon.fx.
 */

#include "ReShade.fxh"

uniform float EdgeStrength <
    ui_type = "slider"; ui_min = 0.0; ui_max = 4.0; ui_step = 0.01; ui_label = "Edge Strength";
> = 0.70;

uniform float EdgeFilter <
    ui_type = "slider"; ui_min = 0.10; ui_max = 2.0; ui_step = 0.01; ui_label = "Edge Filter";
> = 0.60;

uniform float EdgeThickness <
    ui_type = "slider"; ui_min = 0.50; ui_max = 4.0; ui_step = 0.01; ui_label = "Edge Thickness";
> = 0.80;

uniform int PaletteType <
    ui_type = "combo"; ui_items = "Game Original\0Animated Shading\0Water Painting\0";
    ui_label = "Palette Type";
    ui_tooltip = "anzz1 default = 1 (Game Original). The combo is 0-based; 0=GameOriginal,1=Animated,2=Water.";
> = 0;

uniform int UseYuvLuma <
    ui_type = "combo"; ui_items = "Base Color Luma\0YUV Luma\0"; ui_label = "Luma Mode";
> = 1;

uniform int ColorRounding <
    ui_type = "combo"; ui_items = "Off\0On\0"; ui_label = "Color Rounding";
> = 1;

static const float2 rcpres = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
static const float3 lumCoeff = float3(0.2126729, 0.7151522, 0.0721750);

float AvgLuminance(float3 color)
{
    return sqrt(color.x * color.x * lumCoeff.x +
                color.y * color.y * lumCoeff.y +
                color.z * color.z * lumCoeff.z);
}

float3 RGBtoYUV(float3 RGB)
{
    const float3x3 m = float3x3(
        0.2126,  0.7152,  0.0722,
       -0.09991,-0.33609, 0.436,
        0.615,  -0.55861,-0.05639);
    return mul(m, RGB);
}

float3 YUVtoRGB(float3 YUV)
{
    const float3x3 m = float3x3(
        1.000, 0.000,   1.28033,
        1.000,-0.21482,-0.38059,
        1.000, 2.12798, 0.000);
    return mul(m, YUV);
}

float4 PS_Cel(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = float4(tex2D(ReShade::BackBuffer, uv).rgb, 1.0);
    float3 sum   = color.rgb;

    const int NUM = 9;
    const float2 RoundingOffset = float2(0.25, 0.25);
    const float3 thresholds = float3(9.0, 8.0, 6.0);

    const float2 set[9] = {
        float2(-0.0078125, -0.0078125), float2(0.00, -0.0078125), float2(0.0078125, -0.0078125),
        float2(-0.0078125,  0.00),      float2(0.00,  0.00),      float2(0.0078125,  0.00),
        float2(-0.0078125,  0.0078125), float2(0.00,  0.0078125), float2(0.0078125,  0.0078125)
    };

    [unroll]
    for (int i = 0; i < NUM; i++)
    {
        float3 c = tex2D(ReShade::BackBuffer, uv + set[i] * RoundingOffset).rgb;
        if (ColorRounding == 1)
        {
            c.r = round(c.r * thresholds.r) / thresholds.r;
            c.g = round(c.g * thresholds.g) / thresholds.g;
            c.b = round(c.b * thresholds.b) / thresholds.b;
        }
        float l = AvgLuminance(c);
        float3 yuv = RGBtoYUV(c);
        if (UseYuvLuma == 0) yuv.r = round(yuv.r * thresholds.r) / thresholds.r;
        else                 yuv.r = saturate(round(yuv.r * l) / thresholds.r + l);
        sum += YUVtoRGB(yuv);
    }

    float3 shadedColor = sum / NUM;
    float2 pixel = float2(rcpres.x * EdgeThickness, rcpres.y * EdgeThickness);

    float edgeX = dot(tex2D(ReShade::BackBuffer, uv + pixel).rgb, lumCoeff);
    edgeX = dot(float4(tex2D(ReShade::BackBuffer, uv - pixel).rgb, edgeX), float4(lumCoeff, -1.0));

    float edgeY = dot(tex2D(ReShade::BackBuffer, uv + float2(pixel.x, -pixel.y)).rgb, lumCoeff);
    edgeY = dot(float4(tex2D(ReShade::BackBuffer, uv + float2(-pixel.x, pixel.y)).rgb, edgeY), float4(lumCoeff, -1.0));

    float edge = dot(float2(edgeX, edgeY), float2(edgeX, edgeY));

    // anzz1's PaletteType is 1-based (1/2/3); our combo is 0-based (0/1/2).
    if (PaletteType == 0)
        color.rgb = lerp(color.rgb, color.rgb + pow(edge, EdgeFilter) * -EdgeStrength, EdgeStrength);
    else if (PaletteType == 1)
        color.rgb = lerp(color.rgb + pow(edge, EdgeFilter) * -EdgeStrength, shadedColor, 0.25);
    else
        color.rgb = lerp(shadedColor + edge * -EdgeStrength, pow(edge, EdgeFilter) * -EdgeStrength + color.rgb, 0.5);

    return saturate(float4(color.rgb, 1.0));
}

technique PSO_CelShader <
    ui_label = "PSO Cel Shader (anzz1 / Asmodean)";
    ui_tooltip = "Edge-outline cel shading. Color-only (no depth).";
>
{
    pass Cel { VertexShader = PostProcessVS; PixelShader = PS_Cel; }
}
