// asset_registry.c -- char-screen / scene asset registry implementation.
// One-shot patches at attach (apply_initial_scale) plus per-row live
// writes for slider tuning. No per-frame work, no engine fight.

#include "asset_registry.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string.h>

// Forward decl from pso_widescreen.c -- shared logger.
extern void log_line(const char *fmt, ...);

// =====================================================================
// REGISTRY DATA
// =====================================================================
// Initial population: char-create scene descriptor blocks identified
// via r2 static analysis of fcn.004121f0 (char-create scene init).
//
// "selectgamen" = the teal/cyan background plates the user reported as
// unscaled. Two adjacent .data blocks at 0x008FAB0C..0x008FAB34 (10
// floats) and 0x008FAB9C..0x008FABB0 (additional positional floats).
//
// AKIND_RAW entries (256, 0.5, -1) are atlas dims / UV / sentinels --
// NOT scaled but exposed via slider for manual override if needed.

const AssetEntry pso_asset_registry_curated[] = {
    // ---- f256_selectgamen (background plates) -- block 1 -----------
    { "selectgamen.b1.302",  "char_screen.selectgamen", AKIND_SIZE, 0x008FAB0Cu, 302.0f, -100.0f, 2500.0f },
    { "selectgamen.b1.28",   "char_screen.selectgamen", AKIND_POS_Y, 0x008FAB10u,  28.0f, -100.0f, 2500.0f },
    { "selectgamen.b1.250",  "char_screen.selectgamen", AKIND_SIZE, 0x008FAB18u, 250.0f, -100.0f, 2500.0f },
    { "selectgamen.b1.155",  "char_screen.selectgamen", AKIND_POS_X, 0x008FAB1Cu, 155.0f, -100.0f, 2500.0f },
    { "selectgamen.b1.198",  "char_screen.selectgamen", AKIND_SIZE, 0x008FAB20u, 198.0f, -100.0f, 2500.0f },
    { "selectgamen.b1.185",  "char_screen.selectgamen", AKIND_POS_X, 0x008FAB24u, 185.0f, -100.0f, 2500.0f },
    { "selectgamen.b1.249",  "char_screen.selectgamen", AKIND_SIZE, 0x008FAB28u, 249.0f, -100.0f, 2500.0f },
    { "selectgamen.b1.178",  "char_screen.selectgamen", AKIND_POS_X, 0x008FAB2Cu, 178.0f, -100.0f, 2500.0f },
    { "selectgamen.b1.197",  "char_screen.selectgamen", AKIND_SIZE, 0x008FAB30u, 197.0f, -100.0f, 2500.0f },
    { "selectgamen.b1.208",  "char_screen.selectgamen", AKIND_POS_X, 0x008FAB34u, 208.0f, -100.0f, 2500.0f },

    // ---- f256_selectgamen -- block 2 (constants + positional) -----
    { "selectgamen.b2.sentinel",  "char_screen.selectgamen", AKIND_RAW,   0x008FAB9Cu,   -1.0f,    -10.0f,   10.0f },
    { "selectgamen.b2.28",        "char_screen.selectgamen", AKIND_POS_Y, 0x008FABA0u,   28.0f,   -100.0f, 2500.0f },
    { "selectgamen.b2.atlas_dim", "char_screen.selectgamen", AKIND_RAW,   0x008FABA4u,  256.0f,    1.0f,   1024.0f },
    { "selectgamen.b2.uv_half",   "char_screen.selectgamen", AKIND_RAW,   0x008FABA8u,    0.5f,    0.0f,      1.0f },
    { "selectgamen.b2.27",        "char_screen.selectgamen", AKIND_POS_X, 0x008FABACu,   27.0f,   -100.0f, 2500.0f },
    { "selectgamen.b2.32",        "char_screen.selectgamen", AKIND_POS_Y, 0x008FABB0u,   32.0f,   -100.0f, 2500.0f },

    // ---- f256_player_prate (preview anchor) ------------------------
    // From earlier float-scan near string @ 0x008FA4E8.
    { "player_prate.421", "char_screen.player_prate", AKIND_POS_X, 0x008FA514u, 421.0f, -100.0f, 2500.0f },
    { "player_prate.432", "char_screen.player_prate", AKIND_POS_X, 0x008FA518u, 432.0f, -100.0f, 2500.0f },
    { "player_prate.199", "char_screen.player_prate", AKIND_SIZE,  0x008FA51Cu, 199.0f, -100.0f, 2500.0f },
    { "player_prate.112", "char_screen.player_prate", AKIND_POS_Y, 0x008FA520u, 112.0f, -100.0f, 2500.0f },
    { "player_prate.426", "char_screen.player_prate", AKIND_POS_X, 0x008FA528u, 426.0f, -100.0f, 2500.0f },
    { "player_prate.250", "char_screen.player_prate", AKIND_SIZE,  0x008FA52Cu, 250.0f, -100.0f, 2500.0f },

    // ---- f256_player_tex (3D char preview window) ------------------
    { "player_tex.32a",  "char_screen.player_tex", AKIND_POS_Y, 0x008FAE3Cu,  32.0f,  -100.0f, 2500.0f },
    { "player_tex.400",  "char_screen.player_tex", AKIND_POS_X, 0x008FAE4Cu, 400.0f,  -100.0f, 2500.0f },
    { "player_tex.640",  "char_screen.player_tex", AKIND_SIZE,  0x008FAE50u, 640.0f,  -100.0f, 2500.0f },
    { "player_tex.421a", "char_screen.player_tex", AKIND_POS_X, 0x008FAE54u, 421.0f,  -100.0f, 2500.0f },
    { "player_tex.432",  "char_screen.player_tex", AKIND_POS_X, 0x008FAE58u, 432.0f,  -100.0f, 2500.0f },
    { "player_tex.421b", "char_screen.player_tex", AKIND_POS_X, 0x008FAE5Cu, 421.0f,  -100.0f, 2500.0f },
    { "player_tex.348",  "char_screen.player_tex", AKIND_POS_Y, 0x008FAE60u, 348.0f,  -100.0f, 2500.0f },
    { "player_tex.421c", "char_screen.player_tex", AKIND_POS_X, 0x008FAE64u, 421.0f,  -100.0f, 2500.0f },
    { "player_tex.205",  "char_screen.player_tex", AKIND_POS_Y, 0x008FAE68u, 205.0f,  -100.0f, 2500.0f },
    { "player_tex.32b",  "char_screen.player_tex", AKIND_POS_Y, 0x008FAE84u,  32.0f,  -100.0f, 2500.0f },
    { "player_tex.400b", "char_screen.player_tex", AKIND_POS_X, 0x008FAE8Cu, 400.0f,  -100.0f, 2500.0f },
    { "player_tex.640b", "char_screen.player_tex", AKIND_SIZE,  0x008FAE90u, 640.0f,  -100.0f, 2500.0f },

    // ---- f256_tag (info tag widget) --------------------------------
    { "tag.180",  "char_screen.tag", AKIND_POS_Y, 0x008FACA8u, 180.0f, -100.0f, 2500.0f },
    { "tag.184a", "char_screen.tag", AKIND_POS_Y, 0x008FACBCu, 184.0f, -100.0f, 2500.0f },
    { "tag.160a", "char_screen.tag", AKIND_POS_X, 0x008FACC0u, 160.0f, -100.0f, 2500.0f },
    { "tag.184b", "char_screen.tag", AKIND_POS_Y, 0x008FACF4u, 184.0f, -100.0f, 2500.0f },
    { "tag.160b", "char_screen.tag", AKIND_POS_X, 0x008FACF8u, 160.0f, -100.0f, 2500.0f },
    { "tag.145",  "char_screen.tag", AKIND_POS_X, 0x008FAD14u, 145.0f, -100.0f, 2500.0f },
    { "tag.24",   "char_screen.tag", AKIND_POS_Y, 0x008FAD18u,  24.0f, -100.0f, 2500.0f },
    { "tag.174",  "char_screen.tag", AKIND_POS_X, 0x008FAD1Cu, 174.0f, -100.0f, 2500.0f },
    { "tag.32",   "char_screen.tag", AKIND_POS_Y, 0x008FAD20u,  32.0f, -100.0f, 2500.0f },
    { "tag.86",   "char_screen.tag", AKIND_POS_Y, 0x008FAD98u,  86.0f, -100.0f, 2500.0f },
    { "tag.620",  "char_screen.tag", AKIND_POS_X, 0x008FAD9Cu, 620.0f, -100.0f, 2500.0f },
    { "tag.83",   "char_screen.tag", AKIND_POS_Y, 0x008FADA0u,  83.0f, -100.0f, 2500.0f },
};

const int pso_asset_registry_curated_count =
    (int)(sizeof(pso_asset_registry_curated) / sizeof(pso_asset_registry[0]));

// =====================================================================
// HELPERS
// =====================================================================

// SEH-guarded VirtualProtect + write float to a .data address.
static int safe_write_float(uintptr_t va, float v) {
    DWORD old_prot = 0;
    if (!VirtualProtect((LPVOID)va, sizeof(float),
                        PAGE_EXECUTE_READWRITE, &old_prot)) {
        return 0;
    }
    int ok = 1;
    __try {
        *(volatile float *)va = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    DWORD tmp = 0;
    VirtualProtect((LPVOID)va, sizeof(float), old_prot, &tmp);
    return ok;
}

// SEH-guarded read float from a .data address.
static float safe_read_float(uintptr_t va) {
    float v = 0.0f;
    __try { v = *(volatile const float *)va; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0.0f; }
    return v;
}

// =====================================================================
// EXPORTED API
// =====================================================================

// Forward decls for the auto-generated companion array (defined in
// asset_registry_generated.c, produced by tmp_descriptor_scanner.py).
// Linker provides both as 0-count when the file is absent.
extern const AssetEntry pso_asset_registry_generated[];
extern const int pso_asset_registry_generated_count;

__declspec(dllexport) int pso_assets_count(void) {
    return pso_asset_registry_curated_count + pso_asset_registry_generated_count;
}

__declspec(dllexport) const AssetEntry* pso_assets_entry(int idx) {
    if (idx < 0) return NULL;
    if (idx < pso_asset_registry_curated_count)
        return &pso_asset_registry_curated[idx];
    const int j = idx - pso_asset_registry_curated_count;
    if (j < pso_asset_registry_generated_count)
        return &pso_asset_registry_generated[j];
    return NULL;
}

__declspec(dllexport) float pso_assets_get_live(int idx) {
    const AssetEntry *e = pso_assets_entry(idx);
    if (!e) return 0.0f;
    return safe_read_float(e->va);
}

__declspec(dllexport) int pso_assets_set_value(int idx, float value) {
    const AssetEntry *e = pso_assets_entry(idx);
    if (!e) return 0;
    return safe_write_float(e->va, value);
}

// Group-name prefix exclusion list. Entries whose `group` field starts
// with any of these strings are NOT scaled by apply_initial_scale.
// Bisected 2026-05-09: the auto-generator picked up f256_selectgamen_b
// (= login menu) and f256_carsol (= map cursor) which were never part
// of the user's original char-create calibration; scaling their POS_Y
// by 1.65 dropped the login menu to the bottom of the screen and made
// the area-map cursor land in the wrong place. Char-create groups
// (f256_player_*, f256_psotitle, etc.) remain in the scaling set.
static const char *const kScaleExcludeGroups[] = {
    "auto.f256_selectgamen_b", // login menu / select-game screen
    "auto.f256_carsol",        // area-map cursor positions
    // Login-screen ad / advertisement plates. These hold the engine's
    // own login menu Y anchors; scaling them by 1.65 drops the visible
    // login menu (Start Game / Exit Game / Patch Download / Register
    // UserID) to the very bottom of the screen. Bisected 2026-05-09.
    "auto.f256_ad",            // covers f256_ad, f256_ad_eng (prefix-match)
    "auto.f256_adpic",         // covers f256_adpic01, future suffixes
    "auto.f256_hyouji",        // login-display plates ("hyouji" = display)
    "auto.f256_psotitle",      // PSO title plate (Sodaboy already handles)
    "auto.f256_titlealpha",    // title alpha-blend overlay
    "auto.f256_nowloading",    // "now loading" splash
    NULL,
};

static int group_excluded(const char *category) {
    if (!category) return 0;
    for (const char *const *p = kScaleExcludeGroups; *p; ++p) {
        const char *needle = *p;
        size_t n = 0;
        while (needle[n] && category[n] && needle[n] == category[n]) ++n;
        if (needle[n] == 0) return 1; // category has needle as prefix
    }
    return 0;
}

__declspec(dllexport) int pso_assets_apply_initial_scale(
        float stretch, float shift_x, float shift_y) {
    int n_patched = 0;
    int n_skipped_kind = 0;
    int n_skipped_mismatch = 0;
    int n_skipped_group = 0;
    const int total = pso_assets_count();
    for (int i = 0; i < total; ++i) {
        const AssetEntry *e = pso_assets_entry(i);
        if (!e) continue;
        // Skip non-scaling kinds.
        if (e->kind == AKIND_RAW || e->kind == AKIND_NONE ||
            e->kind == AKIND_ANIM_RATE || e->kind == AKIND_COLOR_RGBA) {
            ++n_skipped_kind;
            continue;
        }
        // Skip groups the bisect identified as breaking when scaled.
        if (group_excluded(e->category)) {
            ++n_skipped_group;
            continue;
        }
        // Verify live matches baseline (else: already patched / Sodaboy
        // touched / version drift -- skip to avoid double-scale).
        const float live = safe_read_float(e->va);
        const float diff = (live > e->baseline)
                           ? live - e->baseline
                           : e->baseline - live;
        if (diff > 0.01f) {
            ++n_skipped_mismatch;
            log_line("[pso_widescreen] asset[%d] %s: skip (live=%.2f != baseline=%.2f)",
                     i, e->id, live, e->baseline);
            continue;
        }
        // Compute scaled value per kind.
        float scaled = e->baseline;
        switch (e->kind) {
            case AKIND_POS_X: scaled = e->baseline * stretch + shift_x; break;
            case AKIND_POS_Y: scaled = e->baseline * stretch + shift_y; break;
            case AKIND_SIZE:  scaled = e->baseline * stretch;            break;
            default: break;
        }
        if (safe_write_float(e->va, scaled)) ++n_patched;
    }
    log_line("[pso_widescreen] asset registry: %d/%d scaled "
             "(%d skipped-kind, %d skipped-group, %d skipped-mismatch); "
             "stretch=%.3f shift=(%.1f, %.1f)",
             n_patched, total,
             n_skipped_kind, n_skipped_group, n_skipped_mismatch,
             (double)stretch, (double)shift_x, (double)shift_y);
    return n_patched;
}

__declspec(dllexport) void pso_assets_reset_all(void) {
    int n = 0;
    const int total = pso_assets_count();
    for (int i = 0; i < total; ++i) {
        const AssetEntry *e = pso_assets_entry(i);
        if (!e) continue;
        if (safe_write_float(e->va, e->baseline)) ++n;
    }
    log_line("[pso_widescreen] asset registry: %d/%d reset to baseline",
             n, total);
}

// =====================================================================
// asset_overrides.ini loader
// =====================================================================
// Reads <install>/patches/asset_overrides.ini at attach (called from
// pso_widescreen.c DllMain after pso_assets_apply_initial_scale). Each
// "id=value" line is looked up via linear scan and written to the entry's
// .data VA. Skips IDs that don't match any registry row (forward/backward
// compat for build-version drift).
#include <stdio.h>
#include <stdlib.h>

static int registry_index_of(const char *id) {
    if (!id || !*id) return -1;
    const int n = pso_assets_count();
    for (int i = 0; i < n; ++i) {
        const AssetEntry *e = pso_assets_entry(i);
        if (!e || !e->id) continue;
        if (strcmp(e->id, id) == 0) return i;
    }
    return -1;
}

__declspec(dllexport) int pso_assets_load_overrides_ini(const char *path) {
    if (!path || !*path) return -1;
    FILE *f = NULL;
    if (fopen_s(&f, path, "r") != 0 || !f) {
        log_line("[pso_widescreen] asset overrides: no INI at %s (skipping)", path);
        return 0;
    }
    char line[512];
    int applied = 0, missing = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == ';' || line[0] == '#' || line[0] == '[' ||
            line[0] == '\n' || line[0] == 0) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        // strip whitespace from id end
        char *end = eq - 1;
        while (end > line && (*end == ' ' || *end == '\t' || *end == '\r')) {
            *end-- = 0;
        }
        const char *id = line;
        const float v = (float)atof(eq + 1);
        const int idx = registry_index_of(id);
        if (idx < 0) {
            ++missing;
            continue;
        }
        if (pso_assets_set_value(idx, v)) ++applied;
    }
    fclose(f);
    log_line("[pso_widescreen] asset overrides: %d applied, %d missing-from-registry (path=%s)",
             applied, missing, path);
    return applied;
}
