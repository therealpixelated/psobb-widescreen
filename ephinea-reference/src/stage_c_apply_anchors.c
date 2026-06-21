/*
 * ephinea.dll — Widescreen Cascade: Stage C
 * Original: FUN_52da7ff0 @ 0x52DA7FF0
 *
 * The REAL bulk anchor apply — writes ~700+ engine globals across six bulk
 * loops (~122/90/35/28/235/12 iterations) and ~80 scalar writes, THEN calls
 * the 92-VA function (FUN_52da9280) for an additional 97 arithmetic writes,
 * and continues writing after it returns.
 *
 * NOTE: This is NOT the "92-VA function". Prior documentation often conflates
 * the two. The 92-VA function is a *subset* called from here around line ~270.
 * A port that implements only the 92-VA function will be visibly wrong.
 *
 * See docs/PATCHES_OVERVIEW.md §2, §4 for the authoritative recipe.
 */

#include <windows.h>
#include <math.h>

/* ── Forward declarations ── */
void stage_c_92va_apply(void);           /* FUN_52da9280 — the 97 arithmetic writes */
void* alloc_engine_object(void);         /* FUN_52d46df0 */
void enum_display_modes(void);           /* D3D9/DDraw mode enumeration */
int  match_string_in_list(const char* list, const char* target); /* FUN_52d61a40 */
void set_detour_status(int enabled);     /* FUN_52dc32e0 */

/* ── Engine globals (referenced by address) ── */
extern float  gameRenderW;     /* _DAT_5318deec */
extern float  gameRenderH;     /* _DAT_5318d124 */
extern float  displayW;        /* _DAT_5318d210 */
extern float  displayH;        /* _DAT_5318dcdc */
extern float  hudScaleFactor;  /* _DAT_5387b918 = S */
extern float  g_5318f038;      /* _DAT_5318f038 = aspectMult */
extern float  g_5318d21c;      /* _DAT_5318d21c (from Stage B bucket) */
extern float  g_5318da7c;      /* _DAT_5318da7c (from Stage B bucket) */
extern float  g_5318ee38;      /* _DAT_5318ee38 (from Stage B) */
extern float  g_5318efa4;      /* _DAT_5318efa4 (from Stage B) */
extern int    g_numPlayers;    /* _DAT_53a9027c */
extern int    g_languageId;    /* _DAT_5388ed90 */

/* Base constants (RPM-only, structure verified per PATCHES_OVERVIEW §2) */
#define STOCK_WIDTH      640.0f    /* _DAT_53176128 */
#define STOCK_HEIGHT     480.0f    /* _DAT_53176120 */
#define HALF             2.0f      /* _DAT_53175f20 */
#define ONE              1.0f      /* _DAT_53175e64 */
#define ZERO             0.0f      /* _DAT_531760bc (used as zero sentinel) */

/* ── Bulk loop anchor address ranges ──
 *
 * Each loop iterates over an array of float pointers in the engine .data
 * section and writes the same computed value.
 *
 * Loop 1: 122 VAs at [0x5318F040]  ← gameRenderW
 * Loop 2:  90 VAs at [0x5318E6E0]  ← gameRenderH
 * Loop 3:  35 VAs at [0x5318E650]  += (gameRenderW - 640) / 2
 * Loop 4:  28 VAs at [0x5318F2A0]  += (gameRenderW - 640)
 * Loop 5: 235 VAs at [0x5318E9C8]  += (gameRenderH - 480)
 * Loop 6:  12 VAs at [0x5318EF00]  += (gameRenderH/2 - offset)
 *
 * Each [_DAT_5318xxxx] is a POINTER TO a float in PsoBB.exe's data section.
 * The loop writes *ptr = value for each entry (or += for loops 3-6).
 */

/* Anchor table bases (RVA in engine .data) */
#define ANCHOR_W          0x5318F040   /* 122 entries, gameRenderW */
#define ANCHOR_H          0x5318E6E0   /* 90 entries, gameRenderH */
#define ANCHOR_WADD_HALF  0x5318E650   /* 35 entries, += (W-640)/2 */
#define ANCHOR_WADD_FULL  0x5318F2A0   /* 28 entries, += (W-640) */
#define ANCHOR_HADD_FULL  0x5318E9C8   /* 235 entries, += (H-480) */
#define ANCHOR_HADD_HALF  0x5318EF00   /* 12 entries, += (H/2 - offset) */
#define ANCHOR_SCALE_W    0x5318F00C   /* 2 entries, *= (W/640) */
#define ANCHOR_SCALE_H    0x5318EF40   /* 20 entries, *= (H/480) */
#define ANCHOR_USHORT_H   0x5318E8FC   /* 12 entries, ushort*H */

/* ── Per-player scaling (multiplayer UI adjustments) ── */
#define MAX_PLAYERS       4


/*
 * stage_c_apply_anchors — Main widescreen anchor apply.
 *
 * Call order (verified from decompile):
 *   1. 12 PRNG/allocator calls (2×6 + 24 more — likely UI object creation)
 *   2. ~40 scalar writes (geometry derivations from gameRenderW/H)
 *   3. 12× aspectMult = g_5318f038
 *   4. Bucket constants → display
 *   5. 6 bulk loops (122/90/35/28/235/12 iterations)
 *   6. 2× W/H proportional scale (func_0x524b7cb0 = sqrt or lerp?)
 *   7. 20× H proportional scale
 *   8. 12× ushort H-scale
 *   9. ~10 color constant writes (0xff181818 alpha values)
 *  10. ~6 geometric recomputation (mix of gameRenderW/H derived)
 *  11. Per-player UI scaling (if g_numPlayers > n)
 *  12. Display-mode enumeration + language ID check
 *  13. Detour status set
 *  14. Clamp value computation
 *  15. *** CALLS FUN_52da9280 (92-VA) ***
 *  16. ~25 final scalar writes
 *  17. 6-element function pointer array setup
 *  18. Final increment write
 */
void stage_c_apply_anchors(void)
{
    int i;
    float temp;

    /* ── 1. Allocate 12 engine objects (2×6 batches) ──
     * These are likely UI widget/heap objects created for the widescreen
     * viewport. Each call to FUN_52d46df0 returns a handle/pointer. */
    for (i = 0; i < 6; i++) {
        *(void**)anchor_table_6[i] = alloc_engine_object();
    }
    for (i = 0; i < 6; i++) {
        *(void**)anchor_table_6b[i] = alloc_engine_object();
    }

    /* 24 more individual allocs (see raw decompile for exact _DAT_009a3xxx list) */
    g_009a3840 = alloc_engine_object();
    g_009a3844 = alloc_engine_object();
    /* ... (24 more) ... */
    g_009a38d8 = alloc_engine_object();

    /* ── 2. Scalar geometry derivations (~40 writes) ──
     *
     * Pattern: many writes take the form:
     *   ((g_5318ee38 - ZERO) / ZERO + ONE) * someConstant
     *
     * Where g_5318ee38 and g_5318efa4 were computed in Stage B from
     * func_0x524b7c70 calls (likely sqrt or round). The (x-ZERO)/ZERO+ONE
     * pattern is a normalization — if func_0x524b7c70 is sqrt, then this
     * computes a ratio relative to the stock value.
     *
     * See intermediate/stage_c_raw.c lines ~50-130 for the full list.
     */

    /* ── 3. 12× aspectMult writes ── */
    for (i = 0; i < 12; i++) {
        *(float**)ANCHOR_ASPECTTABLE[i] = g_5318f038;  /* _DAT_5318ee3c + i*4 */
    }

    /* ── 4. Bucket constants → display ── */
    g_0098a320 = g_5318d21c;   /* _DAT_0098a320 */
    g_0098a324 = g_5318da7c;   /* _DAT_0098a324 */

    /* ── 5. Six bulk loops ── */
    for (i = 0; i < 122; i++)  /* 0x7a */
        **(float**)(ANCHOR_W + i*4) = gameRenderW;

    for (i = 0; i < 90; i++)   /* 0x5a */
        **(float**)(ANCHOR_H + i*4) = gameRenderH;

    for (i = 0; i < 35; i++)   /* 0x23 */
        **(float**)(ANCHOR_WADD_HALF + i*4) += (gameRenderW - STOCK_WIDTH) / HALF;

    for (i = 0; i < 28; i++)   /* 0x1c */
        **(float**)(ANCHOR_WADD_FULL + i*4) += (gameRenderW - STOCK_WIDTH);

    for (i = 0; i < 235; i++)  /* 0xeb */
        **(float**)(ANCHOR_HADD_FULL + i*4) += (gameRenderH - STOCK_HEIGHT);

    for (i = 0; i < 12; i++)   /* 0xc */
        **(float**)(ANCHOR_HADD_HALF + i*4) += (gameRenderH / HALF - /* offset */ ZERO);

    /* ── 6. W/H proportional scale (2 entries) ──
     * func_0x524b7cb0 is likely a rounding/truncation function */
    for (i = 0; i < 2; i++) {
        float ratio = (**(float**)(ANCHOR_SCALE_W + i*4)) / STOCK_WIDTH;
        **(float**)(ANCHOR_SCALE_W + i*4) = round_func(ratio * gameRenderW);
    }

    /* ── 7. H proportional scale (20 entries) ── */
    for (i = 0; i < 20; i++) {
        float ratio = (**(float**)(ANCHOR_SCALE_H + i*4)) / STOCK_HEIGHT;
        **(float**)(ANCHOR_SCALE_H + i*4) = round_func(ratio * gameRenderH);
    }

    /* ── 8. ushort H-scale (12 entries) ── */
    for (i = 0; i < 12; i++) {
        float scaled = (float)(**(ushort**)(ANCHOR_USHORT_H + i*4)) * /* _53175e04 */ 1.0f * gameRenderH;
        **(ushort**)(ANCHOR_USHORT_H + i*4) = float_to_ushort(scaled);
    }

    /* ── 9. Color constants ── */
    g_009b8db4 = 0xff181818;
    g_009b8dc0 = 0xff181818;
    g_009b8db8 = 0xfa181818;
    g_009b8dbc = 0xfa181818;
    g_009b8de8 = 0xfa181818;
    g_009b8dec = 0xfa181818;

    /* ── 10. Geometric recomputation (~6 writes) ──
     * Mix of gameRenderW/H derived values with the normalization factor. */
    g_009ca53c = gameRenderW / HALF
               - (/* _53176104 */ - g_009ca53c)
               * ((g_5318efa4 - ZERO) / ZERO + ONE);
    /* ... (5 more, same pattern) ... */

    /* ── 11. Per-player UI scaling ── */
    if (g_numPlayers > 1) {
        g_00927318 = /* _53176150 */;    /* 2-player threshold */
    }
    if (g_numPlayers > 2) {
        g_5318e8ec = /* _53175e28 */;
        for (i = 0; i < g_numPlayers - 2; i++) {
            g_5318e8ec = ((float)(i) / /* _53176040 */ + ONE) * g_5318e8ec;
        }
        g_5318e8ec = (float)(g_numPlayers - 2) * g_5318e8ec + ONE;

        /* Scale 5 entries by the per-player factor */
        for (i = 0; i < 5; i++)
            **(float**)(ANCHOR_PLAYERSCALE + i*4) *= g_5318e8ec;

        /* Update HUD position offsets */
        g_0097f1bc += (float)(g_numPlayers - 2);
        g_0090235c -= (float)(g_numPlayers - 2);
        g_00902358 += (float)(g_numPlayers - 2);
    }

    /* ── 12-13. Display-mode enumeration + language ID ── */
    {
        int found = 0;
        unsigned int count, buffer;
        /* Enumerate display modes via D3D9/DDraw */
        count = (*d3d9_enumfunc)(0, 0);
        buffer = (*d3d9_createfunc)(0x40, count << 2);
        count = (*d3d9_enumfunc)(count, buffer);
        for (i = 0; i < count; i++) {
            char name[0x200];
            (*d3d9_getdesc)(*(uint16*)(buffer + i*4), 0x1001, name, 0x200);
            if (match_string_in_list(name, 0x53140048)) {
                found = 1;
            }
            memset(name, 0, 0x200);
        }
        if (g_languageId == 1 && found == 0) {
            g_languageId = 0;
        }
        if (buffer) {
            (*d3d9_freefunc)(buffer);
        }
        if (g_languageId == 0) {
            g_00841386 = 0x8ec39c;   /* Pointer to default string table */
        } else {
            void* h = (*d3d9_loadfunc)(0x53140054, 0x101, 0);
            (*d3d9_bindfunc)(h);
            g_00841386 = 0x8f83a8;   /* Pointer to localized string table */
        }
    }

    /* ── 14. Detour status ── */
    set_detour_status(1);

    /* ── 15. Clamp value ── */
    if (/* _53176140 */ <= /* _53176144 */ / hudScaleFactor) {
        g_00a11464 = /* _53176140 */;
    } else {
        g_00a11464 = /* _53176144 */ / hudScaleFactor;
    }

    /* ── 16. *** THE 92-VA FUNCTION *** ── */
    stage_c_92va_apply();

    /* ── 17. Final scalar writes (~25) ──
     * Texture-coord / screen-edge updates for the new viewport size */
    g_0077f2d1 = /* _53176190 */;         /* _DAT_0077f2d1 */
    g_0077f32c = /* _53176190 */;
    g_0077f362 = /* _53176190 */;
    g_0077f3c7 = /* _53176190 */;
    g_0077f2ed = gameRenderW + /* _53175e44 */;  /* right edge */
    g_0077f30d = gameRenderW + /* _53175e44 */;
    g_0077f386 = gameRenderW + /* _53175e44 */;
    g_0077f3a7 = gameRenderW + /* _53175e44 */;
    g_0077f3b2 = gameRenderH + /* _53175e44 */;  /* bottom edge */
    g_0077f3d2 = gameRenderH + /* _53175e44 */;

    /* Display-frame offset (screen-space safe zone) */
    g_0091ff30 = displayW + /* _531760b4 */;
    g_0091ff2c = displayH + /* _531760b4 */;
    g_0091ff00 = displayW + /* _531760b4 */;
    g_0091fefc = displayH + /* _531760b4 */;

    /* More texture-edge writes */
    g_00973138 = /* _53176190 */;
    g_00973134 = /* _53176190 */;
    g_00973130 += /* _53175e44 */;
    g_0097312c += /* _53175e44 */;
    g_009f4654 = /* _53176110 */;

    /* ── 18. 6-element function pointer array (column offsets) ── */
    {
        float* ptrs[6] = {
            (float*)0x9f4564,
            (float*)0x9f4594,
            (float*)0x9f45c4,
            (float*)0x9f45f4,
            (float*)0x9f4624,
            (float*)0x9f4654,
        };
        for (i = 0; i < 6; i++) {
            *ptrs[i] = (float)i * /* _531760a4 */ + /* _53176074 */;
        }
    }

    /* ── 19. Final increment ── */
    g_0096ffdc += /* _53176088 */;
}
