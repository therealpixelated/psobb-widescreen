/*
 * ephinea.dll — Widescreen Cascade: Stage B
 * Original: FUN_52dabbd0 @ 0x52DABBD0
 *
 * Computes render-size and selects aspect-multiplier from a 7-level breakpoint
 * ladder. Called by Stage A (FUN_52da9930) after registry/config load.
 *
 * Formula (verified against decompile — see docs/PATCHES_OVERVIEW.md §2):
 *   gameRenderW = (640 * S * aspectMult) / (4/3)   [= 480 * S * aspectMult]
 *   gameRenderH = 480 * S
 *
 * Where S is the scale factor (HUD scale or aspect-derived) and aspectMult is
 * selected from a 7-level nested-if ladder keyed on display aspect ratio.
 */

#include <math.h>

/* ── Engine globals (patched at runtime) ── */

/* Read by Stage A from registry: display width/height */
extern float displayW;   /* _DAT_5318d210 */
extern float displayH;   /* _DAT_5318dcdc */

/* Written by this function */
extern float gameRenderW;      /* _DAT_5318deec = render target width */
extern float gameRenderH;      /* _DAT_5318d124 = render target height */
extern float hudScaleFactor;   /* _DAT_5387b918 = S (master scale) */

/* Breakpoint ladder constants — all in .rdata (undumped, RPM-only).
 * Structure is verified; exact values per §2 of PATCHES_OVERVIEW.
 * The 7 breakpoints ascend: _53175ee8 < _53175f00 < _53175f10 < ...
 */
extern float aspectBreakpoint[7];  /* e.g. {1.30, 1.45, 1.60, 1.70, 1.85, 2.00, 2.35} ... exact RPM-only */

/* Per-bucket aspect multiplier (aspectMult = renderW/renderH base ratio) */
extern float aspectMultTable[7];   /* _DAT_53175e78, _53175e8c, ..., _53176000 */

/* Per-bucket secondary constants — used elsewhere in the scale pipeline */
extern float bucketScale[7];     /* _DAT_53175ea0, _53175ea4, _53175eb8, ... */
extern float bucketDelta[7];     /* _DAT_53175fa8, _53175f20, _53175fd8, ...  */

/* Widescreen mode flags (read by this function) */
extern int   widescreenEnabled;  /* _DAT_53857a5c — 0=off, non-zero=on */
extern float hudScalePercent;    /* _DAT_5382fe9c — HUD scale as percentage (0 = use default) */

/* Base constants (RPM-only, structure verified) */
#define STOCK_WIDTH     640.0f   /* _DAT_53176128 */
#define STOCK_HEIGHT    480.0f   /* _DAT_53176120 */
#define ASPECT_4_3      1.33333f /* _DAT_53175e8c */
#define HUNDRED         100.0f   /* _DAT_53175fc0 */

/* ── Extra globals scaled by S (10 writes) ── */
extern float g_0097a910;   /* _DAT_0097a910 (dVar3/100) */
extern float g_0096e114;   /* _DAT_0096e114 */
extern float g_006f49fd;   /* _DAT_006f49fd */
extern float g_006f4a57;   /* _DAT_006f4a57 */
extern float g_0096e168;   /* _DAT_0096e168 */
extern float g_0096e16c;   /* _DAT_0096e16c */
extern float g_0096e170;   /* _DAT_0096e170 */
extern float g_0096e174;   /* _DAT_0096e174 */
extern float g_0096e178;   /* _DAT_0096e178 */
extern float g_0096e17c;   /* _DAT_0096e17c */
extern float g_5318efa4;   /* _DAT_5318efa4 (func_0x524b7c70 call result) */
extern float g_5318ee38;   /* _DAT_5318ee38 (func_0x524b7c70 call result) */

/* Internal bucket-pair state (written but consumed downstream, not in this function) */
extern float g_5318d21c;   /* _DAT_5318d21c */
extern float g_5318da7c;   /* _DAT_5318da7c */
extern float g_5318f038;   /* _DAT_5318f038 = aspectMult (also used directly) */


/*
 * stage_b_compute_render_size
 *
 * Reads displayW/displayH, computes aspect ratio, selects the correct bucket
 * from the 7-level breakpoint ladder, sets gameRenderW/gameRenderH, and
 * applies the scale factor S to 10+ engine globals.
 */
void stage_b_compute_render_size(void)
{
    float aspect;
    float S;          /* master scale factor */
    float aspectMult; /* aspect multiplier for selected bucket */
    int   bucket;     /* selected breakpoint bucket index (0-6) */

    aspect = displayW / displayH;

    /* ── 7-level nested-if breakpoint ladder ──
     *
     * Structure (from decompile):
     *   if(≥bp[0]){ if(≥bp[1]){ if(≥bp[2]){ if(≥bp[3]){
     *     if(≥bp[4]){ if(≥bp[5]){ if(≥bp[6]) bucket=6; else bucket=5; }
     *                  else bucket=4; }
     *     else bucket=3; }
     *   else bucket=2; }
     *   else bucket=1; }
     *   else bucket=0;
     *
     * Each bucket sets:
     *   g_5318d21c = bucketDelta[bucket]
     *   g_5318da7c = bucketScale[bucket] (or fixed value for some buckets)
     *   aspectMult = aspectMultTable[bucket]
     *   S          = bucketScale[bucket] (or 1.0 for widescreen=OFF case)
     */

    if (aspect >= aspectBreakpoint[0]) {
        if (aspect >= aspectBreakpoint[1]) {
            if (aspect >= aspectBreakpoint[2]) {
                if (aspect >= aspectBreakpoint[3]) {
                    if (aspect >= aspectBreakpoint[4]) {
                        if (aspect >= aspectBreakpoint[5]) {
                            if (aspect >= aspectBreakpoint[6]) {
                                /* Bucket 6 (widest) */
                                g_5318d21c = bucketDelta[6];
                                g_5318da7c = /* see bucketScale[5] variant */;
                                aspectMult = aspectMultTable[6];
                                S = bucketScale[6];
                            } else {
                                /* Bucket 5 */
                                g_5318d21c = bucketDelta[5];
                                g_5318da7c = bucketDelta[4]; /* (note: delta, not scale) */
                                aspectMult = aspectMultTable[5];
                                S = bucketScale[5];
                            }
                        } else {
                            /* Bucket 4 */
                            g_5318d21c = bucketDelta[4];
                            g_5318da7c = bucketDelta[4]; /* (note: delta, not scale) */
                            aspectMult = aspectMultTable[4];
                            S = bucketScale[4];
                        }
                    } else {
                        /* Bucket 3 */
                        g_5318d21c = bucketDelta[4];
                        g_5318da7c = bucketDelta[4];
                        aspectMult = aspectMultTable[3];
                        S = bucketScale[4];  /* (uses bucket 4's scale) */
                    }
                } else {
                    /* Bucket 2 */
                    g_5318d21c = bucketDelta[4];
                    g_5318da7c = bucketScale[2];
                    aspectMult = aspectMultTable[2];
                    S = bucketScale[2];
                }
            } else {
                /* Bucket 1 */
                g_5318d21c = bucketScale[0];  /* (delta, not scale) */
                g_5318da7c = bucketScale[1];  /* (HALF) */
                aspectMult = aspectMultTable[1];
                S = bucketScale[1];
            }
        } else {
            /* Bucket 0 (narrowest widescreen) */
            g_5318d21c = bucketScale[1];  /* (delta) */
            g_5318da7c = bucketScale[0];  /* (delta) */
            aspectMult = aspectMultTable[0];
            S = bucketScale[1];
        }
    } else {
        /* Below lowest breakpoint — 4:3 or narrower */
        g_5318d21c = bucketScale[2];  /* (delta from bucket 2) */
        g_5318da7c = bucketScale[1];  /* (delta from bucket 1) */
        aspectMult = aspectMultTable[7]; /* = 4/3 (identity — no widescreen stretch) */
        S = bucketScale[1];
    }

    /* ── Widescreen OFF vs ON ──
     *
     * When widescreen is disabled (_DAT_53857a5c == 0):
     *   S is overridden to 1.0, and a separate dVar3 computation runs.
     *
     * When enabled and hudScalePercent != 0:
     *   S = hudScalePercent / 100
     *
     * Otherwise S stays at the bucket-selected value.
     */
    if (!widescreenEnabled) {
        S = 1.0f;
        /* dVar3 = (displayH / 480) * _53175ef8 * ((_5318d1e4) + table_lookup(...)) */
        /* This path computes a legacy-format scaling factor. The full decompile
         * at reference/ephinea_decompiled_PRIORBUILD.c:133466-133474 documents
         * the exact table lookup. */
        float dVar3 = (displayH / STOCK_HEIGHT) * /* _DAT_53175ef8 */ 1.0f
                    * /* table lookup */ 1.0f;  /* placeholder — see decompile for exact constants */
        g_0097a910 = dVar3 / HUNDRED;
    } else {
        if (hudScalePercent != 0.0f) {
            S = hudScalePercent / HUNDRED;
        }
        /* dVar3 = (displayH / (S * _53175fd0)) * _53175ef8 * table_lookup(...) */
        float dVar3 = (displayH / (S * /* _DAT_53175fd0 */ 1.0f))
                    * /* _DAT_53175ef8 */ 1.0f
                    * /* table lookup */ 1.0f;  /* placeholder — see decompile */
        g_0097a910 = dVar3 / HUNDRED;
    }

    /* ── Core render-size writes ── */
    gameRenderW = (STOCK_WIDTH * S * aspectMult) / ASPECT_4_3;
    gameRenderH = STOCK_HEIGHT * S;

    /* ── func_0x524b7c70 calls (likely sqrt or round) ── */
    g_5318efa4 = func_0x524b7c70(/* _DAT_531760bc */ 1.0f * S);
    g_5318ee38 = func_0x524b7c70((/* _DAT_531760bc */ 1.0f * gameRenderW) / STOCK_WIDTH);

    /* ── 10 extra `*= S` writes ── */
    g_0096e114 *= S;
    g_006f49fd *= S;
    g_006f4a57 *= S;
    g_0096e168 *= S;
    g_0096e16c *= S;
    g_0096e170 *= S;
    g_0096e174 *= S;
    g_0096e178 *= S;
    g_0096e17c *= S;

    /* ── Store master scale ── */
    hudScaleFactor = S;
}