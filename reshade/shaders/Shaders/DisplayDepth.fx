/*
 * DisplayDepth.fx  -  depth-buffer acceptance test for the PSOBB ReShade package.
 *
 * This is the GATE for SSAO + DOF. Enable it and look:
 *   - a smooth grayscale gradient, near = dark, far = light  => depth WORKS.
 *   - flat gray / pure white / pure black / colored normals / static garbage
 *     => depth is NOT available on this chain. Re-run the installer with
 *        -NoDepth (drops SSAO + DOF; keeps SMAA/Cel/HDR).
 *
 * Reduced standalone variant of crosire's DisplayDepth.fx (BSD-3-Clause).
 * Tune the depth preprocessor defines below if the gradient is inverted or
 * flat (use the ReShade overlay's "Edit global preprocessor definitions").
 */

#include "ReShade.fxh"

uniform int iUIShow <
    ui_type = "combo";
    ui_items = "Depth (linearized gradient)\0Raw depth\0Normals (derived)\0";
    ui_label = "Display Mode";
> = 0;

float3 NormalVector(float2 texcoord)
{
    float3 offset = float3(ReShade::PixelSize.xy, 0.0);
    float2 posCenter = texcoord.xy;
    float2 posNorth  = posCenter - offset.zy;
    float2 posEast   = posCenter + offset.xz;
    float3 vertCenter = float3(posCenter - 0.5, 1.0) * ReShade::GetLinearizedDepth(posCenter);
    float3 vertNorth  = float3(posNorth  - 0.5, 1.0) * ReShade::GetLinearizedDepth(posNorth);
    float3 vertEast   = float3(posEast   - 0.5, 1.0) * ReShade::GetLinearizedDepth(posEast);
    return normalize(cross(vertCenter - vertNorth, vertCenter - vertEast)) * 0.5 + 0.5;
}

float4 PS_Display(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    if (iUIShow == 2) return float4(NormalVector(uv), 1.0);
    float d = ReShade::GetLinearizedDepth(uv);
    if (iUIShow == 1) return float4(d.xxx, 1.0);     // raw linearized
    // mode 0: emphasize the near range so the gradient is visible in-scene
    return float4(saturate(d).xxx, 1.0);
}

technique DisplayDepth <
    ui_label = "DisplayDepth (depth acceptance test)";
    ui_tooltip = "Enable to verify the depth buffer before trusting SSAO/DOF.";
>
{
    pass Display { VertexShader = PostProcessVS; PixelShader = PS_Display; }
}
