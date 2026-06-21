// anzz1_widescreen.c — faithful port of anzz1/tofuman WideScreenPatch.
//
// Source of truth: anzz1's WideScreen.c (credit tofuman), target client
// MTethVer12513 (1.25.13). The 8 address lists below are VERBATIM from
// that reference. apply_anzz1_widescreen() implements patch_widescreen()'s
// coordinate patches with the adaptations documented in anzz1_widescreen.h:
//   - aspect from caller (INI GameAspect), not the monitor
//   - no window/fullscreen hook, no printscreen disable
//   - version mismatch => warn (not ExitProcess)
//   - VirtualProtect RW around every write
//   - ADD-style lists run exactly once per process
//
// Reference algorithm (WideScreen.c patch_widescreen):
//   AR = aspect
//   A  = (AR / (4/3)) * 640 * HUDScale     // horizontal extent
//   B  = (AR / (4/3)) * 128 * HUDScale     // sprite-atlas tile width unit
//   C  = 480 * HUDScale                    // vertical extent
//   D  = (DWORD)(128 * HUDScale)           // integer tile-height unit
// then:
//   res table  0x009006F4 + i*8 (6 entries) <- width/height
//   sprite tbl 0x009A3840..D8              <- B/D combinations
//   ~25 hardcoded float patches (0x009D0040 ...)
//   12 AR writes
//   listHUDWidth   <- A
//   listHUDHeight  <- C
//   listCenterAlign            += (A - 640) / 2
//   listRightAlign             += (A - 640)
//   listVerticalBottom(+Movs+Delay) += (C - 480)
//   listVerticalCenter         += (C/2) - 240
//   ~16 more hardcoded float patches (0x007584EE ...)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include "anzz1_widescreen.h"

// log_line() has external linkage in pso_widescreen.c (the main TU).
extern void log_line(const char *fmt, ...);

// ===========================================================================
// Address lists — VERBATIM from anzz1's WideScreen.c.
// ===========================================================================

static const ULONG listHUDWidth[] = {
  0x004011D2, 0x004011EF, 0x004013A7, 0x00409F4B, 0x0040A02B, 0x0040A04B,
  0x0040C445, 0x0040C469, 0x0040C48B, 0x00453809, 0x0045381D, 0x00483278,
  0x004EB4AA, 0x004EB4F0, 0x004EC2D1, 0x004EF592, 0x004EF59C, 0x00502D7D,
  0x005BC90E, 0x005BD783, 0x005BDB89, 0x007126AC, 0x00719AB1, 0x00719ACE,
  0x00719C5C, 0x00719C6B, 0x00719D44, 0x00719D53, 0x00719E84, 0x0071A21F,
  0x00721E6C, 0x00721E7A, 0x00721F2A, 0x00721FC0, 0x0077F2ED, 0x0077F30D,
  0x0077F386, 0x0077F3A7, 0x00785641, 0x007856B1, 0x00785BD2, 0x007888A6,
  0x007888C6, 0x00788EA6, 0x00788EC6, 0x00804EE0, 0x008051BA, 0x008F8494,
  0x008F853C, 0x008F854C, 0x008F8EAC, 0x008F8EBC, 0x008F8F50, 0x008F9A94,
  0x008F9AA4, 0x008F9AC4, 0x008F9D38, 0x008F9D68, 0x008F9E8C, 0x008F9EF8,
  0x008F9F2C, 0x008F9F34, 0x008F9F58, 0x008FA8A8, 0x008FAE50, 0x008FAE90,
  0x008FFB84, 0x009007B8, 0x00909168, 0x00909D70, 0x00909D84, 0x00909DA8,
  0x0091D8BC, 0x0091DA80, 0x0091E19C, 0x0091FBF0, 0x0091FC54, 0x0091FC74,
  0x0091FF94, 0x00920108, 0x00920164, 0x0092B6EC, 0x0092B71C, 0x0092B740,
  0x0092B780, 0x0096F5E0, 0x0096F694, 0x0096FE84, 0x0096FF60, 0x0096FF9C,
  0x0096FFC0, 0x009701F0, 0x00970640, 0x009706C0, 0x009707F8, 0x0097080C,
  0x009796F4, 0x00979A34, 0x00979A7C, 0x00979A9C, 0x00979DC4, 0x00979E00,
  0x0097A170, 0x0097A1AC, 0x0097A1B8, 0x0097A6F4, 0x0097A740, 0x0097BB0C,
  0x0097BB58, 0x0097BBB0, 0x0097EBBC, 0x0097EC1C, 0x0097EC4C, 0x0098A160,
  0x0098A4A8, 0x0098A4B8, 0x009A3468, 0x009A3488, 0x009A6900, 0x009A6908,
  0x009B8DC4, 0x009B8DDC
};

static const ULONG listHUDHeight[] = {
  0x004011C0, 0x004011DD, 0x004013AF, 0x0040A016, 0x0040A036, 0x00453827,
  0x0045383B, 0x004EB4B4, 0x004EB4FA, 0x004EC2E9, 0x00502D85, 0x007126A5,
  0x00719AD9, 0x00719AF6, 0x00719C08, 0x00719C16, 0x00719C79, 0x00719C8A,
  0x0071A226, 0x0077F3B2, 0x0077F3D2, 0x00785648, 0x00785680, 0x007856B8,
  0x00785705, 0x00785BD9, 0x00785C11, 0x00785C49, 0x007888D1, 0x007888F1,
  0x00788ED1, 0x00788EF1, 0x00804EF4, 0x008051CE, 0x008F9A98, 0x008F9AC8,
  0x008FFB80, 0x00909164, 0x0091D8B8, 0x0091DA7C, 0x0091FBEC, 0x0091FC50,
  0x0091FC70, 0x0091FF90, 0x00920050, 0x00920104, 0x00920160, 0x0096F5D8,
  0x0096F698, 0x0096FF5C, 0x0096FFA8, 0x0096FFBC, 0x009701EC, 0x00970648,
  0x00970660, 0x009706C8, 0x009707F4, 0x00970808, 0x009796EC, 0x00979A28,
  0x00979A70, 0x00979A8C, 0x00979DC8, 0x00979DFC, 0x0097A16C, 0x0097A1A8,
  0x0097A1BC, 0x0097A6F0, 0x0097A73C, 0x0097E9A8, 0x0097E9D8, 0x0097E9E4,
  0x0097E9F0, 0x0097EA00, 0x0097EBB8, 0x0097EC18, 0x0097EC48, 0x0098A15C,
  0x0098A4A4, 0x0098A4B4, 0x009A690C, 0x009A6914, 0x009B8D0C, 0x009B8D54,
  0x00A98498, 0x00A984B4, 0x00A984D0, 0x00A984EC, 0x00A98508, 0x00A98524
};

static const ULONG listCenterAlignItems[] = {
  0x0066DFEF, 0x00791C7A, 0x00792366, 0x00972000, 0x00972040, 0x00972048,
  0x00972060, 0x009720A8, 0x009720E0, 0x009720E8, 0x009720F0, 0x009720F8,
  0x00972100, 0x00972188, 0x00972508, 0x00972518, 0x00972530, 0x00972548,
  0x00972550, 0x00972570, 0x00972578, 0x00972580, 0x00972620, 0x00972698,
  0x00974E3C, 0x00974E44, 0x009FF50C
};

static const ULONG listRightAlignItems[] = {
  0x00783A92, 0x00972010, 0x00972050, 0x00972058, 0x00972540, 0x009720B0,
  0x009720B8, 0x00972078, 0x009725E8, 0x009725D0, 0x00972610, 0x00A11324,
  0x00A11390, 0x00A11398, 0x00A113E8
};

static const ULONG listVerticalBottomAlignItems[] = {
  0x00407F9D, 0x0071A8F8, 0x0071A905, 0x0071A988, 0x0071A995, 0x007403F6,
  0x0074467D, 0x0074468D, 0x007446D8, 0x007446E8, 0x00744723, 0x00744733,
  0x0074477E, 0x0074478E, 0x007583C8, 0x007583E3, 0x007583FE, 0x00758412,
  0x007584F5, 0x0075850F, 0x00758526, 0x0075853F, 0x00761CC7, 0x00761CD9,
  0x00762367, 0x00762602, 0x007627DF, 0x00783AAA, 0x008F87D8, 0x008F87E0,
  0x0096E534, 0x0096E53C, 0x0096E54C, 0x0096E574, 0x0096E5FC, 0x0096E660,
  0x0096FC44, 0x0096FC54, 0x0097061C, 0x00970638, 0x0097066C, 0x009710BC,
  0x00971F44, 0x00971F4C, 0x00971F54, 0x00971F5C, 0x00971F64, 0x00971f6c,
  0x00971f74, 0x00971F7C, 0x00971F84, 0x00971F8C, 0x00971f94, 0x00971f9c,
  0x00971FA4, 0x00971fac, 0x00971FBC, 0x00971FC4, 0x00971FCC, 0x00971FD4,
  0x00971FDC, 0x00971FE4, 0x00971FEC, 0x00971FF4, 0x00971FFC, 0x00972004,
  0x0097200C, 0x00972014, 0x00972024, 0x0097206C, 0x00972094, 0x0097209C,
  0x009720A4, 0x009720B4, 0x009720CC, 0x009720FC, 0x00972224, 0x0097222C,
  0x00972234, 0x0097223C, 0x00972244, 0x0097224C, 0x00972254, 0x0097225C,
  0x00972264, 0x0097227C, 0x00972284, 0x009722DC, 0x009722E4, 0x009722EC,
  0x009722F4, 0x009722FC, 0x00972304, 0x0097230C, 0x00972314, 0x0097231C,
  0x00972324, 0x0097232C, 0x00972334, 0x0097233C, 0x00972344, 0x0097234C,
  0x00972354, 0x0097235C, 0x00972364, 0x0097236C, 0x00972374, 0x0097237C,
  0x00972384, 0x0097238C, 0x00972394, 0x0097239C, 0x009723A4, 0x009723AC,
  0x009723B4, 0x009723BC, 0x009723C4, 0x009723CC, 0x009723D4, 0x009723DC,
  0x009723E4, 0x009723EC, 0x009723F4, 0x009723FC, 0x00972404, 0x0097240C,
  0x00972414, 0x0097241C, 0x00972424, 0x0097242C, 0x00972434, 0x0097243C,
  0x00972444, 0x0097244C, 0x00972454, 0x0097245C, 0x00972464, 0x0097246C,
  0x00972474, 0x0097247C, 0x00972484, 0x0097248C, 0x00972494, 0x0097249C,
  0x009724A4, 0x009724AC, 0x009724B4, 0x009724BC, 0x009724C4, 0x009724CC,
  0x009724D4, 0x009724DC, 0x009724E4, 0x009724EC, 0x009724F4, 0x009724FC,
  0x00972504, 0x00972554, 0x0097255C, 0x00972564, 0x0097256C, 0x0097258C,
  0x00972594, 0x0097259C, 0x009725A4, 0x009725AC, 0x009725B4, 0x009725BC,
  0x009725C4, 0x009725CC, 0x009725D4, 0x009725E4, 0x009725EC, 0x00972614,
  0x0097261C, 0x00972700, 0x00974E40, 0x00974E48, 0x009D0044, 0x009F0A80,
  0x009F0A88, 0x009FF084, 0x009FF09C, 0x009FF0B4, 0x009FF0CC, 0x009FF0E4,
  0x009FF0FC, 0x009FF114, 0x009FF12C, 0x009FF1AC, 0x00A1133C, 0x00A113A4,
  0x00A113AC, 0x00A113F4
};

static const ULONG listVerticalBottomAlignItemsMovs[] = {
  0x006F4D9F, 0x006FFCB8, 0x006FFCE0, 0x006FFCF4, 0x006FFD12, 0x006FFD1C,
  0x006FFD58, 0x006FFD76, 0x0070D2CE, 0x0070D2EC, 0x0070D30A, 0x0070D328,
  0x0070D346, 0x0070D364, 0x0070D382, 0x0070D3A0, 0x0070D3B4, 0x0070D3D2,
  0x0070D3F0, 0x0070D404
};

static const ULONG listVerticalBottomAlignItemsDelay[] = {
  0x009F0A90, 0x009F0A98
};

static const ULONG listVerticalCenterAlignItems[] = {
  0x0066DFFA, 0x00972054, 0x0097205C, 0x009720E4, 0x009720EC, 0x00972104,
  0x009720F4
};

// ===========================================================================
// Write helpers — every target may live in .text (read-only), so each
// write is wrapped in VirtualProtect RW + restore.
// ===========================================================================

static void wr_u32(ULONG addr, DWORD val)
{
    DWORD old_prot = 0;
    if (!VirtualProtect((LPVOID)(uintptr_t)addr, 4, PAGE_EXECUTE_READWRITE, &old_prot)) {
        log_line("[anzz1] wr_u32 VirtualProtect FAILED @ 0x%08lX err=%lu",
                 (unsigned long)addr, GetLastError());
        return;
    }
    *(volatile DWORD *)(uintptr_t)addr = val;
    DWORD tmp = 0;
    VirtualProtect((LPVOID)(uintptr_t)addr, 4, old_prot, &tmp);
}

static void wr_f32(ULONG addr, float val)
{
    DWORD old_prot = 0;
    if (!VirtualProtect((LPVOID)(uintptr_t)addr, 4, PAGE_EXECUTE_READWRITE, &old_prot)) {
        log_line("[anzz1] wr_f32 VirtualProtect FAILED @ 0x%08lX err=%lu",
                 (unsigned long)addr, GetLastError());
        return;
    }
    *(volatile float *)(uintptr_t)addr = val;
    DWORD tmp = 0;
    VirtualProtect((LPVOID)(uintptr_t)addr, 4, old_prot, &tmp);
}

// Read-modify-write for the ADD-style alignment lists: new = stock + delta.
static void add_f32(ULONG addr, float delta)
{
    DWORD old_prot = 0;
    if (!VirtualProtect((LPVOID)(uintptr_t)addr, 4, PAGE_EXECUTE_READWRITE, &old_prot)) {
        log_line("[anzz1] add_f32 VirtualProtect FAILED @ 0x%08lX err=%lu",
                 (unsigned long)addr, GetLastError());
        return;
    }
    *(volatile float *)(uintptr_t)addr = *(volatile float *)(uintptr_t)addr + delta;
    DWORD tmp = 0;
    VirtualProtect((LPVOID)(uintptr_t)addr, 4, old_prot, &tmp);
}

#define ANZZ1_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

// ===========================================================================
// apply_anzz1_widescreen — port of patch_widescreen().
// ===========================================================================

void apply_anzz1_widescreen(float aspect, float A, float B, float C,
                            unsigned long D_in, int render_w, int render_h)
{
    // ADD-style lists must offset EXACTLY ONCE per process: a read-modify-
    // write of stock+delta is not idempotent, so re-running would double the
    // offset and corrupt layout.
    static int applied = 0;
    if (applied) {
        log_line("[anzz1] apply_anzz1_widescreen: already applied this process — skipping");
        return;
    }
    const DWORD D = (DWORD)D_in;
    // 2026-06-17 BLACK-SEAM FIX (LIVE-DIFFED vs Ephinea @hs2.0). The sprite-atlas
    // tile widths below must use a CEILED B (matching Ephinea's aspect-table cascade,
    // which yields integer B=342 @16:9 hs2.0), NOT floor via (DWORD)(B*N). With
    // floor, B=341.333 -> bg tile width = (DWORD)(B*2) = 682 and logo x = (DWORD)(B*4)
    // = 1365; Ephinea uses 684 / 1368 (= 342*2 / 342*4). Our 2px-short tiles leave a
    // gap = the title's thin vertical black seam (~screen x767 @hs2.0). Ceil B once:
    const DWORD Bi = (DWORD)(B + 0.9999f);   // 342 @16:9 hs2.0 / 171 @16:9 hs1.0 (Ephinea-match)

    // ----- Version check (ADAPTED: warn, do not ExitProcess) -----
    // Reference asserts GetImageSize >= 0x00762000 and *(DWORD*)0x00B613FA
    // == 0x4C4E3935 ("59NL"). On mismatch the upstream patch MessageBoxes
    // and ExitProcess(-1). We are a windowed + reparented mod; bailing the
    // whole process is unacceptable, so we log a warning and continue
    // (the address lists are MTethVer12513-specific; mismatched builds may
    // be patched incorrectly, hence the loud warning).
    {
        DWORD ver = 0;
        __try {
            ver = *(volatile DWORD *)0x00B613FA;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ver = 0;
        }
        if (ver != 0x4C4E3935u) {
            log_line("[anzz1] WARNING: client version tag @0x00B613FA=0x%08lX "
                     "(expected 0x4C4E3935 '59NL' / MTethVer12513 1.25.13). "
                     "Address lists may not match this build — proceeding anyway.",
                     (unsigned long)ver);
        }
    }

    // 2026-06-09 REFACTOR (MINOR-3): A/B/C/D and render_w/render_h are now
    // computed ONCE in pso_widescreen.c's ws_compute_scale() and passed in.
    // AR (for the 12 AR writes) is the caller's aspect (INI GameAspect).
    const float AR = aspect;

    // 6-entry resolution table = the engine's RENDER resolution (must equal the
    // D3D backbuffer the CreateDevice hook sets, e.g. 1920x1080). NOT the A/C
    // coordinate extents — those are the HUD layout space, not the res. The
    // caller already applied the `render_w>0 ? render_w : (int)A` fallback.
    const DWORD width  = (DWORD)render_w;
    const DWORD height = (DWORD)render_h;

    log_line("[anzz1] apply: aspect=%.5f -> A=%.2f B=%.2f C=%.2f D=%lu (canvas %lux%lu)",
             aspect, A, B, C, (unsigned long)D,
             (unsigned long)width, (unsigned long)height);

    // ----- Resolution table: 0x009006F4 + i*8 (6 entries) -----
    for (int i = 0; i < 6; i++) {
        wr_u32((ULONG)(0x009006F4 + i * 8), width);
        wr_u32((ULONG)(0x009006F8 + i * 8), height);
    }

    // ----- Sprite-quad atlas table 0x009A3840..0x009A38D8 -----
    // 2026-06-17 LIVE-VERIFIED vs Ephinea (pid 36320 @hs2.0 16:9): EVERY width
    // entry here matches Ephinea byte-for-byte with the uniform Bi ceiling —
    // Bi*4=1368, Bi*2=684, Bi=342 (Ephinea uses the SAME 1368/684/342). Ephinea
    // does NOT floor any entry; the "uniform ceil over-widens the non-bg tiles ->
    // photon-blast UV leak" hypothesis was DISPROVEN (atlas is not the leak source;
    // the leak was the 4:3-stuck char-select backdrop X anchors — see
    // patch_charselect_vertical()). Keep Bi for all entries = Ephinea-exact.
    wr_u32(0x009A3840, Bi * 4);
    wr_u32(0x009A3844, D * 3);
    wr_u32(0x009A3848, Bi);
    wr_u32(0x009A384C, D);
    wr_u32(0x009A3854, Bi * 4);
    wr_u32(0x009A3858, D * 2);
    wr_u32(0x009A385C, Bi);
    wr_u32(0x009A3860, D);
    wr_u32(0x009A3868, Bi * 4);
    wr_u32(0x009A386C, D);
    wr_u32(0x009A3870, Bi);
    wr_u32(0x009A3874, D);
    wr_u32(0x009A387C, Bi * 4);
    wr_u32(0x009A3884, Bi);
    wr_u32(0x009A3888, D);
    wr_u32(0x009A3894, D * 2);
    wr_u32(0x009A3898, Bi * 2);
    wr_u32(0x009A389C, D * 2);
    wr_u32(0x009A38AC, Bi * 2);
    wr_u32(0x009A38B0, D * 2);
    wr_u32(0x009A38B8, Bi * 2);
    wr_u32(0x009A38BC, D * 2);
    wr_u32(0x009A38C0, Bi * 2);
    wr_u32(0x009A38C4, D * 2);
    wr_u32(0x009A38CC, Bi * 2);
    wr_u32(0x009A38D4, Bi * 2);
    wr_u32(0x009A38D8, D * 2);

    // ----- ~25 hardcoded float patches -----
    wr_f32(0x009D0040, (A / 2.0f) - 128.0f);
    wr_f32(0x00761CCC, (A / 2.0f) - 110.0f);
    wr_f32(0x00761CDE, (A / 2.0f) - 110.0f);
    wr_f32(0x0076236C, (A / 2.0f) - 180.0f);
    wr_f32(0x007625F8, (A / 2.0f) - 4.0f);
    wr_f32(0x007627CF, (A / 2.0f) - 4.0f);
    wr_f32(0x008F87D4, A - 63.0f);
    wr_f32(0x008F87E4, A - 63.0f);
    wr_f32(0x009F0ADC, A - 157.0f);
    wr_f32(0x009F0AF4, A - 288.0f);
    wr_f32(0x009F0B0C, A - 157.0f);
    wr_f32(0x009F0B24, A - 288.0f);
    wr_f32(0x009F0B3C, A - 128.0f);
    wr_f32(0x0070D350, A - 384.0f);
    wr_f32(0x009F0B5C, A - 128.0f);
    wr_f32(0x0070D2F6, A - 384.0f);
    wr_f32(0x009FF080, A - 144.0f);
    wr_f32(0x009FF098, A - 144.0f);
    wr_f32(0x009FF0B0, A - 16.0f);
    wr_f32(0x009FF0C8, A - 16.0f);
    wr_f32(0x009FF0E0, A - 272.0f);
    wr_f32(0x009FF0F8, A - 272.0f);
    wr_f32(0x009FF110, A - 143.0f);
    wr_f32(0x009FF128, A - 143.0f);
    wr_f32(0x009FF1A8, A - 144.0f);

    // ----- 12 AR writes -----
    wr_f32(0x00489E53, AR);
    wr_f32(0x0082EC74, AR);
    wr_f32(0x0082ED43, AR);
    wr_f32(0x0082EF4C, AR);
    wr_f32(0x0082F018, AR);
    wr_f32(0x0082F700, AR);
    wr_f32(0x0082F7DB, AR);
    wr_f32(0x00901288, AR);
    wr_f32(0x0098A494, AR);
    wr_f32(0x0098A4DC, AR);
    wr_f32(0x0098A500, AR);
    wr_f32(0x00A33CD0, AR);

    // ----- listHUDWidth <- A -----
    for (int i = 0; i < (int)ANZZ1_COUNTOF(listHUDWidth); i++)
        wr_f32(listHUDWidth[i], A);

    // ----- listHUDHeight <- C -----
    for (int i = 0; i < (int)ANZZ1_COUNTOF(listHUDHeight); i++)
        wr_f32(listHUDHeight[i], C);

    // ----- ADD-style lists (run exactly once via `applied` guard) -----
    for (int i = 0; i < (int)ANZZ1_COUNTOF(listCenterAlignItems); i++)
        add_f32(listCenterAlignItems[i], (A - 640.0f) / 2.0f);

    for (int i = 0; i < (int)ANZZ1_COUNTOF(listRightAlignItems); i++)
        add_f32(listRightAlignItems[i], (A - 640.0f));

    for (int i = 0; i < (int)ANZZ1_COUNTOF(listVerticalBottomAlignItems); i++)
        add_f32(listVerticalBottomAlignItems[i], (C - 480.0f));

    for (int i = 0; i < (int)ANZZ1_COUNTOF(listVerticalBottomAlignItemsMovs); i++)
        add_f32(listVerticalBottomAlignItemsMovs[i], (C - 480.0f));

    for (int i = 0; i < (int)ANZZ1_COUNTOF(listVerticalBottomAlignItemsDelay); i++)
        add_f32(listVerticalBottomAlignItemsDelay[i], (C - 480.0f));

    for (int i = 0; i < (int)ANZZ1_COUNTOF(listVerticalCenterAlignItems); i++)
        add_f32(listVerticalCenterAlignItems[i], (C / 2.0f) - 240.0f);

    // ----- ~16 more hardcoded float patches -----
    wr_f32(0x007584EE, A + 10.0f);
    wr_f32(0x0075851F, A + 10.0f);
    wr_f32(0x00978454, A - 640.0f);
    wr_f32(0x00978458, A - 640.0f - 220.0f);
    wr_f32(0x006F4922, A / 2.0f);
    wr_f32(0x006F4936, C / 2.0f);
    wr_f32(0x009712DC, 240.0f - (C - 480.0f));
    wr_f32(0x007165B8, A - 16.0f);
    wr_f32(0x006F4CF2, 64.0f  * (((B - 128.0f) / 128.0f) + 1.0f));
    wr_f32(0x006F4CFA, 61.0f  * ((((float)D - 128.0f) / 128.0f) + 1.0f));
    wr_f32(0x006F4D02, 320.0f * (((B - 128.0f) / 128.0f) + 1.0f));
    wr_f32(0x006F4D0A, 317.0f * ((((float)D - 128.0f) / 128.0f) + 1.0f));
    wr_f32(0x006F4D4E, 320.0f * (((B - 128.0f) / 128.0f) + 1.0f));
    wr_f32(0x006F4D56, 61.0f  * ((((float)D - 128.0f) / 128.0f) + 1.0f));
    wr_f32(0x006F4D5E, 576.0f * (((B - 128.0f) / 128.0f) + 1.0f));
    wr_f32(0x006F4D66, 317.0f * ((((float)D - 128.0f) / 128.0f) + 1.0f));

    // ADAPTED: SKIP the upstream window/fullscreen hook (memset 0x00482E20 +
    // 0x0082D1D8 myCreateWindowExA) and the printscreen disable (0x007C1424).
    // We are windowed + reparented; those would conflict with our host.

    applied = 1;
    log_line("[anzz1] apply_anzz1_widescreen: DONE (all lists patched once).");
}
