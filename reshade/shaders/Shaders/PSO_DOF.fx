/*
 * PSO_DOF.fx  -  ReShade FX port of the anzz1 PSOBB-widescreen Depth-of-Field shader.
 *
 * Original DoF effect: WrinklyNinja (Release Version 7)
 * PSOBB tuning: Sodaboy (for Ephinea Phantasy Star Online: Blue Burst)
 * PSOBB DX9 .fx packaging: anzz1 (psobb_patches / psobb_widescreen)
 * ReShade FX port: this repo. Blur kernel + focus logic preserved 1:1; the
 *   wrapper depth read is replaced by ReShade::GetLinearizedDepth so anzz1's
 *   nearZ/farZ (0.01 / 1.0) become depth-linearization knobs.
 *
 * Requires a working depth buffer (ReShade Add-on edition + Generic Depth).
 * Verify with DisplayDepth.fx first.
 */

#include "ReShade.fxh"

uniform float2 f2FocusPosition <
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_label = "Focus Position";
    ui_tooltip = "Screen point that stays in focus (anzz1: 0.5, 0.9).";
> = float2(0.5, 0.9);

uniform float fFullFocusRange <
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Full Focus Range";
    ui_tooltip = "Within +/- this depth of the focus point: not blurred (anzz1: 0.1).";
> = 0.1;

uniform float fNoFocusRange <
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "No Focus Range";
    ui_tooltip = "Outside +/- this depth: maximum blur (anzz1: 0.4).";
> = 0.4;

uniform float fDoFAmount <
    ui_type = "slider"; ui_min = 0.0; ui_max = 60.0; ui_step = 0.5; ui_label = "DoF Amount";
    ui_tooltip = "Maximum blur strength (anzz1: 25).";
> = 25.0;

// anzz1 #define DISTANTBLUR : reduce to distance blur (far blurred, near sharp).
#ifndef PSO_DOF_DISTANTBLUR
#define PSO_DOF_DISTANTBLUR 1
#endif

static const float2 rcpres = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);

float LinearDepth(float2 coord) { return ReShade::GetLinearizedDepth(coord); }

float ComputePointBlurMagnitude(float focusdepth, float pointdepth)
{
    float difference = abs(pointdepth - focusdepth);
    float blurmag;
    if (difference <= fFullFocusRange)      blurmag = 0;
    else if (difference >= fNoFocusRange)   blurmag = 1;
    else                                    blurmag = difference / (fNoFocusRange - fFullFocusRange);

    if (pointdepth < 0.6) blurmag = 0;   // don't blur if too close
#if PSO_DOF_DISTANTBLUR
    if (pointdepth >= 0.9999) blurmag = 0;
#endif
    if (blurmag > 1.0) blurmag = 1.0;
    return blurmag * fDoFAmount;
}

float4 ComputeBlurVector(float FocusDepth, float s, float4 SurroundingPoints)
{
    float4 blurdirection = float4(0, 0, 0, 0); // (x+, x-, y+, y-)
    if (s < FocusDepth)
        blurdirection = float4(1, 1, 1, 1);
    else if (s > FocusDepth)
    {
        if (s <= SurroundingPoints.x) blurdirection.x = 1;
        if (s <= SurroundingPoints.y) blurdirection.y = 1;
        if (s <= SurroundingPoints.z) blurdirection.z = 1;
        if (s <= SurroundingPoints.w) blurdirection.w = 1;
    }
    float blurmagnitude = ComputePointBlurMagnitude(FocusDepth, s);
    return blurdirection * blurmagnitude;
}

float4 DoGaussianBlur(float2 UVCoord, float4 blurvector)
{
    const float BlurWeights[13] = {
        0.057424882, 0.058107773, 0.061460144, 0.071020611, 0.088092873,
        0.106530916, 0.114725602, 0.106530916, 0.088092873, 0.071020611,
        0.061460144, 0.058107773, 0.057424882
    };
    const float2 BlurOffsets[13] = {
        float2(-6.0 * rcpres.x, -6.0 * rcpres.y), float2(-5.0 * rcpres.x, -5.0 * rcpres.y),
        float2(-4.0 * rcpres.x, -4.0 * rcpres.y), float2(-3.0 * rcpres.x, -3.0 * rcpres.y),
        float2(-2.0 * rcpres.x, -2.0 * rcpres.y), float2(-1.0 * rcpres.x, -1.0 * rcpres.y),
        float2( 0.0,  0.0),                       float2( 1.0 * rcpres.x,  1.0 * rcpres.y),
        float2( 2.0 * rcpres.x,  2.0 * rcpres.y), float2( 3.0 * rcpres.x,  3.0 * rcpres.y),
        float2( 4.0 * rcpres.x,  4.0 * rcpres.y), float2( 5.0 * rcpres.x,  5.0 * rcpres.y),
        float2( 6.0 * rcpres.x,  6.0 * rcpres.y)
    };

    float4 Color = 0;
    [unroll]
    for (int i = 0; i < 13; i++)
    {
        if (i < 6)        Color += tex2D(ReShade::BackBuffer, UVCoord + (BlurOffsets[i] * float2(0,1) * blurvector.w) * BlurWeights[i]);
        else if (i == 6)  Color += tex2D(ReShade::BackBuffer, UVCoord);
        else              Color += tex2D(ReShade::BackBuffer, UVCoord + (BlurOffsets[i] * float2(0,1) * blurvector.z) * BlurWeights[i]);

        if (i < 6)        Color += tex2D(ReShade::BackBuffer, UVCoord + (BlurOffsets[i] * float2(1,0) * blurvector.y) * BlurWeights[i]);
        else if (i == 6)  Color += tex2D(ReShade::BackBuffer, UVCoord);
        else              Color += tex2D(ReShade::BackBuffer, UVCoord + (BlurOffsets[i] * float2(1,0) * blurvector.x) * BlurWeights[i]);
    }
    return Color / 26.0;
}

float4 PS_DoF(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float s          = LinearDepth(uv);
    float FocusDepth = LinearDepth(f2FocusPosition);

    float4 SurroundingPoints;
    SurroundingPoints.x = LinearDepth(uv + float2(rcpres.x, rcpres.y) * float2(-1,  0));
    SurroundingPoints.y = LinearDepth(uv + float2(rcpres.x, rcpres.y) * float2( 1,  0));
    SurroundingPoints.z = LinearDepth(uv + float2(rcpres.x, rcpres.y) * float2( 0, -1));
    SurroundingPoints.w = LinearDepth(uv + float2(rcpres.x, rcpres.y) * float2( 0,  1));

    float4 BlurVector = ComputeBlurVector(FocusDepth, s, SurroundingPoints);
    return float4(DoGaussianBlur(uv, BlurVector).rgb, 1.0);
}

technique PSO_DOF <
    ui_label = "PSO Depth of Field (anzz1)";
    ui_tooltip = "Distance-based depth of field. Needs a working depth buffer.";
>
{
    pass DoF { VertexShader = PostProcessVS; PixelShader = PS_DoF; }
}
