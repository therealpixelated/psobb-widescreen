/*
 * ephinea.dll — Widescreen Cascade: the "92-VA" function
 * Original: FUN_52da9280 @ 0x52DA9280
 *
 * 97 arithmetic writes across 93 distinct VAs in five factor classes L1–L5,
 * plus 11 inline arithmetic and 2 non-arithmetic operations.
 *
 * Factor classes:
 *   L1 (W-mul):      *val *= (gameRenderW / 640)         — 23 VAs
 *   L2 (H-mul):      *val *= (gameRenderH / 480)         — 17 VAs
 *   L3 (W-add):      *val += (gameRenderW - 640) / 2     — 19 VAs
 *   L4 (H-add):      *val += (gameRenderH - 480) / 2     — 20 VAs
 *   L5 (H-add-raw):  *val += (gameRenderH - 480)         — 7 VAs
 *   inline:          mixed display/render ratios          — 11 VAs
 *   non-arith:       pointer redirect + code patch         — 2 ops
 *
 * NOTE: This is called FROM Stage C (FUN_52da7ff0). It is NOT the complete
 * widescreen apply — Stage C writes ~700+ more globals around this call.
 *
 * Addresses in this listing are engine .data VAs (the DESTINATIONS of the
 * arithmetic). They match PATCHES_OVERVIEW.md §4.
 */

#include <stdint.h>

extern float gameRenderW;   /* _DAT_5318deec */
extern float gameRenderH;   /* _DAT_5318d124 */
extern float displayW;      /* _DAT_5318d210 */
extern float displayH;      /* _DAT_5318dcdc */

#define STOCK_WIDTH  640.0f
#define STOCK_HEIGHT 480.0f
#define HALF          2.0f

/* Pointer helper: read/write a float at a given absolute VA */
#define PTR(va)       ((float*)(va))
#define PTR_U32(va)   ((uint32_t*)(va))

/*
 * stage_c_92va_apply — Apply 97 arithmetic writes across 93 VAs.
 *
 * Order of operations (from decompile):
 *   1. L1: W-mul for first 23 VAs (local_1ac array)
 *   2. L2: H-mul for first 17 VAs (local_b4[0..0x10])
 *   3. L3: W-add for first 19 VAs (local_100 + local_bc + local_b8)
 *   4. L4: H-add for 20 VAs (local_150 + local_10c + local_108 + local_104)
 *   5. L5: H-add-raw for 7 VAs
 *   6. Inline arithmetic (11 writes)
 *   7. Non-arithmetic (2 ops)
 */
void stage_c_92va_apply(void)
{
    float w_factor  = gameRenderW / STOCK_WIDTH;   /* width multiplier */
    float h_factor  = gameRenderH / STOCK_HEIGHT;  /* height multiplier */
    float w_delta   = (gameRenderW - STOCK_WIDTH);  /* raw width delta */
    float h_delta   = (gameRenderH - STOCK_HEIGHT); /* raw height delta */
    float w_add     = w_delta / HALF;               /* half-width delta (letterbox) */
    float h_add     = h_delta / HALF;               /* half-height delta (letterbox) */

    /* ───── L1 — W-mul (23 VAs): *val *= (gameRenderW / 640) ───── */

    /* Engine .data anchors (15 VAs) */
    *PTR(0x008FA154) *= w_factor;   /* L1[0]  */
    *PTR(0x008FA15C) *= w_factor;   /* L1[1]  */
    *PTR(0x008FA144) *= w_factor;   /* L1[2]  */
    *PTR(0x008FA14C) *= w_factor;   /* L1[3]  */
    *PTR(0x008FA134) *= w_factor;   /* L1[4]  */
    *PTR(0x008FA13C) *= w_factor;   /* L1[5]  */
    *PTR(0x008FA124) *= w_factor;   /* L1[6]  */
    *PTR(0x008FA12C) *= w_factor;   /* L1[7]  */
    *PTR(0x008FA0F8) *= w_factor;   /* L1[8]  */
    *PTR(0x008FA098) *= w_factor;   /* L1[9]  */
    *PTR(0x008FA068) *= w_factor;   /* L1[10] */
    *PTR(0x008FA0C8) *= w_factor;   /* L1[11] */
    *PTR(0x008FA1A4) *= w_factor;   /* L1[12] */
    *PTR(0x008F9EC0) *= w_factor;   /* L1[13] */
    *PTR(0x008FA1C0) *= w_factor;   /* L1[14] (also inline displayW/renderW) */

    /* Engine .text imm32 operands (8 VAs — inside PsoBB.exe opcodes) */
    *PTR(0x0040CA07) *= w_factor;   /* L1[15] */
    *PTR(0x0040CA40) *= w_factor;   /* L1[16] */
    *PTR(0x0040CA66) *= w_factor;   /* L1[17] */
    *PTR(0x0040CA8C) *= w_factor;   /* L1[18] */
    *PTR(0x0040CAE2) *= w_factor;   /* L1[19] */
    *PTR(0x0040CB1E) *= w_factor;   /* L1[20] */
    *PTR(0x0040CB44) *= w_factor;   /* L1[21] */
    *PTR(0x0040CB6A) *= w_factor;   /* L1[22] */

    /* ───── L2 — H-mul (17 VAs): *val *= (gameRenderH / 480) ───── */

    *PTR(0x008FA148) *= h_factor;   /* L2[0]  */
    *PTR(0x008FA150) *= h_factor;   /* L2[1]  */
    *PTR(0x008FA128) *= h_factor;   /* L2[2]  */
    *PTR(0x008FA130) *= h_factor;   /* L2[3]  */
    *PTR(0x008FA0F4) *= h_factor;   /* L2[4]  */
    *PTR(0x008FA0C4) *= h_factor;   /* L2[5]  */
    *PTR(0x008FA0D4) *= h_factor;   /* L2[6]  */
    *PTR(0x008FA094) *= h_factor;   /* L2[7]  */
    *PTR(0x008FA0A4) *= h_factor;   /* L2[8]  */
    *PTR(0x008FA064) *= h_factor;   /* L2[9]  */
    *PTR(0x008FA074) *= h_factor;   /* L2[10] */
    *PTR(0x008FA044) *= h_factor;   /* L2[11] */
    *PTR(0x008FA04C) *= h_factor;   /* L2[12] */
    *PTR(0x008FA024) *= h_factor;   /* L2[13] */
    *PTR(0x008FA02C) *= h_factor;   /* L2[14] */
    *PTR(0x008FA104) *= h_factor;   /* L2[15] */
    *PTR(0x008F9EFC) *= h_factor;   /* L2[16] */

    /* ───── L3 — W-add (19 VAs): *val += (gameRenderW - 640) / 2 ─────
     *
     * The original doc (EPHINEA_92VA_PATCH_LIST.md) claimed 16 VAs for L3.
     * Real count from decompile is 19 — three VAs were omitted
     * (0x008FA1D8, 0x008F9F18, 0x008F9F20). */

    /* Main array (16 VAs, local_100[0..0xF]) */
    *PTR(0x008FA108) += w_add;      /* L3[0]  */
    *PTR(0x008FA110) += w_add;      /* L3[1]  */
    *PTR(0x008FA118) += w_add;      /* L3[2]  */
    *PTR(0x008FA0D8) += w_add;      /* L3[3]  (X position, e.g. 459 → 565.667 at 16:9) */
    *PTR(0x008FA0E0) += w_add;      /* L3[4]  */
    *PTR(0x008FA0E8) += w_add;      /* L3[5]  */
    *PTR(0x008FA0A8) += w_add;      /* L3[6]  */
    *PTR(0x008FA0B0) += w_add;      /* L3[7]  */
    *PTR(0x008FA0B8) += w_add;      /* L3[8]  */
    *PTR(0x008FA078) += w_add;      /* L3[9]  */
    *PTR(0x008FA080) += w_add;      /* L3[10] */
    *PTR(0x008FA088) += w_add;      /* L3[11] */
    *PTR(0x008FA050) += w_add;      /* L3[12] */
    *PTR(0x008FA058) += w_add;      /* L3[13] */
    *PTR(0x008FA030) += w_add;      /* L3[14] */
    *PTR(0x008FA038) += w_add;      /* L3[15] */

    /* DOC-OMITTED VAs (3) */
    *PTR(0x008FA1D8) += w_add;      /* L3[16] — DOC OMITS */
    *PTR(0x008F9F18) += w_add;      /* L3[17] — DOC OMITS */
    *PTR(0x008F9F20) += w_add;      /* L3[18] — DOC OMITS */

    /* ───── L4 — H-add (20 VAs): *val += (gameRenderH - 480) / 2 ─────
     *
     * Prior doc claimed 16 VAs. Real count is 20 — four VAs omitted
     * (0x008FA1D0, 0x008FA1AC, 0x008F9F1C, 0x008F9F24). */

    /* Main array (16 VAs, local_150[0..0xF]) */
    *PTR(0x008FA10C) += h_add;      /* L4[0]  */
    *PTR(0x008FA114) += h_add;      /* L4[1]  */
    *PTR(0x008FA11C) += h_add;      /* L4[2]  */
    *PTR(0x008FA0DC) += h_add;      /* L4[3]  */
    *PTR(0x008FA0E4) += h_add;      /* L4[4]  */
    *PTR(0x008FA0EC) += h_add;      /* L4[5]  */
    *PTR(0x008FA0AC) += h_add;      /* L4[6]  */
    *PTR(0x008FA0B4) += h_add;      /* L4[7]  */
    *PTR(0x008FA0BC) += h_add;      /* L4[8]  */
    *PTR(0x008FA07C) += h_add;      /* L4[9]  */
    *PTR(0x008FA084) += h_add;      /* L4[10] */
    *PTR(0x008FA08C) += h_add;      /* L4[11] */
    *PTR(0x008FA054) += h_add;      /* L4[12] */
    *PTR(0x008FA05C) += h_add;      /* L4[13] */
    *PTR(0x008FA034) += h_add;      /* L4[14] */
    *PTR(0x008FA03C) += h_add;      /* L4[15] */

    /* DOC-OMITTED VAs (4) */
    *PTR(0x008FA1D0) += h_add;      /* L4[16] — DOC OMITS */
    *PTR(0x008FA1AC) += h_add;      /* L4[17] — DOC OMITS (also inline dual-listed) */
    *PTR(0x008F9F1C) += h_add;      /* L4[18] — DOC OMITS */
    *PTR(0x008F9F24) += h_add;      /* L4[19] — DOC OMITS */

    /* ───── L5 — H-add-raw (7 VAs): *val += (gameRenderH - 480) ─────
     *
     * NOTE: NO divide by 2! These are the raw height delta applied to
     * bottom-edge-anchored Y coordinates (unlike L4 which halves it). */

    *PTR(0x008FA138) += h_delta;     /* L5[0]  */
    *PTR(0x008FA140) += h_delta;     /* L5[1]  */

    /* Engine .text imm32 operands (4 VAs) */
    *PTR(0x0040CB07) += h_delta;     /* L5[2]  */
    *PTR(0x0040CB2C) += h_delta;     /* L5[3]  */
    *PTR(0x0040CB52) += h_delta;     /* L5[4]  */
    *PTR(0x0040CB78) += h_delta;     /* L5[5]  */

    *PTR(0x008F9F48) += h_delta;     /* L5[6]  */

    /* ───── Inline arithmetic (11 writes) ─────
     *
     * These mix displayW/displayH with renderW/renderH for special
     * coordinate transforms (full-screen overlay, viewport mapping). */

    /* displayW / renderW ratio applied to various anchors */
    *PTR(0x008FA1D8) *= (displayW / gameRenderW);  /* inline[0] — W-coord recenter */
    *PTR(0x008FA1D0) *= (displayH / gameRenderH);  /* inline[1] — H-coord recenter */
    *PTR(0x008FA1D4) *= (displayW / gameRenderW);  /* inline[2] */
    *PTR(0x008FA1CC) *= (displayH / gameRenderH);  /* inline[3] */
    *PTR(0x008FA1C4) *= (displayW / gameRenderW);  /* inline[4] */
    *PTR(0x008FA1C0) *= (displayW / gameRenderW);  /* inline[5] (also L1 dual-listed) */

    /* DLL-internal scratch */
    *PTR(0x5318E980) *= (displayW / STOCK_WIDTH);  /* inline[6] */

    /* Engine .text imm32 */
    *PTR(0x0040D039) *= (displayH / gameRenderH);  /* inline[7] */

    /* Special formula: val = (displayH/renderH)*(val - 480) + displayH */
    *PTR(0x0040D032) = (displayH / gameRenderH) * (*PTR(0x0040D032) - STOCK_HEIGHT) + displayH;

    /* Special formula: val = (displayW/renderW)*((renderW-640)/2 + val) */
    *PTR(0x0040D1DD) = (displayW / gameRenderW) * (w_add + *PTR(0x0040D1DD));

    /* Dual-listed inline + L4 */
    *PTR(0x008FA1AC) *= (displayH / gameRenderH);  /* inline[10] */

    /* ───── Non-arithmetic (2 ops) ───── */

    /* Pointer redirect: writes the ADDRESS of 0x5318E980 to 0x0040D04E
     * (replaces an engine pointer with the scaled scratch value) */
    *PTR_U32(0x0040D04E) = (uint32_t)(uintptr_t)(0x5318E980);

    /* Code patch: installs 11 bytes at 0x0040D305 via helper function.
     * This is a runtime detour/opcode patch in the engine's .text section.
     * See FUN_52dc3460 in the detour installer family. */
    FUN_52dc3460(0x0040D305, 0xB);
}