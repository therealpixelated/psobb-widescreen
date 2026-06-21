/* ============================================================================
 *  pso_widescreen.h  —  unified bake model for the PSOBB widescreen ASI
 *  ----------------------------------------------------------------------------
 *  Target client: PsoBB.exe, build MTethVer12513 / 1.25.13 ("59NL",
 *  *(uint32_t*)0x00B613FA == 0x4C4E3935). Every engine-memory coordinate write
 *  the mod performs is ONE declarative `bake_t` row in kBakes[] (pso_widescreen.c),
 *  applied by ONE value-guarded pass (apply_bakes). The written value is always
 *      EXPR = coeff * base + offset
 *  where `base` is resolved from the live scale context at apply time, so the
 *  whole table is a structural no-op at 4:3 (B_A->640, B_C->480, all deltas->0).
 *
 *  Attribution lives on every row's `src` column (documentation only):
 *    SRC_SEGA    — Sega/Sonic Team stock value, left unchanged (the 4:3 identity).
 *    SRC_ANZZ1   — anzz1 / tofuman "WideScreenPatch": HUD-width/height lists, the
 *                  deanchor ADD families, res/atlas tables, title-plate floats,
 *                  design_w/h. Ported faithfully (aspect from our INI; no
 *                  fullscreen/printscreen hooks; ADD lists run exactly once).
 *    SRC_EPHINEA — Ephinea (ephinea.dll RE): char-select anchor block, streak
 *                  scale, kill-screen hex aspect-fit, list-window bottom-anchor,
 *                  the per-glyph text/HUD quad scaler.
 *    SRC_TRINITY — Trinity widescreen reference: front-end vertical anchors,
 *                  dressing-room / lobby / config-menu X anchors, the aspect writes.
 *    SRC_OURS    — psoharness-original: minimap re-place + zoom, cursor-warp fix,
 *                  char-create backdrop stretch, boot poster, intro video, splash
 *                  widen, the overwrite guard.
 * ========================================================================== */
#ifndef PSO_WIDESCREEN_H
#define PSO_WIDESCREEN_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

/* ---- how a row writes its value ---- */
typedef enum {
    K_SET = 0,   /* *(float*)va    =  coeff*base + offset   (value-guarded, idempotent) */
    K_ADD,       /* *(float*)va   +=  coeff*base + offset   (RMW; applied ONCE)          */
    K_MUL,       /* *(float*)va   *=  coeff*base + offset   (RMW; applied ONCE)          */
    K_U32,       /* *(uint32_t*)va = (uint32_t)(coeff*base+offset) (res/atlas tables)    */
    K_AR,        /* *(float*)va    =  display aspect        (the 12 aspect writes)       */
    K_NOP,       /* write 0x90 x5  (instruction-kill cleanup)                            */
} bake_kind_t;

/* ---- value base: resolved from the live scale ctx at apply time ---- */
typedef enum {
    B_LIT = 0,   /* 1.0  (pure literal => value = offset)                */
    B_A,         /* design width   A = 853.33*HUDScale @16:9 / 640 @4:3  */
    B_C,         /* design height  C = 480*HUDScale                      */
    B_B,         /* sprite tile unit B = 128 * (AR/(4/3)) * HUDScale     */
    B_BCEIL,     /* ceil(B)  (atlas tile widths; Ephinea-match)          */
    B_D,         /* int tile unit  D = (int)(128*HUDScale)               */
    B_AR,        /* display aspect                                       */
    B_RW,        /* render width  (px backbuffer)                        */
    B_RH,        /* render height (px backbuffer)                        */
    B_HUDSCALE,  /* hud_scale                                            */
    B_KX,        /* hud_scale * (16/9)/(4/3)                             */
    B_AFFINEY,   /* render_h / 480                                       */
    B_ANATIVE,   /* A / hud_scale  (native design width, hud-independent;
                    the dressing-room shift is native 213.33, not A-640) */
    B_CNATIVE,   /* C / hud_scale  (native design height)                */
} bake_base_t;

/* ---- attribution (logged; drives no behaviour) ---- */
typedef enum { SRC_SEGA = 0, SRC_ANZZ1, SRC_EPHINEA, SRC_TRINITY, SRC_OURS } bake_src_t;

/* ---- gate bits: a cfg toggle that can suppress a whole group ---- */
#define GATE_ALWAYS        0x00
#define GATE_CHARSELECT    0x01
#define GATE_MINIMAP       0x02
#define GATE_RUNE          0x04
#define GATE_STREAK_SCALE  0x08
#define GATE_HANGAME       0x10
#define GATE_SPLASH        0x20

/* ---- one authoritative entry per engine VA ----
   field order is positional-init friendly:
     { va, kind, base, coeff, offset, src, gate, stock, note, base2 }
   value written = (coeff * base + offset) * base2
   (base2 defaults to B_LIT == 1.0 for the common single-factor row). */
typedef struct {
    uint32_t    va;       /* engine address (.text imm32 or .data float)        */
    uint8_t     kind;     /* bake_kind_t                                        */
    uint8_t     base;     /* bake_base_t                                        */
    float       coeff;    /* value = (coeff*base + offset) * base2              */
    float       offset;
    uint8_t     src;      /* bake_src_t (attribution only)                      */
    uint8_t     gate;     /* GATE_* mask                                        */
    uint32_t    stock;    /* expected pre-write bits (sig-guard; 0 = unchecked) */
    const char *note;     /* short subsystem tag e.g. "csel.banner.x"           */
    uint8_t     base2;    /* second factor (B_LIT==1.0 unless a row needs it)   */
} bake_t;

/* ---- public surface (the only symbols other TUs in this ASI use) ---- */
void log_line(const char *fmt, ...);          /* mod_video / mod_boot_poster    */
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID);

#endif /* PSO_WIDESCREEN_H */
