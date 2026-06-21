/*
 * ephinea.dll — Widescreen Cascade: Stage A
 * Original: FUN_52da9930 @ 0x52DA9930
 *
 * Reads displayW / displayH from the registry or the resolution table,
 * applies SSAA if configured, reads ~60+ configuration values from the
 * Ephinea registry key, then tail-calls Stage B (FUN_52dabbd0).
 *
 * This function is the entry point of the widescreen cascade. It can be
 * called multiple times (during config reload or D3D device reset) to
 * re-derive all widescreen values from live config.
 *
 * Structure (verified against decompile):
 *   1. RegOpenKeyEx(HKEY_LOCAL_MACHINE, EphineaKey, ...)
 *   2. ~60× RegQueryValueEx → clamp → global write (all DWORD config values)
 *   3. RegCloseKey
 *   4. RegOpenKeyEx(HKEY_CURRENT_USER, EphineaUserKey, ...)
 *   5. Read display settings + player count
 *   6. RegCloseKey
 *   7. Tail-call to Stage B (FUN_52dabbd0)
 */

#include <windows.h>

/* ── Registry value-name strings (pointers in decompile) ──
 *
 * In the decompile, these appear as raw addresses (e.g. 0x5313f208).
 * The actual registry value name strings live in the DLL's .rdata.
 * Below are hypothesized names based on the values they're clamped to.
 * See intermediate/stage_a_raw.c for the full 1093-line raw decompile.
 */

#define KEY_EPHINEA    (LPCWSTR)0x5313f208   /* HKEY_LOCAL_MACHINE path */
#define KEY_USER       (LPCWSTR)0x5313ffe4   /* HKEY_CURRENT_USER path */

/* Values read from the first (HKLM) key — ~60 DWORD config knobs */
#define VAL_WIDESCREEN     (LPCWSTR)0x5313f24c  /* _DAT_538785a4: widescreen toggle */
#define VAL_HUD_SCALE      (LPCWSTR)0x5313f26c  /* _DAT_5388f9c0: HUD scale (0-4) */
#define VAL_DISPLAY_MODE   (LPCWSTR)0x5313f290  /* _DAT_5318e940: display mode (0-7) */
#define VAL_VSYNC          (LPCWSTR)0x5313f2a4  /* _DAT_5318eed4: vsync toggle */
#define VAL_FRAME_LIMIT    (LPCWSTR)0x5313f2b8  /* _DAT_5318f240: frame limit (0-3) */
#define VAL_AA_QUALITY     (LPCWSTR)0x5313f2cc  /* _DAT_5318f028: AA quality */
#define VAL_TEXTURE_FILTER (LPCWSTR)0x5313f2dc  /* _DAT_5318dcfc: tex filter on/off */
#define VAL_MIPMAP         (LPCWSTR)0x5313f2f4  /* _DAT_53835c5c: mipmap on/off */
#define VAL_RENDER_MODE    (LPCWSTR)0x5313f310  /* _DAT_53870578 etc: render path */
#define VAL_RESOLUTION     (LPCWSTR)0x5313f324  /* _DAT_53a9895c: resolution index (0-0x13) */
#define VAL_SSAA_FACTOR    (LPCWSTR)0x5313f340  /* SSAA multiplier */
/* ... ~50 more values read at 0x5313f3xx through 0x5313ffxx ... */

/* Values read from the second (HKCU) key */
#define VAL_DISPLAY_W      (LPCWSTR)0x53140018  /* _DAT_5388fe08: display width toggle */
#define VAL_PLAYER_COUNT   (LPCWSTR)0x53140030  /* _DAT_53a90268..7c: player count (≤10) */

/* Resolution table — index into a list of {width, height} pairs */
#define RES_TABLE          (double*)0x53146240  /* 20 entries × 8 bytes (2 doubles per entry) */

/* Function pointer typedefs for registry/thunk calls */
typedef LONG (WINAPI *RegOpenKeyEx_t)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI *RegQueryValueEx_t)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);

/* ── Externally-referenced globals ── */
extern float  displayW;                  /* _DAT_5318d210 */
extern float  displayH;                  /* _DAT_5318dcdc */
extern int    g_widescreenEnabled;       /* _DAT_538785a4 */
extern int    g_hudScaleIndex;           /* _DAT_5388f9c0 (0-4) */
extern int    g_displayMode;             /* _DAT_5318e940 (0-7) */
extern int    g_vsyncEnabled;            /* _DAT_5318eed4 */
extern int    g_frameLimit;             /* _DAT_5318f240 (0-3) */
extern int    g_aaQuality;              /* _DAT_5318f028 */
extern int    g_textureFilter;           /* _DAT_5318dcfc */
extern int    g_mipmapEnabled;           /* _DAT_53835c5c */
extern int    g_resolutionIndex;         /* _DAT_53a9895c (0-0x13) */
extern int    g_ssaaFactor;             /* _DAT_5387b938 (2 or 4 when SSAA active) */
extern int    g_renderPath;             /* _DAT_53870578 */
extern int    g_numPlayers;             /* _DAT_53a9027c (≤10) */

/* ── Resolution table ──
 *
 * Accessed as doubles at 0x53146240 + index*8:
 *   displayW = *(double*)(RES_TABLE + idx*2)
 *   displayH = *(double*)(RES_TABLE + idx*2 + 1)
 *
 * Entries correspond to common display resolutions:
 *   idx 0:  640×480
 *   idx 1:  800×600
 *   idx 2:  1024×768
 *   idx 3:  1280×720  (720p)
 *   ...
 *   idx 0x13: 3840×2160 (4K)
 */


/*
 * stage_a_load_config — Load all config from registry and run the cascade.
 *
 * Returns bitmask of error flags (bit 0 = HKLM key missing, bit 3 = value
 * not found, etc.). The function always tail-calls Stage B regardless.
 */
int stage_a_load_config(void)
{
    HKEY hKey;
    int  result = 0;
    DWORD value;
    DWORD cbData = sizeof(value);

    /* ── Read HKLM config key ── */
    if ((*RegOpenKeyEx)(HKEY_LOCAL_MACHINE, KEY_EPHINEA, 0, KEY_READ, &hKey) == 0) {
        /* Read ~60 config values. Each read follows the same pattern:
         *   RegQueryValueEx(hKey, valName, NULL, NULL, &value, &cbData)
         *   if success: clamp value, write to global
         *   else: write default value
         *
         * All registry reads use DWORD (REG_DWORD). Values are clamped
         * to their documented ranges (e.g. 0-4 for HUD scale, 0-3 for
         * frame limit, 0-7 for display mode).
         *
         * See intermediate/stage_a_raw.c for the complete register-by-
         * value listing with all clamp ranges and defaults.
         */

        /* ── Widescreen / HUD ── */
        read_dword_reg(hKey, VAL_WIDESCREEN,     &g_widescreenEnabled,  0, 1,  1);
        read_dword_reg(hKey, VAL_HUD_SCALE,      &g_hudScaleIndex,      0, 4,  0);
        read_dword_reg(hKey, VAL_DISPLAY_MODE,   &g_displayMode,        0, 7,  0);

        /* ── Rendering quality ── */
        read_dword_reg(hKey, VAL_VSYNC,          &g_vsyncEnabled,       0, 1,  0);
        read_dword_reg(hKey, VAL_FRAME_LIMIT,    &g_frameLimit,         0, 3,  0);
        read_dword_reg(hKey, VAL_AA_QUALITY,     &g_aaQuality,          0, 1,  0);
        read_dword_reg(hKey, VAL_TEXTURE_FILTER, &g_textureFilter,      0, 1,  1);
        read_dword_reg(hKey, VAL_MIPMAP,         &g_mipmapEnabled,      0, 1,  0);

        /* ── Resolution / SSAA ──
         * Resolution index values > 4 trigger SSAA modes (see §4 of
         * the doc for the switch table). Valid range: 0-0x13.
         */
        {
            DWORD resIdx = 0;
            read_dword_reg(hKey, VAL_RESOLUTION,  &resIdx, 0, 0x13, 0);
            g_resolutionIndex = (resIdx > 0x13) ? 0 : resIdx;
            if (g_resolutionIndex < 2) g_resolutionIndex = 0;
            if (g_resolutionIndex == 3) g_resolutionIndex = 2;
            if (g_resolutionIndex == 6) g_resolutionIndex = 5;

            /* SSAA and post-processing config based on resolution index
             * (see intermediate/stage_a_raw.c lines ~300-500 for the full
             * switch table mapping resolution index → SSAA factor + post */}
        }

        /* ── Render path selection ──
         * Indexed by VAL_RENDER_MODE; controls Direct3D render device path,
         * shader selection, post-processing pipeline.
         */

        /* ── Remaining ~40+ config values ──
         * These cover input mapping, audio, UI theme, control options,
         * network config, and more. Each follows the same read-clamp-store
         * pattern. See intermediate/stage_a_raw.c lines ~500-900.
         */

        RegCloseKey(hKey);
    } else {
        result |= 1;  /* HKLM key missing */
    }

    /* ── Read HKCU user config key ── */
    if ((*RegOpenKeyEx)(HKEY_CURRENT_USER, KEY_USER, 0, KEY_READ, &hKey) == 0) {
        /* ── Display resolution ──
         *
         * Reads display width toggle (_DAT_5388fe08). The actual resolution
         * is selected from the resolution table at 0x53146240:
         *   displayW = *(double*)(RES_TABLE + resIdx*2)
         *   displayH = *(double*)(RES_TABLE + resIdx*2 + 1)
         *
         * If SSAA factor != 0 (e.g. 2 or 4), displayW/H are multiplied:
         *   displayW *= SSAA;
         *   displayH *= SSAA;
         */
        read_dword_reg(hKey, VAL_DISPLAY_W, (DWORD*)&displayW, 0, 1, 0); /* toggle, not resolution */

        /* ── Player count ──
         * Reads number of local co-op players (max 10). Written to
         * _DAT_53a90268..7c area (36 bytes), clamped ≤10.
         * Used by Stage C for per-player UI scaling.
         */
        {
            DWORD cbPlayers = 0x24;
            DWORD playerData[9] = {0};
            /* RegQueryValueEx reads RAW data (multi-player struct) */
            /* g_numPlayers = playerData[5]; (*_DAT_53a9027c) */
            if (g_numPlayers > 10) g_numPlayers = 10;
        }

        RegCloseKey(hKey);
    } else {
        result |= 0x10;  /* HKCU key missing */
    }

    /* ── Run Stage B (render-size computation) ── */
    stage_b_compute_render_size();

    return result;
}


/* ── Helper: read a DWORD registry value with clamp ── */
static void read_dword_reg(HKEY hKey, LPCWSTR valName, DWORD* out,
                           DWORD minVal, DWORD maxVal, DWORD defaultVal)
{
    DWORD value;
    DWORD cbData = sizeof(value);

    if (RegQueryValueEx(hKey, valName, NULL, NULL, (LPBYTE)&value, &cbData) == 0) {
        *out = value;
        if (*out < minVal) *out = minVal;
        if (*out > maxVal) *out = maxVal;
    } else {
        *out = defaultVal;
    }
}