// asset_registry.h -- shared header for cross-asi asset registry.
// Owned by pso_widescreen.asi; mod_widescreen_tuner.cpp consumes via
// GetProcAddress on the exported helpers below.
//
// Design goal: every char-create / lobby / title scene asset that the
// engine reads at scene-init time has a row here. Each row carries a
// .data address, baseline value, semantic kind (positional X/Y/size/
// raw constant/animation rate/color), and slider range. One pass at
// attach scales every scale-aware row by the global stretch; per-asset
// sliders write back through the same one-shot path.

#ifndef PSO_ASSET_REGISTRY_H
#define PSO_ASSET_REGISTRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Semantic kind drives how apply_initial_scale treats the entry.
//   POS_X / POS_Y : value = baseline * stretch + shift_axis
//   SIZE          : value = baseline * stretch (no shift)
//   RAW           : do NOT auto-scale (atlas dim / UV / sentinel /
//                   alpha) -- slider still tunes manually
//   ANIM_RATE     : per-frame animation rate; user-tunable directly
//   COLOR_RGBA    : packed 0xAABBGGRR; slider edits whole dword
typedef enum {
    AKIND_NONE       = 0,
    AKIND_POS_X      = 1,
    AKIND_POS_Y      = 2,
    AKIND_SIZE       = 3,
    AKIND_RAW        = 4,
    AKIND_ANIM_RATE  = 5,
    AKIND_COLOR_RGBA = 6,
} AssetKind;

typedef struct {
    const char *id;          // e.g. "selectgamen.plateA.x_extent"
    const char *category;    // e.g. "char_screen.selectgamen"
    int32_t     kind;        // AssetKind
    uintptr_t   va;          // .data address
    float       baseline;    // expected initial value at attach
    float       lo;          // slider range lo
    float       hi;          // slider range hi
} AssetEntry;

// Exported registry data. Defined in asset_registry.c.
extern const AssetEntry pso_asset_registry[];
extern const int        pso_asset_registry_count;

// Exported helpers (see asset_registry.c for impl).
//   - count()                : returns registry size
//   - entry(i)               : returns Nth entry pointer (or NULL)
//   - get_live(i)            : reads current value at registry[i].va
//   - set_value(i, value)    : writes value to registry[i].va
//                              (with VirtualProtect + size-aware write).
//                              returns 1 on success, 0 on failure.
//   - apply_initial_scale()  : one-shot pass; for each scale-aware row
//                              writes baseline * stretch (+ shift if
//                              POS_X / POS_Y). Returns count patched.
//   - reset_all()            : restores baselines to every row.
__declspec(dllexport) int        pso_assets_count(void);
__declspec(dllexport) const AssetEntry* pso_assets_entry(int idx);
__declspec(dllexport) float      pso_assets_get_live(int idx);
__declspec(dllexport) int        pso_assets_set_value(int idx, float value);
__declspec(dllexport) int        pso_assets_apply_initial_scale(
                                    float stretch, float shift_x, float shift_y);
__declspec(dllexport) void       pso_assets_reset_all(void);
__declspec(dllexport) int        pso_assets_load_overrides_ini(const char *path);

#ifdef __cplusplus
}
#endif

#endif // PSO_ASSET_REGISTRY_H
