/*
 * ReShade.fxh  -  minimal, self-authored common header for the PSOBB shaders.
 *
 * This is an INDEPENDENT re-implementation of the small, well-known ReShade FX
 * common interface (the ReShade namespace, BackBuffer/DepthBuffer samplers,
 * GetLinearizedDepth, PostProcessVS, BUFFER_* metrics). It exists so the PSOBB
 * shaders in this repo compile against a stock ReShade 5.x / 6.x runtime
 * without bundling crosire's reshade-shaders package. The upstream interface it
 * mirrors is dedicated to the public domain (CC0-1.0).
 *
 * The depth-input transform defines below match upstream names so the ReShade
 * overlay's "Edit global preprocessor definitions" can tune them live. If
 * SSAO/DOF look wrong, these are the knobs (start by toggling
 * RESHADE_DEPTH_INPUT_IS_REVERSED and lowering RESHADE_DEPTH_LINEARIZATION_FAR_PLANE).
 */

#pragma once

#ifndef RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN
    #define RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN 0
#endif
#ifndef RESHADE_DEPTH_INPUT_IS_REVERSED
    #define RESHADE_DEPTH_INPUT_IS_REVERSED 0
#endif
#ifndef RESHADE_DEPTH_INPUT_IS_LOGARITHMIC
    #define RESHADE_DEPTH_INPUT_IS_LOGARITHMIC 0
#endif
#ifndef RESHADE_DEPTH_LINEARIZATION_FAR_PLANE
    #define RESHADE_DEPTH_LINEARIZATION_FAR_PLANE 1000.0
#endif
#ifndef RESHADE_DEPTH_MULTIPLIER
    #define RESHADE_DEPTH_MULTIPLIER 1
#endif
#ifndef RESHADE_DEPTH_INPUT_X_SCALE
    #define RESHADE_DEPTH_INPUT_X_SCALE 1
#endif
#ifndef RESHADE_DEPTH_INPUT_Y_SCALE
    #define RESHADE_DEPTH_INPUT_Y_SCALE 1
#endif
#ifndef RESHADE_DEPTH_INPUT_X_OFFSET
    #define RESHADE_DEPTH_INPUT_X_OFFSET 0
#endif
#ifndef RESHADE_DEPTH_INPUT_Y_OFFSET
    #define RESHADE_DEPTH_INPUT_Y_OFFSET 0
#endif
#ifndef RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET
    #define RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET 0
#endif
#ifndef RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET
    #define RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET 0
#endif

// Backbuffer metrics. ReShade defines BUFFER_WIDTH/HEIGHT as runtime constants.
#define BUFFER_PIXEL_SIZE float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT)
#define BUFFER_SCREEN_SIZE float2(BUFFER_WIDTH, BUFFER_HEIGHT)
#define BUFFER_ASPECT_RATIO (BUFFER_WIDTH * BUFFER_RCP_HEIGHT)

namespace ReShade
{
    static const float2 PixelSize  = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
    static const float2 ScreenSize = float2(BUFFER_WIDTH, BUFFER_HEIGHT);
    static const float  AspectRatio = BUFFER_WIDTH * BUFFER_RCP_HEIGHT;

    // The runtime backbuffer (sRGB-correct color) supplied by ReShade.
    texture BackBufferTex : COLOR;
    sampler BackBuffer { Texture = BackBufferTex; SRGBTexture = false; };

    // The depth buffer supplied by ReShade's Generic Depth addon.
    texture DepthBufferTex : DEPTH;
    sampler DepthBuffer { Texture = DepthBufferTex; };

    float GetLinearizedDepth(float2 texcoord)
    {
#if RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN
        texcoord.y = 1.0 - texcoord.y;
#endif
        texcoord.x = (texcoord.x - RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET * BUFFER_RCP_WIDTH) / RESHADE_DEPTH_INPUT_X_SCALE + RESHADE_DEPTH_INPUT_X_OFFSET / 2.0 * RESHADE_DEPTH_INPUT_X_SCALE;
        texcoord.y = (texcoord.y - RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET * BUFFER_RCP_HEIGHT) / RESHADE_DEPTH_INPUT_Y_SCALE + RESHADE_DEPTH_INPUT_Y_OFFSET / 2.0 * RESHADE_DEPTH_INPUT_Y_SCALE;

        float depth = tex2Dlod(DepthBuffer, float4(texcoord, 0, 0)).x * RESHADE_DEPTH_MULTIPLIER;

#if RESHADE_DEPTH_INPUT_IS_LOGARITHMIC
        const float C = 0.01;
        depth = (exp(depth * log(C + 1.0)) - 1.0) / C;
#endif
#if RESHADE_DEPTH_INPUT_IS_REVERSED
        depth = 1.0 - depth;
#endif
        const float N = 1.0;
        depth /= RESHADE_DEPTH_LINEARIZATION_FAR_PLANE - depth * (RESHADE_DEPTH_LINEARIZATION_FAR_PLANE - N);
        return depth;
    }
}

// Full-screen triangle vertex shader (the ReShade convention).
void PostProcessVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD)
{
    texcoord.x = (id == 2) ? 2.0 : 0.0;
    texcoord.y = (id == 1) ? 2.0 : 0.0;
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
