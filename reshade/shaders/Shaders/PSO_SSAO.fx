/*
 * PSO_SSAO.fx  -  ReShade FX port of the anzz1 PSOBB-widescreen SSAO shader.
 *
 * Original SSAO technique: Arkano22
 *   (https://www.gamedev.net/topic/550699-ssao-no-halo-artifacts/)
 * GLSL assembly / optimization: Martins Upitis (martinsh)
 *   (devlog-martinsh.blogspot.com)
 * PSOBB DX9 .fx adaptation: anzz1 (psobb_patches / psobb_widescreen)
 * ReShade FX port: this repo. Math + tuning preserved 1:1 from anzz1's SSAO.fx;
 *   the wrapper-space depth read is replaced by ReShade::GetLinearizedDepth so
 *   anzz1's nearZ/farZ (0.004 / 1.0) become depth-linearization knobs rather
 *   than raw texture ranges (see RESHADE_DEPTH_LINEARIZATION_FAR_PLANE).
 *
 * Requires a working depth buffer (ReShade Add-on edition + Generic Depth).
 * Verify with DisplayDepth.fx first; if depth is flat/garbage this effect is
 * a no-op / noise.
 *
 * Pass layout matches the original: AO -> H-blur -> V-blur -> combine.
 */

#include "ReShade.fxh"

uniform int fSSAO_Samples <
    ui_type = "slider"; ui_min = 4; ui_max = 32; ui_label = "Samples";
    ui_tooltip = "AO sample count (anzz1: 16).";
> = 16;

uniform int fSSAO_minsamples <
    ui_type = "slider"; ui_min = 1; ui_max = 8; ui_label = "Min Samples";
> = 2;

uniform float fSSAO_Radius <
    ui_type = "slider"; ui_min = 1.0; ui_max = 32.0; ui_step = 0.1; ui_label = "Radius";
> = 12.0;

uniform float fSSAO_aoClamp <
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "AO Clamp";
    ui_tooltip = "Depth clamp - reduces edge haloing (anzz1: 0.1).";
> = 0.1;

uniform float fSSAO_diffarea <
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Self-shadow Reduction";
> = 0.2;

uniform float fSSAO_gdisplace <
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Gauss Center";
> = 0.4;

uniform float fSSAO_aoWidth <
    ui_type = "slider"; ui_min = 0.5; ui_max = 6.0; ui_step = 0.1; ui_label = "Gauss Width";
> = 2.0;

uniform float fSSAO_noiseamount <
    ui_type = "slider"; ui_min = 0.0; ui_max = 0.01; ui_step = 0.0001; ui_label = "Noise Amount";
> = 0.0002;

uniform float fSSAO_lumThreshold <
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Luminance Threshold";
> = 0.3;

uniform float fSSAO_miststart <
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Mist Start";
> = 0.05;

uniform float fSSAO_mistend <
    ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01; ui_label = "Mist End";
> = 0.7;

#ifndef PSO_SSAO_PI
#define PSO_SSAO_PI 3.14159265
#endif

// Two ping-pong RTs: a pass may not sample the same texture it renders to, so
// AO->HBlur->VBlur alternate between A and B.
texture PSO_SSAO_TexA { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler PSO_SSAO_SampA { Texture = PSO_SSAO_TexA; AddressU = CLAMP; AddressV = CLAMP; };
texture PSO_SSAO_TexB { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler PSO_SSAO_SampB { Texture = PSO_SSAO_TexB; AddressU = CLAMP; AddressV = CLAMP; };

static const float2 rcpres  = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
static const float  width   = BUFFER_WIDTH;
static const float  height  = BUFFER_HEIGHT;

// Linearized [0..1] depth. anzz1's readDepth() collapses to this under ReShade.
float GetDepth(float2 uv) { return ReShade::GetLinearizedDepth(uv); }

float2 rand(float2 coord)
{
    float noiseX = ((frac(1.0 - coord.x * (width / 2.0)) * 0.25) + (frac(coord.y * (height / 2.0)) * 0.75)) * 2.0 - 1.0;
    float noiseY = ((frac(1.0 - coord.x * (width / 2.0)) * 0.75) + (frac(coord.y * (height / 2.0)) * 0.25)) * 2.0 - 1.0;
    return float2(noiseX, noiseY) * 2.0;
}

float doMist(float2 coord)
{
    // anzz1 mist uses the linearized depth directly.
    float depth = GetDepth(coord);
    return clamp((depth - fSSAO_miststart) / fSSAO_mistend, 0.0, 1.0);
}

float compareDepths(float depth1, float depth2, inout int far)
{
    float garea = fSSAO_aoWidth;
    float diff  = (depth1 - depth2) * 100.0;
    if (diff < fSSAO_gdisplace) garea = fSSAO_diffarea; else far = 1;
    float gauss = pow(2.7182, -2.0 * (diff - fSSAO_gdisplace) * (diff - fSSAO_gdisplace) / (garea * garea));
    return gauss;
}

float calAO(float2 tex, float depth, float dw, float dh)
{
    float dd = fSSAO_Radius - depth;
    float temp = 0.0, temp2 = 0.0;
    float2 coord  = float2(tex.x + dw * dd, tex.y + dh * dd);
    float2 coord2 = float2(tex.x - dw * dd, tex.y - dh * dd);
    int far = 0;
    temp = compareDepths(depth, GetDepth(coord), far);
    if (far > 0)
    {
        temp2 = compareDepths(GetDepth(coord2), depth, far);
        temp += (1.0 - temp) * temp2;
    }
    return temp;
}

float4 PS_SSAO(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float2 noise = rand(uv);
    float  depth = GetDepth(uv);
    float  fog   = doMist(uv);

    float3 color    = tex2D(ReShade::BackBuffer, uv).rgb;
    float3 lumcoeff = float3(0.299, 0.587, 0.114);
    float  lum      = dot(color, lumcoeff);

    float w = (1.0 / width)  / clamp(depth, fSSAO_aoClamp, 1.0) * noise.x;
    float h = (1.0 / height) / clamp(depth, fSSAO_aoClamp, 1.0) * noise.y;

    float ao = 0.0;
    int samp = int(float(fSSAO_minsamples) + clamp(1.0 - (fog + pow(lum, 4.0)), 0.0, 1.0) * (float(fSSAO_Samples) - float(fSSAO_minsamples)));

    float dl = PSO_SSAO_PI * (3.0 - sqrt(5.0));
    float dz = 1.0 / float(samp);
    float l  = 0.0;
    float z  = 1.0 - dz / 2.0;

    [loop]
    for (int i = 0; i <= samp; i++)
    {
        float r  = sqrt(1.0 - z);
        float pw = cos(l) * r * (1.0 - doMist(uv));
        float ph = sin(l) * r * (1.0 - doMist(uv));
        ao += calAO(uv, depth, pw * w, ph * h);
        z -= dz;
        l += dl;
    }

    ao /= float(samp) + 0.1;
    ao  = 1.0 - ao;
    ao  = lerp(ao, 1.0, fog);
    return float4(ao, ao, ao, 1.0);
}

// Gaussian separable blur on the AO buffer (anzz1 BLUR_GAUSSIAN path).
// HBlur reads A, writes B.  VBlur reads B, writes A.  Combine reads A.
float4 PS_HBlur(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float c = tex2D(PSO_SSAO_SampA, uv).r;
    float b = c * 0.2270270270;
    b += tex2D(PSO_SSAO_SampA, uv + float2(rcpres.x * 1.3846153846, 0)).r * 0.3162162162;
    b += tex2D(PSO_SSAO_SampA, uv - float2(rcpres.x * 1.3846153846, 0)).r * 0.3162162162;
    b += tex2D(PSO_SSAO_SampA, uv + float2(rcpres.x * 3.2307692308, 0)).r * 0.0702702703;
    b += tex2D(PSO_SSAO_SampA, uv - float2(rcpres.x * 3.2307692308, 0)).r * 0.0702702703;
    return float4(b, b, b, 1.0);
}

float4 PS_VBlur(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float c = tex2D(PSO_SSAO_SampB, uv).r;
    float b = c * 0.2270270270;
    b += tex2D(PSO_SSAO_SampB, uv + float2(0, rcpres.y * 1.3846153846)).r * 0.3162162162;
    b += tex2D(PSO_SSAO_SampB, uv - float2(0, rcpres.y * 1.3846153846)).r * 0.3162162162;
    b += tex2D(PSO_SSAO_SampB, uv + float2(0, rcpres.y * 3.2307692308)).r * 0.0702702703;
    b += tex2D(PSO_SSAO_SampB, uv - float2(0, rcpres.y * 3.2307692308)).r * 0.0702702703;
    return float4(b, b, b, 1.0);
}

float4 PS_Combine(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = tex2D(ReShade::BackBuffer, uv);
    float  ao    = clamp(tex2D(PSO_SSAO_SampA, uv).r, fSSAO_aoClamp, 1.0);

    // LUMINANCE_CONSIDERATION: lift AO off bright pixels (anzz1).
    float luminance = (color.r * 0.2125) + (color.g * 0.7154) + (color.b * 0.0721);
    luminance = clamp(3.0 * max(0.0, luminance - fSSAO_lumThreshold), 0.0, 1.0);
    ao = lerp(ao, 1.0, luminance);

    color.rgb *= ao;
    return color;
}

technique PSO_SSAO <
    ui_label = "PSO SSAO (anzz1)";
    ui_tooltip = "Screen-space ambient occlusion. Needs a working depth buffer.";
>
{
    pass AO     { VertexShader = PostProcessVS; PixelShader = PS_SSAO;    RenderTarget = PSO_SSAO_TexA; }
    pass HBlur  { VertexShader = PostProcessVS; PixelShader = PS_HBlur;   RenderTarget = PSO_SSAO_TexB; }
    pass VBlur  { VertexShader = PostProcessVS; PixelShader = PS_VBlur;   RenderTarget = PSO_SSAO_TexA; }
    pass Combine{ VertexShader = PostProcessVS; PixelShader = PS_Combine; }
}
