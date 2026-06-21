/* ================================================================
 * ephinea.dll — Widescreen Cascade: Shared Constants & Types
 *
 * This header consolidates all the named externs, constants, and
 * function pointer types used by the widescreen cascade functions
 * (Stages A, B, C, 92-VA, vertex transform, re-anchor, deanchor).
 *
 * All `_DAT_5318xxxx` references point into the ephinea.dll .data
 * section at the current build's ASLR base (0x78480000).
 *
 * Constants marked (RPM-only) have structure verified from the
 * decompile but their literal values are read from undumped .rdata.
 * ================================================================ */

#ifndef EPHINEA_WIDESCREEN_H
#define EPHINEA_WIDESCREEN_H

#include <stdint.h>

/* ================================================================
 * 1. BASE CONSTANTS (RPM-only, structure verified per docs/PATCHES_OVERVIEW.md §2)
 * ================================================================ */

#define STOCK_WIDTH      640.0f     /* _DAT_53176128 — stock engine render width */
#define STOCK_HEIGHT     480.0f     /* _DAT_53176120 — stock engine render height */
#define ASPECT_4_3       1.33333f   /* _DAT_53175e8c — 4:3 aspect ratio */
#define HALF             2.0f       /* _DAT_53175f20 — divisor for half-delta */
#define ONE              1.0f       /* _DAT_53175e64 — identity scale */
#define ZERO             0.0f       /* _DAT_531760bc — zero sentinel */
#define HUNDRED          100.0f     /* _DAT_53175fc0 — percentage divisor */

/* ================================================================
 * 2. ENGINE GLOBALS (written by the cascade, read by detours)
 * ================================================================ */

/* Display dimensions (from registry / resolution table — Stage A) */
extern float displayW;              /* _DAT_5318d210 */
extern float displayH;              /* _DAT_5318dcdc */

/* Render dimensions (computed by Stage B) */
extern float gameRenderW;           /* _DAT_5318deec */
extern float gameRenderH;           /* _DAT_5318d124 */

/* Master scale factor S (= HUD scale or aspect-derived) */
extern float hudScaleFactor;        /* _DAT_5387b918 */

/* Stage B intermediate outputs (consumed by Stage C) */
extern float g_5318f038;            /* _DAT_5318f038 — aspectMult */
extern float g_5318d21c;            /* _DAT_5318d21c — bucket delta */
extern float g_5318da7c;            /* _DAT_5318da7c — bucket scale/delta */
extern float g_5318ee38;            /* _DAT_5318ee38 — func_0x524b7c70(ratio*W) */
extern float g_5318efa4;            /* _DAT_5318efa4 — func_0x524b7c70(ratio*H) */

/* ================================================================
 * 3. CONFIG GLOBALS (read by Stage A from registry)
 * ================================================================ */

extern int g_widescreenEnabled;     /* _DAT_538785a4 */
extern int g_hudScaleIndex;         /* _DAT_5388f9c0 (0-4) */
extern int g_displayMode;           /* _DAT_5318e940 (0-7) */
extern int g_vsyncEnabled;          /* _DAT_5318eed4 */
extern int g_frameLimit;            /* _DAT_5318f240 (0-3) */
extern int g_aaQuality;             /* _DAT_5318f028 */
extern int g_textureFilter;         /* _DAT_5318dcfc */
extern int g_mipmapEnabled;         /* _DAT_53835c5c */
extern int g_resolutionIndex;       /* _DAT_53a9895c (0-0x13) */
extern int g_ssaaFactor;            /* _DAT_5387b938 */
extern int g_renderPath;            /* _DAT_53870578 */
extern int g_numPlayers;            /* _DAT_53a9027c */

extern int g_languageId;            /* _DAT_5388ed90 */
extern int g_widescreenActive;      /* _DAT_00a95edc (dynamic flag) */

/* ================================================================
 * 4. ANCHOR TABLE DAT constants (for the 92-VA function)
 * ================================================================ */

/* Breakpoint ladder constants (RPM-only, 7 levels) */
extern float aspectBreakpoints[7];   /* _DAT_53175ee8, _53175f00, …, _53175f50 */
extern float aspectMultTable[7];     /* _DAT_53175e78, _53175e8c, …, _53176000 */
extern float bucketScale[7];         /* _DAT_53175ea0, _53175ea4, …, _53176044 */
extern float bucketDelta[7];         /* _DAT_53175fa8, _53175f20, …, _5317607c */

/* Resolution table (index → {width, height} as doubles) */
extern double resolutionTable[];    /* _DAT_53146240 — 20 entries × 16 bytes */

/* ================================================================
 * 5. FUNCTION FORWARD DECLARATIONS
 * ================================================================ */

/* Stage A — registry loader */
int  stage_a_load_config(void);

/* Stage B — render-size computation with aspect breakpoint ladder */
void stage_b_compute_render_size(void);

/* Stage C — bulk anchor apply (the ~700+ global writes) */
void stage_c_apply_anchors(void);

/* 92-VA — 97 arithmetic writes across 93 distinct VAs */
void stage_c_92va_apply(void);

/* Per-draw detour handlers */
void vertex_aspect_correct_transform(int* vertexBuffer,
                                     undefined4 param2,
                                     undefined4 param3,
                                     undefined4 param4);
void ui_edge_reanchor(undefined4 param1, undefined4 param2);
void deanchor_pin(int* savedScale);

/* ================================================================
 * 6. TOOL FUNCTIONS
 * ================================================================ */

/* Helper for the many registry reads in Stage A */
static void read_dword_reg(HKEY hKey, LPCWSTR valName, DWORD* out,
                           DWORD minVal, DWORD maxVal, DWORD defaultVal);

#endif /* EPHINEA_WIDESCREEN_H */