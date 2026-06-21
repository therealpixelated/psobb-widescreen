// pso_widescreen.c — stack-agnostic widescreen / resolution-override ASI
// for PSOBB.
//
// VERSION 2026-06-19b — Trinity-gap comprehensive-coverage pass. Added 35
// value-guarded X-axis bakes for elements Ephinea+Trinity widescreen but we
// previously MISSED (dressing-room rows, char-select Tab/Enter key-prompts,
// F1 Help, login/fixed-chat textures, the in-game menu dispatch table, and
// 2 config-menu X's). All idempotent + 4:3 no-op + uniqueness-verified vs the
// live psobb.exe; 0x0096F2B8 (per-frame RMW patch-bar width) intentionally
// skipped as address-unsafe. New fns: patch_dressingroom_layout /
// patch_lobby_extras / patch_frontend_misc / patch_ingame_menu_table /
// patch_config_menus + fold-ins to patch_charselect_layout/banner_center.
//
// Originally Sodaboy's d3d8.dll wrapper bundled this with his
// post-FX (MSAA/SMAA/SSAO/etc.) — extracting just the widescreen + scale
// part means anyone can pick whichever d3d8.dll they want (Sodaboy's,
// d3d8to11, dgVoodoo, native, etc.) and still get a non-4:3 backbuffer.
//
// Hook strategy (chosen to be d3d8-implementation-agnostic):
//
// 1. IAT-patch psobb.exe's import of `d3d8.dll!Direct3DCreate8`. The
// patch lives in PSOBB's import table, so it fires regardless of
// which DLL is currently resolving the d3d8.dll name — Sodaboy's
// shim, d3d8to11, native d3d8, etc.
//
// 2. When PSOBB calls our hooked Direct3DCreate8, we forward to the
// real one, get an IDirect3D8 *factory back, then vtable-patch
// its `CreateDevice` slot (slot 15 in the d3d8 vtable). The
// vtable patch is once-per-factory; subsequent CreateDevice
// calls on the same instance flow through us.
//
// 3. Our CreateDevice hook rewrites the user's D3DPRESENT_PARAMETERS
// to substitute the configured BackBufferWidth/Height and the
// windowed flag, then forwards to the real CreateDevice. The
// engine still queries device caps + creates its swapchain
// against the new resolution.
//
// What we deliberately do NOT do (yet):
// - HUD scale — Sodaboy's HUDScale knob requires hooking the engine's
// HUD draw path (DrawPrimitiveUP with screen-space coords). That
// belongs in a separate ASI / a follow-up patch.
// - FOV / aspect adjustment — PSOBB's projection matrix is built
// internally; getting "hor+" (more world horizontally instead of
// vertical squish) needs a SetTransform(D3DTS_PROJECTION) hook.
// Also a follow-up.
// - Post-FX (MSAA/SMAA/SSAO/CelShader/DOF/HDR) — those were Sodaboy's
// d3d8.dll post-pipeline. Now dgVoodoo is out of the chain too,
// ReShade is the right home for those (already installed at
// <install>\reshade-shaders).
//
// Config: <psobb.exe dir>\widescreen.cfg, simple Key=Value lines, no
// sections. Same format Sodaboy uses so an existing user's file just
// works. We only consume Windowed / Width / Height / DebugLogsEnabled
// — keys we don't recognise are ignored without complaint.

#include <Windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <MinHook.h>          // MinHook detours (char-create composer hooks).
#include "asset_registry.h"
#include "pso_widescreen.h"

// REFACTOR: sodaboy_tables.h retired — the entire Sodaboy LOOP/
// Region coordinate bake was removed (anzz1 is the sole static coordinate
// source of truth). No other TU consumes those tables.

// ---- Minimal D3D8 ABI we need ----
//
// We don't include d3d8.h because it's not always present in modern SDKs
// and we only need vtable layouts, not the full type machinery. The
// IDirect3D8 vtable indices are stable since DirectX 8.0 (2000).

typedef struct IDirect3D8         IDirect3D8;
typedef struct IDirect3DDevice8   IDirect3DDevice8;

// D3DPRESENT_PARAMETERS as of d3d8: layout is critical, the engine
// passes a pointer to one of these and we mutate fields by offset.
typedef struct {
    UINT  BackBufferWidth;
    UINT  BackBufferHeight;
    DWORD BackBufferFormat;
    UINT  BackBufferCount;
    DWORD MultiSampleType;
    DWORD SwapEffect;
    HWND  hDeviceWindow;
    BOOL  Windowed;
    BOOL  EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat;
    DWORD Flags;
    UINT  FullScreen_RefreshRateInHz;
    UINT  FullScreen_PresentationInterval;
} D3DPRESENT_PARAMETERS_X;

// IDirect3D8::CreateDevice signature — same calling convention as DX8.
typedef HRESULT (STDMETHODCALLTYPE *CreateDevice_t)(
    IDirect3D8 *self,
    UINT Adapter,
    DWORD DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS_X *pPresentationParameters,
    IDirect3DDevice8 **ppReturnedDeviceInterface);

// IDirect3DDevice8::SetTransform — slot 37 in the d3d8 device vtable.
// We intercept D3DTS_PROJECTION (transform type 3) to apply hor+ FOV
// correction, otherwise pass through unmodified.
typedef struct {
    float _11, _12, _13, _14;
    float _21, _22, _23, _24;
    float _31, _32, _33, _34;
    float _41, _42, _43, _44;
} D3DMATRIX_X;

typedef HRESULT (STDMETHODCALLTYPE *SetTransform_t)(
    IDirect3DDevice8 *self, DWORD State, const D3DMATRIX_X *pMatrix);

#define D3DTS_VIEW       2
#define D3DTS_PROJECTION 3

// IDirect3DDevice8::SetViewport — slot 40 in the d3d8 device vtable.
// PSOBB calls SetViewport(0, 0, engine_w, engine_h) — usually the
// launcher-selected resolution like 1024x768 — once the device is up,
// even though we forced a 3840x2160 backbuffer. The result without a
// hook is "engine renders to upper-left 1024x768 region of a 4K
// backbuffer". We override X/Y/Width/Height to fill the full
// configured backbuffer (MinZ/MaxZ pass through unchanged).
typedef struct {
    DWORD X;
    DWORD Y;
    DWORD Width;
    DWORD Height;
    float MinZ;
    float MaxZ;
} D3DVIEWPORT8_X;

typedef HRESULT (STDMETHODCALLTYPE *SetViewport_t)(
    IDirect3DDevice8 *self, const D3DVIEWPORT8_X *pViewport);

// FVF / vertex-shader tracking for HUDScale.
// In d3d8, IDirect3DDevice8::SetVertexShader (vtable slot 50) takes a
// DWORD that's either an FVF (low bits) or a vertex-shader handle. RHW
// (transformed-and-lit) verts have the D3DFVF_XYZRHW bit (= 0x4) set in
// their position type. Sodaboy's HUDScale only affects RHW geometry
// because that's what the engine uses for HUD/UI/font/splash 2D draws.
#define D3DFVF_POSITION_MASK 0x000e
#define D3DFVF_XYZ           0x0002
#define D3DFVF_XYZRHW        0x0004

typedef HRESULT (STDMETHODCALLTYPE *SetVertexShader_t)(
    IDirect3DDevice8 *self, DWORD Handle);

// IDirect3DDevice8::DrawPrimitiveUP — slot 53. Engine submits 2D HUD
// quads here with RHW pixel-space coords. We scale x/y by 1/HUDScale
// so they cover more pixels (= bigger HUD) when HUDScale > 1.
typedef HRESULT (STDMETHODCALLTYPE *DrawPrimitiveUP_t)(
    IDirect3DDevice8 *self, DWORD PrimitiveType, UINT PrimitiveCount,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride);

// IDirect3DDevice8::DrawIndexedPrimitiveUP — slot 54.
typedef HRESULT (STDMETHODCALLTYPE *DrawIndexedPrimitiveUP_t)(
    IDirect3DDevice8 *self, DWORD PrimitiveType,
    UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount,
    const void *pIndexData, DWORD IndexDataFormat,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride);

// d3d8.dll!Direct3DCreate8 signature.
typedef IDirect3D8 *(WINAPI *Direct3DCreate8_t)(UINT SDKVersion);

// ---- Config ----
//
// We read these once at DllMain attach. There's no live-reload because
// CreateDevice fires very early in PSOBB startup (before any user could
// have edited the config) and changing resolution mid-flight requires
// IDirect3DDevice8::Reset, which we don't intercept.

static struct {
    int   enabled;            // master switch — default 0, set to 1 in cfg
    int   windowed;           // 0=fullscreen, 1=windowed, 2=borderless windowed
    int   width;              // target dimensions (window + optional backbuffer)
    int   height;
    // Sodaboy convention — engine canvas is FIXED at logical
    // 1920x1080 regardless of physical Width/Height. The d3d8 wrapper
    // (Sodaboy or dgVoodoo) handles upscaling 1920x1080 to physical.
    // Patching engine internals (res_table, anchors) with PHYSICAL Width/
    // Height makes downstream coord math 2x too big at 4K, which is what
    // crippled our 2D UI in the baseline diff. Default 1920/1080;
    // INI keys LogicalWidth / LogicalHeight override.
    int   logical_width;
    int   logical_height;
    int   override_backbuffer; // 1 = rewrite pp->BackBufferWidth/Height to width/height.
                               // Default 0 (stretch mode): leave engine's native
                               // backbuffer alone and let the swap chain stretch
                               // to the borderless window. Setting to 1 selects
                               // the engine-native render path which requires
                               // matching memory patches in psobb.exe to avoid
                               // RHW HUD verts being mapped to the upper-left.
    int   override_viewport;   // 1 = rewrite SetViewport to backbuffer dims. Only
                               // useful with override_backbuffer=1. Default 0.
    int   integer_scale;       // 1 = "bump the base render": render the backbuffer at a
                               // CLEAN integer divisor of the physical Width/Height
                               // (capped ~1080p) so the present upscale is a clean Nx
                               // (4K -> render 1920x1080, present 2x) instead of an
                               // ugly non-integer/aspect-mismatched stretch (1280x800
                               // 16:10 -> 4K 16:9). Fixes the Wine/wined3d 4K present
                               // perf cliff w/o dgVoodoo. No-op <=1440p. Forces
                               // override_backbuffer + override_viewport. Default 0.
    int   fov_correction;      // 1 = scale projection m11 for hor+ widescreen
    float game_aspect;         // engine-assumed aspect ratio (default 4/3)
    // REFACTOR: the Sodaboy LOOP/Region coordinate bake is RETIRED
    // (anzz1 is the sole static coordinate source of truth). The following cfg
    // fields are GONE: patch_validator_nop, patch_res_table, patch_hud_rects,
    // patch_extra_ui, patch_flare_pin, patch_charscreen_scale,
    // patch_asset_registry, load_asset_overrides_ini, patch_title_art,
    // patch_title_real, title_real_mode, patch_phase3, widescreen_engine_anzz1.
    // Their INI keys are accepted-and-ignored (unknown-key fall-through);
    // WidescreenEngine specifically logs an obsolete-key warning for one
    // release. override_backbuffer / override_viewport / fov_correction /
    // logical_width / logical_height are KEPT — they are device-plumbing knobs
    // consumed by the CreateDevice / SetViewport / SetTransform hooks, NOT
    // sodaboy-apply selectors.
    int   patch_splash_phase2; // 1 = widen the SEGA/boot splash sprite-quad
                               // table at .data 0x009A3420 (4 entries × 0x20
                               // stride; x1@+0x00, x2@+0x08). D3 
                               // now ALWAYS-ON on the default path (folded into
                               // apply_static_patches), de-conflicted against
                               // anzz1 — the 2 X-coords anzz1 owns
                               // (0x9A3468/0x9A3488, in listHUDWidth) are SKIPPED
                               // so anzz1's horizontal extent A wins there.
                               // Default 1; per-feature knob (D1).
    float hud_scale;           // master UI mem-patch scale (mults the patched
                               // patched UI offsets). 1.0 = canonical widescreen
                               // positioning; >1 = HUD spread further toward
                               // edges; <1 = HUD shrinks toward 4:3 anchor (loses
                               // widescreen positioning). Default 1.0.
    float hud_compress;        // RHW hook divide knob, decoupled from HUDScale.
                               // 1.0 = no compression; >1 = HUD verts divided
                               // around (0,0) → smaller HUD, more world visible.
                               // This is the "MORE ROOM" knob.
    // CURSOR-WARP FIX. PSOBB's in-game mouse menus physically
    // SetCursorPos the OS pointer to the selected menu element. The warp
    // target is computed from the engine's UI-scale globals ([0xAF035C]
    // etc) at 3 sites (0x0070994B / 0x0070A018 / 0x0070A3FF), all of which
    // call SetCursorPos through the SAME import slot [0x008F8344] — and
    // those 3 are the ONLY callers of that slot in psobb.exe. But the HUD
    // is additionally scaled OUTSIDE the engine-scale path: HUDCompress
    // via a draw-time RHW center-divide (scale_rhw_apply) and HUDScale via
    // Sodaboy .text imm32 rewrites. Neither updates [0xAF035C], so the OS
    // cursor lands at the un-extra-scaled engine position while the element
    // is DRAWN elsewhere → the pointer physically jumps off the element.
    //
    // Fix: import-hook [0x008F8344] and transform the warp target through
    // the SAME center-relative scale the HUD is actually drawn at, so the
    // OS cursor lands on the drawn element. Center = client-rect center
    // (== backbuffer center for the RHW divide, which works in BB px).
    // HUDCompress: drawn = center + (p-center)/compress  → cursor same.
    // HUDScale:    drawn = center + (p-center)*hud_scale  (folded in too;
    // the engine-scale globals carry the canonical 1.0 HUD
    // position, the Sodaboy imm32 path grows it by hud_scale
    // about the canvas, which to first order is a center-
    // relative scale at the screen level).
    // Default ON. Set CursorWarpFix=0 to disable for A/B testing.
    int   cursor_warp_fix;
    // HANGAME-TITLE-MENU FIX (proven via live RE). The hangame
    // title "Login / Start Game / Exit Game" ListWindow is constructed by
    // sub_00761c48 (gated behind the hangame getter 0x0082D2F8) from an
    // in-image layout struct at 0x00974E08 = { X(+0x00)=210.0, Y(+0x04)=340.0,
    // W(+0x08)=220.0, ... } passed to CreateListWindowObject_0073566c. There
    // is exactly ONE xref to 0x00974E08 (the push at 0x00761C63), so the
    // struct is private to this menu — patching it cannot affect any in-game
    // list window. The container copies X/Y from the struct AT CONSTRUCTION,
    // so a live data-patch after the title is up does nothing; we must patch
    // the in-image struct at ASI-load (before the title screen builds). At the
    // global 1.5x UI matrix, design X=210 lands the box left-of-center (~480px,
    // behind the logo). Setting X=530 centers the 220-wide box (center 640 ->
    // 960px); Y=560 drops it to the lower-center over "PRESS ENTER KEY".
    // LIVE-VALIDATED by writing the constructed container's X/Y and
    // confirming on screen. INI: PatchHangameTitleMenu / HangameMenuX / HangameMenuY.
    int   patch_hangame_title_menu;  // 1 = patch the 0x00974E08 layout struct. Default 1.
    float hangame_menu_x;            // native (hs=1.0) X seeded into the menu. Default 320.
    float hangame_menu_y;            // native (hs=1.0) Y seeded into the menu. Default 326.667.
    // FLARE-SCALE (Bug 1), clean data-patch. The title photon
    // flare (effect_nt particles, draw fcn.00801DF9 → 0x0082B440 @0x00802010)
    // reads its size from a static effect-descriptor at 0x00A101C0: +0x14 =
    // X-scale, +0x18 = Y-scale, both stock 4.0. LIVE-VALIDATED: writing 6.0
    // (1.5×, matching the UI) widens the flare across the 16:9 canvas; it grows
    // about its own (left) anchor so no recenter is needed; the write persists
    // (nothing rewrites it at runtime) and the on-disk value is a static 4.0
    // initializer, so an ASI-load patch sticks. We do NOT touch the shared
    // projector globals [0xAF02xx] (the PatchFlarePin lesson).
    // CAVEAT: 0x00A101C0 is an effect-TYPE descriptor array (stride 0x28);
    // +0x14 is effect-type-0's scale, which MAY be shared by gameplay effects
    // of type 0 (not confirmable statically). So this is **opt-in / default
    // OFF** — enable PatchFlareScale=1 and verify in-game effects look right
    // before relying on it. INI: PatchFlareScale / FlareScale.
    int   patch_flare_scale;   // 1 = patch the 0x00A101C0 flare-descriptor scale. Default 0 (opt-in).
    float flare_scale;         // X+Y scale written to the flare descriptor. Default 6.0 (1.5× of stock 4.0).
    // RUNE-EMBLEM SCALE. Title circular rune seal (TitleEP4_11/_12,
    // the ring + 3 spinning orbs). Scales with the MASTER UI scale (hud_scale),
    // stock values read live from the binary. INI: PatchRuneScale (on/off).
    int   patch_rune_scale;    // 1 = scale the title rune emblem with the master UI scale. Default 1.
    // PHOTON-STREAK MOVE. The title "photon blast" streak group (effect_nt,
    // textures effect_nt.xvm #43 / descriptor 0x00A10878) is spawned by the title-scene
    // constructor, which hardcodes its origin vector (0,0,-47) as three code immediates:
    // X @ 0x006F8366 (stock 0.0), Y @ 0x006F8372 (stock 0.0), Z @ 0x006F837E (-47.0).
    // Patched at ASI LOAD (before the title constructs), the streak is born at the new
    // origin — clean, beam-only, no hook (the shared projector 0xAF02C8/CC is untouched).
    // Offsets are design-space world units (resolution-independent; engine projects to
    // the render res). INI: PatchStreakMove / StreakDX / StreakDY.
    int   patch_streak_move;   // 1 = move the title photon streak. Default 1.
    // PHOTON-STREAK SCALE FIX. The streak emitter reads its viewport-space
    // spawn position as `[0x96E27C] * affine` (X) and `[0x96E278] * affine` (Y), where
    // affine = viewport_w / design_w = 1920 / (853.333 * hud_scale). At hud_scale > 1.0
    // the affine shrinks (e.g. 1.125 at hs2.0) and the streak collapses toward the upper-
    // left corner. Ephinea at hs2.0 uses 330.0/402.0 to compensate. The correct fix is to
    // bake scale-compensated constants at ASI load: constant = target_px / affine =
    // target_px * hud_scale / 2.25, giving the same pixel position at any HudScale.
    // Target matches Ephinea's hs2.0 pixel position: X_px=371.25, Y_px=452.25.
    // 0x96E27C_new = 165.0 * hud_scale  (165 * 2.0 = 330 = Ephinea at hs2.0)
    // 0x96E278_new = 201.0 * hud_scale  (201 * 2.0 = 402 = Ephinea at hs2.0)
    // INI: PatchStreakScale.
    int   patch_streak_scale;  // 1 = fix streak position scale. Default 1.
    // CHAR-SELECT 16:9 layout (info panel / char list / legend).
    int   patch_charselect;    // 1 = reposition char-select UI for 16:9. Default 1.
    // CHAR-CREATE convenience: ESC at the TOP (category/class-select)
    // page transitions back to char-select. Stock ESC at the top does nothing
    // (deeper pages already back up one step in the engine). Additionally gated on
    // patch_charselect (the char-create escape hatch). Default 1.
    // char-create knobs REMOVED. The fields cc_stretch / cc_content /
    // cc_content_dx / cc_stride_left / cc_stride_right / cc_split / cc_debug drove
    // the per-frame composer hook cc_quad_fix_c (@0x0082BB74), which was DELETED per
    // the no-per-frame-patching mandate. They had no other readers, so the field
    // decls + defaults are gone too. Char-create now renders via the static
    // apply_startup_bakes() pass + stock; the runtime-only class-select content rows
    // are intentionally not strided (no static source exists to bake).
    float streak_dx;           // native (hs=1.0) origin-X offset (world units). Default 5.0.
    float streak_dy;           // native (hs=1.0) origin-Y offset (world units). Default 0.0.
    // MINIMAP zoom + position bake (folded in from the standalone
    // minimap_zoom-asi). One-shot writes after anzz1 applies — anzz1 puts the
    // minimap at bg=(693,64,128,128) / vp=(757,128,128,128) in 16:9 design
    // space; this nudges it up-right and zooms it out. Knobs are absolute
    // design-space coords (NOT deltas) and the zoom is the .text imm32 at
    // 0x00804A5D (smaller = wider field of view).
    int   patch_minimap;       // 1 = bake minimap position + zoom. Default 1.
    float minimap_bg_x;        // bg origin X (design space).
    float minimap_bg_y;        // bg origin Y.
    float minimap_vp_x;        // viewport origin X (design space).
    float minimap_vp_y;        // viewport origin Y.
    float minimap_zoom;        // engine zoom divisor (smaller = zoomed out).
    // REFACTOR: patch_phase3 (0x0082B440 splash inline hook),
    // patch_title_art (0x82BB74 push-imm sites) and patch_title_real /
    // title_real_mode (the 8 imm32 0x006f4cf2..0x006f4d66 rewrites) are
    // RETIRED with the Sodaboy path — anzz1 owns the title plate via its
    // 0x009FF080..1A8 floats + the 8 0x006F4CF2.. imm32 writes (anzz1:378-385).
    // Their INI keys fall through harmlessly.
    // (Historical title-art / title-real RE notes lived here; removed with
    // the retired patch_title_art / patch_title_real fields. anzz1 owns the
    // title plate now.)
    // / RESOLVED quest-transition Y patch.
    // Static RE pass (radare2) identified the quest-loading screen draw fn:
    // fcn.0078ddb8 @ 0x0078ddb8  (gated on [0xa9ca40] & 0x2)
    // reads quad-vert tables at 0x9ff080 (4 verts) and 0x9ff0e0 (4 verts),
    // RHW-anchor at 0x9ff1a8 (NOW LOADING text); sprite emit goes through
    // fcn.0082b148 -> fcn.00836d04 (24-byte src verts -> 28-byte d3d8 RHW).
    // `f256_nowloading` (data 0x0097ab20) and `f064_questloadapn16`
    // (data 0x0091f368) are asset names looked up by separate string-table
    // readers — not the draw site, but they confirm the rendering path.
    //
    // Sodaboy's wrapper patches the X coords of these tables (Region A2 in
    // this file already covers F480_M{16,144,272,143}); X coords are correct.
    // But Y coords (352, 416, 480) are LEFT UNTOUCHED. Under d3d8to11 — which
    // maps RHW directly to physical viewport — Y=480 lands at 480/1080 = 44%
    // from top, putting all the loading-screen content in the upper-left
    // quadrant. THIS is the "white frame edges in upper-left" symptom.
    //

    // (Was: phase1_max_call — removed cleanup. Was a knob for
    // the now-dead patch_splash_phase1 path; never consumed since 2026-
    // 05-06 phase-1 retirement.)
    int   debug_log;
    int   debug_log_verbose;   // 1 = firehose mode: emits the extra per-feature
                               // diagnostic lines (e.g. the char-create backdrop
                               // column-fill trace). Default 0. DebugLogsEnabled
                               // must ALSO be 1 for the verbose lines to land
                               // in pso_widescreen.log (verbose is purely a
                               // gating extension on top of debug_log).
    // (Was: publish_logical_canvas — removed cleanup.
    // Deprecated since d3d8to11 wrapper update; the apply
    // path is a logging no-op. Stack-detection logging that this knob
    // gated is preserved separately via detect_d3d8_stack().)

    // ---- Boot poster (mod_boot_poster.c) ----
    // Centered PNG overlay during the very first "NOW LOADING" black
    // screen. Auto-disables once SEGA splash atlas appears, the engine
    // loads a real floor, or MaxSeconds elapses. Lives in pso_widescreen
    // because it's pure visual / D3D8 plumbing — same architectural rule
    // as the splash-phase patches above.
    int   boot_poster_enabled;
    int   boot_poster_disable_after_floor;
    int   boot_poster_disable_after_splash;
    int   boot_poster_max_seconds;
    float boot_poster_max_screen_pct;
    char  boot_poster_path[MAX_PATH];

    // ---- P3 video player (mod_video.c) — Stage 1 ----
    // Skippable FFmpeg-decoded video, blitted via the same Present quad as
    // the boot poster. Default OFF (VideoEnable=0) => mod_video stays fully
    // dormant and the build is byte-identical in behavior to today.
    int   video_enabled;           // VideoEnable        (0 = OFF, default)
    int   video_skippable;         // VideoSkippable     (1 = Enter/Esc skip)
    int   video_max_seconds;       // VideoMaxSeconds    (per-playback watchdog)
    int   video_skip_debounce_ms;  // VideoSkipDebounceMs(release-then-press arm)
    int   video_audio;             // VideoAudio         (sibling .wav via mci)
    int   video_diag;              // VideoDiag          (1 = red half-quad diag)
    int   video_trigger;           // VideoTrigger       (VID_TRIGGER_OFF/BOOT/CHARCREATE)
    int   video_decoder;           // VideoDecoder       (VID_DECODER_MF/FFMPEG)
    char  video_path[MAX_PATH];    // VideoPath          (source .mp4/.mov)
    char  video_ffmpeg[MAX_PATH];  // VideoFfmpeg        ("" => "ffmpeg" on PATH)

    // REFACTOR: widescreen_engine_anzz1 (the anzz1-vs-sodaboy
    // mutual-exclusion selector) is GONE. anzz1 is the sole engine; there is
    // no longer any if/else on a selector. The WidescreenEngine INI key is
    // accepted-and-warned for one release (see load_config).

    char  log_path[MAX_PATH];
} g_cfg;

// ---- ws_scale_ctx — the ONE universal scaling gate refactor) ----
//
// A single computed struct, derived once at the end of load_config() via
// ws_compute_scale(). The master gate everything consults is `g_scale.enabled`
// (no widescreen_engine selector branch anywhere). front-end-native is the
// VALUE hud_scale==1.0, not a branch. The A/B/C/D anzz1 extents AND the
// render_w/render_h derivation are computed HERE, in exactly one place, then
// passed to the anzz1 bake (MINOR-3).
typedef struct {
    int   enabled;             // master: g_cfg.enabled. The ONE gate.
    float hud_scale;           // UI magnitude. 1.0 == front-end native.
    float hud_compress;        // RHW divide ("MORE ROOM"); runtime path.
    float game_aspect;         // AR feeding the anzz1 extents.
    int   render_w, render_h;  // physical render res for the 0x009006F4 table.
    float A, B, C;             // anzz1 horizontal/atlas/vertical extents.
    DWORD D;                   // anzz1 integer tile-height unit.
} ws_scale_ctx;

static ws_scale_ctx g_scale;   // populated at end of load_config().
static void ws_compute_scale(ws_scale_ctx *s);  // defined after load_config().

// ---- Boot poster bridge (defined in mod_boot_poster.c) ----
extern void boot_poster_init_from_cfg(const char *path, int enabled,
                                      int disable_after_floor,
                                      int disable_after_splash_atlas,
                                      int max_seconds, float max_screen_pct);
extern void boot_poster_on_present(void *device, int viewport_w, int viewport_h);
extern void boot_poster_log_summary(void);

// ---- P3 video bridge (defined in mod_video.c) ----
// VideoTrigger modes — MUST mirror the VID_TRIGGER_* macros in mod_video.h
// (kept inline here to match this file's extern-bridge style; mod_video.c is
// the consumer of the value and owns the authoritative definitions).
#define VID_TRIGGER_OFF         0
#define VID_TRIGGER_BOOT        1
#define VID_TRIGGER_CHARCREATE  2
// VideoDecoder modes — MUST mirror the VID_DECODER_* macros in mod_video.h.
#define VID_DECODER_MF          0
#define VID_DECODER_FFMPEG      1
extern void mod_video_init(const char *path, int enabled, int skippable,
                           const char *ffmpeg_path, int max_seconds,
                           int skip_debounce_ms, int audio, int diag,
                           int trigger, int decoder);
extern void mod_video_on_present(void *device, int viewport_w, int viewport_h);
extern void mod_video_log_summary(void);

// ---- Logging ----
//
// File log next to the EXE, gated on DebugLogsEnabled=1. We write via
// CreateFile/WriteFile to keep CRT use minimal — DllMain is a fragile
// place to call into the C runtime.

static void resolve_log_path(void)
{
    char exe[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, exe, MAX_PATH) == 0) return;
    char *slash = strrchr(exe, '\\');
    if (!slash) return;
    *(slash + 1) = 0;
    _snprintf_s(g_cfg.log_path, MAX_PATH, _TRUNCATE,
                "%spso_widescreen.log", exe);
}

// Persistent log file handle. Opened lazily on first emit, kept until
// DLL_PROCESS_DETACH. Avoids the syscall storm of CreateFile+CloseHandle
// per call when verbose mode emits thousands of lines per frame.
static HANDLE g_log_handle = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_cs;
static int  g_log_cs_init = 0;

// External-linkage so mod_boot_poster.c (separate translation unit) can
// route its log lines through the same handle / file.
void log_line(const char *fmt, ...)
{
    if (!g_cfg.debug_log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(buf);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    if (!g_cfg.log_path[0]) return;
    if (!g_log_cs_init) {
        // First-ever call: init lock + open file. Race here is benign
        // because DllMain is serialized by the loader; the very first
        // log_line() call comes from DllMain.
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_init = 1;
    }
    EnterCriticalSection(&g_log_cs);
    if (g_log_handle == INVALID_HANDLE_VALUE) {
        g_log_handle = CreateFileA(g_cfg.log_path, FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_log_handle != INVALID_HANDLE_VALUE) {
            SetFilePointer(g_log_handle, 0, NULL, FILE_END);
        }
    }
    if (g_log_handle != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(g_log_handle, buf, (DWORD)n, &wrote, NULL);
        WriteFile(g_log_handle, "\r\n", 2, &wrote, NULL);
        // DIAG BUILD flush per line so a scene transition or a
        // char-create crash can never lose buffered lines. The kernel-mode
        // FlushFileBuffers commits the WriteFile to disk before we return.
        FlushFileBuffers(g_log_handle);
    }
    LeaveCriticalSection(&g_log_cs);
}

static void load_config(void)
{
    g_cfg.enabled            = 0;
    g_cfg.windowed           = 1;
    g_cfg.width              = 0;
    g_cfg.height             = 0;
    g_cfg.logical_width      = 1920;   // Sodaboy-convention engine canvas
    g_cfg.logical_height     = 1080;
    g_cfg.override_backbuffer = 0; // stretch mode by default
    g_cfg.override_viewport   = 0;
    g_cfg.integer_scale       = 0; // "bump base render" to a clean divisor; opt-in
    g_cfg.fov_correction      = 0;
    g_cfg.game_aspect         = 16.0f / 9.0f;  // BAKED (was INI GameAspect=16:9)
    // REFACTOR: Sodaboy LOOP/Region bake RETIRED — anzz1 is the
    // sole static coordinate source of truth. patch_validator_nop /
    // patch_res_table / patch_hud_rects / patch_extra_ui / patch_flare_pin /
    // patch_charscreen_scale / patch_asset_registry / load_asset_overrides_ini
    // defaults removed (fields gone).
    g_cfg.patch_splash_phase2 = 0;  // BAKED OFF (was INI PatchSplashPhase2=0). on
                                    // the default path (folded into
                                    // apply_static_patches). 0x009A3420 quad
                                    // table; de-conflicted vs anzz1 (skips the
                                    // 2 X-coords anzz1 owns). Per-feature knob.
    g_cfg.hud_scale           = 1.0f;  // mem patches: canonical widescreen
    g_cfg.hud_compress        = 1.0f;  // RHW divide: no compression by default
    g_cfg.cursor_warp_fix     = 1;     // 2026-05-25: keep OS cursor on the
                                       // drawn menu element under HUD scale.
    g_cfg.patch_hangame_title_menu = 1;// 2026-05-25: center+lower the hangame
                                       // title menu via the 0x974E08 struct.
    g_cfg.hangame_menu_x      = 320.0f;       // native (hs=1.0) title menu X. Rebaked 480*(1/1.5).
    g_cfg.hangame_menu_y      = 326.666687f; // native (hs=1.0) title menu Y. Rebaked 490*(1/1.5) -> bits 0x43A35556. (NOT 326.6666565f: that rounds to 0x43A35555, 1 ULP low.)
    g_cfg.patch_rune_scale    = 1;     // 2026-05-26: scale rune emblem with master UI scale.
    g_cfg.patch_charselect    = 1;     // Char-select 16:9 layout (info panel,
                                       // char list, frames, legend buttons +
                                       // footer Enter/Cancel stride pokes).
                                       // Model pan runs unconditionally via
                                       // patch_charselect_model_pan() (orthogonal world-space).
    // char-create per-frame knob defaults REMOVED with the deleted
    // cc_quad_fix_c composer hook (no-per-frame-patching mandate). Char-create is
    // owned by the static apply_startup_bakes() pass; no per-frame cfg drives it.
    g_cfg.patch_streak_move   = 0;     // 2026-06-17: DISABLED — Ephinea writes NOTHING to the
                                       // streak imms 0x006F8366/72 (CONFIRMED absent from
                                       // _ephinea_delta.csv); our 5px origin nudge was a pure
                                       // divergence AND can't fix the real deanchor (streak
                                       // GEOMETRY rides the affine). Leave the streak byte-stock.
    g_cfg.streak_dx           = 5.0f;  // native (hs=1.0) streak X. Rebaked 7.5*(1/1.5).
    g_cfg.streak_dy           = 0.0f;
    g_cfg.patch_streak_scale  = 1;     // 2026-06-18: fix streak position at any HudScale.
    // minimap bake (dialed live, replaces the standalone
    // minimap_zoom-asi). bg+vp move together; zoom 0.53 (~47% of stock
    // 1.1338) widens the FOV substantially without making icons too small.
    g_cfg.patch_minimap       = 1;
    g_cfg.minimap_bg_x        = 708.33f;
    g_cfg.minimap_bg_y        =  19.0f;
    g_cfg.minimap_vp_x        = 772.33f;
    g_cfg.minimap_vp_y        =  83.0f;
    g_cfg.minimap_zoom        =  0.53f;
    g_cfg.patch_flare_scale   = 0;     // 2026-05-26: opt-in (default OFF) until
                                       // in-game effect-type-0 safety is checked.
    g_cfg.flare_scale         = 6.0f;  // 1.5× of the stock 4.0 descriptor scale.
    // REFACTOR: patch_phase3 / patch_title_art / patch_title_real /
    // title_real_mode / widescreen_engine_anzz1 defaults removed (fields gone).
    g_cfg.debug_log           = 0;
    g_cfg.debug_log_verbose   = 0;
    // Boot poster defaults — feature ON by default, sensible auto-disable.
    g_cfg.boot_poster_enabled              = 1;
    g_cfg.boot_poster_disable_after_floor  = 1;
    g_cfg.boot_poster_disable_after_splash = 1;
    g_cfg.boot_poster_max_seconds          = 30;
    g_cfg.boot_poster_max_screen_pct       = 80.0f;
    _snprintf_s(g_cfg.boot_poster_path, MAX_PATH, _TRUNCATE,
                "patches\\psobb_boot_poster.png");
    // P3 video defaults — BAKED ON (the char-create intro is a shipped feature,
    // not a user tweak). Plays in-process via Media Foundation, skippable with
    // Enter, no watchdog cap (plays to EOF or skip). Asset ships in the install
    // at patches\video\ (relative; vid_resolve_path resolves vs the game dir).
    g_cfg.video_enabled          = 1;
    g_cfg.video_skippable        = 1;
    g_cfg.video_max_seconds      = 0;   // 0 = no watchdog: play to EOF or skip
    g_cfg.video_skip_debounce_ms = 500;
    g_cfg.video_audio            = 1;   // 2026-06-11: BAKED ON. Our MF->XAudio2 AAC
                                        // track plays with the intro cover. Engine
                                        // intro audio is suppressed in parallel by
                                        // the play-by-id gate (0x00828E4C) + STOP-ALL
                                        // (0x00829AD0) in mod_video.c.
                                        // Compile-time default; no INI key.
    g_cfg.video_diag             = 0;   // VideoDiag OFF by default
    g_cfg.video_trigger          = VID_TRIGGER_CHARCREATE;  // intro at char-create
    g_cfg.video_decoder          = VID_DECODER_MF;   // in-process MF by default
    _snprintf_s(g_cfg.video_path, MAX_PATH, _TRUNCATE,
                "patches\\video\\pso_intros_16x9.mp4");
    _snprintf_s(g_cfg.video_ffmpeg, MAX_PATH, _TRUNCATE,
                "patches\\video\\ffmpeg.exe");
    char exe[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, exe, MAX_PATH) == 0) return;
    char *slash = strrchr(exe, '\\');
    if (!slash) return;
    *(slash + 1) = 0;
    // OUR ASI reads ONLY from `patches\pso_widescreen.ini` (next to
    // the .asi). The widescreen.cfg fallback was removed 
    // sharing a config file with Sodaboy's d3d8.dll caused real
    // confusion (an `Enabled=0` line in widescreen.cfg looks like a
    // global widescreen kill-switch but only ever gated OUR ASI;
    // Sodaboy's widescreen is gated solely by the presence of
    // Width / Height keys). Two ASIs, two config files; no overlap.
    //
    // If `pso_widescreen.ini` is missing we keep the in-source
    // defaults set above (which are intentionally `Enabled=0` so a
    // bare install doesn't fight Sodaboy).
    char path[MAX_PATH];
    _snprintf_s(path, MAX_PATH, _TRUNCATE, "%spatches\\pso_widescreen.ini", exe);
    FILE *f = NULL; fopen_s(&f, path, "rb");
    if (!f) return;
    char line[256];
    // AUTO-ENABLE: track whether the INI carried an explicit
    // Enabled= line. If it did, that's a dev escape hatch and FORCE-overrides
    // the resolution-aspect auto-detect below (for A/B testing). If it didn't,
    // the resolution's aspect ratio is the SINGLE source of truth.
    int enabled_explicit = 0;
    // WINDOW MODE: track whether the INI carried an explicit
    // Windowed= line. If it did, the INI wins; otherwise the launcher's
    // widescreen.cfg Windowed= (read below) is the source of truth. Without
    // this, the launcher's "virtual widescreen" (Windowed=2) was ignored and
    // g_cfg.windowed stayed at its default of 1 -> the mod ran windowed.
    int windowed_explicit = 0;
    // ASPECT VIDEO: track whether the INI carried an explicit
    // VideoPath= line. If it did, the user's path wins and the aspect-based
    // intro-video auto-select below is skipped. If it didn't, the resolved
    // resolution's aspect picks 16:9 vs 4:3 (same threshold as enable).
    int video_path_explicit = 0;
    while (fgets(line, sizeof(line), f)) {
        // Trim trailing \r\n + leading whitespace.
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == 0 || *p == '\r' || *p == '\n')
            continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p, *val = eq + 1;
        // Right-trim key + value.
        for (char *t = key + strlen(key); t > key && (t[-1]==' '||t[-1]=='\t'); --t) t[-1]=0;
        while (*val == ' ' || *val == '\t') ++val;
        for (char *t = val + strlen(val); t > val && (t[-1]==' '||t[-1]=='\t'||t[-1]=='\r'||t[-1]=='\n'); --t) t[-1]=0;
        // Enabled is OUR knob — Sodaboy's widescreen.cfg doesn't have it,
        // so a stock cfg leaves us off (no fight with his wrapper). Set
        // Enabled=1 explicitly when you've swapped to a different d3d8.dll
        // (e.g. d3d8to11 / native) and want this ASI to take over.
        if      (_stricmp(key, "Enabled")           == 0) { g_cfg.enabled = atoi(val); enabled_explicit = 1; }
        else if (_stricmp(key, "Windowed")          == 0) { g_cfg.windowed = atoi(val); windowed_explicit = 1; }
        else if (_stricmp(key, "Width")             == 0) g_cfg.width          = atoi(val);
        else if (_stricmp(key, "Height")            == 0) g_cfg.height         = atoi(val);
        else if (_stricmp(key, "LogicalWidth")      == 0) {
            int v = atoi(val);
            if (v >= 320 && v <= 8192) g_cfg.logical_width = v;
        }
        else if (_stricmp(key, "LogicalHeight")     == 0) {
            int v = atoi(val);
            if (v >= 240 && v <= 8192) g_cfg.logical_height = v;
        }
        else if (_stricmp(key, "FovCorrection")     == 0) g_cfg.fov_correction      = atoi(val);
        else if (_stricmp(key, "OverrideBackBuffer")== 0) g_cfg.override_backbuffer = atoi(val);
        else if (_stricmp(key, "OverrideViewport")  == 0) g_cfg.override_viewport   = atoi(val);
        else if (_stricmp(key, "IntegerScale")      == 0) g_cfg.integer_scale       = atoi(val);
        // REFACTOR: PatchValidatorNop / PatchResTable / PatchHudRects /
        // PatchExtraUiAnchors / PatchFlarePin / PatchCharScreenScale /
        // PatchAssetRegistry / LoadAssetOverridesIni / PatchPhase3 / PatchTitleArt /
        // PatchTitleReal / TitleRealMode keys RETIRED with the Sodaboy path.
        // Old INIs carrying them fall through harmlessly (unknown-key path).
        else if (_stricmp(key, "PatchSplashPhase2") == 0) g_cfg.patch_splash_phase2 = atoi(val);
        else if (_stricmp(key, "HUDScale")          == 0) {
            float s = (float)atof(val);
            if (s > 0.1f && s < 10.0f) g_cfg.hud_scale = s;
        }
        else if (_stricmp(key, "HUDCompress")       == 0) {
            float s = (float)atof(val);
            if (s > 0.1f && s < 10.0f) g_cfg.hud_compress = s;
        }
        else if (_stricmp(key, "CursorWarpFix")     == 0) g_cfg.cursor_warp_fix = atoi(val);
        else if (_stricmp(key, "PatchHangameTitleMenu") == 0) g_cfg.patch_hangame_title_menu = atoi(val);
        // SINGLE-FRAME ingest-normalize: HangameMenuX/Y INIs are written
        // in the legacy 1280x720 (hs=1.5) design space (the doc comments + any shipped
        // INIs describe 1.5-space numbers). The g_cfg field is now native (hs=1.0), so
        // pre-divide on ingest by the input-normalization constant (1.0f/1.5f). This is
        // NOT a calibration frame — it's a one-time unit conversion of the INI value, so
        // an unchanged INI lands bit-for-bit where it did before the refactor.
        else if (_stricmp(key, "HangameMenuX")      == 0) {
            float x = (float)atof(val);
            if (x >= -2048.0f && x <= 4096.0f) g_cfg.hangame_menu_x = x * (1.0f/1.5f);
        }
        else if (_stricmp(key, "HangameMenuY")      == 0) {
            float y = (float)atof(val);
            if (y >= -2048.0f && y <= 4096.0f) g_cfg.hangame_menu_y = y * (1.0f/1.5f);
        }
        else if (_stricmp(key, "PatchRuneScale")    == 0) g_cfg.patch_rune_scale = atoi(val);
        else if (_stricmp(key, "PatchCharSelect")   == 0) g_cfg.patch_charselect = atoi(val);
        // the char-create per-frame knobs (CcStretch / CcContent /
        // CcContentDx / CcStrideLeft / CcStrideRight / CcSplit / CcDebug) were
        // REMOVED with the deleted cc_quad_fix_c composer hook. They were never
        // INI-parsed; old INIs naming those keys still fall through harmlessly
        // (unknown-key path). Char-create is owned by apply_startup_bakes() now.
        else if (_stricmp(key, "PatchStreakMove")   == 0) g_cfg.patch_streak_move = atoi(val);
        else if (_stricmp(key, "PatchStreakScale")  == 0) g_cfg.patch_streak_scale = atoi(val);
        // SINGLE-FRAME ingest-normalize: StreakDX/DY INIs are in legacy
        // 1.5-space; pre-divide by the input-normalization constant (1.0f/1.5f) into the
        // native field so an unchanged INI is bit-for-bit preserved (StreakDY=0 stays 0).
        else if (_stricmp(key, "StreakDX")          == 0) g_cfg.streak_dx = (float)atof(val) * (1.0f/1.5f);
        else if (_stricmp(key, "StreakDY")          == 0) g_cfg.streak_dy = (float)atof(val) * (1.0f/1.5f);
        else if (_stricmp(key, "PatchMinimap")      == 0) g_cfg.patch_minimap = atoi(val);
        else if (_stricmp(key, "MinimapBgX")        == 0) g_cfg.minimap_bg_x = (float)atof(val);
        else if (_stricmp(key, "MinimapBgY")        == 0) g_cfg.minimap_bg_y = (float)atof(val);
        else if (_stricmp(key, "MinimapVpX")        == 0) g_cfg.minimap_vp_x = (float)atof(val);
        else if (_stricmp(key, "MinimapVpY")        == 0) g_cfg.minimap_vp_y = (float)atof(val);
        else if (_stricmp(key, "MinimapZoom")       == 0) {
            float z = (float)atof(val);
            if (z >= 0.1f && z <= 3.0f) g_cfg.minimap_zoom = z;
        }
        else if (_stricmp(key, "PatchFlareScale")   == 0) g_cfg.patch_flare_scale = atoi(val);
        else if (_stricmp(key, "FlareScale")        == 0) {
            float s = (float)atof(val);
            if (s > 0.5f && s < 32.0f) g_cfg.flare_scale = s;
        }
        else if (_stricmp(key, "GameAspect")        == 0) {
            // Allow override for unusual builds. Either bare float
            // ("1.333") or "W:H" form ("4:3", "16:10"). Default 4:3.
            float a = (float)atof(val);
            const char *colon = strchr(val, ':');
            if (colon) {
                float w = (float)atof(val);
                float h = (float)atof(colon + 1);
                if (w > 0 && h > 0) a = w / h;
            }
            if (a > 0.1f && a < 10.0f) g_cfg.game_aspect = a;
        }
        else if (_stricmp(key, "WidescreenEngine")  == 0) {
            // REFACTOR: the engine selector is GONE — anzz1 is the
            // only engine. Accept-and-warn for one release so existing INIs
            // don't appear broken; the value is otherwise ignored.
            log_line("[pso_widescreen] WidescreenEngine='%s' is OBSOLETE; anzz1 is now the only engine (key ignored)", val);
        }
        else if (_stricmp(key, "DebugLogsEnabled")  == 0) g_cfg.debug_log = atoi(val);
        else if (_stricmp(key, "DebugLogVerbose")   == 0) g_cfg.debug_log_verbose = atoi(val);
        // PublishLogicalCanvas removed cleanup — was a hint
        // for an older d3d8to11 wrapper rev that's no longer relevant
        // (current wrapper pins to physical viewport unconditionally).
        // Boot poster knobs — see [boot_poster] block in widescreen.cfg.sample.
        else if (_stricmp(key, "BootPosterEnabled")           == 0) g_cfg.boot_poster_enabled              = atoi(val);
        else if (_stricmp(key, "BootPosterPath")              == 0) {
            _snprintf_s(g_cfg.boot_poster_path, MAX_PATH, _TRUNCATE, "%s", val);
        }
        else if (_stricmp(key, "BootPosterMaxScreenPct")      == 0) {
            float p = (float)atof(val);
            if (p > 1.0f && p <= 100.0f) g_cfg.boot_poster_max_screen_pct = p;
        }
        else if (_stricmp(key, "BootPosterDisableAfterFloor") == 0) g_cfg.boot_poster_disable_after_floor  = atoi(val);
        else if (_stricmp(key, "BootPosterDisableAfterSplashAtlas") == 0) g_cfg.boot_poster_disable_after_splash = atoi(val);
        else if (_stricmp(key, "BootPosterMaxSeconds")        == 0) {
            int s = atoi(val);
            if (s >= 0 && s <= 600) g_cfg.boot_poster_max_seconds = s;
        }
        // P3 video knobs (Stage 1). VideoEnable=0 (default) keeps mod_video
        // dormant and the binary byte-identical-in-behavior to today.
        else if (_stricmp(key, "VideoEnable")  == 0) g_cfg.video_enabled   = atoi(val);
        else if (_stricmp(key, "VideoPath")    == 0) {
            _snprintf_s(g_cfg.video_path, MAX_PATH, _TRUNCATE, "%s", val);
            video_path_explicit = 1;
        }
        else if (_stricmp(key, "VideoSkippable") == 0) g_cfg.video_skippable = atoi(val);
        else if (_stricmp(key, "VideoFfmpeg")  == 0) {
            _snprintf_s(g_cfg.video_ffmpeg, MAX_PATH, _TRUNCATE, "%s", val);
        }
        // VideoAudio INI key REMOVED. video_audio is now a compile-time
        // default (BAKED ON in load_config) — our XAudio2 AAC track always plays with
        // the intro cover and the engine intro audio is muted in parallel. No knob.
        else if (_stricmp(key, "VideoMaxSeconds") == 0) {
            int s = atoi(val);
            if (s >= 0 && s <= 600) g_cfg.video_max_seconds = s;
        }
        else if (_stricmp(key, "VideoSkipDebounceMs") == 0) {
            int d = atoi(val);
            if (d >= 0 && d <= 10000) g_cfg.video_skip_debounce_ms = d;
        }
        else if (_stricmp(key, "VideoDiag")   == 0) g_cfg.video_diag = atoi(val);
        else if (_stricmp(key, "VideoTrigger") == 0) {
            // String-valued: which event auto-starts playback.
            // off        — never (default)
            // boot       — once at boot/title (covers the boot splash)
            // charcreate — when the char-create scene becomes active (covers
            // the scripted starfield intro), per Stage 2.
            if      (_stricmp(val, "charcreate") == 0) g_cfg.video_trigger = VID_TRIGGER_CHARCREATE;
            else if (_stricmp(val, "boot")       == 0) g_cfg.video_trigger = VID_TRIGGER_BOOT;
            else                                       g_cfg.video_trigger = VID_TRIGGER_OFF;
        }
        else if (_stricmp(key, "VideoDecoder") == 0) {
            // String-valued: which decode backend produces frames.
            // mf                 — in-process Media Foundation (default)
            // ffmpeg | external  — external ffmpeg.exe -> rawvideo BGRA pipe
            g_cfg.video_decoder =
                (_stricmp(val, "ffmpeg") == 0 || _stricmp(val, "external") == 0)
                    ? VID_DECODER_FFMPEG : VID_DECODER_MF;
        }
        // Sodaboy keys we deliberately ignore: MSAA / SMAA / SSAO /
        // CelShader / DOF / HDR / HUDScale. ReShade owns post-FX now
        // and HUDScale needs an engine-side hook (TODO follow-up).
    }
    fclose(f);
    // PSOHARNESS_HUDSCALE env override REMOVED per directive. The INI
    // (Enabled + HUDScale) is the SINGLE source of truth for scale. No environment
    // variable may silently override the INI — the old env path was leaving live
    // clients force-scaled (e.g. 1.5) while the INI read 1.0, which is exactly the
    // confusion this deletes. Harness A/B scale testing must edit the INI, not env.
    //
    // RESOLUTION SOURCE = launcher's widescreen.cfg. The launcher
    // writes the player's chosen resolution to widescreen.cfg in the exe ROOT
    // dir (NOT patches\). Read it as the PRIMARY resolution source when the INI
    // didn't pin Width/Height. Precedence: INI Width/Height > widescreen.cfg >
    // monitor (GetSystemMetrics, below). This feeds the aspect auto-detect.
    // also read the launcher's WINDOW MODE here. The launcher writes
    // the player's chosen mode (0=fullscreen, 1=windowed, 2=virtual/borderless
    // fullscreen — Sodaboy convention, identical to ours; see g_cfg.windowed doc)
    // into widescreen.cfg next to Width/Height. We previously read ONLY
    // Width/Height, so a launcher set to "virtual widescreen" (Windowed=2) was
    // silently dropped and the mod ran in a plain titled window. Read the cfg
    // UNCONDITIONALLY (not only when Width/Height are unset) so the Windowed value
    // is always picked up; an explicit Windowed= / Width= / Height= in
    // pso_widescreen.ini still wins (windowed_explicit / nonzero g_cfg.width/height).
    {
        char cfgpath[MAX_PATH];
        _snprintf_s(cfgpath, MAX_PATH, _TRUNCATE, "%swidescreen.cfg", exe);
        FILE *cf = NULL; fopen_s(&cf, cfgpath, "rb");
        if (cf) {
            int cw = 0, ch = 0, cwin = -1;
            char cline[256];
            while (fgets(cline, sizeof(cline), cf)) {
                char *q = cline;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '#' || *q == ';' || *q == 0 || *q == '\r' || *q == '\n')
                    continue;
                char *ceq = strchr(q, '=');
                if (!ceq) continue;
                *ceq = 0;
                char *ckey = q, *cval = ceq + 1;
                for (char *t = ckey + strlen(ckey); t > ckey && (t[-1]==' '||t[-1]=='\t'); --t) t[-1]=0;
                while (*cval == ' ' || *cval == '\t') ++cval;
                for (char *t = cval + strlen(cval); t > cval && (t[-1]==' '||t[-1]=='\t'||t[-1]=='\r'||t[-1]=='\n'); --t) t[-1]=0;
                if      (_stricmp(ckey, "Width")    == 0) cw   = atoi(cval);
                else if (_stricmp(ckey, "Height")   == 0) ch   = atoi(cval);
                else if (_stricmp(ckey, "Windowed") == 0) cwin = atoi(cval);
            }
            fclose(cf);
            // Resolution: launcher cfg fills it only if the INI didn't pin it.
            if ((g_cfg.width <= 0 || g_cfg.height <= 0) && cw >= 320 && ch >= 240) {
                g_cfg.width = cw; g_cfg.height = ch;
            }
            // Window mode: launcher cfg is the source of truth unless the INI
            // set Windowed explicitly. Range-checked to our 0/1/2 semantics.
            if (!windowed_explicit && cwin >= 0 && cwin <= 2) {
                g_cfg.windowed = cwin;
                log_line("[pso_widescreen] window mode from widescreen.cfg: Windowed=%d "
                         "(0=fullscreen,1=windowed,2=virtual/borderless)", cwin);
            }
        }
    }
    // Resolution AUTO-DETECT: the shipped INI carries NO Width/Height (only
    // Enabled + HUDScale), so default them to the primary monitor's resolution.
    // Without this, width/height stay 0 and the render_w fallback (=(int)A=853,
    // the design width) breaks the widescreen render path / hangs the device.
    // An explicit Width/Height in the INI (dev override) still wins; the
    // widescreen.cfg read above already filled them if the launcher set them.
    if (g_cfg.width <= 0 || g_cfg.height <= 0) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        if (sw >= 320 && sh >= 240) { g_cfg.width = sw; g_cfg.height = sh; }
    }
    // Auto-derive game aspect from Width/Height. Without this, the default
    // 4:3 game_aspect means the anzz1 widescreen path computes 640-wide
    // engine coords for a 1920-wide backbuffer -> 3.0x horizontal stretch
    // instead of the correct 2.25x. INI GameAspect (if set above) wins.
    if (g_cfg.width > 0 && g_cfg.height > 0) {
        float derived = (float)g_cfg.width / (float)g_cfg.height;
        // Only auto-set when GameAspect wasn't explicitly provided. The
        // default 4/3 is the sentinel; anything else means user set it.
        const float kStockAspect = 4.0f / 3.0f;
        if (g_cfg.game_aspect > kStockAspect - 0.001f
            && g_cfg.game_aspect < kStockAspect + 0.001f
            && derived > 0.1f && derived < 10.0f) {
            g_cfg.game_aspect = derived;
        }
    }
    // ASPECT AUTO-ENABLE (owner directive): the resolution's aspect
    // ratio decides whether widescreen applies. 4:3 (or narrower) -> stock
    // client (OFF); anything wider than 4:3 (16:10, 16:9, 21:9, 32:9, ...) ->
    // widescreen (ON). The +0.01 epsilon keeps true 4:3 modes OFF while letting
    // 16:10 (1.6) and up turn ON. An explicit Enabled= in the INI force-
    // overrides this (dev A/B escape hatch).
    if (g_cfg.width > 0 && g_cfg.height > 0) {
        float ar = (float)g_cfg.width / (float)g_cfg.height;
        int auto_on = (ar > 4.0f / 3.0f + 0.01f) ? 1 : 0;
        if (!enabled_explicit) g_cfg.enabled = auto_on;
        if (enabled_explicit) {
            log_line("[pso_widescreen] aspect auto-detect: %dx%d AR=%.4f -> widescreen %s (ini Enabled=%d overrides -> %s)",
                     g_cfg.width, g_cfg.height, ar, auto_on ? "ON" : "OFF",
                     g_cfg.enabled, g_cfg.enabled ? "ON" : "OFF");
        } else {
            log_line("[pso_widescreen] aspect auto-detect: %dx%d AR=%.4f -> widescreen %s",
                     g_cfg.width, g_cfg.height, ar, auto_on ? "ON" : "OFF");
        }
    }
    // INTEGER-SCALE ("bump the base render"): on a high-DPI borderless
    // screen, render the backbuffer at a CLEAN integer divisor of the physical
    // Width/Height (capped ~1080p) so the d3d8 present upscales by a clean integer
    // factor. WHY: a 1280x800 (16:10) backbuffer stretched to a 4K (16:9) window is a
    // 3.0x/2.7x non-integer + aspect-mismatched blit — the Wine/wined3d perf cliff
    // (~20 FPS) the tester hit without dgVoodoo. Rendering 1920x1080 (= 4K/2) and
    // presenting a clean 2x is far faster (and sharper). N = ceil(height/1440) keeps
    // the render in (720..1440]; 4K->2 (1920x1080), 5K->2 (1440), 8K->3 (1440),
    // <=1440p->1 (native, no-op). Sets logical=physical/N and forces the engine-native
    // render path; ws_compute_scale's render_w follows logical so the engine res table
    // matches the real BB (no "2x too big at 4K" coord bug).
    if (g_cfg.integer_scale && g_cfg.windowed == 2 &&
        g_cfg.width >= 1280 && g_cfg.height >= 720) {
        int N = (g_cfg.height + 1439) / 1440;          // ceil(height / 1440)
        if (N < 1) N = 1;
        if (N > 1) {                                   // <=1440p: leave native (N==1 no-op)
            g_cfg.logical_width       = g_cfg.width  / N;
            g_cfg.logical_height      = g_cfg.height / N;
            g_cfg.override_backbuffer = 1;
            g_cfg.override_viewport   = 1;
            log_line("[pso_widescreen] integer-scale: physical %dx%d -> render %dx%d "
                     "(clean %dx present upscale)",
                     g_cfg.width, g_cfg.height,
                     g_cfg.logical_width, g_cfg.logical_height, N);
        } else {
            log_line("[pso_widescreen] integer-scale: physical %dx%d <=1440p -> native (no-op)",
                     g_cfg.width, g_cfg.height);
        }
    }
    // ASPECT VIDEO SELECT: pick the aspect-appropriate intro video
    // from the SAME resolved resolution the enable auto-detect used, so the two
    // decisions always agree (16:9 -> widescreen ON + 16:9 video; 4:3 ->
    // widescreen OFF + 4:3 video). An explicit VideoPath= in the INI wins.
    if (!video_path_explicit && g_cfg.width > 0 && g_cfg.height > 0) {
        float ar = (float)g_cfg.width / (float)g_cfg.height;
        if (ar > 4.0f / 3.0f + 0.01f)
            _snprintf_s(g_cfg.video_path, MAX_PATH, _TRUNCATE, "patches\\video\\pso_intros_16x9.mp4");
        else
            _snprintf_s(g_cfg.video_path, MAX_PATH, _TRUNCATE, "patches\\video\\pso_intros_4x3.mp4");
        log_line("[pso_widescreen] intro video: %s (AR=%.4f)", g_cfg.video_path, (double)ar);
    }
    // Record which cfg file we ended up reading so the log makes the
    // path-resolution explicit (helps users diagnose "why aren't my
    // knobs taking effect" when both files exist).
    log_line("[pso_widescreen] cfg loaded from: %s", path);

    // ---- THE ONE SCALING GATE: compute g_scale ONCE, here. ----
    ws_compute_scale(&g_scale);
}

// ws_compute_scale — derive the single ws_scale_ctx from g_cfg, ONCE, at the
// end of load_config. The anzz1 A/B/C/D extents (was anzz1_widescreen.c:243-251)
// AND the render_w/render_h derivation (was anzz1:259-260) are computed HERE so
// the magnitude lives in exactly one place (MINOR-3); the resolved values are
// then handed to apply_anzz1_widescreen. front-end-native is the VALUE
// hud_scale==1.0 — no branch.
static void ws_compute_scale(ws_scale_ctx *s)
{
    s->enabled      = g_cfg.enabled;
    s->hud_scale    = g_cfg.hud_scale;
    s->hud_compress = g_cfg.hud_compress;
    s->game_aspect  = g_cfg.game_aspect;

    const float AR = g_cfg.game_aspect;
    float A = (AR / (4.0f / 3.0f)) * 640.0f;
    float B = (AR / (4.0f / 3.0f)) * 128.0f;
    float C = 480.0f;
    DWORD D = 128;
    A *= g_cfg.hud_scale;
    B *= g_cfg.hud_scale;
    C *= g_cfg.hud_scale;
    D = (DWORD)((float)D * g_cfg.hud_scale);
    s->A = A; s->B = B; s->C = C; s->D = D;

    // 6-entry resolution table = engine RENDER resolution (must equal the D3D
    // backbuffer the CreateDevice hook sets). In override_backbuffer mode the BB is the
    // LOGICAL canvas (e.g. integer-scale's physical/N = 1920x1080 @ 4K); in stretch mode
    // the BB is the engine-native backbuffer (== physical Width/Height). Pick the one
    // that matches the actual BB, else the res table is sized wrong (coords 2x too big at
    // 4K). Fall back to the A/C coordinate extents only if unset (same rule as anzz1).
    const int rw_src = (g_cfg.override_backbuffer && g_cfg.logical_width  > 0)
                       ? g_cfg.logical_width  : g_cfg.width;
    const int rh_src = (g_cfg.override_backbuffer && g_cfg.logical_height > 0)
                       ? g_cfg.logical_height : g_cfg.height;
    s->render_w = (rw_src > 0) ? rw_src : (int)A;
    s->render_h = (rh_src > 0) ? rh_src : (int)C;

    log_line("[pso_widescreen] ws_compute_scale: enabled=%d aspect=%.5f hud_scale=%.4f "
             "-> A=%.2f B=%.2f C=%.2f D=%lu render=%dx%d",
             s->enabled, s->game_aspect, s->hud_scale,
             s->A, s->B, s->C, (unsigned long)s->D, s->render_w, s->render_h);
}

// ---- Hook plumbing ----

static volatile LONG g_draw_calls_total      = 0;
static volatile LONG g_draw_calls_accepted   = 0;
static volatile LONG g_draw_calls_rejected   = 0;
static volatile LONG g_rej_invalid_args      = 0;
static volatile LONG g_rej_no_player         = 0;
static volatile LONG g_rej_bbox_too_big      = 0;
static volatile LONG g_last_log_milestone    = 0;

// IDirect3DDevice8::Present — slot 15. We intercept it to call the
// boot-poster overlay BEFORE the engine flips the backbuffer, so the
// custom poster lands in the same frame as the engine's "NOW LOADING"
// black screen. Once the poster auto-disables (splash atlas appears,
// floor changes, MaxSeconds elapse), this hook becomes a near no-op.
typedef HRESULT (STDMETHODCALLTYPE *Present_t)(
    IDirect3DDevice8 *self, const RECT *pSrc, const RECT *pDst,
    HWND hOverride, void *pDirtyRegion);

static Direct3DCreate8_t real_Direct3DCreate8 = NULL;
static CreateDevice_t    real_CreateDevice    = NULL;
static SetTransform_t    real_SetTransform    = NULL;
static SetViewport_t     real_SetViewport     = NULL;
static SetVertexShader_t real_SetVertexShader = NULL;
static DrawPrimitiveUP_t real_DrawPrimitiveUP = NULL;
static DrawIndexedPrimitiveUP_t real_DrawIndexedPrimitiveUP = NULL;
static Present_t         real_Present         = NULL;
static int               g_last_vp_w          = 0;  // last SetViewport-observed dims
static int               g_last_vp_h          = 0;
static int               g_vtable_patched     = 0;
static int               g_device_patched     = 0;
static int               g_logged_first_vp    = 0;
static DWORD             g_current_fvf        = 0;
static int               g_logged_first_hud   = 0;
static int               g_logged_present_geom = 0;  // black-quadrant diag one-shot

// Borderless-windowed style normalisation: PSOBB's window starts as a
// regular WS_OVERLAPPEDWINDOW. To get borderless we strip the caption /
// thick-frame / system-menu bits and let the OS treat it as a popup.
// Called once on the first CreateDevice when Windowed=2.
static void make_borderless(HWND hwnd, int w, int h)
{
    if (!hwnd) return;
    // WS_CHILD GUARD: when the harness/editor reparents the game as a
    // WS_CHILD into its own pane, the host owns the window geometry — a child must
    // NEVER strip its caption or resize/maximize itself. Bail so the embedded A/B
    // path is unaffected (it also forces Windowed=1, so this is belt-and-braces).
    if (GetWindowLongPtrA(hwnd, GWL_STYLE) & WS_CHILD) {
        log_line("[pso_widescreen] make_borderless: window is WS_CHILD (embedded) — skipping borderless");
        return;
    }
    LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
               WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME);
    style |= WS_POPUP;
    SetWindowLongPtrA(hwnd, GWL_STYLE, style);
    LONG_PTR ex = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
    ex &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    SetWindowLongPtrA(hwnd, GWL_EXSTYLE, ex);
    // MONITOR-AWARE FULL-BLEED: borderless must cover the monitor the
    // game window is ACTUALLY on — the owner's virtual widescreen display — NOT
    // the Windows PRIMARY monitor. The old SM_CXSCREEN/CYSCREEN path centered on
    // primary, so on a multi-mon setup (physical 4:3 primary + virtual widescreen)
    // the window landed on the wrong screen. Query the window's nearest monitor and
    // fill its full rect (true borderless fullscreen, like Trinity's strip-caption
    // + maximize). Fall back to the primary-centered w x h path if the query fails.
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoA(mon, &mi)) {
        int mx = mi.rcMonitor.left;
        int my = mi.rcMonitor.top;
        int mw = mi.rcMonitor.right  - mi.rcMonitor.left;
        int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        SetWindowPos(hwnd, NULL, mx, my, mw, mh,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        log_line("[pso_widescreen] make_borderless: full-bleed on monitor rect "
                 "%d,%d %dx%d (window's own display)", mx, my, mw, mh);
        return;
    }
    // Fallback: center the requested w x h on the primary monitor (legacy path).
    if (w > 0 && h > 0) {
        RECT scr; scr.left = scr.top = 0;
        scr.right  = GetSystemMetrics(SM_CXSCREEN);
        scr.bottom = GetSystemMetrics(SM_CYSCREEN);
        int x = (scr.right  - w) / 2; if (x < 0) x = 0;
        int y = (scr.bottom - h) / 2; if (y < 0) y = 0;
        SetWindowPos(hwnd, NULL, x, y, w, h,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        log_line("[pso_widescreen] make_borderless: monitor query failed — "
                 "centered %dx%d on primary (fallback)", w, h);
    } else {
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_NOMOVE | SWP_NOSIZE);
    }
}

// Hor+ FOV correction: scale m._11 of the projection matrix by
// game_aspect / display_aspect. The engine builds its projection
// assuming a 4:3 frustum; multiplying the X term tightens the
// horizontal frustum so a wider backbuffer shows MORE world
// horizontally instead of horizontally-stretching the same FOV.
//
// Math:
// m._11_eng = cot(fovY/2) / game_aspect      (engine writes this)
// m._11_we_want = cot(fovY/2) / display_aspect
// factor = (game_aspect / display_aspect)    (multiply by this)
//
// At 4:3 → 16:9 (1.778) factor = 0.75; the X frustum gets ~33% wider.
// At 4:3 → 21:9 (2.333) factor ≈ 0.571; same idea, more aggressive.

static HRESULT STDMETHODCALLTYPE Hook_SetTransform(
    IDirect3DDevice8 *self, DWORD State, const D3DMATRIX_X *pMatrix)
{
    if (g_cfg.fov_correction && State == D3DTS_PROJECTION && pMatrix &&
        g_cfg.width > 0 && g_cfg.height > 0) {
        const float disp_aspect =
            (float)g_cfg.width / (float)g_cfg.height;
        if (disp_aspect > 0.0f) {
            const float factor = g_cfg.game_aspect / disp_aspect;
            D3DMATRIX_X corrected = *pMatrix;
            corrected._11 *= factor;
            return real_SetTransform(self, State, &corrected);
        }
    }
    return real_SetTransform(self, State, pMatrix);
}

// Force the engine's draws to fill the full backbuffer. PSOBB explicitly
// SetViewports its launcher-selected resolution after CreateDevice; if
// we let that through verbatim, the engine renders into a (e.g.) 1024x768
// rectangle in the upper-left of our 4K backbuffer. Rewriting the
// viewport to match the configured backbuffer fixes the corner-render
// without touching any engine code. MinZ/MaxZ pass through.
static HRESULT STDMETHODCALLTYPE Hook_SetViewport(
    IDirect3DDevice8 *self, const D3DVIEWPORT8_X *pViewport)
{
    if (g_cfg.override_viewport && pViewport &&
        g_cfg.width > 0 && g_cfg.height > 0) {
        // Viewport sized to LOGICAL backbuffer (matches CreateDevice's BB
        // override . Engine RHW coords land 1:1 on the BB pixels.
        //
        // Bug fix 2026-05-10b: ONLY override if the incoming viewport is
        // clearly the "main scene" — i.e. close to full BB size. The engine
        // also calls SetViewport with small rects for sub-renders (minimap
        // content, HUD micro-viewports, render targets); previously our
        // unconditional override clobbered those, rendering the minimap
        // content at 1920x1080 instead of its tiny corner rect. Heuristic:
        // require incoming Width >= 60% of logical BB to be considered a
        // "main scene" viewport.
        const int lw = g_cfg.logical_width  > 0 ? g_cfg.logical_width  : 1920;
        const int lh = g_cfg.logical_height > 0 ? g_cfg.logical_height : 1080;
        const DWORD threshold_w = (DWORD)(lw * 6 / 10);
        const DWORD threshold_h = (DWORD)(lh * 6 / 10);
        if (pViewport->Width < threshold_w || pViewport->Height < threshold_h) {
            // Sub-render viewport (minimap, HUD piece, RT) — pass through.
            g_last_vp_w = (int)pViewport->Width;
            g_last_vp_h = (int)pViewport->Height;
            return real_SetViewport(self, pViewport);
        }
        D3DVIEWPORT8_X vp = *pViewport;
        const DWORD orig_w = vp.Width;
        const DWORD orig_h = vp.Height;
        vp.X = 0;
        vp.Y = 0;
        vp.Width  = (DWORD)lw;
        vp.Height = (DWORD)lh;
        if (!g_logged_first_vp) {
            g_logged_first_vp = 1;
            log_line("[pso_widescreen] SetViewport override: %lux%lu -> %lux%lu (logical; threshold=%lux%lu)",
                     (unsigned long)orig_w, (unsigned long)orig_h,
                     (unsigned long)vp.Width, (unsigned long)vp.Height,
                     (unsigned long)threshold_w, (unsigned long)threshold_h);
        }
        // Cache for boot poster (Hook_Present uses these dims to size the
        // centered overlay rect).
        g_last_vp_w = (int)vp.Width;
        g_last_vp_h = (int)vp.Height;
        return real_SetViewport(self, &vp);
    }
    if (pViewport) {
        g_last_vp_w = (int)pViewport->Width;
        g_last_vp_h = (int)pViewport->Height;
    }
    return real_SetViewport(self, pViewport);
}

// In-game re-assert, fired once per FE->IG transition (defined below).
static void ingame_reassert_on_transition(void);

// The char-select 1.0-look PIN was REVERTED. Char-select now SCALES
// with HudScale exactly like Ephinea (global affine 0x00ACC0E8 = 2.25/hud_scale,
// design_w 0x0098A4B8 = 853.333*hud_scale) — NO pin, NO boot design_w/h capture.
// patch_charselect_vertical() bakes the Ephinea-exact hs-scaled layout instead.

// ---- ESC -> escape-the-char-create-trap poll (char-create scene 5 only) -----
// ESC (VK_ESCAPE / DIK 1) is NOT in char-create's keymap, so the engine ignores
// it; natively char-create is a one-way trap (reboot required to leave).
//
// (Thread B): NO single scene-machine request() value lands on the
// char-SELECT slot list. Char-select is NOT a scene-machine scene — it is a
// front-end *screen* (screen-id [0x00A165F0]==6) in a SECOND, independent
// navigation layer, rebuilt only by setting screen-id 6 + firing the dirty pump
// WHILE a front-end menu host scene (one ticking 0x0081AA00) is active. Both the
// old go_charselect()@0x004107D8 (request 0xB -> LOBBY) and raw request(2)
// (-> TITLE) only rewrite the scene index (Layer 1); neither touches Layer 2's
// screen-id, so each falls through to login/title (the observed
// "lobby->join-fail->title"). The clean Option-A transition (drive Layer 2 +
// return Layer 1 to the menu host scene) needs ONE live read of the host-scene
// index [0x00AAB384] in a working char-select slot list, which is owed to
// psobb-live-re and NOT yet pinned.
//
// HONEST FALLBACK (Thread B Option B): request(2) -> TITLE (scene 2). This at
// least BREAKS the one-way char-create trap (the user can re-navigate to
// char-select from the front-end) without faking a transition that lands on a
// broken networking state. The engine re-fetches the character list cleanly from
// title. Upgrade to Option A (in-place char-select screen rebuild) once the live
// host-scene index is captured.
//
// GATE: the engine's CURRENT-SCENE global int at [0x00AAB384]. scene 5 ==
// char-create (class-select / appearance / name pages). Gating on ==5 EXCLUDES
// title, the intro (scene 3), char-select (scene 2/11) and in-game — the scene
// read alone is the tight, disasm-certain gate.
//
// Runs every frame from Hook_Present (always-on Present hook, INDEPENDENT of
// VideoEnable) inside that hook's SEH so it can NEVER throw into the engine's
// Present. The [0x00AAB384] read is a plain .data load, always valid.
//
// request() = the engine's scene-request post @0x007A60DC (cdecl, 1 int arg:
// the requested scene id; writes [0xAAB388]=N, [0xAAB394]=1; the pump then sets
// [0xAAB384]=N). request(2) stages scene 2 (TITLE).
// ---- FIX A: char-create class-info DESCRIPTION-text X pin (scene-5 gated) -----
// The class-info description text ("Proficient with bladed weapons…" / "Ranger /
// Human / Male…") is drawn by the char-create category/class radial-menu widget
// render (0x004F4E94) via RenderText3D. Its design-X is a single inline immediate:
// at 0x004F4FA2 `mov dword [esp],0x439D8000` (=315.0f); the 4 float bytes sit at
// 0x004F4FA5 (stock LE = 00 80 9D 43 = 315.0f). The info BOX rides the composer
// +160 stride but this text does NOT (it bypasses the composer), so it orphans
// center-left. We move it +160 (design space) to ride into the box.
//
// ONE-TIME EARLY BAKE (was a scene-gated per-frame poke; changed 
// the old code poked 315->475 every frame while scene[0x00AAB384]==5 from inside
// Present, which runs AFTER the frame is already rendered. So the FIRST char-
// create frame still drew the text at the OLD 315 (wrong spot) and only the NEXT
// frame showed 475 — the user-visible "text loads in a different spot initially".
//
// We now bake the widescreen value ONCE, on the first Present frame where the
// design width is readable as widescreen (design_w > 640), which lands long
// BEFORE char-create is ever reached (first frame after device/resolution init).
// So the very first char-create render is already at 475 — no first-frame flash,
// and NO per-frame writes (race-free). Live tests showed 0x004F4FA5 is the class-
// description X and poking it moves nothing in the dressing room / elsewhere
// (isolated single inline imm), so a permanent widescreen bake is safe; there is
// nothing to restore on leave, so the per-frame restore is gone too. At true 4:3
// (design_w <= 640) we do nothing and leave the stock 315.0.
// hs_peek_f32 is defined far below; forward-declare it here so the
// design_w read isn't implicit-int (mirrors the decl at the startup-bake block).
static float hs_peek_f32(uint32_t va);

// Bidirectional value-guarded .text float writer. UNLIKE cc_poke_imm_f32 (which
// has an `if(*p!=stock)return` precondition that blocks restoring a non-stock
// site), this writes whenever the current value differs from the target — so it
// can both POKE the char-create value and RESTORE 315.0. VirtualProtect RW around
// the 4-byte store, restore protection, all under SEH. Idempotent: no write when
// already at the target.
static void cc_text_force_f32(uint32_t va, float v)
{
    volatile float *p = (volatile float *)(uintptr_t)va;
    __try {
        if (*p == v) return;            // value-guard: already correct, no write
        DWORD old;
        if (!VirtualProtect((LPVOID)(uintptr_t)va, 4, PAGE_EXECUTE_READWRITE, &old)) return;
        *p = v;
        DWORD tmp; VirtualProtect((LPVOID)(uintptr_t)va, 4, old, &tmp);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

#define CC_TEXT_DESC_X_VA  0x004F4FA5u   // imm32 float operand of `mov [esp],315.0` @0x004F4FA2
// SECOND text lever the prior bake MISSED (RE workflow wolxzzbol): the inner
// 5-column pen-X base, a .data float at 0x0091E3F4 (stock 315.0f) read once at
// 0x004F503C feeding RenderText3D at 0x004F5053. Same char-create class-info
// widget (sub_004F4E94). Baking ONLY 0x004F4FA5 left these rows at 315 while the
// title line moved to 475 -> the "rows load in a different spot" misalignment.
// Bake it in lockstep with the identical stride. .data = writable (no protect
// flip needed; cc_text_force_f32's VirtualProtect is a harmless no-op there).
#define CC_TEXT_DESC_X2_VA 0x0091E3F4u

// Shared char-create right-pane horizontal-shift tuning knob. The class-description
// TEXT (this lever) and the whole class info BOX (cc_box_x_install splice, below) are
// BOTH shifted right by the SAME rigid amount = CC_RIGHTPANE_X_FACTOR*(design_w-640)
// so the moved box and the text inside it stay aligned. The parent tunes this ONE
// #define until the box is balanced; the text and box move together by construction.
// (Text rides RenderText3D 0x0082b54f, the box rides RenderChallengePanelElement
// 0x004e9d8c — different draw paths, so each needs its own bake, but the same factor.)
#define CC_RIGHTPANE_X_FACTOR 0.40f   // shared: right-pane rigid shift = factor*(design_w-640)
                                      // reduced 0.60->0.40 — the info box strode
                                      // too far right (big empty gap from the class list).
                                      // Box + description text move in lockstep (both use this).

// Scale-INVARIANT right-pane stride , owner: "same gap at 1.0 as 2.0").
// design_w (0x0098A4B8) = 853.333 * HudScale, so the raw span (design_w-640) GROWS with
// HudScale: 213 @hs1.0 but 1067 @hs2.0 -> the info box strode 5x further right at 2.0,
// blowing the class-list<->pane gap wide open. Key the shift to the UNIT-scale design
// width (design_w / HudScale = 853.33 @16:9, CONSTANT at every HudScale) so the shift is
// the SAME number of DESIGN px at 1.0/1.5/2.0. The affine then scales the whole layout
// uniformly => the class-list<->info-pane gap is proportionally identical at every scale.
// Only the class-info box (cc_box_x_stub path 2, design-X>=300) and the description text
// use this; the backdrop tiles ride g_cc_kx/ky (path 1) and are untouched.
static float cc_rightpane_offset(float dw)
{
    float s = g_cfg.hud_scale;
    float dw_unit = (s > 0.01f) ? (dw / s) : dw;   // 853.33 @16:9 at ANY HudScale
    if (dw_unit < 640.0f) dw_unit = 640.0f;        // never negative span (4:3 safety)
    return CC_RIGHTPANE_X_FACTOR * (dw_unit - 640.0f);
}

// One-shot early bake: on the first Present frame where design_w reads widescreen
// (> 640), bake the description-text X immediate to 315 + factor*span ONCE, then
// never write again (g_cc_text_baked guard). This lands long before char-create is
// reached, so its first render is already at the correct X (no first-frame flash).
// At true 4:3 (design_w <= 640) do nothing — leave the stock 315.0. The stride
// matches the info box's design-space stride at all HudScales (the box splice uses
// the SAME CC_RIGHTPANE_X_FACTOR). No per-frame poke, no restore-on-leave (the bake
// is permanent + safe: 0x004F4FA5 is the isolated class-description X, proven inert
// elsewhere).
static int g_cc_text_baked = 0;     // 0 = not yet baked, 1 = baked (never writes again)
static void cc_text_early_bake(void)
{
    if (g_cc_text_baked) return;    // already baked once — pure no-op thereafter
    __try {
        float dw = hs_peek_f32(0x0098A4B8u);   // design_w: 853.33 @16:9 hs1.0, 640 @4:3, 1280 @hs1.5
        if (dw > 640.0f) {
            // Stride keyed to design_w (CC_RIGHTPANE_X_FACTOR*span) — the EXACT SAME
            // factor the info-box splice (cc_box_x_install) uses, so the description
            // text rides aligned inside the moved box at every HudScale. Bake BOTH
            // description-text X levers in lockstep so the title line (0x004F4FA5 .text)
            // and the inner rows (0x0091E3F4 .data) move together — fixing the prior
            // single-lever misalignment. Both feed RenderText3D -> RenderUIQuad
            // 0x0082B440 (applies the affine itself), NOT the composer 0x0082BB74, so
            // the static write fully positions them with zero per-frame cost and no flicker.
            float cc_desc_x = 315.0f + cc_rightpane_offset(dw);   // scale-invariant stride
            cc_text_force_f32(CC_TEXT_DESC_X_VA,  cc_desc_x);   // 0x004F4FA5 (.text imm32)
            cc_text_force_f32(CC_TEXT_DESC_X2_VA, cc_desc_x);   // 0x0091E3F4 (.data float)
            g_cc_text_baked = 1;
            log_line("[cc_text] early bake desc-X 0x4F4FA5+0x91E3F4 = %.1f (dw=%.1f)", cc_desc_x, dw);
        }
        // dw not yet widescreen-readable (pre-device init): leave g_cc_text_baked
        // 0 so a later frame bakes it. dw <= 640 true-4:3 also leaves it 0 (nothing
        // to bake) — harmless: the loop just keeps reading the .data load each frame
        // until widescreen, or forever at 4:3 (a single compare + branch).
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// Present (slot 15) — render the boot poster ONTO the current backbuffer
// before the engine flips it. The boot_poster module checks its own
// auto-disable conditions on each call, so once any condition trips this
// hook becomes a single integer compare + branch.
static HRESULT STDMETHODCALLTYPE Hook_Present(
    IDirect3DDevice8 *self, const RECT *pSrc, const RECT *pDst,
    HWND hOverride, void *pDirtyRegion)
{
    // FIX A — one-shot early bake of the char-create description-text X. Fires
    // ONCE on the first frame design_w reads widescreen (long before char-create),
    // then self-retires to a single compare+branch. NOT a per-frame poke, so the
    // first char-create frame is already at the correct X (no "loads in a different
    // spot initially" flash). SEH-firewalled so it can never throw into Present.
    __try {
        cc_text_early_bake();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    __try {
        boot_poster_on_present((void *)self, g_last_vp_w, g_last_vp_h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Boot poster handler swallows its own exceptions internally;
        // this is just an extra firewall so a wrapper-specific edge case
        // never poisons the engine's Present.
    }
    // P3 video (Stage 1). Cheap `if(!enabled) return;` no-op when disabled,
    // so VideoEnable=0 (default) is byte-identical to today. SEH-firewalled
    // the same way as the boot poster (mod_video also wraps internally).
    __try {
        mod_video_on_present((void *)self, g_last_vp_w, g_last_vp_h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    // black-quadrant diagnostic (one-shot): the reparent wall
    // renders each quadrant's content into a top-left sub-rect with black
    // right/bottom margins. Capture the actual present geometry — Present's
    // own src/dst rects, the engine window client rect, and the cached
    // viewport — so we can see WHICH of these is smaller than the window.
    if (g_cfg.debug_log && !g_logged_present_geom) {
        g_logged_present_geom = 1;
        __try {
            RECT cr = {0,0,0,0};
            // Engine HWND global (same VA the cursor-warp fix uses as
            // kEngineHwndVA, spelled literally here because that #define
            // lives later in the file).
            HWND hwnd = *(HWND volatile *)0x00ACBED8u;
            if (hwnd) GetClientRect(hwnd, &cr);
            char srcbuf[64] = "NULL", dstbuf[64] = "NULL";
            if (pSrc) _snprintf_s(srcbuf, sizeof(srcbuf), _TRUNCATE,
                                  "(%ld,%ld)-(%ld,%ld)", pSrc->left, pSrc->top,
                                  pSrc->right, pSrc->bottom);
            if (pDst) _snprintf_s(dstbuf, sizeof(dstbuf), _TRUNCATE,
                                  "(%ld,%ld)-(%ld,%ld)", pDst->left, pDst->top,
                                  pDst->right, pDst->bottom);
            log_line("[pso_widescreen] PRESENT-GEOM: client=%ldx%ld pSrc=%s "
                     "pDst=%s hOverride=%p vp=%dx%d cfg=%dx%d logical=%dx%d",
                     cr.right - cr.left, cr.bottom - cr.top, srcbuf, dstbuf,
                     (void *)hOverride, g_last_vp_w, g_last_vp_h,
                     g_cfg.width, g_cfg.height, g_cfg.logical_width,
                     g_cfg.logical_height);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    ingame_reassert_on_transition();   // edge-driven; no per-frame work
    // char-select 1.0-look PIN reverted — char-select scales with
    // HudScale like Ephinea (no per-frame design-divisor pin from Present).
    return real_Present(self, pSrc, pDst, hOverride, pDirtyRegion);
}

// SetVertexShader (slot 50) — track current FVF so the draw hooks can
// tell whether they're getting RHW (HUD) or world-space (3D) verts.
// Handles with the high bits clear are FVF dwords; with the high byte
// set they're vertex-shader handles. Either way we forward unchanged.
static HRESULT STDMETHODCALLTYPE Hook_SetVertexShader(
    IDirect3DDevice8 *self, DWORD Handle)
{
    g_current_fvf = Handle;
    return real_SetVertexShader(self, Handle);
}

// Scale RHW HUD verts around their centroid, with save/restore so the
// engine's buffer is left untouched once we return. Naive in-place
// would compound across calls when psobb reuses a vertex buffer:
// 1.5⁵ ≈ 7.6× by the 5th draw → off-screen → black. With save/restore
// the scaling is a one-shot view modification per call.
//
// The save buffer lives on the caller's stack (HUD draws are tiny —
// well under a few hundred verts) which avoids HeapAlloc latency in
// the render hot path AND dodges the buffer-pointer-aliasing trap we
// hit with HeapAlloc + memcpy (d3d8to11 requires the engine's original
// src pointer, even when bytes are identical).
//
// Returns 1 if the buffer was modified — caller MUST then call
// scale_rhw_restore after the real-call finishes; 0 otherwise.
#define SCALE_SAVE_MAX_VERTS 4096

static void log_call_milestone_if_due(void)
{
    LONG total = g_draw_calls_total;
    if (total >= 100 && total / 500 != g_last_log_milestone / 500) {
        g_last_log_milestone = total;
        const volatile uint32_t *pc = (const volatile uint32_t *)0x00AAE168u;
        log_line("[pso_widescreen] HUDCompress: total=%ld acc=%ld | rej_invalid=%ld rej_no_player=%ld rej_bbox=%ld | PlayerCount=%u fvf=0x%08x",
                 (long)total, (long)g_draw_calls_accepted,
                 (long)g_rej_invalid_args, (long)g_rej_no_player,
                 (long)g_rej_bbox_too_big,
                 (unsigned)*pc, g_current_fvf);
    }
}

static int scale_rhw_apply(const void *src, UINT count, UINT stride,
                           float *saved_xy /* size 2*count floats */)
{
    InterlockedIncrement(&g_draw_calls_total);
    log_call_milestone_if_due();
    if (g_cfg.hud_compress == 1.0f) return 0;
    if (!src || stride < 16 || count == 0 || count > SCALE_SAVE_MAX_VERTS) {
        InterlockedIncrement(&g_draw_calls_rejected);
        InterlockedIncrement(&g_rej_invalid_args);
        return 0;
    }
    // Scene gate REMOVED — user wants ALL screens compressed including
    // title and select screens. Combined with centered-divide below, the
    // "title art clustered upper-left" failure mode is gone: every quad
    // shrinks toward the screen center instead of (0,0).
    BYTE *p = (BYTE *)src;
    float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
    for (UINT i = 0; i < count; ++i) {
        const float *xy = (const float *)(p + (size_t)i * stride);
        saved_xy[i*2 + 0] = xy[0];
        saved_xy[i*2 + 1] = xy[1];
        if (xy[0] < min_x) min_x = xy[0];
        if (xy[0] > max_x) max_x = xy[0];
        if (xy[1] < min_y) min_y = xy[1];
        if (xy[1] > max_y) max_y = xy[1];
    }
    const float bbox_w = max_x - min_x;
    const float bbox_h = max_y - min_y;
    const float screen_w = (float)(g_cfg.width  > 0 ? g_cfg.width  : 1920);
    const float screen_h = (float)(g_cfg.height > 0 ? g_cfg.height : 1080);
    // Bbox cap REMOVED — user wants full-screen title art compressed too.
    // We still skip if the bbox is genuinely degenerate (suspicious tiny
    // line-list or single-vertex point) but otherwise pass everything.
    (void)bbox_w; (void)bbox_h; (void)screen_w; (void)screen_h;
    // HUDCompress "MORE ROOM": cfg=1.5 means each vert's offset from the
    // SCREEN CENTER is divided by 1.5 — every quad shrinks toward
    // center. Adjacent verts stay adjacent (no fragmentation), and
    // centered things stay centered (no upper-left clustering). The
    // entire visible 2D content occupies 1/1.5 of the screen, exposing
    // more 3D world / black border depending on the screen.
    const float inv = 1.0f / g_cfg.hud_compress;
    const float cx = screen_w * 0.5f;
    const float cy = screen_h * 0.5f;
    for (UINT i = 0; i < count; ++i) {
        float *xy = (float *)(p + (size_t)i * stride);
        xy[0] = cx + (xy[0] - cx) * inv;
        xy[1] = cy + (xy[1] - cy) * inv;
    }
    InterlockedIncrement(&g_draw_calls_accepted);
    if (!g_logged_first_hud) {
        g_logged_first_hud = 1;
        log_line("[pso_widescreen] HUDCompress active: cfg=%.2f -> divide-by-cfg around (0,0) "
                 "(first call count=%u stride=%u fvf=0x%08x bbox=%.0fx%.0f)",
                 g_cfg.hud_compress, count, stride, g_current_fvf, bbox_w, bbox_h);
    }
    return 1;
}

static void scale_rhw_restore(const void *src, UINT count, UINT stride,
                              const float *saved_xy)
{
    BYTE *p = (BYTE *)src;
    for (UINT i = 0; i < count; ++i) {
        float *xy = (float *)(p + (size_t)i * stride);
        xy[0] = saved_xy[i*2 + 0];
        xy[1] = saved_xy[i*2 + 1];
    }
}

static HRESULT STDMETHODCALLTYPE Hook_DrawPrimitiveUP(
    IDirect3DDevice8 *self, DWORD PrimitiveType, UINT PrimitiveCount,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
    // Vertex count = primitive count * vertices-per-prim, but for our
    // scaling we only need the count of stride-sized records that must
    // be touched. UP variants pack all verts into the buffer so:
    // D3DPRIMITIVETYPE values from d3d8.h:
    // 1 = D3DPT_POINTLIST       (vertex_count = primitive_count)
    // 2 = D3DPT_LINELIST        (vertex_count = primitive_count * 2)
    // 3 = D3DPT_LINESTRIP       (vertex_count = primitive_count + 1)
    // 4 = D3DPT_TRIANGLELIST    (vertex_count = primitive_count * 3)
    // 5 = D3DPT_TRIANGLESTRIP   (vertex_count = primitive_count + 2)
    // 6 = D3DPT_TRIANGLEFAN     (vertex_count = primitive_count + 2)
    UINT vcount;
    switch (PrimitiveType) {
        case 1: vcount = PrimitiveCount;       break;
        case 2: vcount = PrimitiveCount * 2;   break;
        case 3: vcount = PrimitiveCount + 1;   break;
        case 4: vcount = PrimitiveCount * 3;   break;
        case 5: vcount = PrimitiveCount + 2;   break;
        case 6: vcount = PrimitiveCount + 2;   break;
        default: vcount = PrimitiveCount * 3;  break;
    }
    float saved[2 * SCALE_SAVE_MAX_VERTS];
    int modified = scale_rhw_apply(pVertexStreamZeroData, vcount,
                                   VertexStreamZeroStride, saved);
    HRESULT hr = real_DrawPrimitiveUP(self, PrimitiveType, PrimitiveCount,
                                      pVertexStreamZeroData,
                                      VertexStreamZeroStride);
    if (modified) {
        scale_rhw_restore(pVertexStreamZeroData, vcount,
                          VertexStreamZeroStride, saved);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitiveUP(
    IDirect3DDevice8 *self, DWORD PrimitiveType,
    UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount,
    const void *pIndexData, DWORD IndexDataFormat,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
    float saved[2 * SCALE_SAVE_MAX_VERTS];
    int modified = scale_rhw_apply(pVertexStreamZeroData, NumVertices,
                                   VertexStreamZeroStride, saved);
    HRESULT hr = real_DrawIndexedPrimitiveUP(self, PrimitiveType,
                                             MinVertexIndex, NumVertices,
                                             PrimitiveCount, pIndexData,
                                             IndexDataFormat,
                                             pVertexStreamZeroData,
                                             VertexStreamZeroStride);
    if (modified) {
        scale_rhw_restore(pVertexStreamZeroData, NumVertices,
                          VertexStreamZeroStride, saved);
    }
    return hr;
}

// Patch the device's vtable once the first device comes back from
// CreateDevice. The vtable is shared across all IDirect3DDevice8
// instances created by the same IDirect3D8 factory, so one patch
// covers the whole process. We patch:
// slot 15 = Present                (boot poster pre-flip overlay)
// slot 37 = SetTransform           (FOV / aspect correction)
// slot 40 = SetViewport            (full-backbuffer override)
// slot 72 = DrawPrimitiveUP        (HUDScale on RHW 2D draws)
// slot 73 = DrawIndexedPrimitiveUP (HUDScale on RHW 2D draws)
// slot 76 = SetVertexShader        (FVF tracking for HUDScale)
static void patch_device_vtable(IDirect3DDevice8 *dev)
{
    if (!dev || g_device_patched) return;
    void **vt = *(void ***)dev;
    DWORD old_prot = 0;
    // Present hook — armed when the boot poster OR the P3 video player is
    // enabled in cfg, since those are the features that need a pre-flip
    // overlay. Guarding the install keeps wrapper-startup paths clean for
    // users who disable both.
    if (g_cfg.boot_poster_enabled || g_cfg.video_enabled) {
        if (VirtualProtect(&vt[15], sizeof(void*), PAGE_READWRITE, &old_prot)) {
            real_Present = (Present_t)vt[15];
            vt[15] = (void *)&Hook_Present;
            DWORD tmp = 0;
            VirtualProtect(&vt[15], sizeof(void*), old_prot, &tmp);
            log_line("[pso_widescreen] device vtable[15] patched: real Present=0x%p",
                     (void*)real_Present);
        } else {
            log_line("[pso_widescreen] device vtable[15] VirtualProtect FAILED err=%lu",
                     GetLastError());
        }
    }
    if (VirtualProtect(&vt[37], sizeof(void*), PAGE_READWRITE, &old_prot)) {
        real_SetTransform = (SetTransform_t)vt[37];
        vt[37] = (void *)&Hook_SetTransform;
        DWORD tmp = 0;
        VirtualProtect(&vt[37], sizeof(void*), old_prot, &tmp);
        log_line("[pso_widescreen] device vtable[37] patched: real SetTransform=0x%p",
                 (void*)real_SetTransform);
    } else {
        log_line("[pso_widescreen] device vtable[37] VirtualProtect FAILED err=%lu",
                 GetLastError());
    }
    if (VirtualProtect(&vt[40], sizeof(void*), PAGE_READWRITE, &old_prot)) {
        real_SetViewport = (SetViewport_t)vt[40];
        vt[40] = (void *)&Hook_SetViewport;
        DWORD tmp = 0;
        VirtualProtect(&vt[40], sizeof(void*), old_prot, &tmp);
        log_line("[pso_widescreen] device vtable[40] patched: real SetViewport=0x%p",
                 (void*)real_SetViewport);
    } else {
        log_line("[pso_widescreen] device vtable[40] VirtualProtect FAILED err=%lu",
                 GetLastError());
    }
    // HUDScale hooks — only install when scaling is requested.
    // Correct d3d8 IDirect3DDevice8 vtable slots:
    // 72 = DrawPrimitiveUP        (NOT 53 — that's EndStateBlock)
    // 73 = DrawIndexedPrimitiveUP (NOT 54 — that's ApplyStateBlock)
    // 76 = SetVertexShader        (NOT 50 — that's SetRenderState)
    if (g_cfg.hud_compress != 1.0f) {
        if (VirtualProtect(&vt[76], sizeof(void*), PAGE_READWRITE, &old_prot)) {
            real_SetVertexShader = (SetVertexShader_t)vt[76];
            vt[76] = (void *)&Hook_SetVertexShader;
            DWORD tmp = 0;
            VirtualProtect(&vt[76], sizeof(void*), old_prot, &tmp);
            log_line("[pso_widescreen] device vtable[76] patched: real SetVertexShader=0x%p",
                     (void*)real_SetVertexShader);
        }
        if (VirtualProtect(&vt[72], sizeof(void*), PAGE_READWRITE, &old_prot)) {
            real_DrawPrimitiveUP = (DrawPrimitiveUP_t)vt[72];
            vt[72] = (void *)&Hook_DrawPrimitiveUP;
            DWORD tmp = 0;
            VirtualProtect(&vt[72], sizeof(void*), old_prot, &tmp);
            log_line("[pso_widescreen] device vtable[72] patched: real DrawPrimitiveUP=0x%p (HUDCompress=%.2f)",
                     (void*)real_DrawPrimitiveUP, g_cfg.hud_compress);
        }
        if (VirtualProtect(&vt[73], sizeof(void*), PAGE_READWRITE, &old_prot)) {
            real_DrawIndexedPrimitiveUP = (DrawIndexedPrimitiveUP_t)vt[73];
            vt[73] = (void *)&Hook_DrawIndexedPrimitiveUP;
            DWORD tmp = 0;
            VirtualProtect(&vt[73], sizeof(void*), old_prot, &tmp);
            log_line("[pso_widescreen] device vtable[73] patched: real DrawIndexedPrimitiveUP=0x%p",
                     (void*)real_DrawIndexedPrimitiveUP);
        }
    }
    g_device_patched = 1;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(
    IDirect3D8 *self,
    UINT Adapter,
    DWORD DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS_X *pp,
    IDirect3DDevice8 **out)
{
    if (pp && g_cfg.enabled) {
        // GATE on g_cfg.enabled when Enabled=0 we are in
        // pure passthrough — Sodaboy's d3d8.dll has already configured
        // the backbuffer + windowed flag the way IT wants for its own
        // widescreen recipe, and overriding either here clobbers his
        // calculations and produces visible artifacts (giant central
        // oval over the minimap). With Enabled=1 (modernize / no-
        // Sodaboy chain) we own the widescreen end-to-end and
        // OverrideBackBuffer / Windowed / borderless-resize all run.
        const UINT orig_w = pp->BackBufferWidth;
        const UINT orig_h = pp->BackBufferHeight;
        const BOOL orig_w_flag = pp->Windowed;
        // BackBuffer override is opt-in via OverrideBackBuffer=1. Sodaboy
        // convention (corrected after the SEGA splash regression):
        // backbuffer dims are LOGICAL canvas (1920x1080 default) so the
        // engine renders to a surface that matches its res_table view of
        // the world, while the OS window sizes to PHYSICAL Width/Height
        // and the d3d8 wrapper handles BB->window upscaling at present.
        // Previously we set BB to physical 3840x2160, which left the
        // engine's 1920x1080 RHW coords in the upper-left quadrant of a
        // 4K surface (visible as the SEGA splash filling only the top-
        // left corner with the rest black).
        if (g_cfg.override_backbuffer) {
            const int lw = g_cfg.logical_width  > 0 ? g_cfg.logical_width  : 1920;
            const int lh = g_cfg.logical_height > 0 ? g_cfg.logical_height : 1080;
            pp->BackBufferWidth  = (UINT)lw;
            pp->BackBufferHeight = (UINT)lh;
        }
        // Windowed mode: 0=fullscreen, 1=windowed, 2=borderless windowed.
        // For 1 and 2, the d3d8 'Windowed' flag is TRUE. The borderless
        // distinction lives in the OS window style, applied below.
        pp->Windowed = (g_cfg.windowed != 0) ? TRUE : FALSE;
        if (g_cfg.windowed == 2) {
            // The borderless window always sizes to PHYSICAL Width/Height
            // (the user's actual screen resolution) so the d3d8 wrapper
            // has somewhere to stretch the LOGICAL backbuffer to. If
            // Width/Height were unset, fall back to BB dims (= logical).
            int win_w = g_cfg.width  > 0 ? g_cfg.width  : (int)pp->BackBufferWidth;
            int win_h = g_cfg.height > 0 ? g_cfg.height : (int)pp->BackBufferHeight;
            make_borderless(pp->hDeviceWindow ? pp->hDeviceWindow : hFocusWindow,
                            win_w, win_h);
            log_line("[pso_widescreen] borderless window sized to %dx%d (physical)", win_w, win_h);
        }
        log_line("[pso_widescreen] CreateDevice: BackBuffer %ux%u -> %ux%u (logical), "
                 "window=%dx%d (physical), windowed %d -> %d (mode=%d, override_bb=%d)",
                 (unsigned)orig_w, (unsigned)orig_h,
                 (unsigned)pp->BackBufferWidth, (unsigned)pp->BackBufferHeight,
                 g_cfg.width, g_cfg.height,
                 (int)orig_w_flag, (int)pp->Windowed, g_cfg.windowed,
                 g_cfg.override_backbuffer);
    } else if (pp) {
        log_line("[pso_widescreen] CreateDevice: passthrough "
                 "(Enabled=0; Sodaboy or other wrapper owns BB %ux%u, windowed=%d)",
                 (unsigned)pp->BackBufferWidth, (unsigned)pp->BackBufferHeight,
                 (int)pp->Windowed);
    }
    HRESULT hr = real_CreateDevice(self, Adapter, DeviceType, hFocusWindow,
                                   BehaviorFlags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        // Patch unconditionally when we successfully created a device —
        // the SetViewport hook is needed any time we override the
        // backbuffer size, otherwise the engine's launcher-resolution
        // SetViewport leaves us drawing in a corner. SetTransform's
        // FOV path is gated inside Hook_SetTransform on fov_correction.
        patch_device_vtable(*out);
    }
    return hr;
}

static IDirect3D8 *WINAPI Hook_Direct3DCreate8(UINT SDKVersion)
{
    IDirect3D8 *d3d = real_Direct3DCreate8 ? real_Direct3DCreate8(SDKVersion) : NULL;
    if (!d3d) {
        log_line("[pso_widescreen] real Direct3DCreate8(%u) returned NULL", SDKVersion);
        return d3d;
    }
    if (!g_vtable_patched) {
        // Vtable lives at *(void***)d3d. CreateDevice is slot 15 in the
        // d3d8 IDirect3D8 vtable (post QueryInterface/AddRef/Release at
        // 0..2 then 12 enum/cap query methods).
        void **vt = *(void ***)d3d;
        DWORD old_prot = 0;
        if (VirtualProtect(&vt[15], sizeof(void*), PAGE_READWRITE, &old_prot)) {
            real_CreateDevice = (CreateDevice_t)vt[15];
            vt[15] = (void *)&Hook_CreateDevice;
            DWORD tmp = 0;
            VirtualProtect(&vt[15], sizeof(void*), old_prot, &tmp);
            g_vtable_patched = 1;
            log_line("[pso_widescreen] vtable[15] patched: real CreateDevice=0x%p",
                     (void*)real_CreateDevice);
        } else {
            log_line("[pso_widescreen] vtable[15] VirtualProtect FAILED err=%lu",
                     GetLastError());
        }
    }
    return d3d;
}

// ---- IAT patch on psobb.exe's d3d8.dll!Direct3DCreate8 slot ----
//
// We do this rather than inline-hooking the export because IAT lives in
// psobb.exe's own image — independent of which d3d8.dll resolved at
// load time. If the user later swaps d3d8.dll for any other variant
// the IAT slot is already redirected to us.

static int patch_iat(HMODULE mod, const char *target_dll, const char *target_fn,
                     void *new_fn, void **old_fn_out)
{
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    DWORD imp_rva = nt->OptionalHeader.DataDirectory[
        IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva) return 0;
    PIMAGE_IMPORT_DESCRIPTOR imp =
        (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)mod + imp_rva);
    for (; imp->Name; ++imp) {
        const char *dll = (const char *)((BYTE*)mod + imp->Name);
        if (_stricmp(dll, target_dll) != 0) continue;
        PIMAGE_THUNK_DATA orig = imp->OriginalFirstThunk
            ? (PIMAGE_THUNK_DATA)((BYTE*)mod + imp->OriginalFirstThunk)
            : (PIMAGE_THUNK_DATA)((BYTE*)mod + imp->FirstThunk);
        PIMAGE_THUNK_DATA cur  = (PIMAGE_THUNK_DATA)((BYTE*)mod + imp->FirstThunk);
        for (; orig->u1.AddressOfData; ++orig, ++cur) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            PIMAGE_IMPORT_BY_NAME ibn =
                (PIMAGE_IMPORT_BY_NAME)((BYTE*)mod + orig->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, target_fn) != 0) continue;
            DWORD old_prot = 0;
            if (!VirtualProtect(&cur->u1.Function, sizeof(void*),
                                PAGE_READWRITE, &old_prot)) {
                return 0;
            }
            if (old_fn_out) *old_fn_out = (void *)cur->u1.Function;
            cur->u1.Function = (DWORD_PTR)new_fn;
            DWORD tmp = 0;
            VirtualProtect(&cur->u1.Function, sizeof(void*), old_prot, &tmp);
            return 1;
        }
    }
    return 0;
}

// ---- Sodaboy-replication memory patches ----
//
// Writes the same psobb.exe data-section modifications that Sodaboy's
// d3d8.dll wrapper applies. Captured live via psobbdbg from a side-by-
// side run (full diff in memory: sodaboy_widescreen_patches.md).
// Without these the engine's RHW HUD verts assume the launcher's chosen
// resolution (e.g. 1024x768) and end up squashed into the upper-left of
// any larger backbuffer.
//
// Three classes of patch:
// 1. NOP 32 bytes at psobb.exe!0x00482e20..0x00482e3f.
// Original: a 10-byte `mov dword [0x00a46c72], 0` followed by
// `push "WINDOW_MODE"` + `call ini_lookup` + `cmp eax, 4` validation
// block that REJECTS any non-tabled resolution. NOP'ing it lets the
// engine accept arbitrary widths/heights.
// 2. Overwrite the 6-entry resolution table at psobb.exe!0x009006f4
// (originally 640x480/800x600/1024x768/1280x960/1280x1024/1600x1200,
// 48 bytes total) with 6 copies of the desired (W,H) so every
// table-lookup picks our resolution.
// 3. Overwrite the 8-row HUD-rect table at psobb.exe!0x009a3840
// (40 dwords / 160 bytes) with values scaled to the new aspect.
// For 1920x1080: { (1023,576,255,192), (1023,384,255,192), ...,
// (511,384,511,384), (511,0,511,384) }. Hardcoded for 1080p only;
// other resolutions need their own table (TODO: derive from cfg).

// Sanity check Sodaboy uses to verify it's running against psobb.exe
// (these are stable code/data signatures of the supported build).
#define PSOBB_SIG_82D1D0 0x246c8b50u  // bytes "P\x8bl$" at code 0x0082d1d0
#define PSOBB_SIG_82D1D4 0x15ff5568u  // bytes "hU\xff\x15"
// (Was: PSOBB_SIG_482E20 / PSOBB_SIG_482E24 — removed cleanup.
// They were defined for the patch_validator_nop path but never wired into
// the sig_check() call. The validator NOP is gated by `g_cfg.patch_validator_nop`
// only and defaults off because it depends on Sodaboy's 0x82d1d8 fn-ptr
// hijack to provide a callback that's not present without the wrapper.)

static int sig_check(uintptr_t addr, uint32_t expected, const char *label)
{
    uint32_t got = *(volatile uint32_t *)addr;
    if (got == expected) return 1;
    log_line("[pso_widescreen] sig MISMATCH %s @ 0x%p: expected 0x%08x got 0x%08x",
             label, (void *)addr, expected, got);
    return 0;
}

static int patch_write(uintptr_t addr, const void *src, size_t len, const char *label)
{
    DWORD old_prot = 0;
    if (!VirtualProtect((LPVOID)addr, len, PAGE_EXECUTE_READWRITE, &old_prot)) {
        log_line("[pso_widescreen] VirtualProtect FAILED %s @ 0x%p err=%lu",
                 label, (void *)addr, GetLastError());
        return 0;
    }
    memcpy((void *)addr, src, len);
    DWORD tmp = 0;
    VirtualProtect((LPVOID)addr, len, old_prot, &tmp);
    // icache-flush hygiene if the target is a .text instruction
    // immediate (VA below the .data boundary 0x008F8000), flush the instruction
    // cache so the CPU never prefetches/executes stale bytes. Harmless for .data
    // VAs (skipped). Matches the convention in patch_frontend_vertical /
    // patch_patch_status_screen, which flush .text imms the same way. These
    // writes all run at ASI attach (long before the target code executes), so on
    // x86 this is correctness-by-construction rather than a live-coherency need —
    // but it makes the shared helper match the file's own convention.
    if (addr < 0x008F8000u) {
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)addr, len);
    }
    log_line("[pso_widescreen] patched %s @ 0x%p (%zu bytes)",
             label, (void *)addr, len);
    return 1;
}

// Rewrite the rel32 displacement of a CALL (E8 imm32) at `site` to target
// `shim` instead of `expect_target`. Validates the existing call: first byte
// must be 0xE8 AND the current call destination must equal `expect_target`,
// so a non-matching build is left untouched. Used for one-call-site shims
// (e.g. the char-select scene's ResetNPCCamera call) — finer-grain than a
// MinHook detour on the target function (which would catch every caller).
static int redirect_call(uint32_t site, uint32_t expect_target, void *shim)
{
    uint8_t *p = (uint8_t *)(uintptr_t)site;
    if (p[0] != 0xE8) {
        log_line("[pso_widescreen] redirect_call: not a CALL @ 0x%08x (byte=0x%02x)", site, p[0]);
        return 0;
    }
    uint32_t cur_tgt = (uint32_t)(site + 5 + *(int32_t *)(p + 1));
    if (cur_tgt != expect_target) {
        log_line("[pso_widescreen] redirect_call: target mismatch @ 0x%08x (expected 0x%08x got 0x%08x)",
                 site, expect_target, cur_tgt);
        return 0;
    }
    int32_t rel = (int32_t)((uintptr_t)shim - (uintptr_t)(site + 5));
    DWORD old;
    if (!VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(int32_t *)(p + 1) = rel;
    DWORD t; VirtualProtect(p + 1, 4, old, &t);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    log_line("[pso_widescreen] redirect_call: 0x%08x: 0x%08x -> %p", site, expect_target, shim);
    return 1;
}

// SCALE-AWARE UI COORDS. The engine's 2D UI logical canvas scales
// INVERSELY with HUDScale: logical_W = 853.33 * hud_scale, so the design->screen
// matrix [0xACC0E8] = 1920 / (853.33 * hud_scale) = 2.25 / hud_scale. (Verified
// live: HUDScale 1.0 -> canvas 853x480, matrix 2.25; HUDScale 1.5 -> 1280x720,
// matrix 1.5.) All our title/menu/char-select design coords were dialed in the
// standard 1280x720 space, i.e. the canvas at HUDScale 1.5. To hold the SAME
// on-screen position at ANY HUDScale, a design coord must be written as
// design_1.5 * (hud_scale / 1.5). This keeps the widescreen LAYOUT (goal 1)
// fixed on screen independent of the HUD-scale knob (goal 2).
// SINGLE-FRAME: kUiDesignScale (1.5) and the dual-calibration model
// are REMOVED. Every UI design coord now lives in ONE native frame dialed at the
// hs=1.0 canvas; ui_coord(x) is just x * hud_scale (uniform multiplier). The old
// 1.5-space config defaults (hangame menu x/y, streak dx) were rebaked to their
// native equivalents (value * (1.0f/1.5f)) so the engine receives bit-identical
// values at hud_scale==1.0. `calib` is gone — there is no second frame.
static float ui_coord(float design)
{
    float hs = g_cfg.hud_scale;
    if (!(hs > 0.1f && hs < 10.0f)) hs = 1.0f;
    return design * hs;
}

// ============================================================
// CHAR-CREATE HEX BACKDROP 16:9 FILL — add tile column(s) at the function tail
// (RenderChallengePanelBackground_004ed008), NOT a per-frame composer poke.
// ============================================================
// PROBLEM: char-create's hex backdrop is drawn by
// RenderChallengePanelBackground_004ed008 as 8 tiles over two STATIC position
// tables (0x0091DBE0 = 6 tiles ids 12..17; 0x0091DBD0 = 2 tiles ids 12,13).
// Columns are contiguous in design space: X=0 W256 ->0..256, X=256 W128 ->256..384,
// X=384 W256 ->384..640. The backdrop therefore covers only design X 0..640 (=screen
// 0..1440 at 16:9 hs1.0), leaving the right ~25% dark. The composer applies the 2D
// affine (x2.25); tile WIDTH is the shared global [0x00A7221C] so scaling positions
// (live-proven by the parent) opens GAPS, and the per-tile Xscale imm @0x004E9D0F is
// SHARED with the title bar — both are the wrong lever.
//
// FIX (this code): after the engine's own two loops finish, emit ADDITIONAL static-
// positioned tiles at design X=640, 896, ... (each W256, contiguous, no gaps) until
// the columns cover design_w. This reuses ids 12 & 13 (W=256, Y=0 and Y=256) — the
// SAME ids the engine already renders this frame, so the per-id UV/size record at
// 0x009B76E0[id] is provably valid — and calls the engine's own
// SetChallengePanelScale_004e9ce8 with a {X,Y} struct shaped exactly like the
// engine's 8-byte table entries (the loops stride ebx by 8 => the position struct is
// exactly {float X, float Y}; width/UV come from 0x009B76E0[id], NOT this struct).
// Because we drive the engine's own emit path with a static struct, these are static
// background tiles — NOT a per-frame value-poke. No flicker.
//
// SPLICE: 0x004ed06d, the `call 0x004D28A4` (E8 32 58 FE FF, exactly 5 bytes) that
// runs immediately AFTER both loops and BEFORE the base-fill globals block at
// 0x004ed072. At this point esp is balanced to the function frame (every loop call
// did `add esp,0xC`) and no GP register is live across the call (sub_004D28A4 loads
// its own absolute addrs 0xA48820/0xA487C0 and is register-independent; the base-fill
// block re-loads everything fresh). We E9 to a stub that emits the extra column(s)
// then jmps a trampoline that RE-ISSUES the original call (with a correctly relocated
// rel32 — a verbatim byte-copy of an E8 would mis-relocate) and E9s back to 0x004ed072.
//
// GATES (runtime, in the handler — the .text splice is always present but inert):
// (1) screen-id [0x00A165F0] == 12  => char-create only (NOT Challenge mode, which
// also reaches this "ChallengePanel" framework). 4:3 char-create still no-ops
// via gate (2).
// (2) design_w (hs_peek_f32 0x0098A4B8) > 640  => 4:3 (640) is a hard no-op (no
// extra columns emitted), matching the rest of the front-end-bake no-op-at-4:3
// convention.
// The column count is data-driven on design_w, so HudScale 1.5 (design_w 1280)
// naturally adds more columns (X=640, 896, 1152) to fill its wider design space.

// RE-DERIVED, then RE-CORRECTED from the LIVE disasm + scrim geometry:
// RenderChallengePanelBackground_004ed008 draws in this order:
// [tile loop1 ids12..17] [tile loop2 ids12..13]  -> native backdrop tiles, X 0..640
// 0x004ED06D  call 0x004D28A4 (update_viewport)  -> our CLEAN splice point
// 0x004ED103  call 0x0082B5D8 (SCRIM A, X 140..384, alpha 0->255 gradient fade-in)
// 0x004ED16E  call 0x0082B5D8 (SCRIM B, X 384..design_w, alpha 255->236 near-black)
// 0x004ED176  call 0x004D2968 (unknown_ui_viewport) ... depth/state restore ... ret
// The native tiles cover X 0..640 contiguously (col@0 big id12/13, col@256 narrow
// id14..17 stack, col@384 big id12/13). The ONLY hole is X 640..design_w (the right
// ~213px at hs1.0): no tile there, so the near-black scrim B composites over raw
// background = the "dark gap" the user sees. Scrim B's right edge is already widened
// to design_w by our startup bake (verified live: verts 0x9B8DC4/DDC = 853.33), so it
// DOES cover the hole -- the hole just has nothing teal underneath it.
// FIX (clean, engine-native): splice at 0x004ED06D, i.e. AFTER the tile loops but
// BEFORE both scrims, and emit extra big tiles id12(top,Y=0)+id13(bottom,Y=256) at the
// natural 256-wide grid step from X=640 to design_w. Because they emit in the SAME
// phase as the engine's own tiles, both scrims composite over them IDENTICALLY -> the
// fill is indistinguishable from a native column. No overlap (step == tile width), no
// copy-paste, no scrim edit. Challenge mode (shares this fn) stays byte-identical off
// char-create via the screen-id gate; 4:3 no-ops via the design_w>640 gate.
#define CC_BACKDROP_SPLICE_VA   0x004ED06Du     // call 0x004D28A4 (after tiles, BEFORE both scrims)
#define CC_BACKDROP_SPLICE_LEN  5
#define CC_BACKDROP_RESUME_VA   0x004ED072u     // first byte after the spliced call
#define CC_SETSCALE_VA          0x004E9CE8u     // SetChallengePanelScale_004e9ce8
#define CC_SUB_004D28A4_VA      0x004D28A4u     // the original call target (re-issued)
#define CC_SCREENID_VA          0x00A165F0u     // front-end screen id (char-create=12)
#define CC_DESIGNW_VA           0x0098A4B8u     // design_w (anzz1-owned); FE = 853.33
#define CC_DESIGNH_VA           0x0098A4B4u     // design_h (anzz1-owned); FE = 480 (960 @hs2.0)
#define CC_TILE_W               256.0f          // big tile width = natural grid step (NO overlap)
#define CC_DEPTH                0xC61B3C00u     // the depth arg the engine passes (-9935.0f)
#define CC_EXTRA_ID_TOP         12              // top tile id (Y=0),   big teal hex
#define CC_EXTRA_ID_BOT         13              // bottom tile id (Y=256), big teal hex
// Begin the fill at passed-X 256. The LOOP2/fill render path adds a CONSTANT +128
// design-X offset (proven via draw_capture: passed 384->render 512, passed 640->render
// 768), so passed 256 lands at render 384 -- exactly where the solid LOOP1 coverage
// (render 0..384) ends. In widescreen the engine pushes its own LOOP2 column from the
// 4:3 spot (render 384..640) to render 512..768, opening a GAP at render 384..512; our
// fill columns at passed 256/512/768 -> render 384/640/896 cover that internal gap AND
// the right-edge gap (render 768..design_w) in one gapless, grid-aligned sweep.
#define CC_FILL_X_START         256.0f          // passed-X start (+128 path offset -> render 384)
#define CC_EXTRA_COL_MAX        12              // safety cap on emitted extra columns

// cdecl signature of SetChallengePanelScale_004e9ce8: (int id, float depth_bits,
// const float *pos_xy). depth is pushed as raw 32 bits (the engine pushes the
// imm 0xC61B3C00); we pass it as a float-typed arg whose bit pattern is identical.
typedef void (__cdecl *PFN_set_challenge_scale)(int id, unsigned int depth, const float *pos_xy);

// ASI-owned static position structs, each shaped EXACTLY like the engine's 8-byte
// {float X, float Y} table entries. X is set per-column at emit time; Y is fixed
// (top column tile Y=0, bottom column tile Y=256), matching ids 12/13 in the
// engine's own LOOP2 table 0x0091DBD0 = (384,0)(384,256).
static float g_cc_col_top[CC_EXTRA_COL_MAX][2];   // {X, 0}
static float g_cc_col_bot[CC_EXTRA_COL_MAX][2];   // {X, 256}

static uint8_t  g_cc_backdrop_orig[CC_BACKDROP_SPLICE_LEN] = {0};
static void    *g_cc_backdrop_tramp = NULL;       // RWX: re-issue call + E9 resume
static int      g_cc_backdrop_installed = 0;
static LONG     g_cc_backdrop_emit_n = 0;         // diag: emit invocations

// Handler — runs once per RenderChallengePanelBackground call (i.e. once per
// char-create / Challenge-mode backdrop draw). Gated; emits the extra column(s).
// __try-wrapped: a fault here must never crash the render thread.
static void __stdcall cc_backdrop_emit(void)
{
    __try {
        const int screen_id = *(volatile int32_t *)(uintptr_t)CC_SCREENID_VA;
        if (screen_id != 12) return;                 // char-create only
        const float dw = hs_peek_f32(CC_DESIGNW_VA);
        if (dw <= 640.0f) return;                    // 4:3 hard no-op

        PFN_set_challenge_scale set_scale =
            (PFN_set_challenge_scale)(uintptr_t)CC_SETSCALE_VA;

        // The engine's solid LOOP1 coverage ends at render X=384; LOOP2 (its right
        // column) sits at render 512..768 in widescreen, leaving TWO holes: the internal
        // 384..512 and the right edge 768..design_w. We run BEFORE the two scrims, so each
        // extra tile is darkened by scrim B exactly like the engine's own LOOP2 column ->
        // the fill is indistinguishable from a native column. Emit big tiles
        // id12(top,Y=0)+id13(bottom,Y=256) on the natural 256-wide grid from passed-X 256
        // (render 384) up to design_w. Passed 256/512/768 -> render 384/640/896 at hs1.0;
        // step == tile width => abutting, NO overlap and NO gap, full height per column.
        int cols = 0;
        for (float x = CC_FILL_X_START; x < dw && cols < CC_EXTRA_COL_MAX;
             x += CC_TILE_W) {
            g_cc_col_top[cols][0] = x;   g_cc_col_top[cols][1] = 0.0f;
            g_cc_col_bot[cols][0] = x;   g_cc_col_bot[cols][1] = 256.0f;
            set_scale(CC_EXTRA_ID_TOP, CC_DEPTH, g_cc_col_top[cols]);
            set_scale(CC_EXTRA_ID_BOT, CC_DEPTH, g_cc_col_bot[cols]);
            ++cols;
        }
        if (g_cfg.debug_log && g_cfg.debug_log_verbose) {
            LONG n = InterlockedIncrement(&g_cc_backdrop_emit_n);
            if (n <= 8)
                log_line("[pso_widescreen] cc-backdrop: +%d col(s) from X=256 (dw=%.1f)",
                         cols, dw);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// Naked stub: preserve all GP regs + flags, call the emitter, then jmp the
// trampoline (which re-issues the original `call 0x004D28A4` and resumes at
// 0x004ed072). The emitter is __stdcall (cleans its own 0 args); SetChallengePanelScale
// is cdecl and the emitter balances those pushes itself, so esp is intact on return.
__declspec(naked) static void cc_backdrop_stub(void)
{
    __asm {
        pushad
        pushfd
        call  cc_backdrop_emit
        popfd
        popad
        jmp   dword ptr [g_cc_backdrop_tramp]
    }
}

// Install the tail-splice. Sig-guarded (refuses unless the stock E8-call bytes are
// present) and idempotent (installed-flag + sig check). The trampoline is built
// manually (the saved prologue is not byte-copied) because the spliced instruction
// is a relative E8 call that must be RE-ENCODED at the trampoline's address.
static void cc_backdrop_install(void)
{
    if (g_cc_backdrop_installed) return;
    // Stock bytes at 0x004ED06D: E8 32 58 FE FF = call 0x004D28A4 (re-issued by tramp).
    static const uint8_t kExpected[CC_BACKDROP_SPLICE_LEN] = {
        0xE8, 0x32, 0x58, 0xFE, 0xFF
    };
    const uint8_t *p = (const uint8_t *)(uintptr_t)CC_BACKDROP_SPLICE_VA;
    for (int i = 0; i < CC_BACKDROP_SPLICE_LEN; ++i) {
        if (p[i] != kExpected[i]) {
            log_line("[pso_widescreen] cc-backdrop: splice sig mismatch byte %d: "
                     "got 0x%02x expected 0x%02x — refusing to hook", i, p[i], kExpected[i]);
            return;
        }
        g_cc_backdrop_orig[i] = p[i];
    }
    // Trampoline (RWX): re-issue the original call (correctly relocated) then E9 back.
    // [0]  E8 <rel32 -> 0x004D28A4>
    // [5]  E9 <rel32 -> 0x004ED072>
    void *page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
                              PAGE_EXECUTE_READWRITE);
    if (!page) {
        log_line("[pso_widescreen] cc-backdrop: trampoline alloc FAILED err=%lu",
                 GetLastError());
        return;
    }
    uint8_t *t = (uint8_t *)page;
    t[0] = 0xE8;
    int32_t rel_call = (int32_t)(CC_SUB_004D28A4_VA - ((uintptr_t)t + 5));
    memcpy(t + 1, &rel_call, 4);
    t[5] = 0xE9;
    int32_t rel_resume = (int32_t)(CC_BACKDROP_RESUME_VA - ((uintptr_t)t + 10));
    memcpy(t + 6, &rel_resume, 4);
    g_cc_backdrop_tramp = page;

    // Write the E9 splice at 0x004ED06D (exactly 5 bytes == the original call's length,
    // so no NOP padding is needed).
    DWORD old_prot = 0;
    if (!VirtualProtect((LPVOID)(uintptr_t)CC_BACKDROP_SPLICE_VA, CC_BACKDROP_SPLICE_LEN,
                        PAGE_EXECUTE_READWRITE, &old_prot)) {
        log_line("[pso_widescreen] cc-backdrop: VirtualProtect FAILED err=%lu", GetLastError());
        return;
    }
    uint8_t patch[CC_BACKDROP_SPLICE_LEN];
    patch[0] = 0xE9;
    int32_t rel_to_stub = (int32_t)((uintptr_t)&cc_backdrop_stub - (CC_BACKDROP_SPLICE_VA + 5));
    memcpy(patch + 1, &rel_to_stub, 4);
    memcpy((void *)(uintptr_t)CC_BACKDROP_SPLICE_VA, patch, CC_BACKDROP_SPLICE_LEN);
    DWORD tmp = 0;
    VirtualProtect((LPVOID)(uintptr_t)CC_BACKDROP_SPLICE_VA, CC_BACKDROP_SPLICE_LEN, old_prot, &tmp);
    FlushInstructionCache(GetCurrentProcess(),
                          (LPCVOID)(uintptr_t)CC_BACKDROP_SPLICE_VA, CC_BACKDROP_SPLICE_LEN);
    g_cc_backdrop_installed = 1;
    log_line("[pso_widescreen] cc-backdrop hex-fill armed @ 0x%08x -> stub 0x%p tramp 0x%p",
             (unsigned)CC_BACKDROP_SPLICE_VA, (void *)&cc_backdrop_stub, g_cc_backdrop_tramp);
}

// ============================================================
// CHAR-CREATE CLASS-SELECT INFO BOX — RIGID RIGHT-SHIFT SPLICE 
// ============================================================
// GOAL: slide the WHOLE right-hand class-info box (cyan 9-slice frame + 4 class
// previews + "Hunter" banner + "Enter OK" prompt + class-name labels + the
// description sub-box) right by CC_RIGHTPANE_X_FACTOR*(design_w-640), WITHOUT moving
// the LEFT class list.
//
// CHOKEPOINT (proven against the decompiled source + live draw_capture):
// RenderChallengePanelElement_004e9d8c is the universal element draw fn —
// void __cdecl RenderChallengePanelElement_004e9d8c(int, float, float *param_3,
// undefined4 *, float *)
// where param_3 (==ebx) points at the element's {X, W, ...} record. Every box element
// AND every left-list row flows through here (chain emitter 0x0082bcae <- composer
// 0x0082bb20 <- this fn's epilogue 0x004e9e3c). The element X is read at:
// 0x004e9dcb: 8B 0B        mov ecx,[ebx]        ; ecx = element X (design float)
// 0x004e9dcd: 89 4C 24 08  mov [esp+8],ecx      ; store X into the outgoing arg slot
// and W is consumed next at 0x004e9dd1: D9 43 04  fld dword [ebx+4].
//
// DISCRIMINATOR (one gate, both sides tested by the parent's two screenshots):
// - screen-id [0x00A165F0] == 12  => char-create (covers BOTH class-select AND the
// dressing-room/appearance sub-screen — both are screen-id 12; the parent verifies
// neither breaks). Off char-create => stock path, byte-identical behaviour.
// - design X >= 300.0f  => RIGHT box only. The left class list draws at design-X<=292
// (screen<=657); every right-box element is at design-X>=307 (screen>=690). Clean
// gap 292..307 -> threshold 300 splits them with margin. design-X<300 => NO offset
// (left list stays put). FALSE-POSITIVE guard: a left-list X that crept >=300 would
// wrongly shift — but the live capture shows the list capped at 292, and the box
// floor at 307, a 15px cushion either side of 300.
// - design_w > 640 (install-time) => 4:3 is a hard no-op (splice not installed).
//
// SSE, not x87: at 0x004e9dbd the engine does `fld [0xA7221C]` leaving a LIVE scale
// in st0 that the resumed code (`fld [ebx+4]; fmul st1`) depends on. Touching the x87
// stack would corrupt it, so the stub does its float compare/add in xmm0 (SSE leaves
// the FPU stack untouched). EFLAGS is provably dead at the resume point (only
// fld/fmul/fstp/mov/call follow until well past — no conditional branch reads our
// comiss flags), so clobbering EFLAGS in the stub is safe.
//
// SPLICE: 0x004e9dcb. Sig-guard the 6 stock bytes 8B 0B 89 4C 24 08; refuse on
// mismatch. Replace with E9 <rel32 -> stub> (5 bytes) + 0x90 NOP (1 byte, pads the
// 6-byte stock window). Resume VA = 0x004e9dd1. We ENTERED via JMP so esp is unchanged
// vs the original instruction stream => [esp+8] is the exact same outgoing-arg slot the
// stock `mov [esp+8],ecx` would have written. The offset rides the SAME
// CC_RIGHTPANE_X_FACTOR as the description-text bake (cc_text_early_bake) so the moved
// box and its text stay aligned by construction. One-time .text edit — the engine runs
// it per-frame; we do NOT poke per-frame (allowed category, same as the 0x004ED06D
// backdrop splice and the 0x004F4FA5 text bake).
#define CC_BOX_X_SPLICE_VA   0x004E9DCBu     // mov ecx,[ebx] (element X read)
#define CC_BOX_X_SPLICE_LEN  6               // 8B 0B 89 4C 24 08
#define CC_BOX_X_RESUME_VA   0x004E9DD1u     // fld dword [ebx+4] (first byte after the 6)

static float  g_cc_box_x_thresh = 300.0f;            // design-X gate (>= => right box)
static float  g_cc_box_x_offset = 0.0f;              // factor*(design_w-640), set at install
static float  g_cc_kx           = 1.0f;              // backdrop stretch factor = design_w/640, set at install
static float  g_cc_ky           = 1.0f;              // backdrop VERTICAL stretch = design_h/480, set at install
// VERTICAL CENTERING (LIVE draw_capture diagnosis): the 8 backdrop tiles
// are stock 640x512 (design). UNIFORM stretch kx=ky (round hexes) fills design_w
// (640*kx=1706 @hs2.0) but OVERSHOOTS height (512*ky=1365 vs design_h=960) and the
// stack is TOP-ANCHORED (top tile Y=0 pinned) -> the bright lower mosaic falls off the
// bottom and the dark upper tiles dominate the visible viewport. Center the stretched
// stack on the viewport by subtracting (512*ky - design_h)/2 from every tile Y-position.
#define CC_BACKDROP_STOCK_H 512.0f                  // backdrop stack stock design height (live-measured)
static float  g_cc_y_center     = 0.0f;             // = (CC_BACKDROP_STOCK_H*ky - design_h)/2, set at install
static void  *g_cc_box_x_resume = (void *)CC_BOX_X_RESUME_VA;   // -> 0x004e9dd1
static int    g_cc_box_x_installed = 0;

// Naked stub: entered by JMP from 0x004e9dcb (esp unchanged => esp == E, the frame after
// the fn prologue). Per the decompiled RenderChallengePanelElement_004e9d8c(int id, float
// depth, float *pos, undefined4 *scale, ...): edi = param_2 = DEPTH; ebx = param_3 = pos
// record (X at [ebx]); ebp = param_4 = scale record (scale_x at [ebp][0], normally 1.0).
// DISCRIMINATOR: the backdrop tiles are the only elements emitted with depth 0xC61B3C00
// (SetChallengePanelScale(id,0xC61B3C00,&pos) in both loops of RenderChallengePanelBackground);
// menu buttons (FACE/HAIR/CHARACTER NAME, depth != 0xC61B3C00) and the class-info box use
// other depths -- so edi==0xC61B3C00 selects ONLY the backdrop, never the CHARACTER NAME
// button (which shares an element ID with a backdrop tile -- why the earlier id-gate
// wrongly stretched it). Float work in xmm0/xmm1 (x87 stack untouched -> the live st0
// Y-scale survives). EFLAGS dead at resume. eax/edx (lea'd at 0x004e9dc3/dc7) NOT clobbered.
//
// THREE paths under screen-id 12:
// (1) BACKDROP TILE (depth 0xC61B3C00): horizontal STRETCH by kx=design_w/640 -- scale
// the X-POSITION (write X*kx to [esp+8] = param_1[0] = LEFT) AND the X-SCALE (write
// kx to [ebp][0] = scale_x => RenderUIPiece WIDTH = scale_x*x0 = kx*x0). Each tile's
// span [X, X+x0] -> kx*[X, X+x0]; the 8 native tiles spread+widen to cover design
// 0..design_w gap-free, NO duplicated columns, NO box-shift. Writing [ebp][0] is safe:
// the scale record is a per-call stack local rebuilt to {1.0,1.0} each tile, so it
// self-resets (no persistent mutation -> Challenge Mode / other panels untouched).
// (2) CLASS-INFO BOX element (id NOT backdrop, design-X >= 300): rigid right-shift by
// factor*(design_w-640) -- the original cc-box-move feature, unchanged.
// (3) else: stock (left class list / non-char-create) -- mov ecx,[ebx]; mov [esp+8],ecx.
__declspec(naked) static void cc_box_x_stub(void)
{
    __asm {
        cmp   dword ptr ds:[0x00A165F0], 12          // screen-id == char-create?
        jne   orig
        cmp   edi, 0xC61B3C00                         // param_2 depth == backdrop tile depth?
        jne   chkbox                                  // (menu buttons/box use other depths)
        // (1) backdrop tile (depth 0xC61B3C00) -> proportional X stretch (pos + width)
        // AND vertical Y stretch (height + lower-tile Y-position) so the 4:3-authored
        // hex plane (design Y 0..576) fills design_h (the black-bottom fix, .
        movss xmm0, dword ptr [ebx]                  // tile design X
        mulss xmm0, dword ptr ds:[g_cc_kx]           // X * kx  (spread the left edge)
        movss dword ptr [esp+8], xmm0                // -> param_1[0] (LEFT)
        movss xmm1, dword ptr ds:[g_cc_kx]           // kx
        movss dword ptr [ebp], xmm1                  // scale_x = kx => WIDTH = kx*x0 (widen)
        // Y-SCALE (height): [ebp+4] is scale_y (read by the resumed code at 0x004e9de1
        // fld [ebp+4]; the result lands in param_1[4]). ebp=param_4 is a per-call stack
        // local rebuilt to {1,1,1,1} each tile, so this write self-resets (same safety as
        // the [ebp] X-scale write). Height *= ky => plane bottom 576 -> 576*ky covers dh.
        movss xmm2, dword ptr [ebp+4]                // tile scale_y (normally 1.0)
        mulss xmm2, dword ptr ds:[g_cc_ky]           // scale_y *= ky => HEIGHT = ky*h0
        movss dword ptr [ebp+4], xmm2                // -> param_1[4] (HEIGHT)
        // Y-POSITION ([esp+0xC]) is computed AFTER this resume (fstp [esp+0xC] @0x004e9dd6),
        // so it is stretched by the companion cc_box_y_stub spliced at 0x004e9df2.
        jmp   dword ptr ds:[g_cc_box_x_resume]       // -> 0x004e9dd1
    chkbox:
        // (2) class-info box element: rigid right-shift if design-X >= 300.
        // GATED to char-create sub-page 0 (class-select) only. The appearance /
        // dressing-room sub-pages (0x00A7223C != 0) draw the OK/Back, Character-Name
        // and menu-cursor elements, which already land via their OWN .data source
        // anchors (patch_dressingroom_layout, Trinity MOD_X_R). Shifting them here too
        // double-moved them off-screen. Box striding is retained on the class-select page.
        cmp   dword ptr ds:[0x00A7223C], 0           // char-create sub-page == 0 (class-select)?
        jne   orig                                    // appearance/sub-pages -> ride own source
        movss xmm0, dword ptr [ebx]                  // element X
        comiss xmm0, dword ptr ds:[g_cc_box_x_thresh] // X >= 300 ?
        jb    orig                                    // design-X < 300 (left list) -> no shift
        addss xmm0, dword ptr ds:[g_cc_box_x_offset] // + factor*span
        movss dword ptr [esp+8], xmm0                // store shifted X into the arg slot
        jmp   dword ptr ds:[g_cc_box_x_resume]       // -> 0x004e9dd1
    orig:
        mov   ecx, dword ptr [ebx]                   // stock: ecx = element X
        mov   dword ptr [esp+8], ecx                 // stock: store X
        jmp   dword ptr ds:[g_cc_box_x_resume]       // -> 0x004e9dd1
    }
}

// Install the box-X splice. Sig-guarded (refuses unless the 6 stock bytes match) +
// idempotent (installed flag). 4:3 no-op (design_w<=640 => not installed). Sets
// g_cc_box_x_offset = CC_RIGHTPANE_X_FACTOR*(design_w-640) at install (char-create
// design_w is always the front-end native 853.33 — HudScale doesn't apply front-end —
// so the install-time offset is correct and never needs a per-frame recompute).
static uint8_t g_cc_box_x_orig[CC_BOX_X_SPLICE_LEN] = {0};
static void cc_box_x_install(void)
{
    if (g_cc_box_x_installed) return;
    const float dw = hs_peek_f32(CC_DESIGNW_VA);
    if (dw <= 640.0f) {                               // 4:3 hard no-op (never install)
        log_line("[pso_widescreen] cc-box-x: SKIP (design_w=%.1f <= 640, 4:3 no-op)", dw);
        return;
    }
    // Stock bytes at 0x004E9DCB: 8B 0B 89 4C 24 08.
    static const uint8_t kExpected[CC_BOX_X_SPLICE_LEN] = {
        0x8B, 0x0B, 0x89, 0x4C, 0x24, 0x08
    };
    const uint8_t *p = (const uint8_t *)(uintptr_t)CC_BOX_X_SPLICE_VA;
    for (int i = 0; i < CC_BOX_X_SPLICE_LEN; ++i) {
        if (p[i] != kExpected[i]) {
            log_line("[pso_widescreen] cc-box-x: splice sig mismatch byte %d: "
                     "got 0x%02x expected 0x%02x — refusing to hook", i, p[i], kExpected[i]);
            return;
        }
        g_cc_box_x_orig[i] = p[i];
    }
    // Install-time offset = scale-invariant stride (factor*(design_w/HudScale-640)).
    // SAME helper as cc_text_early_bake so box + description text stay locked together,
    // and the class-list<->pane gap is identical at 1.0/1.5/2.0 (owner request .
    g_cc_box_x_offset = cc_rightpane_offset(dw);
    // Backdrop stretch factor kx = design_w/640 (1.333 @ front-end 853.33). Front-end
    // design_w is fixed (HudScale doesn't apply to the front-end), so install-time is
    // correct. dw>640 is guaranteed here (4:3 returns above) so kx>1.0; at 4:3 the splice
    // is never installed => stock 4:3 backdrop (natural no-op).
    g_cc_kx = dw / 640.0f;
    // Backdrop VERTICAL stretch factor ky = kx (UNIFORM scaling). LIVE TEST proved
    // ky = design_h/480 (=2.0 @hs2.0) is too small vs kx = design_w/640 (=2.667) — the
    // hex cells render wider-than-tall (anisotropic). Poking ky live to 2.667 (=kx) made
    // them uniform/round. So reuse the already-computed kx. dw>640 guaranteed here (4:3
    // returns above) so kx>1.0 => the cc_box_y_install ky>1.0 guard still holds.
    const float dh = hs_peek_f32(CC_DESIGNH_VA);     // design_h (=960 @hs2.0); feeds the centering offset
    g_cc_ky = g_cc_kx;
    // Center the uniformly-stretched (round-hex) backdrop on the viewport. The stack is
    // stock 512 tall; *ky overshoots design_h, and the stub pins the top tile at Y=0, so
    // subtract half the overshoot from every tile Y so the bright lower mosaic isn't shoved
    // off-screen. Clamp >=0 (never pull a non-overshooting backdrop up off the top edge).
    g_cc_y_center = (CC_BACKDROP_STOCK_H * g_cc_ky - dh) * 0.5f;
    if (g_cc_y_center < 0.0f) g_cc_y_center = 0.0f;

    DWORD old_prot = 0;
    if (!VirtualProtect((LPVOID)(uintptr_t)CC_BOX_X_SPLICE_VA, CC_BOX_X_SPLICE_LEN,
                        PAGE_EXECUTE_READWRITE, &old_prot)) {
        log_line("[pso_widescreen] cc-box-x: VirtualProtect FAILED err=%lu", GetLastError());
        return;
    }
    uint8_t patch[CC_BOX_X_SPLICE_LEN];
    patch[0] = 0xE9;                                  // jmp rel32 -> stub
    int32_t rel_to_stub = (int32_t)((uintptr_t)&cc_box_x_stub - (CC_BOX_X_SPLICE_VA + 5));
    memcpy(patch + 1, &rel_to_stub, 4);
    patch[5] = 0x90;                                  // NOP pads the 6th stock byte
    memcpy((void *)(uintptr_t)CC_BOX_X_SPLICE_VA, patch, CC_BOX_X_SPLICE_LEN);
    DWORD tmp = 0;
    VirtualProtect((LPVOID)(uintptr_t)CC_BOX_X_SPLICE_VA, CC_BOX_X_SPLICE_LEN, old_prot, &tmp);
    FlushInstructionCache(GetCurrentProcess(),
                          (LPCVOID)(uintptr_t)CC_BOX_X_SPLICE_VA, CC_BOX_X_SPLICE_LEN);
    g_cc_box_x_installed = 1;
    log_line("[pso_widescreen] cc-box-x + backdrop-stretch armed @ 0x%08x -> stub 0x%p "
             "(box_offset=%.1f thresh=%.1f kx=%.3f ky=%.3f dw=%.1f dh=%.1f factor=%.2f)",
             (unsigned)CC_BOX_X_SPLICE_VA, (void *)&cc_box_x_stub,
             g_cc_box_x_offset, g_cc_box_x_thresh, (double)g_cc_kx, (double)g_cc_ky,
             dw, dh, (double)CC_RIGHTPANE_X_FACTOR);
}

// ============================================================
// CHAR-CREATE HEX BACKDROP — VERTICAL (Y-POSITION) COMPANION SPLICE 
// ============================================================
// THE BLACK-BOTTOM FIX (companion to the cc_box_x_stub Y-SCALE write above).
// In RenderChallengePanelElement_004e9d8c the element Y-POSITION output slot
// ([esp+0xC] = param_1[1] consumed by RenderUIPiece_0082b6bc as vy = pos + tile*scale_y)
// is COMPUTED at 0x004e9dd6 (fstp [esp+0xC]) — AFTER the cc_box_x_stub resume point
// (0x004e9dd1), so the X stub cannot touch it. The 4:3-authored backdrop tiles sit at
// design Y {0, 144, 288, 432} (top edges), bottom-most plane reaching design Y 576 of a
// design_h that is 960 @hs2.0 -> the lower ~40% is black. We multiply EACH backdrop
// tile's Y-position by ky=design_h/480 so the abutting stack spreads from the shared
// origin (top tile Y=0 stays put) to fill design_h: {0,144,288,432}*ky and bottoms
// 576*ky >= design_h. Together with the height *= ky in cc_box_x_stub this fills gap-free.
//
// SPLICE: 0x004e9df2 (stock `E8 8D 00 00 00` = call 0x004E9E84 GetChallengePanelElementPointer).
// The Y-pos slot is live AND addressable here: 0x004e9df2 precedes the `push esi`
// @0x004e9e13, so esp is still at store-level => [esp+0xC] is the SAME Y slot fstp wrote.
// edi (param_2 DEPTH) is NOT clobbered between 0x004e9dcb and 0x004e9df2 (only ebx is
// reloaded @0x004e9dda) => the edi==0xC61B3C00 backdrop discriminator is still valid.
// The re-issued call reads edx; the stub touches only xmm2 + [esp+0xC] (no GP reg), so
// edx/eax/ecx/ebx/edi all survive into the call. EFLAGS dead at resume (next is
// `mov eax,[0xAF0370]`, no flag read).
//
// GATE (identical to the X backdrop branch): screen-id [0x00A165F0]==12 (char-create)
// AND edi==0xC61B3C00 (backdrop tile depth). Off-gate => byte-identical: re-issue the
// call, resume. 4:3 => cc_box_x_install returns before this installs (install no-op).
// Sig-guarded (refuses unless the stock E8-call bytes match) + idempotent (installed flag);
// the trampoline re-encodes the relative E8 call at the tramp address (cc_backdrop_install
// idiom, since a byte-copied E8 would mis-target).
#define CC_BOX_Y_SPLICE_VA    0x004E9DF2u     // call 0x004E9E84 (after the Y-pos fstp, before push esi)
#define CC_BOX_Y_SPLICE_LEN   5               // E8 8D 00 00 00
#define CC_BOX_Y_RESUME_VA    0x004E9DF7u     // first byte after the spliced call
#define CC_BOX_Y_CALL_TGT_VA  0x004E9E84u     // GetChallengePanelElementPointer (re-issued)

static uint8_t  g_cc_box_y_orig[CC_BOX_Y_SPLICE_LEN] = {0};
static void    *g_cc_box_y_tramp = NULL;      // RWX: re-issue call + E9 resume
static int      g_cc_box_y_installed = 0;
static void    *g_cc_box_y_resume = (void *)CC_BOX_Y_RESUME_VA;   // -> 0x004e9df7

// Naked stub: entered by JMP from 0x004e9df2 (esp unchanged). Gate on screen-id 12 +
// edi==backdrop-depth; on gate scale the Y-position slot [esp+0xC] by ky (xmm only ->
// GP regs + x87 stack untouched). Then jmp the trampoline (re-issues `call 0x004E9E84`
// then E9 -> 0x004e9df7). Off-gate skips the math and falls through to the same tramp.
__declspec(naked) static void cc_box_y_stub(void)
{
    __asm {
        cmp   dword ptr ds:[0x00A165F0], 12          // screen-id == char-create?
        jne   resume
        cmp   edi, 0xC61B3C00                         // param_2 depth == backdrop tile depth?
        jne   resume                                  // (box/list/buttons use other depths)
        movss xmm2, dword ptr [esp+0x0C]              // backdrop tile Y-position
        mulss xmm2, dword ptr ds:[g_cc_ky]            // Y *= ky => spread down (uniform, round hexes)
        subss xmm2, dword ptr ds:[g_cc_y_center]      // - (512*ky - design_h)/2 => CENTER on viewport
        movss dword ptr [esp+0x0C], xmm2              // -> param_1[1] (TOP)
    resume:
        jmp   dword ptr ds:[g_cc_box_y_tramp]         // re-issue call 0x004E9E84, then -> 0x004e9df7
    }
}

// Install the Y-position companion splice. Trampoline built manually (the spliced
// instruction is a relative E8 call that must be RE-ENCODED at the tramp address, not
// byte-copied) — same idiom as the retired cc_backdrop_install. Sig-guarded + idempotent.
// Call ONLY after cc_box_x_install (which sets g_cc_ky and is the 4:3 gate); this fn
// additionally no-ops when g_cc_ky<=1.0 so a 4:3 / mis-ordered call never patches.
static void cc_box_y_install(void)
{
    if (g_cc_box_y_installed) return;
    if (g_cc_ky <= 1.0f) {                            // 4:3 / not-yet-computed => no-op
        log_line("[pso_widescreen] cc-box-y: SKIP (ky=%.3f <= 1.0, 4:3 no-op)", (double)g_cc_ky);
        return;
    }
    // Stock bytes at 0x004E9DF2: E8 8D 00 00 00 = call 0x004E9E84 (re-issued by the tramp).
    static const uint8_t kExpected[CC_BOX_Y_SPLICE_LEN] = {
        0xE8, 0x8D, 0x00, 0x00, 0x00
    };
    const uint8_t *p = (const uint8_t *)(uintptr_t)CC_BOX_Y_SPLICE_VA;
    for (int i = 0; i < CC_BOX_Y_SPLICE_LEN; ++i) {
        if (p[i] != kExpected[i]) {
            log_line("[pso_widescreen] cc-box-y: splice sig mismatch byte %d: "
                     "got 0x%02x expected 0x%02x — refusing to hook", i, p[i], kExpected[i]);
            return;
        }
        g_cc_box_y_orig[i] = p[i];
    }
    // Trampoline (RWX): re-issue the original call (correctly relocated) then E9 back.
    // [0]  E8 <rel32 -> 0x004E9E84>
    // [5]  E9 <rel32 -> 0x004E9DF7>
    void *page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
                              PAGE_EXECUTE_READWRITE);
    if (!page) {
        log_line("[pso_widescreen] cc-box-y: trampoline alloc FAILED err=%lu", GetLastError());
        return;
    }
    uint8_t *t = (uint8_t *)page;
    t[0] = 0xE8;
    int32_t rel_call = (int32_t)(CC_BOX_Y_CALL_TGT_VA - ((uintptr_t)t + 5));
    memcpy(t + 1, &rel_call, 4);
    t[5] = 0xE9;
    int32_t rel_resume = (int32_t)(CC_BOX_Y_RESUME_VA - ((uintptr_t)t + 10));
    memcpy(t + 6, &rel_resume, 4);
    g_cc_box_y_tramp = page;

    // Write the E9 splice at 0x004E9DF2 (exactly 5 bytes == the original call's length,
    // so no NOP padding is needed).
    DWORD old_prot = 0;
    if (!VirtualProtect((LPVOID)(uintptr_t)CC_BOX_Y_SPLICE_VA, CC_BOX_Y_SPLICE_LEN,
                        PAGE_EXECUTE_READWRITE, &old_prot)) {
        log_line("[pso_widescreen] cc-box-y: VirtualProtect FAILED err=%lu", GetLastError());
        return;
    }
    uint8_t patch[CC_BOX_Y_SPLICE_LEN];
    patch[0] = 0xE9;                                  // jmp rel32 -> stub
    int32_t rel_to_stub = (int32_t)((uintptr_t)&cc_box_y_stub - (CC_BOX_Y_SPLICE_VA + 5));
    memcpy(patch + 1, &rel_to_stub, 4);
    memcpy((void *)(uintptr_t)CC_BOX_Y_SPLICE_VA, patch, CC_BOX_Y_SPLICE_LEN);
    DWORD tmp = 0;
    VirtualProtect((LPVOID)(uintptr_t)CC_BOX_Y_SPLICE_VA, CC_BOX_Y_SPLICE_LEN, old_prot, &tmp);
    FlushInstructionCache(GetCurrentProcess(),
                          (LPCVOID)(uintptr_t)CC_BOX_Y_SPLICE_VA, CC_BOX_Y_SPLICE_LEN);
    g_cc_box_y_installed = 1;
    log_line("[pso_widescreen] cc-box-y (hex bottom-fill) armed @ 0x%08x -> stub 0x%p "
             "tramp 0x%p (ky=%.3f)",
             (unsigned)CC_BOX_Y_SPLICE_VA, (void *)&cc_box_y_stub, g_cc_box_y_tramp,
             (double)g_cc_ky);
}

// REFACTOR: RETIRED with the Sodaboy path —
// HUD_VANILLA / REF_W / REF_H (dead Sodaboy HUD-rect reference),
// load_psobb_image / read_stock_dword / read_stock_float (stock-image
// reader for the additive LOOP4/5/6 absolute conversion),
// canvas_scale_t / compute_canvas_scale (Sodaboy LOOP scale derivation),
// and the entire Phase-3 sprite-primitive inline hook on 0x0082B440
// (splash_quad_rewrite_c / stub_82b440_phase3 / install_phase3_hook + its
// globals). The MinHook include moved to the top of the file. anzz1 owns
// the coordinate bake; the char-create composer MinHooks remain below.

// CHAR-SELECT 3D MODEL pan — shim on the char-select scene's call to
// ResetNPCCamera at 0x004138A3. Runs the ORIGINAL reset (0x0081E1F4), then pans
// the ACTIVE preview camera +g_model_dx in world X so the 3D character shifts
// LEFT (centered between the CHARACTERS panel and the info box). Char-select-
// EXCLUSIVE: only this ONE call site is redirected — other menu preview scenes
// that share ResetNPCCamera are untouched. Rides the engine's existing per-frame
// camera reset (no new per-frame code of ours). World-space offset = orthogonal
// to UI / HUDScale.
static float g_model_dx = 8.0f;
static __declspec(naked) void rb_charsel_camera(void)
{
    __asm {
        push ecx                                       /* preserve this (thiscall) */
        mov  eax, 0x0081E1F4                           /* ResetNPCCamera           */
        call eax
        pop  ecx
        mov  eax, dword ptr ds:[0x00A489F4]            /* active camera index      */
        mov  eax, dword ptr [eax*4 + 0x00A48A00]       /* camera object ptr        */
        test eax, eax
        je   done
        fld  dword ptr [eax+0x3C]                      /* target.X += g_model_dx   */
        fadd dword ptr ds:[g_model_dx]
        fstp dword ptr [eax+0x3C]
        fld  dword ptr [eax+0x48]                      /* source.X += g_model_dx   */
        fadd dword ptr ds:[g_model_dx]
        fstp dword ptr [eax+0x48]
    done:
        ret
    }
}

// ============================================================
// EFFECT-DEANCHOR (in-game 16:9 wide-fill, 
// ============================================================
// The in-game 2D affine (0x00ACC0E8/EC) is forced WIDE (2.25 @hs1.0 / 1.5 @hs1.5)
// by the F_VPRESET source bakes in apply_startup_bakes so the HUD fills 16:9. But
// that affine is GLOBAL — it is multiplied into 2D verts inside the shared submit
// helper 0x0082B158 (-> emitter 0x00836D04), which also carries SCREEN-SPACE combat
// effects (photon blasts / billboards). Three effect-draw CALL sites route those
// screen-space effects through 0x0082B158; left alone they would inherit the wide
// affine and STRETCH. We bracket each: save the two affine floats -> write 1.0f ->
// run the real submit -> restore. Net: HUD wide, effects native.
//
// The strip value is literally 1.0f regardless of HudScale (we render those effects
// in native 2D space, then restore whatever wide value the HUD uses).
//
// These three CALL redirects are CODE-FLOW detours and are WIPED by the FE->IG
// transition (unlike the immediate .text affine bakes which survive). So they are
// (re)installed from reassert_ingame_hooks(), called every in-game worker tick —
// including the hud_scale==1.0 wide-fill lever (the affine is wide at 1.0 too).
static const uint32_t kStripSites[3] = { 0x004A9C0Cu, 0x004A9D28u, 0x005474D4u }; // all `call 0x0082B158`

// Static save slots for the deanchor wrap. Render-thread-only and NON-RECURSIVE
// (the screen-space effect submit does not re-enter one of the 3 strip sites), so
// a static save area is safe and keeps the args stack pristine for forwarding.
static volatile uint32_t g_deanchor_save_x = 0;   // saved 0x00ACC0E8 (raw bits)
static volatile uint32_t g_deanchor_save_y = 0;   // saved 0x00ACC0EC (raw bits)
static volatile uint32_t g_deanchor_ret    = 0;   // stashed real caller return addr

// rb_deanchor_shim — transparent cdecl wrapper around 0x0082B158.
// Mechanism (no arg-count knowledge, forwards args UNTOUCHED, EAX return flows
// through, non-volatiles untouched): save+override the affine, then SWAP the
// caller's return address for our after-stub and JMP (not CALL) into the real
// function so the args stay exactly where the caller laid them. The function
// returns into rb_deanchor_after, where we restore the affine and JMP to the real
// caller return (esp still at the args -> the cdecl caller cleans them, as it would
// have after the original call). flags/scratch regs: cdecl callers don't rely on
// EFLAGS/ECX/EDX across a call; EAX (return value) is preserved across the restore.
__declspec(naked) static void rb_deanchor_shim(void)
{
    __asm {
        // entry: [esp]=caller_ret, [esp+4..]=cdecl args
        mov  eax, dword ptr ds:[0x00ACC0E8]            // save SCALE_X
        mov  dword ptr ds:[g_deanchor_save_x], eax
        mov  eax, dword ptr ds:[0x00ACC0EC]            // save SCALE_Y
        mov  dword ptr ds:[g_deanchor_save_y], eax
        mov  dword ptr ds:[0x00ACC0E8], 0x3F800000     // SCALE_X <- 1.0f
        mov  dword ptr ds:[0x00ACC0EC], 0x3F800000     // SCALE_Y <- 1.0f
        mov  eax, dword ptr [esp]                       // eax = real caller return addr
        mov  dword ptr ds:[g_deanchor_ret], eax         // stash it
        mov  eax, offset rb_deanchor_after
        mov  dword ptr [esp], eax                        // redirect return to our after-stub
        mov  eax, 0x0082B158
        jmp  eax                                         // tail-enter real submit; args intact, [esp]=after-stub
    rb_deanchor_after:
        // real submit returned: cdecl args still on stack, eax = return value
        push eax                                        // preserve return value
        mov  eax, dword ptr ds:[g_deanchor_save_x]
        mov  dword ptr ds:[0x00ACC0E8], eax             // restore SCALE_X
        mov  eax, dword ptr ds:[g_deanchor_save_y]
        mov  dword ptr ds:[0x00ACC0EC], eax             // restore SCALE_Y
        pop  eax                                        // restore return value
        jmp  dword ptr ds:[g_deanchor_ret]              // -> real caller (esp at args)
    }
}

// ============================================================
// KILL-SCREEN HEXAGON-LAYER ASPECT-FIT PRE-PASS (Ephinea FUN_52da6e50 port,
// 
// ============================================================
// Site: 0x0067C4ED `call 0x0082B558` inside the boss kill-screen hexagon-layer
// fullscreen 2D-overlay render. Ephinea installs a 5-byte detour of the CALL
// (FUN_52dc3240(FUN_52da6e50, 0x67c4ed, 5)) that runs an aspect-fit / recenter
// PRE-PASS over the packed (x,y) f32 vertex array immediately before the submit
// helper 0x0082B558, so the fullscreen layer fills the (taller/wider) physical
// viewport height-fit + centered instead of being left-anchored X-stretched.
//
// Descriptor (built at 0x67C4B2.., arg0 = ecx = lea[esp+0x1C4]):
// [+0x00] vertPtr   (lea[esp+0x44])      — packed f32 pairs (x,y), stride 8
// [+0x04] colorPtr  (lea[esp+0x144])
// [+0x08] 0
// [+0x0C] vertCount (ebx)
// The vert array is FRESHLY BUILT per call into a stack buffer (the 0x67C42B
// loop writes [esp+0x44..]), so the in-place `*=k; +=off` does NOT accumulate
// across frames (no idempotency hazard).
//
// MATH (faithful to FUN_52da6e50, mapped to OUR globals; renderW/renderH = the
// DESIGN dims design_w[0x0098A4B8] / design_h[0x0098A4B4] = 853.33*hud_scale /
// 480*hud_scale in-game, == Ephinea _DAT_5318deec / _DAT_5318d124). These are
// DESIGN dims, NOT the physical backbuffer (g_scale.render_w/h) — the earlier
// physical-dims build double-scaled (k=1080/480=2.25 then ANOTHER 2.25 from the
// affine = ~5x overscale). With design dims the 16:9 else-branch gives k=design_h/
// 480 = hud_scale and off = (design_w-640*k)/2; at design 640x480 -> k=1,off=0
// (4:3 identity), and we also install-gate on game_aspect>4:3.
//
// COMPOSITION: this pre-pass writes DESIGN-space coords (height-fit + centered);
// 0x0082B558 THEN multiplies by the global affine 0x00ACC0E8 (=render_w/design_w
// = 2.25/hud_scale). k=hud_scale and affine=2.25/hud_scale CANCEL -> the on-screen
// result is hud_scale-INVARIANT (full-height, centered, margins 240px @1920). That
// is EXACTLY Ephinea's compose (its submit applies the same affine after the
// pre-pass). Do NOT also deanchor/strip the affine at this site (Ephinea lets it
// apply); this is NOT a double-bake.
//
// INSTALL: boot .text CALL-site redirect (one-shot, persistent) — this is a
// render routine NOT in the kStripSites effect-deanchor set, so it is NOT wiped
// by the FE->IG transition (it behaves like the persistent .text affine bakes).
// Installed once in apply_static_patches via redirect_call.
//
// C helper: in-place aspect-fit the packed (x,y) f32 vert array. renderW/renderH
// are the LIVE DESIGN dims (design_w 0x0098A4B8 / design_h 0x0098A4B4), read
// PER-CALL via hs_peek_f32 so they track the FE<->IG flip + hud_scale (a boot-
// captured g_scale would be both the wrong magnitude AND stale). Faithful port of
// FUN_52da6e50: pillarbox (renderW/renderH < 4/3) scales by width + centers
// vertically; else (16:9-wide, our case) scales by height + centers horizontally.
static void __cdecl ks_hexfit_transform(float *vptr, int n)
{
    if (!vptr || n <= 0) return;
    const float renderW = hs_peek_f32(0x0098A4B8u);         // design_w (853.33*hud_scale in-game)
    const float renderH = hs_peek_f32(0x0098A4B4u);         // design_h (480*hud_scale in-game)
    if (!(renderW > 1.0f) || !(renderH > 1.0f)) return;     // unset/front-end -> bail (no transform)
    const int   pillarbox = (renderW / renderH) < (4.0f / 3.0f);
    float k, off;
    if (pillarbox) { k = renderW / 640.0f; off = renderH / 2.0f - (480.0f * k) / 2.0f; }
    else           { k = renderH / 480.0f; off = renderW / 2.0f - (640.0f * k) / 2.0f; }
    for (int i = 0; i < n; i++) {
        vptr[2 * i]     *= k;
        vptr[2 * i + 1] *= k;
        if (pillarbox) vptr[2 * i + 1] += off;
        else           vptr[2 * i]     += off;
    }
}

// ks_hexfit_shim — naked CALL-site redirect for 0x0067C4ED. On entry the cdecl
// args are already on the stack: [esp]=caller_ret(0x67C4F2), [esp+4]=descPtr,
// [esp+8]=count, [esp+0xC]=0xC2EA0000, [esp+0x10]=0x42. We read descPtr=[esp+4],
// pull vptr=[descPtr+0] and n=[descPtr+0xC], run the transform, then tail-jmp to
// 0x0082B558 with ALL args UNTOUCHED (the cdecl caller at 0x67C4F2 does its
// `add esp,0x1E4` cleanup as before). No after-stub: nothing to restore.
__declspec(naked) static void ks_hexfit_shim(void)
{
    __asm {
        // entry: [esp]=caller_ret, [esp+4]=descPtr, [esp+8]=count, ...
        pushad                                          // preserve ALL caller regs
        mov  eax, dword ptr [esp + 0x20 + 4]            // descPtr (pushad pushed 0x20 bytes)
        mov  edx, dword ptr [eax + 0x0C]                // n = desc[+0x0C] (vertCount)
        mov  eax, dword ptr [eax + 0x00]                // vptr = desc[+0x00]
        push edx                                        // arg2: n
        push eax                                        // arg1: vptr
        call ks_hexfit_transform                        // cdecl
        add  esp, 8
        popad                                           // restore caller regs
        mov  eax, 0x0082B558
        jmp  eax                                        // tail-enter real submit; args intact
    }
}

// ks_hexfit_install — boot .text CALL-site redirect of 0x0067C4ED to our shim.
// One-shot persistent (NOT a kStripSites/effect-deanchor detour -> the FE->IG
// transition does not wipe it). Gated by caller on g_cfg.enabled && aspect>4:3.
static int ks_hexfit_install(void)
{
    if (!redirect_call(0x0067C4EDu, 0x0082B558u, (void *)&ks_hexfit_shim)) {
        log_line("[pso_widescreen] killscreen hexfit: SKIP (redirect failed)");
        return 0;
    }
    log_line("[pso_widescreen] killscreen hexfit: aspect-fit pre-pass installed (render %dx%d)",
             g_scale.render_w, g_scale.render_h);
    return 1;
}

// ============================================================
// IN-GAME LIST-WINDOW BOTTOM-ANCHOR (Ephinea FUN_52d8cb90 port, 
// ============================================================
// Site: 0x0073FF92 `call 0x00737F80` — the [0xA48A3C]==1 (single-player) arm of
// ListWindowObject_Create_0073ff34. The case pushes 0x972DEC
// (item_bank_and_quest_list_dimensions = design {385.0, 184.0}) then [ebp+0xc]
// (the id), then calls ListWindowObject_InitBase_00737f80(xy*, id), which stores
// xy[0]->[0x9F3130] (base X), xy[1]->[0x9F3134] (base Y), id->[0x9F3128]. That
// {X,Y} is the CONSTRUCTION-TIME base position of the in-game item-bank / quest-
// counter / shop LIST WINDOW.
//
// Ephinea (FUN_52d8cb90) replaces the orig xy with a scratch {liveX, Y'} where
// Y' = (renderH-480)+Yoffset, bottom-anchoring the window to the taller widescreen
// viewport, plus a live-screen-bounds clamp and X=*liveX substitution. Ephinea's
// renderH (_DAT_5318d124 = 480*scale) is the DESIGN height = our design_h
// [0x0098A4B4] (480 native / 720 @hs1.5), NOT physical render_h.
//
// FAITHFUL-EFFECT PORT (not byte-faithful): we port the DOMINANT effect = the
// base-Y bottom anchor `Y = orig_xy[1] + (design_h - 480.0f)` and KEEP X =
// orig_xy[0]. Ephinea's clamp branch and X=*liveX substitution dereference DLL-
// private live-screen-rect pointers (Yoffset/PAD/p108/p10c/p114) that are NOT
// resolvable to engine VAs, so we deliberately drop them (the clamp only mattered
// at extreme aspect ratios to keep the window on-screen). Gate: design_h>480 (==
// hs>1.0); at design_h==480 the term is 0 -> byte-identical no-op. We do NOT gate
// on Ephinea's private flag [0x00A95EDC] (we never set it).
//
// COMPOSITION: this sets the window's DESIGN-space base Y before the global 2D
// affine 0x00ACC0E8/EC scales it -> the (design_h-480) term is in design units
// and the affine maps it to screen correctly. It self-scales via design_h (which
// already encodes hud_scale), so it is correct at all hud_scales.
//
// INSTALL: boot .text redirect of `call 0x737f80` @0x0073FF92 to our naked stub
// (stub tail-jmps to 0x00737F80). RE-ASSERT REQUIRED: ListWindowObject_Create is
// a CODE-FLOW detour reachable both front-end (game-list) and in-game (bank/shop/
// quest); the FE->IG transition WIPES .text code-flow hooks, so we re-install it
// every in-game worker tick (reassert_ingame_hooks) if reverted to stock.
// Ephinea hooks ONLY the ==1 arm; we do the same (do NOT touch the ==2 / ==3-4
// arms at 0x0073FF73 / 0x0073FFF9) for a faithful single-bake port.
static volatile uint32_t g_lwy_scratch[2];   // {X, Y'} substituted vec2f arg

// lw_yanchor_shim — naked cdecl wrapper of ListWindowObject_InitBase (0x00737F80)
// at the ==1-arm call 0x0073FF92. On entry (caller pushed `push [ebp+0xc]` then
// `push 0x972DEC`): [esp]=caller_ret(0x73FF97), [esp+4]=xy* (=0x972DEC), [esp+8]=id.
// We compute scratch.x = xy[0]; scratch.y = xy[1] + (design_h - 480); rewrite the
// on-stack xy arg to &g_lwy_scratch, then tail-jmp to 0x00737F80 (args otherwise
// intact -> the cdecl caller's `add esp,8` cleans up as before). No after-stub.
static void __cdecl lw_yanchor_compute(const float *xy)
{
    // LIVE-VERIFIED vs a running Ephinea (memory:
    // listwindow-yanchor-ephinea-verified-offby150). Ephinea FUN_52d8cb90 computes
    // Y = design_h - C1 + C2,  C1=480, C2=334  =>  Y = design_h - 146
    // using its OWN .rdata re-base 334, NOT the stock ctor base 184. The first port
    // used base 184 (=stock 0x972DEC Y) -> design_h-296 = +150px too HIGH. Fixed.
    // NB Ephinea re-bases at 4:3 too (design_h=480 -> Y=334, not stock 184).
    float dh = hs_peek_f32(0x0098A4B4u);                // design_h: 480 / 720@hs1.5 / 960@hs2.0
    float newy = dh - 480.0f + 334.0f;                 // Ephinea: dh - 146 (C1=480, C2=334)
    g_lwy_scratch[1] = *(volatile uint32_t *)&newy;

    // X: KEEP STOCK 385. The earlier "Ephinea live viewport-edge X = *(float*)0x00973CEC"
    // port was MIS-MAPPED: disasm (memory listwindow-yanchor-ephinea-verified-offby150)
    // shows 0x00973CEC is entry +0x2C of a 24-slot CODE-ADDRESS table @0x00973CC0 (all
    // 0x0075xxxx), BYTE-IDENTICAL in Ephinea AND ours at the front-end — NOT a coordinate.
    // Derefing it yields a ~1e-38 denormal -> X~=0 -> the bank/shop window snaps far-left
    // (a regression vs stock 385), and the old `lx>-3000 && lx<5000` guard does NOT reject
    // a denormal. Ephinea DOES track the widescreen left edge (the X-drop is material) but
    // the global was wrong; finding the TRUE viewport-left global needs a live in-game
    // bank/shop read (currently blocked: no PSO server). Until then, stock 385 is correct-
    // enough and never regresses. The Y bottom-anchor above is unaffected + correct.
    g_lwy_scratch[0] = *(const volatile uint32_t *)&xy[0];   // X = stock design 385 (safe)
}

__declspec(naked) static void lw_yanchor_shim(void)
{
    __asm {
        pushad                                          // preserve ALL caller regs
        mov  eax, dword ptr [esp + 0x20 + 4]            // xy* (pushad pushed 0x20 bytes)
        push eax                                        // arg: const float* xy
        call lw_yanchor_compute                         // fills g_lwy_scratch
        add  esp, 4
        popad
        // rewrite the on-stack xy arg ([esp+4]) to point at our scratch pair.
        mov  dword ptr [esp + 4], offset g_lwy_scratch
        mov  eax, 0x00737F80
        jmp  eax                                        // tail-enter InitBase; id arg intact
    }
}

// lw_yanchor_redirect — install/re-install the call-redirect at 0x0073FF92.
// Idempotent + steady-state quiet: only attempt the redirect when the site is
// STOCK (`call 0x00737F80`); a site already pointing at our shim is silently
// skipped (mirrors reassert_ingame_hooks' deanchor pattern). Returns 1 iff we
// (re)installed this call.
static int lw_yanchor_redirect(void)
{
    const uint32_t site = 0x0073FF92u;
    uint8_t *p = (uint8_t *)(uintptr_t)site;
    __try {
        if (p[0] != 0xE8) return 0;                                  // not a CALL -> leave
        uint32_t cur = (uint32_t)(site + 5 + *(int32_t *)(p + 1));
        if (cur != 0x00737F80u) return 0;                            // already ours / foreign -> skip
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return redirect_call(site, 0x00737F80u, (void *)&lw_yanchor_shim);
}

// ====================================================================
// char-select 1.0-PIN REVERTED — the native-frame constants
// HS_FE_DESIGN_W/H_NATIVE were only used to bake the .data anchors below in a
// FIXED 853 frame for the pin. With the pin gone, char-select SCALES with
// HudScale (Ephinea-style), so the anchors below stride in the LIVE hud-scaled
// frame (s->A / s->C). The macros are removed (no remaining users).


// ALL per-frame char-create render-thread machinery has been
// REMOVED — the sub_0082BB74 composer hook (cc_quad_fix_c / cc_okbtn_install /
// cc_classify_quad), the sub_0070D53C node logger, the sub_0070DEE4 reflow
// observer, the sub_0082B440 info-text MOVE hook, the sub_0082B6BC panel-bake
// probe, patch_charcreate_okcancel_anchors(), and the
// cc_is_charcreate()/cc_scene_active()/cc_root_vtable_is() gate helpers are all
// gone. Char-create is owned entirely by the static apply_startup_bakes() pass
// (.text imm32 + .rdata float rewrites, value-guarded) plus stock geometry; the
// runtime-only class-select content rows are intentionally not strided.

// Char-select 3D MODEL pan — SEPARATE from patch_charselect_layout so it survives
// PatchCharSelect=0 (the 2D-UI patch overstretches at HUDScale 1.0 and is gated
// off; the model pan is HUDScale-independent world-space camera offset). Redirects
// the scene's ResetNPCCamera call (the per-frame menu-camera reset) to our shim.
static int patch_charselect_model_pan(void)
{
    if (!redirect_call(0x004138A3u, 0x0081E1F4u, (void *)&rb_charsel_camera)) {
        log_line("[pso_widescreen] charselect: model-pan SKIP (redirect failed)");
        return 0;
    }
    log_line("[pso_widescreen] charselect: model pan +%.1f world-X installed", g_model_dx);
    return 1;
}

// REFACTOR: apply_sodaboy_patches (the entire Sodaboy LOOP1-6 +
// Region A1/A2/G coordinate bake, ~800 lines, incl. the inline patch_res_table
// / patch_validator_nop / patch_extra_ui Region-H / patch_title_real /
// patch_title_art blocks) is RETIRED. anzz1 is the sole static coordinate
// source of truth (see apply_static_patches -> apply_anzz1_widescreen).

// Photon-blast lens flare position pin .
// The function at psobb!0x0083A5F0 copies a state struct's fields into
// three globals: canvas_div (0xAF02D8), flare X (0xAF02C8), flare Y
// (0xAF02CC). It runs at title-screen entry, clobbering any user-
// calibrated flare position written before that point.
//
// We NOP just the 17 bytes that write flare X/Y, leaving the canvas_div
// write intact (we do want canvas_div updated by the engine; that's the
// 1024x768-reference divisor for HUD-rect projection). After this patch
// 0xAF02C8/CC stay wherever the tuner's INI override left them, no
// per-frame re-apply needed.
//
// 0x0083A5F0:  8b 4c 24 04        mov ecx, [esp+4]
// 0x0083A5F4:  8b 41 18           mov eax, [ecx+0x18]
// 0x0083A5F7:  a3 d8 02 af 00     mov [0xAF02D8], eax     ; KEEP (canvas_div)
// 0x0083A5FC:  8b 51 08           mov edx, [ecx+8]        ; NOP -+
// 0x0083A5FF:  89 15 c8 02 af 00  mov [0xAF02C8], edx     ; NOP  | 17 bytes
// 0x0083A605:  8b 41 0c           mov eax, [ecx+0xc]      ; NOP  |
// 0x0083A608:  a3 cc 02 af 00     mov [0xAF02CC], eax     ; NOP -+
// 0x0083A60D:  c3                 ret
// ============================================================
// Character-create / general scaling pass through 0x0082BB74
// ============================================================
// Per-call-site scale of the 7-float anchor+basis sprite struct that
// 0x0082BB74 reads from ecx. Replaces the 5-byte CALL at 0x0082BB1B
// (return-addr 0x0082BB20 — caller dominant during char-create) with
// a CALL into a tiny FPU stub that scales:
// anchor_x   = ecx[0x00] * stretch + shift_x
// anchor_y   = ecx[0x04] * stretch + shift_y
// basis_a_x  = ecx[0x0C] * stretch
// basis_b_x  = ecx[0x10] * stretch
// basis_a_y  = ecx[0x14] * stretch
// basis_b_y  = ecx[0x18] * stretch
//
// Defaults: native 640x480 -> 1120x840 (1.75x), centered in 1920x1080
// (shift_x = (1920 - 640*1.75) / 2 = 400,
// shift_y = (1080 - 480*1.75) / 2 = 120).
// Live-tunable via mod_widescreen_tuner sliders (resolves these globals
// by name through GetProcAddress on pso_widescreen.asi).
//
// was a one-shot live-edit via psobbdbg /alloc + /write
// to validate; baked here for persistence after the user confirmed
// it correctly scaled the char-create UI.

__declspec(dllexport) volatile float pso_widescreen_charscreen_stretch = 1.65f;
__declspec(dllexport) volatile float pso_widescreen_charscreen_shift_x = 0.0f;
__declspec(dllexport) volatile float pso_widescreen_charscreen_shift_y = 0.0f;

// REFACTOR: apply_charscreen_scale_patch (0x0082BB1B FPU stub
// redirect; superseded by the cc_okbtn composer), apply_charscreen_data_patches
// (+ CharScreenFloat / kCharScreenSelectgamen; overlapped anzz1 0x8F8/0x8FA
// floats) and apply_flare_pin_patch (0x0083A5FC NOP; collapsed login UI when on)
// are RETIRED with the Sodaboy path. The dllexport pso_widescreen_charscreen_*
// tuner globals above are KEPT for external (tuner) ABI compatibility.

// Splash widescreen phase-2 (D3 fold, widens the SEGA/boot splash
// sprite-quad table at 0x009A3420 (4 entries × 0x20 stride, x1@+0x00, x2@+0x08).
// Stock layout {0,319},{0,319},{320,639},{320,639}; we multiply the X coords by
// 4/3 to fill a 16:9+ viewport. Now ALWAYS-ON on the default path (folded into
// apply_static_patches), gated only by its own PatchSplashPhase2 knob (D1).
//
// VA-OVERLAP AUDIT (vs the anzz1 atlas / listHUDWidth): the 8 target VAs are
// x1: 0x9A3420, 0x9A3440, 0x9A3460, 0x9A3480   (stock 0/0/320/320)
// x2: 0x9A3428, 0x9A3448, 0x9A3468, 0x9A3488   (stock 319/319/639/639)
// The anzz1 sprite atlas is 0x9A3840..0x9A38D8 — NO overlap there. BUT anzz1's
// listHUDWidth contains TWO of these splash VAs — 0x9A3468 and 0x9A3488 — to
// which anzz1 writes the horizontal extent A (the right-quad x2). To keep anzz1
// authoritative for the HUD extent (and avoid a double-bake where splash's
// 639*4/3 would clobber anzz1's exact A at non-16:9 aspects), this function
// SKIPS those two VAs. The other 6 splash coords (anzz1-free) are widened.
#define kSplashAnzz1OwnedX2A 0x009A3468u   // entry 2 x2 — anzz1 listHUDWidth (A)
#define kSplashAnzz1OwnedX2B 0x009A3488u   // entry 3 x2 — anzz1 listHUDWidth (A)
static void apply_splash_phase2(void)
{
    if (!g_cfg.patch_splash_phase2) {
        log_line("[pso_widescreen] splash-phase2: SKIP (PatchSplashPhase2=0)");
        return;
    }
    const uintptr_t kTableVA = 0x009A3420u;
    const unsigned  kEntries = 4;
    const unsigned  kStride  = 0x20;
    const float     stock_x[4][2] = {
        {   0.0f, 319.0f },
        {   0.0f, 319.0f },
        { 320.0f, 639.0f },
        { 320.0f, 639.0f },
    };
    // P5 D2: parameterize on the live extent (g_scale.A/640 = canvas/640).
    // IDENTITY GUARD: g_scale.A/640 is bit-identical to the legacy 4/3 ONLY at
    // 16:9 (both 0x3FAAAAAB); at a non-16:9 aspect it differs from today's
    // unconditional 4/3. Keep the EXACT legacy 4/3 at the lever-off default
    // (byte-identical at ANY aspect, matching today's build); diverge to the live
    // canvas/640 only when hs != 1.0.
    const float sp_hs = g_cfg.hud_scale;
    const float stretch = (sp_hs > 0.999f && sp_hs < 1.001f)
                              ? (4.0f / 3.0f)         // legacy default (byte-identical)
                              : (g_scale.A / 640.0f); // live canvas/640 at hs != 1.0
    int skipped = 0;
    for (unsigned i = 0; i < kEntries; i++) {
        uintptr_t base = kTableVA + i * kStride;
        const uintptr_t x1_va = base + 0x00;
        const uintptr_t x2_va = base + 0x08;
        DWORD old_prot = 0;
        if (!VirtualProtect((LPVOID)base, kStride, PAGE_EXECUTE_READWRITE, &old_prot)) {
            log_line("[pso_widescreen] splash-phase2: VirtualProtect FAILED entry=%u err=%lu",
                     i, GetLastError());
            continue;
        }
        *(float *)x1_va = stock_x[i][0] * stretch;
        // De-conflict vs anzz1: leave the 2 right-quad x2 coords anzz1 owns.
        if (x2_va == kSplashAnzz1OwnedX2A || x2_va == kSplashAnzz1OwnedX2B) {
            ++skipped;
        } else {
            *(float *)x2_va = stock_x[i][1] * stretch;
        }
        DWORD tmp = 0;
        VirtualProtect((LPVOID)base, kStride, old_prot, &tmp);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)base, kStride);
    }
    log_line("[pso_widescreen] splash-phase2: 0x009A3420 table stretched x%.4f "
             "(4 entries, %d x2-coord(s) left to anzz1: 0x9A3468/0x9A3488)",
             stretch, skipped);
}

// ============================================================
// d3d8to11 logical-canvas hint 
// ============================================================
// Problem: under d3d8to11.dll the engine's RHW UI verts are mapped to NDC
// using m_viewport.W / m_viewport.H — i.e. the PHYSICAL backbuffer (e.g.
// 1920×1080). But the engine submits in its 4:3-reference *scaled* logical
// canvas — for HUDScale=1.75 16:9 that's 1493.33 × 840 — because the
// in-binary imm32s are 4:3 (e.g. plate (64,61)-(320,317)) AND the engine
// applies viewport-scale globals [0xacc0e8] = 1493/640 = 2.33 and
// [0xacc0ec] = 840/480 = 1.75 (set by the engine's viewport setup at
// fcn.0082f308). Net effect in 1920-wide backbuffer: plate lands at
// 149..746 (40% of width) — left-anchored / "in upper-left" instead of
// filling the screen.
//
// Sodaboy avoids this by patching the imm32s in-place to canvas values
// AND keeping [0xacc0e8] = 1.0 — engine submits 149..746 directly into a
// 1493-wide native viewport, then his d3d8.dll maps to physical with a
// fixed-aspect NDC.
//
// We can't replicate Sodaboy without his d3d8 wrapper. But d3d8to11.dll
// + exports `d3d8to11_set_logical_canvas(float w, float h)`
// which historically told the wrapper "treat all RHW verts as if the
// SCREEN were (w,h) for NDC mapping". As of the architectural fix in
// d3d8to11 the wrapper IGNORES this hint and pins the RHW
// divisor to the physical D3D11 viewport — single, stack-honest divisor
// regardless of submission space.
//
// Why we ditched the hint: one global divisor cannot map two
// incompatible widget populations (engine-stretched widgets in physical
// pixels vs 4:3-reference widgets in 640×480 logical) onto the same
// screen. The hint was cosmetic for one population at the cost of the
// other; not architecturally sound.
//
// What replaces it: detect the wrapper at attach via the new
// `d3d8to11_get_wrapper_info` export. New ASIs gate behavior on the
// detected stack rather than blasting global divisors.
//
// Wrapper detection contract (d3d8to11 >= 
// - Export `d3d8to11_get_wrapper_info(unsigned int *out_version)`.
// - Returns 0xD3D81011u to confirm "I am d3d8to11".
// - `out_version` (optional) receives a packed YYYYMMDD int.
// - Symbol absent → wrapper is Sodaboy / native d3d8 / d3d8to9 /
// dgVoodoo / DXVK-d3d8 / unknown. Use safe defaults.
typedef unsigned int (__cdecl *PFN_get_wrapper_info)(unsigned int *out_version);

// Detected stack (set in detect_d3d8_stack). Values:
// 0 = unknown / not yet detected (assume Sodaboy-compatible)
// 1 = d3d8to11 (RHW divide pinned to physical viewport in HLSL)
static int  g_detected_stack       = 0;
static char g_detected_dll_path[MAX_PATH];
static unsigned int g_detected_d3d8to11_version = 0;

static const char *detected_stack_name(void)
{
    switch (g_detected_stack) {
        case 1:  return "d3d8to11";
        default: return "unknown(Sodaboy/native/d3d8to9/etc.)";
    }
}

// detect_d3d8_stack: probe d3d8.dll at attach time, classify which
// wrapper is providing Direct3DCreate8. Result drives downstream gates.
static void detect_d3d8_stack(void)
{
    HMODULE d3d8 = GetModuleHandleA("d3d8.dll");
    if (!d3d8) {
        log_line("[pso_widescreen] detect_d3d8_stack: d3d8.dll not loaded at attach (deferred resolve?)");
        return;
    }
    GetModuleFileNameA(d3d8, g_detected_dll_path, sizeof(g_detected_dll_path));
    PFN_get_wrapper_info gwi = (PFN_get_wrapper_info)
        (void *)GetProcAddress(d3d8, "d3d8to11_get_wrapper_info");
    if (gwi) {
        unsigned int ver = 0;
        unsigned int magic = gwi(&ver);
        if (magic == 0xD3D81011u) {
            g_detected_stack = 1;
            g_detected_d3d8to11_version = ver;
            log_line("[pso_widescreen] detect_d3d8_stack: d3d8to11 (rhw-fix-v=%u) at %s",
                     ver, g_detected_dll_path);
            return;
        }
        log_line("[pso_widescreen] detect_d3d8_stack: gwi present but bad magic 0x%08X at %s",
                 magic, g_detected_dll_path);
        return;
    }
    // Legacy probe: did the older d3d8to11 export `d3d8to11_set_logical_canvas`
    // but no `_get_wrapper_info`? Treat as d3d8to11-old (canvas-hint era).
    if (GetProcAddress(d3d8, "d3d8to11_set_logical_canvas")) {
        g_detected_stack = 1;
        log_line("[pso_widescreen] detect_d3d8_stack: d3d8to11 (legacy, pre-rhw-fix) at %s",
                 g_detected_dll_path);
        return;
    }
    log_line("[pso_widescreen] detect_d3d8_stack: %s at %s",
             detected_stack_name(), g_detected_dll_path);
}

// publish_logical_canvas removed cleanup — was deprecated
// since d3d8to11 wrapper update; function body had become a
// pure no-op log line. detect_d3d8_stack() above continues to publish
// the wrapper detection separately.

// ============================================================
// Cursor-warp fix (added 
// ============================================================
// PSOBB's in-game mouse menus warp the OS pointer to the selected
// element. The warp formula (3 sites: 0x0070994B / 0x0070A018 /
// 0x0070A3FF) maps a logical-canvas menu coord into client pixels using
// the engine UI-scale globals [0xAF0350]/[0xAF0354]/[0xAF035C]; the
// element is then DRAWN by the widget renderer (0x00733C2C/0x00733C36)
// reading the SAME globals — so stock psobb is self-consistent.
//
// Our HUD-scale knobs scale the rendered HUD OUTSIDE that path:
// - HUDCompress: a draw-time RHW center-divide (scale_rhw_apply) that
// does  p' = center_px + (p - center_px) / compress  in backbuffer
// pixel space (center = bb/client center).
// - HUDScale: Sodaboy .text imm32 rewrites that grow the HUD about the
// widescreen canvas.
// Neither touches [0xAF035C], so the engine warp computes the
// canonical (un-extra-scaled) position while the element is drawn
// elsewhere → the OS cursor physically jumps off the drawn element.
//
// Fix: hook the SetCursorPos IMPORT SLOT in psobb.exe's IAT
// ([0x008F8344]). Static RE confirms exactly 3 callers of that slot and
// all 3 are the menu warp sites — so every call through it is a warp we
// must correct. We transform the warp target through the same
// center-relative scale the HUD is drawn at:
//
// S = (1 / HUDCompress) * HUDScale
// out = center + (in - center) * S
//
// center = window client-rect center mapped to screen coords (the same
// space SetCursorPos consumes — post-ClientToScreen). When both knobs
// are 1.0, S == 1.0 and the transform is the identity (zero behavior
// change), so this is safe to leave on unconditionally.

typedef BOOL (WINAPI *SetCursorPos_t)(int X, int Y);
static SetCursorPos_t real_SetCursorPos = NULL;
static volatile LONG  g_cursor_warps_seen     = 0;
static volatile LONG  g_cursor_warps_adjusted = 0;
static int            g_cursor_logged_first    = 0;

// Engine HWND global (push dword [0xACBED8] feeds GetClientRect /
// ClientToScreen at all 3 warp sites).
#define kEngineHwndVA  0x00ACBED8u

static BOOL WINAPI Hook_SetCursorPos(int X, int Y)
{
    InterlockedIncrement(&g_cursor_warps_seen);

    const float compress = g_cfg.hud_compress;
    const float scale    = g_cfg.hud_scale;
    const float S = (compress != 0.0f ? (1.0f / compress) : 1.0f) * scale;

    // Identity → pass straight through (also the disabled path).
    if (!g_cfg.cursor_warp_fix || (S > 0.99999f && S < 1.00001f)) {
        return real_SetCursorPos ? real_SetCursorPos(X, Y)
                                 : SetCursorPos(X, Y);
    }

    int out_x = X, out_y = Y;
    __try {
        HWND hwnd = *(HWND volatile *)kEngineHwndVA;
        RECT rc;
        POINT center = {0, 0};
        if (hwnd && GetClientRect(hwnd, &rc)) {
            // Client-rect center, then map to screen (the space X/Y are in).
            center.x = (rc.left + rc.right)  / 2;
            center.y = (rc.top  + rc.bottom) / 2;
            ClientToScreen(hwnd, &center);
            const float cx = (float)center.x;
            const float cy = (float)center.y;
            out_x = (int)(cx + ((float)X - cx) * S + 0.5f);
            out_y = (int)(cy + ((float)Y - cy) * S + 0.5f);
            InterlockedIncrement(&g_cursor_warps_adjusted);
            if (!g_cursor_logged_first) {
                g_cursor_logged_first = 1;
                log_line("[pso_widescreen] cursor-warp-fix: S=%.4f "
                         "(1/HUDCompress=%.4f * HUDScale=%.4f) center=(%d,%d) "
                         "first warp (%d,%d) -> (%d,%d)",
                         S, (compress != 0.0f ? 1.0f / compress : 1.0f),
                         scale, center.x, center.y, X, Y, out_x, out_y);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out_x = X; out_y = Y;  // never poison the engine's warp
    }
    return real_SetCursorPos ? real_SetCursorPos(out_x, out_y)
                             : SetCursorPos(out_x, out_y);
}

// Install the SetCursorPos IAT hook on psobb.exe. SetCursorPos lives in
// user32.dll. patch_iat() (defined above) finds the import descriptor
// and swaps the slot, returning the original thunk so we can forward.
static void install_cursor_warp_fix(void)
{
    if (!g_cfg.cursor_warp_fix) {
        log_line("[pso_widescreen] cursor-warp-fix: SKIP (CursorWarpFix=0)");
        return;
    }
    HMODULE psobb = GetModuleHandleA(NULL);
    if (patch_iat(psobb, "user32.dll", "SetCursorPos",
                  (void *)&Hook_SetCursorPos,
                  (void **)&real_SetCursorPos)) {
        log_line("[pso_widescreen] cursor-warp-fix: SetCursorPos IAT hooked "
                 "(real=0x%p) HUDScale=%.2f HUDCompress=%.2f S=%.4f",
                 (void *)real_SetCursorPos, g_cfg.hud_scale, g_cfg.hud_compress,
                 (g_cfg.hud_compress != 0.0f ? 1.0f / g_cfg.hud_compress : 1.0f)
                     * g_cfg.hud_scale);
    } else {
        log_line("[pso_widescreen] cursor-warp-fix: SetCursorPos NOT FOUND in "
                 "psobb.exe import table — fix inactive");
    }
}

// ============================================================
// CHAR-CREATE 16:9 — AUTHORITATIVE-SOURCE OWNERSHIP refactor)
// ============================================================
// Replaces the per-frame char-create frame-patch (cc_quad_fix_c @0x0082BB74 +
// cc_text_obs_c @0x0082B440 + the shared negative gate cc_scene_active) with full
// authoritative-source ownership. Every char-create content element the frame-patch
// moved is relocated at its TRUE static source (a .text bake immediate or a .rdata
// float), keyed so 4:3 (design_w==640) is a byte-identical no-op. ALL char-create
// render-thread MinHooks are DELETED (the last one, cc_quad_fix_c @0x0082BB74, was
// removed per the no-per-frame-patching mandate) — 0x0082BB74 and
// 0x0082B440 are byte-stock, so ship-select cannot leak (no hook to fire on it).
//
// All shifts derive from one quantity, evaluated once at apply time AFTER
// apply_anzz1_widescreen (anzz1 owns design_w, the backdrop table, and the
// wheel/info immediates):
// dw       = hs_peek_f32(0x0098A4B8)   design_w: 853.33 @16:9 hs1.0, 640 @4:3, 1280 @hs1.5
// span     = dw - 640.0                 213.33 @16:9 ; 0.0 @4:3  (HARD no-op key)
// stride_R = 0.75 * span                160.0 @16:9 hs1.0 (reproduces old cc_stride_right)
// At design_w==640, span==0 => stride_R==0 => every new value == stock => no write
// fires (cc_poke_imm_f32 is value-guarded). See _charcreate_source_ownership_spec.md.

// Forward decl: hs_peek_f32 is defined later (in the P5 D3 primitives block); the
// source bake below reads design_w through it. Without this prototype the call
// implicit-declares int-returning hs_peek_f32 (C4013) and dw reads garbage.
static float hs_peek_f32(uint32_t va);

// Value-guarded imm32 .text rewrite (mirrors hs_poke_f32 but for a code immediate /
// .rdata float). Idempotent: writes only on change; SEH-guarded; restores page
// protection. The "*p == stock" precondition means a re-run or a relocated reader
// can never corrupt a non-stock site.
static void cc_poke_imm_f32(uint32_t imm_va, float stock, float v)
{
    volatile float *p = (volatile float *)(uintptr_t)imm_va;
    __try {
        if (*p != stock) return;        // safety: only patch a site still at its stock imm
        if (*p == v) return;            // value-guard (no-op at 4:3 where v==stock)
        DWORD old;
        if (!VirtualProtect((LPVOID)(uintptr_t)imm_va, 4, PAGE_EXECUTE_READWRITE, &old)) return;
        *p = v;
        DWORD tmp; VirtualProtect((LPVOID)(uintptr_t)imm_va, 4, old, &tmp);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// apply_startup_bakes — THE one-shot front-end startup bake pass
// (). ONE linear sweep of value-guarded source-immediate
// rewrites that own the front-end scenes' 16:9 horizontal layout, with NO scene
// gate and NO frame-patch: the engine re-bakes each cell from its source imm on
// every scene-build, and W1 (0x0082F309) is dead in-game, so a .text/.rdata
// immediate edit survives the front-end->in-game transition where a code-flow
// hook would be wiped.
//
// Called from apply_static_patches IMMEDIATELY AFTER apply_anzz1_widescreen
// (anzz1 owns design_w 0x0098A4B8 -> 853.333 @16:9 hs1.0 by the time we run).
// Gated on g_cfg.patch_charselect (the old PatchCharSelect=0 escape hatch is
// preserved, same gate the folded char-create + login writes used).
//
// 4:3 NO-OP BY CONSTRUCTION: at design_w==640 -> span==0 -> stride_R==0 and
// a_plus_10==650 -> every v collapses to its stock -> cc_poke_imm_f32's
// `if(*p==v)return` value-guard fires -> byte-identical stock at 640x480.
//
// This folds the former apply_charcreate_source_bake (the 4 char-create writes
// are reproduced here verbatim — de-duped to ONE copy) and adds the 3 missed
// Scene-02 login-menu ctor SEED copies. GATED/owed items (selectgamen plates,
// char-create X1 0x0096E52C, char-select 0x004137DE final class, stats panel)
// are deliberately NOT included this pass — see
static void apply_startup_bakes(int render_w)
{
    if (!g_cfg.patch_charselect) {
        log_line("[pso_widescreen] startup-bakes: SKIP (PatchCharSelect=0)");
        return;
    }
    const float dw       = hs_peek_f32(0x0098A4B8u);   // design_w  (anzz1-owned, set first)
    const float a_plus_10 = dw + 10.0f;                 // 863.333 @16:9, 650.0 @4:3 (==stock no-op)
    const float span      = dw - 640.0f;                // 213.33 @16:9 hs1.0 ; 0.0 @4:3 (HARD no-op key)
    // (char-create content-row stride is RUNTIME-ONLY with no static source -> NOT
    // baked and NOT strided; the per-frame hook that used to move it was deleted
    // per the no-per-frame-patching mandate. See the Scene-09 note below.)

    // ---- Scene 02: Login / Start menu ctor SEED copies (the 3 MISSED right-edge
    // anchors). anzz1 baked the animation-RESEED copies (0x7584EE/51F = A+10) but
    // never the ctor SEEDs -> at 16:9 the slide-in lerps from a stale design-650
    // toward 863, a visible pop / wrong rest-frame. Right (A+10) class. 4:3-safe.
    cc_poke_imm_f32(0x007583C1u, 650.0f, a_plus_10);          // menu primary X1   (mov [edx+0x1c])
    cc_poke_imm_f32(0x007583F3u, 650.0f, a_plus_10);          // menu secondary X2 (mov [edx+0x2c])
    cc_poke_imm_f32(0x0075840Bu, 650.0f, a_plus_10);          // menu anim-target  (mov [edx+0x34])

    // ---- Scene 09 char-create bake: the 4 MISTARGETED WordSelect writes are GONE ----
    // The old writes here (0x006FFD80->[0xA96D60], 0x006FFCEA->[0xA96DA0],
    // 0x0096E554, 0x0096E548) did NOT touch char-create. Live disasm + the harness
    // symbol map proved those cells/floats belong to the in-game WordSelect /
    // quick-chat / list-window menu, read ONLY by WordSelectMenu_State0x12_Update
    // (0x00702488), ListWindowEntity_UpdatePosition (0x00706c78),
    // CreateWordSelectMenuEntity (0x00701d00) and WordSelectEntity_ConfigA
    // (0x00707d50). So baking them shifted the quick-chat list window +160px
    // in-game/lobby (a global, scene-independent corruption) and did nothing for
    // char-create. They are DELETED — do NOT reintroduce.
    // See memory: charcreate-source-bake-mistargeted-wordselect.

    // ---- Scene 09 char-create class-select CONTENT ROWS: runtime_only (NO bake) ----
    // an adversarial 4-angle source-hunt CONFIRMED the right-pane CONTENT
    // rows (class header, description, 9-slice border, per-row stat bars) have NO
    // value-guard-bakeable static source — they are emitted by SHARED generic widget
    // renderers (e.g. sub_0071ff10 @0x0071FF10) whose design X = runtime heap field
    // + offset, never an immediate. the per-frame composer hook that used
    // to stride these rows (cc_quad_fix_c @0x0082BB74) was DELETED per the
    // no-per-frame-patching mandate, so the content rows are intentionally NOT
    // strided — there is no allowed mechanism to move them and no static source to
    // bake. The settled class-select screen renders correctly without it.
    //
    // NOTE: the hunt ALSO found a leak-safe static source for a SEPARATE element —
    // the RenderVersionInfo-slot rect 0x006F4CC4 (right-pane X0=320 @0x006F4D4E,
    // X1=576 @0x006F4D5E, char-create-exclusive). Baking it (+stride_R) was tried
    // and REVERTED: it moves a DIFFERENT quad than the hook (RenderUIQuad backing vs
    // composer rows), so on its own it detaches the backing from the rows, and it was
    // NOT part of the known-good "what we had" state (which left it native). Whether
    // that backing should ALSO slide for 16:9 is deferred to a LIVE draw_capture at
    // scene 5 (front-end nav is server-blocked tonight) — do NOT re-add it unverified.
    // See memory: charcreate-source-bake-mistargeted-wordselect.

    // ---- Scene 09 char-create class-select INFO BOX (the blue 9-slice panel): SHIFT RIGHT
    // (live RE on PSOBB.IO @ screen-id [0x00A165F0]==12, disasm-verified).
    // The big blue 9-slice info panel on the right (cyan 9-slice frame + 4 class previews +
    // "Hunter" banner + "Enter OK" prompt + name labels) sits design 307->607 (SCREEN
    // 690.75->1365.75 @ the 2D affine SCALE_X 0x00ACC0E8 == 2.25, 16:9 hs1.0), leaving a
    // big empty gap on its right -> visibly off-center in 16:9. We shift the WHOLE box right
    // so it rides with the (separately baked) class-description text and balances the layout.
    //
    // box X now handled by the unified RenderChallengePanelElement splice (cc_box_x_*), see below.
    //
    // The earlier attempt baked 10 mov-imm32 X immediates inside
    // ChallengeObjectSubPanel_UpdateVisuals_004f4a00. Live draw_capture PROVED that did NOT
    // move the box: every box element reads its X from a HEAP struct ([ebx]) inside
    // RenderChallengePanelElement_004e9d8c, NOT from those immediates (which fed culled /
    // irrelevant sub-elements). Those 10 bakes are REVERTED — leaving them in would DOUBLE
    // the offset now that the splice owns the shift. The single clean lever is the one-time
    // .text splice in cc_box_x_install() (see CC_BOX_X_SPLICE_VA), which the game's own code
    // runs each frame and which offsets the heap X on the right-pane path only.

    // ---- F_VPRESET in-game 2D-affine SCALE_X/Y source bakes (16:9 in-game wide-fill)
    // F_VPRESET (0x0082F5D8) stamps the in-game 2D affine (0x00ACC0E8/EC) back to a
    // HARDCODED 1.0 immediate on the front-end->in-game transition, and W1 (the
    // derived FE writer at 0x0082F309) is DEAD in-game — so without this the HUD
    // renders at design_w geometry but 1.0 scale (under-fills 16:9). Patch the two
    // imm32 operands to render_w/design_w so in-game fills the wide canvas.
    // ig_aff = render_w / dw : 2.25 @hs1.0(16:9), 1.5 @hs1.5, 1.0 @4:3(render_w==dw==640)
    // FE-SAFE asymmetry: on the front-end W1 re-derives the correct value every
    // frame and WINS (the patched F_VPRESET value is harmlessly overwritten); only
    // in-game (where W1 is dead) does the patched value hold. 4:3 is a value-guarded
    // no-op (ig_aff==1.0==stock -> cc_poke_imm_f32's `*p==v` guard fires). The
    // immediate .text edit SURVIVES the transition wipe (only code-flow detours get
    // reverted), so no worker / scene gate is needed for the affine itself. See
    //
    const float ig_aff = (dw > 0.0f) ? ((float)render_w / dw) : 1.0f;
    cc_poke_imm_f32(0x0082F8F4u, 1.0f, ig_aff);   // F_VPRESET SCALE_X imm (mov [0x00ACC0E8],imm)
    cc_poke_imm_f32(0x0082F914u, 1.0f, ig_aff);   // F_VPRESET SCALE_Y imm (mov [0x00ACC0EC],imm)

    // ---- Lobby game-list "NN-NN" bar 4:3 right-ceiling -> design_w (16:9 full width) ----
    // (static RE, disasm-verified). The lobby NN-NN/game-list bar is emitted
    // by sub_00722010 -> sub_0082b558 -> sub_00836dc8. CONTRARY to the old "0x82b558
    // bypasses the affine" belief, the affine LOOP at 0x00836e02 (fmul [0x00ACC0E8])
    // DOES run for this quad: sub_0082b558 forwards a vertex DESCRIPTOR whose +0xC dword
    // is a vertex COUNT (the caller hard-sets it to 2: `mov eax,2; mov [esp+0x74],eax`
    // @0x721fa5/0x721fcf), and 0x836dc8 gates the affine on `cmp [eax+0xC],1; jle` =
    // count>1 -> runs. So the bar is NOT a bypass — it floats because its right-edge
    // vertex X is the literal 4:3 ceiling 640.0 (0x44200000 imm @0x00721FC0): 640*2.25
    // = 1440 of 1920 -> ~480px right gap. Raise that one imm to design_w; the SAME
    // in-game affine then maps it to render_w (dw*(render_w/dw)=render_w, so this is
    // HudScale-robust — the F_VPRESET affine just baked above and this share `dw`).
    // Patches ONE mov-imm operand (no shared cell -> leak-safe); value-guarded so 4:3
    // (dw==640) is a byte-identical no-op. The bar's left vertex X stays 0 (left edge).
    // VISUAL-PENDING: confirm at a live 16:9 lobby that the bar reaches the far-right
    // edge with no overshoot (server login was blocked when this shipped).
    cc_poke_imm_f32(0x00721FC0u, 640.0f, dw);     // NN-NN bar right-edge vertex X (853.33 @16:9 hs1.0)

    log_line("[pso_widescreen] startup-bakes: done (dw=%.3f a+10=%.3f ig_aff=%.4f; cc rows=runtime-only/not-strided; nnbar=dw)",
             dw, a_plus_10, ig_aff);
}

// ============================================================
// CHAR-CREATE CONTENT-STRIDE COMPOSER HOOK — DELETED 
// ============================================================
// Per the owner's no-per-frame-patching mandate, the per-frame char-create
// composer detour on sub_0082BB74 (cc_quad_fix_c / stub_82bb74_charcreate /
// cc_okbtn_install + the cc_classify_quad classifier and the [ccobs] OBSERVE
// state) has been DELETED end-to-end. 0x0082BB74 is now byte-stock again.
//
// Char-create now renders via the static one-shot apply_startup_bakes() pass
// (above) plus stock engine geometry. The class-select right-pane CONTENT rows
// (class header / description / 9-slice border / per-row stat bars) are
// RUNTIME-only widget-tree data with NO value-guard-bakeable static source, so
// they are intentionally NOT strided — there is no allowed (non-per-frame)
// mechanism to move them. (RE also showed the hook was mis-gated: it tested
// *(int*)0x00AAB384==5, but live char-create class-select is scene==3 /
// screen-id [0x00A165F0]==12, so the hook never fired there anyway.)

// ============================================================
// =====================================================================
// AD-SCREEN (patch-server news / status) LAYOUT — port of Trinity's
// AdDrawLineTask / PatchAdDrawLineTask (TrinityDLL resolution.cpp:39-69,
// resolution.h adDrawLineTaskOffsets + addrAdDrawLineTaskI 0x00408C9D).
//
// The psobb.io "patch server" news screen (MOTD box + the "Current Status" /
// "All Status" labels + the blue/red status bars) is laid out by a per-
// construction object. Its layout floats live at (obj - 0x5C04) + offset,
// authored in 4:3 design space. Stock leaves the status labels + bars at
// 640/480 anchors; in widescreen the labels stay top/left-anchored while
// the bars move -> "Current Status"/"All Status" end up UP in the news box
// instead of below their bars (live-confirmed . We were MISSING
// this hook entirely (it touches no VA any other bake covers).
//
// Trinity hooks the ctor's `mov [0x00A3AD14], eax` (5 bytes a3 14 ad a3 00)
// and re-anchors each tagged float: axis 0 (X) = A-(640-f); axis 1 (Y) =
// C-(480-f). We mirror it with our design extents (A=design_w, C=design_h),
// held in g_ad_A/g_ad_C globals so the anchor space is LIVE-tunable
// (write_memory) without a rebuild while dialing in 16:9 hs2.0 parity. The
// hook site is a SEH-prologue (the ctor) => fires once per construction =>
// the in-place re-anchor never compounds. 4:3 / disabled => not installed.
// =====================================================================
#define AD_HOOK_VA       0x00408C9Du   // mov [0x00A3AD14], eax  (a3 14 ad a3 00)
#define AD_HOOK_LEN      5
#define AD_HOOK_RESUME   0x00408CA2u
#define AD_OBJ_BASE_OFF  0x5C04        // base = *0x00A3AD14 - 0x5C04 (Trinity)

// (offset_from_base, axis): axis 0 = X right-anchor, 1 = Y bottom-anchor.
static const struct { unsigned int off; unsigned char axis; } g_ad_offsets[] = {
    {0x0024,0},{0x0028,1},{0x0040,0},{0x0054,1},{0x2498,0},{0x251C,0},{0x25A0,0},
    {0x2600,1},{0x2610,1},{0x262C,1},{0x263C,1},{0x2658,1},{0x2664,0},{0x2668,1},
    {0x2684,1},{0x2694,1},{0x26B0,1},{0x26C0,1},{0x26DC,1},{0x26E8,0},{0x26EC,1},
    {0x34F4,1},{0x3608,0},{0x3628,1},{0x3638,1},{0x3654,1},{0x3664,1},{0x3680,1},
    {0x368C,0},{0x3690,1},{0x40D8,1},{0x40F4,1},{0x420C,1},{0x4228,1},
};
static float g_ad_A = 640.0f;          // design_w anchor space (set at install; live-tunable)
static float g_ad_C = 480.0f;          // design_h anchor space (set at install; live-tunable)
static void *g_ad_resume    = (void *)AD_HOOK_RESUME;
static int   g_ad_installed = 0;
static uint8_t g_ad_orig[AD_HOOK_LEN] = {0};

// C helper: re-anchor the freshly-constructed ad-screen object's layout floats.
static void ad_draw_line_patch(void)
{
    uint32_t objp = *(volatile uint32_t *)(uintptr_t)0x00A3AD14u;
    if (!objp) return;
    uint8_t *base = (uint8_t *)(uintptr_t)(objp - AD_OBJ_BASE_OFF);
    const float A = g_ad_A, C = g_ad_C;
    const int n = (int)(sizeof(g_ad_offsets) / sizeof(g_ad_offsets[0]));
    for (int i = 0; i < n; ++i) {
        float *p = (float *)(base + g_ad_offsets[i].off);
        __try {
            float f = *p;
            *p = (g_ad_offsets[i].axis == 0) ? (A - (640.0f - f))
                                             : (C - (480.0f - f));
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* skip on a mismatched build */ }
    }
}

// Naked stub: re-issue the hooked `mov [0x00A3AD14], eax`, re-anchor, resume.
__declspec(naked) static void ad_draw_line_stub(void)
{
    __asm {
        mov   dword ptr ds:[0x00A3AD14], eax     // original instruction (store obj ptr)
        pushad
        call  ad_draw_line_patch
        popad
        jmp   dword ptr ds:[g_ad_resume]          // -> 0x00408CA2
    }
}

// Install the ad-screen hook. Sig-guarded + idempotent; 4:3 / disabled no-op.
static void patch_ad_draw_line(const ws_scale_ctx *s)
{
    if (g_ad_installed) return;
    if (!s->enabled || s->A <= 640.0f) {
        log_line("[pso_widescreen] ad-screen: SKIP (4:3 / disabled)");
        return;
    }
    static const uint8_t kExpected[AD_HOOK_LEN] = { 0xA3, 0x14, 0xAD, 0xA3, 0x00 };
    const uint8_t *p = (const uint8_t *)(uintptr_t)AD_HOOK_VA;
    for (int i = 0; i < AD_HOOK_LEN; ++i) {
        if (p[i] != kExpected[i]) {
            log_line("[pso_widescreen] ad-screen: sig mismatch byte %d got 0x%02x "
                     "expected 0x%02x — refusing to hook", i, p[i], kExpected[i]);
            return;
        }
        g_ad_orig[i] = p[i];
    }
    g_ad_A = s->A;   // design_w (1706.67 @hs2.0); live-tunable via write_memory(&g_ad_A)
    g_ad_C = s->C;   // design_h (960 @hs2.0)
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)(uintptr_t)AD_HOOK_VA, AD_HOOK_LEN,
                        PAGE_EXECUTE_READWRITE, &old)) {
        log_line("[pso_widescreen] ad-screen: VirtualProtect FAILED err=%lu", GetLastError());
        return;
    }
    uint8_t patch[AD_HOOK_LEN];
    patch[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)&ad_draw_line_stub - (AD_HOOK_VA + 5));
    memcpy(patch + 1, &rel, 4);
    memcpy((void *)(uintptr_t)AD_HOOK_VA, patch, AD_HOOK_LEN);
    DWORD tmp = 0;
    VirtualProtect((LPVOID)(uintptr_t)AD_HOOK_VA, AD_HOOK_LEN, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)AD_HOOK_VA, AD_HOOK_LEN);
    g_ad_installed = 1;
    log_line("[pso_widescreen] ad-screen: AdDrawLineTask hooked @0x00408C9D "
             "(A=%.1f C=%.1f, %d offsets)", (double)g_ad_A, (double)g_ad_C,
             (int)(sizeof(g_ad_offsets) / sizeof(g_ad_offsets[0])));
}


// THE TWO APPLY HELPERS refactor)
// ============================================================
// ONE universal scaling gate (g_scale.enabled, checked by the caller), ONE
// static-patch helper, ONE engine-patch helper. No widescreen_engine selector
// branch anywhere. Per-feature knobs survive as internal early-return guards.

// apply_static_patches — every .data / .text-immediate / NOP write, in ONE
// linear pass, each idempotent + value/sig-guarded. ORDER: (1) the anzz1
// coordinate bake FIRST (the source of truth; one-shot `applied`-guarded),
// then (2) patch_minimap_layout (bake-on-top of anzz1's 0xA11324/E8), then the
// engine-independent companions (each keeps its per-feature knob). splash-phase2
// is de-conflicted vs anzz1 inside the function (skips the 2 shared x2 VAs).
/* ============================================================================
 *  UNIFIED kBakes[] TABLE + CORE
 *  ----------------------------------------------------------------------------
 *  ui_coord() is defined far above (~L2019); no forward decl needed.
 *  apply_special() is defined just below apply_bakes() but called by it. */
static void apply_special(const ws_scale_ctx *s);   /* fwd: apply_bakes calls it */

static const bake_t kBakes[] = {
/* ===== kBakes[] — 732 authoritative rows (anzz1 folded + patch_* extracted) ===== */
/* fields: va, kind, base, coeff, offset, src, gate, stock, note, base2 */
  /* ---- SRC_ANZZ1 : 566 rows ---- */
  { 0x004011C0, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x004011D2, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x004011DD, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x004011EF, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00407F9D, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00409F4B, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0040A016, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0040A02B, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0040A036, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0040A04B, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0040C445, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0040C469, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0040C48B, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00453809, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0045381D, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00453827, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0045383B, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00483278, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00489E53, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x004EB4AA, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x004EB4B4, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x004EB4F0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x004EB4FA, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x004EC2D1, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x004EC2E9, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x004EF592, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x004EF59C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00502D7D, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00502D85, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x005BC90E, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x005BD783, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x005BDB89, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0066DFEF, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x0066DFFA, K_ADD, B_C, 0.5f, -240.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.vcenter", B_LIT },
  { 0x006F4922, K_SET, B_A, 0.5f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4936, K_SET, B_C, 0.5f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4CF2, K_SET, B_B, 0.5f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4CFA, K_SET, B_D, 0.4765625f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4D02, K_SET, B_B, 2.5f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4D0A, K_SET, B_D, 2.4765625f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4D4E, K_SET, B_B, 2.5f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4D56, K_SET, B_D, 0.4765625f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4D5E, K_SET, B_B, 4.5f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4D66, K_SET, B_D, 2.4765625f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4D9F, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x006FFCB8, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x006FFCE0, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x006FFCF4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x006FFD12, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x006FFD1C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x006FFD58, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x006FFD76, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D2CE, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D2EC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D2F6, K_SET, B_A, 1.0f, -384.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x0070D30A, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D328, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D346, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D350, K_SET, B_A, 1.0f, -384.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x0070D364, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D382, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D3A0, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D3B4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D3D2, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D3F0, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D404, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x007126A5, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x007126AC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x007165B8, K_SET, B_A, 1.0f, -16.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x00719AB1, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00719ACE, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00719AD9, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00719AF6, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00719C08, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00719C16, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00719C5C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00719C6B, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00719C79, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00719C8A, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00719D44, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00719D53, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00719E84, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0071A21F, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0071A226, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0071A8F8, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0071A905, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0071A988, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0071A995, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00721E6C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00721E7A, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00721F2A, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x007403F6, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0074467D, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0074468D, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x007446D8, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x007446E8, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00744723, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00744733, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0074477E, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0074478E, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x007583C8, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x007583E3, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x007583FE, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00758412, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x007584EE, K_SET, B_A, 1.0f, 10.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x007584F5, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0075850F, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0075851F, K_SET, B_A, 1.0f, 10.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x00758526, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0075853F, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00761CC7, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00761CCC, K_SET, B_A, 0.5f, -110.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x00761CD9, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00761CDE, K_SET, B_A, 0.5f, -110.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x00762367, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0076236C, K_SET, B_A, 0.5f, -180.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x007625F8, K_SET, B_A, 0.5f, -4.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x00762602, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x007627CF, K_SET, B_A, 0.5f, -4.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x007627DF, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0077F2ED, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0077F30D, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0077F386, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0077F3A7, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0077F3B2, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0077F3D2, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00783A92, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00783AAA, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00785641, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00785648, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00785680, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x007856B1, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x007856B8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00785705, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00785BD2, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00785BD9, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00785C11, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00785C49, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x007888A6, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x007888C6, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x007888D1, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x007888F1, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00788EA6, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00788EC6, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00788ED1, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00788EF1, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00791C7A, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00792366, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00804EE0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00804EF4, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x008051BA, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008051CE, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0082EC74, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x0082ED43, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x0082EF4C, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x0082F018, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x0082F700, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x0082F7DB, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x008F8494, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F853C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F854C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F87D4, K_SET, B_A, 1.0f, -63.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x008F87D8, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x008F87E0, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x008F87E4, K_SET, B_A, 1.0f, -63.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x008F8EAC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F8EBC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F8F50, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9A94, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9A98, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x008F9AA4, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9AC4, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9AC8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x008F9D38, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9D68, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9E8C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9EF8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9F2C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9F34, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9F58, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008FA8A8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008FAE50, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008FAE90, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008FFB80, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x008FFB84, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009006F4, K_U32, B_RW, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.w", B_LIT },
  { 0x009006F8, K_U32, B_RH, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.h", B_LIT },
  { 0x009006FC, K_U32, B_RW, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.w", B_LIT },
  { 0x00900700, K_U32, B_RH, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.h", B_LIT },
  { 0x00900704, K_U32, B_RW, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.w", B_LIT },
  { 0x00900708, K_U32, B_RH, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.h", B_LIT },
  { 0x0090070C, K_U32, B_RW, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.w", B_LIT },
  { 0x00900710, K_U32, B_RH, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.h", B_LIT },
  { 0x00900714, K_U32, B_RW, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.w", B_LIT },
  { 0x00900718, K_U32, B_RH, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.h", B_LIT },
  { 0x0090071C, K_U32, B_RW, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.w", B_LIT },
  { 0x00900720, K_U32, B_RH, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.h", B_LIT },
  { 0x009007B8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00901288, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x00909164, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00909168, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00909D70, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00909D84, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00909DA8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0091D8B8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0091D8BC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0091DA7C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0091DA80, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0091E19C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0091FBEC, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0091FBF0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0091FC50, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0091FC54, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0091FC70, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0091FC74, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0091FF90, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0091FF94, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00920050, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00920104, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00920108, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00920160, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00920164, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0092B6EC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0092B71C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0092B740, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0092B780, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0096E534, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096E53C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096E54C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096E574, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096E5FC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096E660, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096F5D8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0096F5E0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0096F694, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0096F698, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0096FC44, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096FC54, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096FE84, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0096FF5C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0096FF60, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0096FF9C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0096FFA8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0096FFBC, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0096FFC0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009701EC, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009701F0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097061C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00970638, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00970640, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00970648, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00970660, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097066C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009706C0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009706C8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009707F4, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009707F8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00970808, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097080C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009710BC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009712DC, K_SET, B_C, -1.0f, 720.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x00971F44, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F4C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F54, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F5C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F64, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F6C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F74, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F7C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F84, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F8C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F94, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F9C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FA4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FAC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FBC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FC4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FCC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FD4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FDC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FE4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FEC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FF4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FFC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972000, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972004, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097200C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972010, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00972014, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972024, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972040, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972048, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972050, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00972054, K_ADD, B_C, 0.5f, -240.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.vcenter", B_LIT },
  { 0x00972058, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x0097205C, K_ADD, B_C, 0.5f, -240.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.vcenter", B_LIT },
  { 0x00972060, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x0097206C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972078, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00972094, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097209C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009720A4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009720A8, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x009720B0, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x009720B4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009720B8, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x009720CC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009720E0, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x009720E4, K_ADD, B_C, 0.5f, -240.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.vcenter", B_LIT },
  { 0x009720E8, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x009720EC, K_ADD, B_C, 0.5f, -240.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.vcenter", B_LIT },
  { 0x009720F0, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x009720F4, K_ADD, B_C, 0.5f, -240.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.vcenter", B_LIT },
  { 0x009720F8, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x009720FC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972100, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972104, K_ADD, B_C, 0.5f, -240.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.vcenter", B_LIT },
  { 0x00972188, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972224, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097222C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972234, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097223C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972244, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097224C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972254, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097225C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972264, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097227C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972284, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009722DC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009722E4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009722EC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009722F4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009722FC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972304, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097230C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972314, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097231C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972324, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097232C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972334, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097233C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972344, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097234C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972354, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097235C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972364, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097236C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972374, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097237C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972384, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097238C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972394, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097239C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723A4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723AC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723B4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723BC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723C4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723CC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723D4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723DC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723E4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723EC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723F4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723FC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972404, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097240C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972414, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097241C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972424, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097242C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972434, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097243C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972444, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097244C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972454, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097245C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972464, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097246C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972474, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097247C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972484, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097248C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972494, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097249C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724A4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724AC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724B4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724BC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724C4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724CC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724D4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724DC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724E4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724EC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724F4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724FC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972504, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972508, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972518, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972530, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972540, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00972548, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972550, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972554, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097255C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972564, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097256C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972570, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972578, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972580, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x0097258C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972594, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097259C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725A4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725AC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725B4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725BC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725C4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725CC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725D0, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x009725D4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725E4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725E8, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x009725EC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972610, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00972614, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097261C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972620, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972698, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972700, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00974E3C, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00974E40, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00974E44, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00974E48, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00978454, K_SET, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x00978458, K_SET, B_A, 1.0f, -860.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009796EC, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009796F4, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00979A28, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00979A34, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00979A70, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00979A7C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00979A8C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00979A9C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00979DC4, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00979DC8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00979DFC, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00979E00, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097A16C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097A170, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097A1A8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097A1AC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097A1B8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097A1BC, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097A6F0, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097A6F4, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097A73C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097A740, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097BB0C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097BB58, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097BBB0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097E9A8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097E9D8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097E9E4, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097E9F0, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097EA00, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097EBB8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097EBBC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097EC18, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097EC1C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097EC48, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097EC4C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0098A15C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0098A160, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0098A494, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x0098A4A4, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0098A4A8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0098A4B4, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0098A4B8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0098A4DC, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x0098A500, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x009A3468, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009A3488, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009A3840, K_U32, B_BCEIL, 4.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3844, K_U32, B_D, 3.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3848, K_U32, B_BCEIL, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A384C, K_U32, B_D, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3854, K_U32, B_BCEIL, 4.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3858, K_U32, B_D, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A385C, K_U32, B_BCEIL, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3860, K_U32, B_D, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3868, K_U32, B_BCEIL, 4.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A386C, K_U32, B_D, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3870, K_U32, B_BCEIL, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3874, K_U32, B_D, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A387C, K_U32, B_BCEIL, 4.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3884, K_U32, B_BCEIL, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3888, K_U32, B_D, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3894, K_U32, B_D, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3898, K_U32, B_BCEIL, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A389C, K_U32, B_D, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38AC, K_U32, B_BCEIL, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38B0, K_U32, B_D, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38B8, K_U32, B_BCEIL, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38BC, K_U32, B_D, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38C0, K_U32, B_BCEIL, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38C4, K_U32, B_D, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38CC, K_U32, B_BCEIL, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38D4, K_U32, B_BCEIL, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38D8, K_U32, B_D, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A6900, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009A6908, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009A690C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009A6914, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009B8D0C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009B8D54, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009B8DC4, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009B8DDC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009D0040, K_SET, B_A, 0.5f, -128.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009D0044, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009F0A80, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009F0A88, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009F0A90, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.delay", B_LIT },
  { 0x009F0A98, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.delay", B_LIT },
  { 0x009F0ADC, K_SET, B_A, 1.0f, -157.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009F0AF4, K_SET, B_A, 1.0f, -288.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009F0B0C, K_SET, B_A, 1.0f, -157.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009F0B24, K_SET, B_A, 1.0f, -288.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009F0B3C, K_SET, B_A, 1.0f, -128.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009F0B5C, K_SET, B_A, 1.0f, -128.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF080, K_SET, B_A, 1.0f, -144.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF084, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF098, K_SET, B_A, 1.0f, -144.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF09C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF0B0, K_SET, B_A, 1.0f, -16.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF0B4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF0C8, K_SET, B_A, 1.0f, -16.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF0CC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF0E0, K_SET, B_A, 1.0f, -272.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF0E4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF0F8, K_SET, B_A, 1.0f, -272.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF0FC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF110, K_SET, B_A, 1.0f, -143.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF114, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF128, K_SET, B_A, 1.0f, -143.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF12C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF1A8, K_SET, B_A, 1.0f, -144.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF1AC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF50C, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00A11324, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00A1133C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00A11390, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00A11398, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00A113A4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00A113AC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00A113E8, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00A113F4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00A33CD0, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x00A98498, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00A984B4, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00A984D0, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00A984EC, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00A98508, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00A98524, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  /* ---- SRC_EPHINEA : 81 rows ---- */
  { 0x004137D0, K_SET, B_A, 0.5f, -21.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000000, "csel.footer.cancel.x (.text imm; stock299 + (A-640)/2)", B_LIT },
  { 0x008F9F18, K_SET, B_A, 0.5f, -289.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x41F80000, "csel.banner.x (SELECT CHARACTER origin; stock31 + (A-6", B_LIT },
  { 0x008FA024, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row0 (1.0->hud)", B_LIT },
  { 0x008FA02C, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row1 (1.0->hud)", B_LIT },
  { 0x008FA030, K_SET, B_A, 0.5f, 275.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4414C000, "csel.xhalf 595+half (=595+A/2-320)", B_LIT },
  { 0x008FA034, K_SET, B_C, 0.5f, 1.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43710000, "csel.yhalfh 241+dh/2 (=241+C/2-240)", B_LIT },
  { 0x008FA038, K_SET, B_A, 0.5f, 275.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4414C000, "csel.xhalf 595+half", B_LIT },
  { 0x008FA03C, K_SET, B_C, 0.5f, 78.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x439F0000, "csel.yhalfh 318+dh/2 (=318+C/2-240)", B_LIT },
  { 0x008FA044, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row2 (1.0->hud)", B_LIT },
  { 0x008FA04C, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row3 (1.0->hud)", B_LIT },
  { 0x008FA050, K_SET, B_A, 0.5f, 223.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4407C000, "csel.xhalf 543+half (=543+A/2-320)", B_LIT },
  { 0x008FA054, K_SET, B_C, 0.5f, 1.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43710000, "csel.yhalfh 241+dh/2", B_LIT },
  { 0x008FA058, K_SET, B_A, 0.5f, 223.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4407C000, "csel.xhalf 543+half", B_LIT },
  { 0x008FA05C, K_SET, B_C, 0.5f, 78.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x439F0000, "csel.yhalfh 318+dh/2", B_LIT },
  { 0x008FA064, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row4 (1.0->hud)", B_LIT },
  { 0x008FA068, K_SET, B_KX, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.wmul.kx row0 (1.0->kx)", B_LIT },
  { 0x008FA074, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row5 (1.0->hud)", B_LIT },
  { 0x008FA078, K_SET, B_A, 0.5f, 275.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4414C000, "csel.xhalf 595+half", B_LIT },
  { 0x008FA07C, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2 (=73+C/2-240)", B_LIT },
  { 0x008FA080, K_SET, B_A, 0.5f, 295.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4419C000, "csel.xhalf 615+half (=615+A/2-320)", B_LIT },
  { 0x008FA084, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2", B_LIT },
  { 0x008FA088, K_SET, B_A, 0.5f, 275.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4414C000, "csel.xhalf 595+half", B_LIT },
  { 0x008FA08C, K_SET, B_C, 0.5f, 165.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43CA8000, "csel.yhalfh 405+dh/2 (=405+C/2-240)", B_LIT },
  { 0x008FA094, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row6 (1.0->hud)", B_LIT },
  { 0x008FA098, K_SET, B_KX, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.wmul.kx row1 (1.0->kx)", B_LIT },
  { 0x008FA0A4, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row7 (1.0->hud)", B_LIT },
  { 0x008FA0A8, K_SET, B_A, 0.5f, 223.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4407C000, "csel.xhalf 543+half", B_LIT },
  { 0x008FA0AC, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2", B_LIT },
  { 0x008FA0B0, K_SET, B_A, 0.5f, 223.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4407C000, "csel.xhalf 543+half", B_LIT },
  { 0x008FA0B4, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2", B_LIT },
  { 0x008FA0B8, K_SET, B_A, 0.5f, 223.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4407C000, "csel.xhalf 543+half", B_LIT },
  { 0x008FA0BC, K_SET, B_C, 0.5f, 165.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43CA8000, "csel.yhalfh 405+dh/2", B_LIT },
  { 0x008FA0C4, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row8 (1.0->hud)", B_LIT },
  { 0x008FA0C8, K_SET, B_KX, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.wmul.kx row2 (1.0->kx)", B_LIT },
  { 0x008FA0D4, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row9 (1.0->hud)", B_LIT },
  { 0x008FA0D8, K_SET, B_A, 0.5f, 139.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43E58000, "csel.panel.center.x (stock459 + half)", B_LIT },
  { 0x008FA0DC, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2", B_LIT },
  { 0x008FA0E0, K_SET, B_A, 0.5f, 166.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43F30000, "csel.panel.center.x (stock486 + half)", B_LIT },
  { 0x008FA0E4, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2", B_LIT },
  { 0x008FA0E8, K_SET, B_A, 0.5f, 139.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43E58000, "csel.panel.center.x (stock459 + half)", B_LIT },
  { 0x008FA0EC, K_SET, B_C, 0.5f, 165.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43CA8000, "csel.yhalfh 405+dh/2", B_LIT },
  { 0x008FA0F4, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row10 (1.0->hud)", B_LIT },
  { 0x008FA0F8, K_SET, B_KX, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.wmul.kx row3 (1.0->kx)", B_LIT },
  { 0x008FA104, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row11 (1.0->hud)", B_LIT },
  { 0x008FA108, K_SET, B_A, 0.5f, -289.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x41F80000, "csel.banner.x (stock31 + half)", B_LIT },
  { 0x008FA10C, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2", B_LIT },
  { 0x008FA110, K_SET, B_A, 0.5f, -289.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x41F80000, "csel.banner.x (stock31 + half)", B_LIT },
  { 0x008FA114, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2", B_LIT },
  { 0x008FA118, K_SET, B_A, 0.5f, -289.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x41F80000, "csel.banner.x (stock31 + half)", B_LIT },
  { 0x008FA11C, K_SET, B_C, 0.5f, 165.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43CA8000, "csel.yhalfh 405+dh/2", B_LIT },
  { 0x008FA124, K_SET, B_KX, 410.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43CD0000, "csel.xkx 410*kx", B_LIT },
  { 0x008FA128, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row12 (1.0->hud)", B_LIT },
  { 0x008FA12C, K_SET, B_KX, 214.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43560000, "csel.xkx 214*kx", B_LIT },
  { 0x008FA130, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row13 (1.0->hud)", B_LIT },
  { 0x008FA138, K_SET, B_C, 1.0f, -24.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43E40000, "csel.ydh 456+dh (=456+C-480)", B_LIT },
  { 0x008FA13C, K_SET, B_KX, 426.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D50000, "csel.xkx 426*kx", B_LIT },
  { 0x008FA140, K_SET, B_C, 1.0f, -55.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D48000, "csel.ydh 425+dh (=425+C-480)", B_LIT },
  { 0x008FA144, K_SET, B_KX, 213.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43550000, "csel.xkx 213*kx", B_LIT },
  { 0x008FA148, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row14 (1.0->hud)", B_LIT },
  { 0x008FA14C, K_SET, B_KX, 411.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43CD8000, "csel.xkx 411*kx", B_LIT },
  { 0x008FA150, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row15 (1.0->hud)", B_LIT },
  { 0x008FA15C, K_SET, B_KX, 229.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43650000, "csel.xkx 229*kx", B_LIT },
  { 0x008FA1A4, K_SET, B_KX, 15.6f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4179999A, "csel.xkx 15.6*kx", B_LIT },
  { 0x008FA1AC, K_SET, B_C, 0.5f, -168.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42900000, "csel.yhay (72+dh/2)*affine_y — want=(72+(C-480)/2)*aff", B_AFFINEY },
  { 0x008FA1C0, K_SET, B_KX, 462.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43E70000, "csel.xkxay 462*kx*affine_y — TWO runtime bases; want=4", B_AFFINEY },
  { 0x008FA1C4, K_SET, B_AFFINEY, 50.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42480000, "csel.ay 50*affine_y", B_LIT },
  { 0x008FA1CC, K_SET, B_AFFINEY, 40.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42200000, "csel.ay 40*affine_y", B_LIT },
  { 0x008FA1D0, K_SET, B_C, 0.5f, -141.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42C60000, "csel.yhay (99+dh/2)*affine_y — want=(99+(C-480)/2)*aff", B_AFFINEY },
  { 0x008FA1D4, K_SET, B_AFFINEY, 16.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x41800000, "csel.ay 16*affine_y", B_LIT },
  { 0x008FA1D8, K_SET, B_A, 0.5f, 235.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x440AC000, "csel.xhay (555+half)*affine_y — want=(555+(A-640)/2)*a", B_AFFINEY },
  { 0x008FA528, K_SET, B_A, 1.0f, -214.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D50000, "csel.fgright.questslider.x (stock426 + (A-640)); hs1.0", B_LIT },
  { 0x008FA52C, K_SET, B_C, 0.5f, 10.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x437A0000, "csel.details.pane.y 250+(C-480)/2 (=250+C/2-240) MOD_Y", B_LIT },
  { 0x008FABF8, K_SET, B_A, 1.0f, -208.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D80000, "csel.fgright.dressroom-confirm.x (stock432 + (A-640));", B_LIT },
  { 0x008FAC00, K_SET, B_A, 1.0f, -212.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D60000, "csel.fgright.sibling.x (stock428 + (A-640)); hs1.0 dR=", B_LIT },
  { 0x008FAC08, K_SET, B_A, 1.0f, -212.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D60000, "csel.fgright.recreate-confirm.x (stock428 + (A-640)); ", B_LIT },
  { 0x008FADC8, K_SET, B_A, 0.5f, -256.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42800000, "csel.banner.x (stock64 + half)", B_LIT },
  { 0x0091D988, K_ADD, B_ANATIVE, 1.0f, -640.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x44178000, "dr.toprow.ok_back.x  WRITTEN VALUE = stock(606.0)+ (s-", B_LIT },
  { 0x0091DC74, K_ADD, B_ANATIVE, 1.0f, -640.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x44170000, "dr.charname.field.x  WRITTEN VALUE = stock(604.0)+ (s-", B_LIT },
  { 0x0091DD1C, K_ADD, B_ANATIVE, 1.0f, -640.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x44198000, "dr.botrow.ok_back.x  WRITTEN VALUE = stock(614.0)+ (s-", B_LIT },
  { 0x0096E27C, K_SET, B_LIT, 165.0f, 0.0f, SRC_EPHINEA, GATE_STREAK_SCALE, 0x42F80000, "streak.scale.x (Ephinea byte-delta 124.0->165.0, the [", B_LIT },
  { 0x00972148, K_SET, B_A, 0.5f, -233.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42AE0000, "csel.banner.x (stock87 + half)", B_LIT },
  /* ---- SRC_TRINITY : 58 rows ---- */
  { 0x004013A7, K_SET, B_A, 1.0f, -1.0f, SRC_TRINITY, GATE_CHARSELECT, 0x441FC000, "csel.backdrop.w (far-corner X; MOD_X_R; 639->design_w-", B_LIT },
  { 0x004013AF, K_SET, B_C, 1.0f, -1.0f, SRC_TRINITY, GATE_CHARSELECT, 0x43EF8000, "csel.backdrop.h (far-corner Y; MOD_Y_B; 479->design_h-", B_LIT },
  { 0x00403114, K_SET, B_C, 1.0f, -130.0f, SRC_TRINITY, GATE_ALWAYS, 0x43AF0000, "fe.login.connstr.y MOD_Y_B (stock 350)", B_LIT },
  { 0x0041061C, K_SET, B_A, 0.5f, 112.0f, SRC_TRINITY, GATE_CHARSELECT, 0x43D80000, "csel.tabkey.detailson.x (.text imm; stock432 + half)", B_LIT },
  { 0x00410628, K_SET, B_C, 1.0f, -59.0f, SRC_TRINITY, GATE_ALWAYS, 0x43D28000, "csel.tab.details_on.y MOD_Y_B (stock 421)", B_LIT },
  { 0x0041066B, K_SET, B_A, 0.5f, 112.0f, SRC_TRINITY, GATE_CHARSELECT, 0x43D80000, "csel.tabkey.detailsoff.x (.text imm; stock432 + half)", B_LIT },
  { 0x00410672, K_SET, B_C, 1.0f, -59.0f, SRC_TRINITY, GATE_ALWAYS, 0x43D28000, "csel.tab.details_off.y MOD_Y_B (stock 421)", B_LIT },
  { 0x004137C2, K_SET, B_A, 0.5f, -164.0f, SRC_TRINITY, GATE_CHARSELECT, 0x431C0000, "csel.footer.enter.x (.text imm; stock156 + (A-640)/2);", B_LIT },
  { 0x004137C9, K_SET, B_C, 1.0f, -59.0f, SRC_TRINITY, GATE_ALWAYS, 0x43D28000, "csel.enterkey.y MOD_Y_B (stock 421)", B_LIT },
  { 0x004137D7, K_SET, B_C, 1.0f, -59.0f, SRC_TRINITY, GATE_ALWAYS, 0x43D28000, "csel.esckey.y MOD_Y_B (stock 421)", B_LIT },
  { 0x004137DE, K_SET, B_A, 0.5f, 112.0f, SRC_TRINITY, GATE_CHARSELECT, 0x43D80000, "csel.footer.details.x (.text imm orphan; stock432 + (A", B_LIT },
  { 0x004EC0BE, K_SET, B_C, 1.0f, 12.0f, SRC_TRINITY, GATE_ALWAYS, 0x43F60000, "dr.honeycomb.bottom.y MOD_Y_B (stock 492)", B_LIT },
  { 0x004ED0BF, K_SET, B_C, 1.0f, 120.0f, SRC_TRINITY, GATE_ALWAYS, 0x44160000, "dr.leftgrad.botL.y MOD_Y_B (stock 600)", B_LIT },
  { 0x004ED0D3, K_SET, B_C, 1.0f, 120.0f, SRC_TRINITY, GATE_ALWAYS, 0x44160000, "dr.leftgrad.botR.y MOD_Y_B (stock 600)", B_LIT },
  { 0x004ED144, K_SET, B_C, 1.0f, 120.0f, SRC_TRINITY, GATE_ALWAYS, 0x44160000, "dr.rightgrad.botL.y MOD_Y_B (stock 600)", B_LIT },
  { 0x004ED14E, K_SET, B_C, 1.0f, 120.0f, SRC_TRINITY, GATE_ALWAYS, 0x44160000, "dr.rightgrad.botR.y MOD_Y_B (stock 600)", B_LIT },
  { 0x0070D422, K_SET, B_C, 1.0f, -220.0f, SRC_TRINITY, GATE_ALWAYS, 0x43820000, "patch.curstatus.bar.y MOD_Y_B (stock 260)", B_LIT },
  { 0x0070D47C, K_SET, B_C, 1.0f, -116.0f, SRC_TRINITY, GATE_ALWAYS, 0x43B60000, "patch.allstatus.bar.y MOD_Y_B (stock 364)", B_LIT },
  { 0x0070D4D6, K_SET, B_C, 1.0f, -250.0f, SRC_TRINITY, GATE_CHARSELECT, 0x43660000, "patch.current-status.title.y (MOD_Y_B: C-(480-230)); .", B_LIT },
  { 0x0070D4E0, K_SET, B_C, 1.0f, -200.0f, SRC_TRINITY, GATE_CHARSELECT, 0x438C0000, "patch.current-status.progress.y (MOD_Y_B: C-(480-280))", B_LIT },
  { 0x0070D4F4, K_SET, B_C, 1.0f, -146.0f, SRC_TRINITY, GATE_CHARSELECT, 0x43A70000, "patch.all-status.title.y (MOD_Y_B: C-(480-334)); .text", B_LIT },
  { 0x0070D508, K_SET, B_C, 1.0f, -96.0f, SRC_TRINITY, GATE_CHARSELECT, 0x43C00000, "patch.all-status.progress.y (MOD_Y_B: C-(480-384)); .t", B_LIT },
  { 0x00790AF5, K_SET, B_A, 0.5f, -160.0f, SRC_TRINITY, GATE_ALWAYS, 0x43200000, "cfg.kbd_overwrite_custom_settings.x (.text MOD_X_C)", B_LIT },
  { 0x00799ECA, K_SET, B_A, 0.5f, -140.0f, SRC_TRINITY, GATE_ALWAYS, 0x43340000, "cfg.leave_team_confirm.x (.text MOD_X_C)", B_LIT },
  { 0x008F9E80, K_SET, B_C, 1.0f, -110.0f, SRC_TRINITY, GATE_ALWAYS, 0x43B90000, "csel.pleaseselect.y MOD_Y_B (stock 370)", B_LIT },
  { 0x0091DAE8, K_SET, B_C, 1.0f, 91.5f, SRC_TRINITY, GATE_ALWAYS, 0x440EE000, "dr.transition.honeycomb.y MOD_Y_B (stock 571.5)", B_LIT },
  { 0x0091DC70, K_SET, B_C, 1.0f, -84.0f, SRC_TRINITY, GATE_ALWAYS, 0x43C60000, "dr.charname.field.y MOD_Y_B (stock 396)", B_LIT },
  { 0x0091DC84, K_SET, B_C, 1.0f, -84.0f, SRC_TRINITY, GATE_ALWAYS, 0x43C60000, "dr.inputname.field.y MOD_Y_B (stock 396)", B_LIT },
  { 0x0091DD20, K_SET, B_C, 1.0f, -46.0f, SRC_TRINITY, GATE_ALWAYS, 0x43D90000, "dr.bottom.okback.y MOD_Y_B (stock 434)", B_LIT },
  { 0x0091E194, K_SET, B_C, 1.0f, -64.0f, SRC_TRINITY, GATE_ALWAYS, 0x43D00000, "dr.grayline.y MOD_Y_B (stock 416)", B_LIT },
  { 0x0096E52C, K_SET, B_A, 1.0f, 40.0f, SRC_TRINITY, GATE_ALWAYS, 0x442A0000, "fe.fixedchat.next.x MOD_X_R +right (stock 680)", B_LIT },
  { 0x0096FFFC, K_SET, B_A, 0.5f, 0.0f, SRC_TRINITY, GATE_ALWAYS, 0x43A00000, "fe.f1help.x MOD_X_C +half", B_LIT },
  { 0x00970FF0, K_SET, B_C, 1.0f, -550.0f, SRC_TRINITY, GATE_ALWAYS, 0xC28C0000, "fe.ime.id.y MOD_Y_B (stock -70)", B_LIT },
  { 0x00971350, K_SET, B_A, 0.5f, 69.0f, SRC_TRINITY, GATE_ALWAYS, 0x43C28000, "ig.create_party.party_mode_submenu.x (MOD_X_C)", B_LIT },
  { 0x00971358, K_SET, B_A, 0.5f, 69.0f, SRC_TRINITY, GATE_ALWAYS, 0x43C28000, "ig.create_party.difficulty_submenu.x (MOD_X_C)", B_LIT },
  { 0x00972070, K_SET, B_A, 0.5f, -188.0f, SRC_TRINITY, GATE_ALWAYS, 0x43040000, "ig.battle_result.x (MOD_X_C)", B_LIT },
  { 0x00972128, K_SET, B_A, 1.0f, -252.0f, SRC_TRINITY, GATE_ALWAYS, 0x43C20000, "ig.cmode_records.x (MOD_X_R)", B_LIT },
  { 0x00972138, K_SET, B_A, 0.5f, -50.0f, SRC_TRINITY, GATE_ALWAYS, 0x43870000, "ig.cmode_area_number_popup.x (MOD_X_C)", B_LIT },
  { 0x009721E0, K_SET, B_A, 0.5f, -169.0f, SRC_TRINITY, GATE_ALWAYS, 0x43170000, "ig.in_battle_description.x (MOD_X_C)", B_LIT },
  { 0x009721F8, K_SET, B_A, 0.5f, -32.0f, SRC_TRINITY, GATE_ALWAYS, 0x43900000, "ig.cmode_reward_dialog.x (MOD_X_C)", B_LIT },
  { 0x00972200, K_SET, B_A, 0.5f, -221.0f, SRC_TRINITY, GATE_ALWAYS, 0x42C60000, "ig.cmode_srank_name_dialog.x (MOD_X_C)", B_LIT },
  { 0x00972208, K_SET, B_A, 0.5f, 39.0f, SRC_TRINITY, GATE_ALWAYS, 0x43B38000, "ig.cmode_reward_dialog_submenu.x (MOD_X_C)", B_LIT },
  { 0x00972510, K_SET, B_A, 0.5f, -50.0f, SRC_TRINITY, GATE_CHARSELECT, 0x43870000, "lobby.soccer.score.x (MOD_X_C: stock270 + (A-640)/2)", B_LIT },
  { 0x00972538, K_SET, B_A, 0.5f, -139.0f, SRC_TRINITY, GATE_ALWAYS, 0x43350000, "ig.info_counter_create_party.x (MOD_X_C)", B_LIT },
  { 0x00972568, K_SET, B_A, 1.0f, -245.0f, SRC_TRINITY, GATE_ALWAYS, 0x43C58000, "ig.battle_disconnect_dlg.x (MOD_X_R)", B_LIT },
  { 0x009725D8, K_SET, B_A, 0.5f, -59.0f, SRC_TRINITY, GATE_ALWAYS, 0x43828000, "ig.cmode_srank_name_caret.x (MOD_X_C)", B_LIT },
  { 0x009725F0, K_SET, B_A, 0.5f, -279.0f, SRC_TRINITY, GATE_CHARSELECT, 0x42240000, "shipsel.login-error.x (MOD_X_C: stock41 + (A-640)/2)", B_LIT },
  { 0x009725F4, K_SET, B_C, 0.5f, -112.0f, SRC_TRINITY, GATE_ALWAYS, 0x43000000, "shipselect.loginerror.y MOD_Y_C +half (stock 128)", B_LIT },
  { 0x009725F8, K_SET, B_A, 0.5f, -221.0f, SRC_TRINITY, GATE_ALWAYS, 0x42C60000, "ig.join_room_password_request.x (MOD_X_C)", B_LIT },
  { 0x00972600, K_SET, B_A, 0.5f, -211.0f, SRC_TRINITY, GATE_ALWAYS, 0x42DA0000, "ig.join_room_password_caret.x (MOD_X_C)", B_LIT },
  { 0x00972638, K_SET, B_A, 0.5f, 37.0f, SRC_TRINITY, GATE_ALWAYS, 0x43B28000, "ig.create_party_name_field.x (MOD_X_C)", B_LIT },
  { 0x00972640, K_SET, B_A, 0.5f, 37.0f, SRC_TRINITY, GATE_ALWAYS, 0x43B28000, "ig.create_party_password_field.x (MOD_X_C)", B_LIT },
  { 0x00972688, K_SET, B_A, 0.5f, -289.0f, SRC_TRINITY, GATE_ALWAYS, 0x41F80000, "ig.team_invitation_explanation.x (MOD_X_C)", B_LIT },
  { 0x00979BCC, K_SET, B_A, 0.5f, 0.0f, SRC_TRINITY, GATE_ALWAYS, 0x43A00000, "ig.battle_countdown_texture.x (MOD_X_C; offset 0)", B_LIT },
  { 0x0097E458, K_SET, B_C, 1.0f, -90.0f, SRC_TRINITY, GATE_ALWAYS, 0x43C30000, "fe.login.botright.y MOD_Y_B (stock 390)", B_LIT },
  { 0x0097E468, K_SET, B_A, 1.0f, -95.0f, SRC_TRINITY, GATE_ALWAYS, 0x44084000, "fe.login.botright.x MOD_X_R +right (stock 545)", B_LIT },
  { 0x009F24E4, K_SET, B_A, 0.5f, 168.0f, SRC_TRINITY, GATE_ALWAYS, 0x43F40000, "ig.team_invitation_explanation_submenu.x (MOD_X_C)", B_LIT },
  { 0x009F986C, K_SET, B_A, 0.5f, 0.0f, SRC_TRINITY, GATE_ALWAYS, 0x43A00000, "ig.battle_countdown_bg_texture.x (MOD_X_C; offset 0)", B_LIT },
  /* ---- SRC_OURS : 27 rows ---- */
  { 0x004EC0AF, K_ADD, B_A, 1.0f, -640.0f, SRC_OURS, GATE_CHARSELECT, 0x44230000, "dr.honeycomb.enter_exit.right_edge  WRITTEN VALUE = st", B_LIT },
  { 0x004EC951, K_ADD, B_A, 1.0f, -640.0f, SRC_OURS, GATE_CHARSELECT, 0x442A0000, "dr.honeycomb.transition.right_edge  WRITTEN VALUE = st", B_LIT },
  { 0x006F49FD, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.seal.outer.mag (code imm32 of `push 430.0`; ONE-S", B_LIT },
  { 0x006F4A57, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.seal.inner.mag (code imm32 of `push 178.0`; ONE-S", B_LIT },
  { 0x00719F96, K_NOP, B_LIT, 0.0f, 0.0f, SRC_OURS, GATE_ALWAYS, 0x1115BDE8, "bb.line.cleanup.1; NOP 5 bytes over CALL 0x0082b558 (t", B_LIT },
  { 0x00719FD4, K_NOP, B_LIT, 0.0f, 0.0f, SRC_OURS, GATE_ALWAYS, 0x11157FE8, "bb.line.cleanup.2; NOP 5 bytes over CALL 0x0082b558 (t", B_LIT },
  { 0x00721FC0, K_SET, B_A, 1.0f, 0.0f, SRC_OURS, GATE_ALWAYS, 0x44200000, "lobby.nnbar.rightEdgeX (= dw; 640 4:3-ceiling raised t", B_LIT },
  { 0x007583C1, K_SET, B_A, 1.0f, 10.0f, SRC_OURS, GATE_ALWAYS, 0x44228000, "login.menu.primaryX1 (a_plus_10 = dw+10)", B_LIT },
  { 0x007583F3, K_SET, B_A, 1.0f, 10.0f, SRC_OURS, GATE_ALWAYS, 0x44228000, "login.menu.secondaryX2 (a_plus_10 = dw+10)", B_LIT },
  { 0x0075840B, K_SET, B_A, 1.0f, 10.0f, SRC_OURS, GATE_ALWAYS, 0x44228000, "login.menu.animTarget (a_plus_10 = dw+10)", B_LIT },
  { 0x0096E114, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.orb.size (live read*hud_scale, ONE-SHOT; no fixed", B_LIT },
  { 0x0096E168, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.orb.ofs[0] (table -137,-79,137,-79,0,156; this fl", B_LIT },
  { 0x0096E16C, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.orb.ofs[1] (stock -79; ONE-SHOT live*hud_scale)", B_LIT },
  { 0x0096E170, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.orb.ofs[2] (stock 137; ONE-SHOT live*hud_scale)", B_LIT },
  { 0x0096E174, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.orb.ofs[3] (stock -79; ONE-SHOT live*hud_scale)", B_LIT },
  { 0x0096E178, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.orb.ofs[4] (stock 0; ONE-SHOT live*hud_scale)", B_LIT },
  { 0x0096E17C, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.orb.ofs[5] (stock 156; ONE-SHOT live*hud_scale)", B_LIT },
  { 0x009A3420, K_SET, B_A, 0.0f, 0.0f, SRC_OURS, GATE_SPLASH, 0x00000000, "splash.e0.x1; stock 0.0 * stretch. stretch=4/3 @hs1.0 ", B_LIT },
  { 0x009A3428, K_SET, B_A, 0.4984375f, 0.0f, SRC_OURS, GATE_SPLASH, 0x00000000, "splash.e0.x2; stock 319.0 * stretch. At hs!=1.0 = 319*", B_LIT },
  { 0x009A3440, K_SET, B_A, 0.0f, 0.0f, SRC_OURS, GATE_SPLASH, 0x00000000, "splash.e1.x1; stock 0.0 * stretch -> 0. No sig-guard. ", B_LIT },
  { 0x009A3448, K_SET, B_A, 0.4984375f, 0.0f, SRC_OURS, GATE_SPLASH, 0x00000000, "splash.e1.x2; stock 319.0 * stretch = 319*A/640 @hs!=1", B_LIT },
  { 0x009A3460, K_SET, B_A, 0.5f, 0.0f, SRC_OURS, GATE_SPLASH, 0x00000000, "splash.e2.x1; stock 320.0 * stretch = 320*A/640 @hs!=1", B_LIT },
  { 0x009A3480, K_SET, B_A, 0.5f, 0.0f, SRC_OURS, GATE_SPLASH, 0x00000000, "splash.e3.x1; stock 320.0 * stretch = 320*A/640 @hs!=1", B_LIT },
  { 0x00A1132C, K_SET, B_LIT, 128.0f, 0.0f, SRC_OURS, GATE_MINIMAP, 0x00000000, "minimap.vp.w = const float w=128.0 (NOT a design-X coo", B_LIT },
  { 0x00A11330, K_SET, B_LIT, 128.0f, 0.0f, SRC_OURS, GATE_MINIMAP, 0x00000000, "minimap.vp.h = const float h=128.0 (NOT a design coord", B_LIT },
  { 0x00A11400, K_SET, B_LIT, 128.0f, 0.0f, SRC_OURS, GATE_MINIMAP, 0x00000000, "minimap.bg.w = const float w=128.0 (NOT a design-X coo", B_LIT },
  { 0x00A11404, K_SET, B_LIT, 128.0f, 0.0f, SRC_OURS, GATE_MINIMAP, 0x00000000, "minimap.bg.h = const float h=128.0 (NOT a design coord", B_LIT },
};


/* ============================================================================
 * RECODE CORE (staging) — resolve_base / gate_on / apply_bakes.
 * Folds into pso_widescreen.c after kBakes[]. Assumes: g_cfg (config struct),
 * ws_scale_ctx, log_line, kBakes[]. No <math.h> (B_BCEIL uses anzz1's +0.9999
 * integer ceil, matching its Bi).
 * ========================================================================== */

#define ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t f32_bits(float v) { uint32_t u; memcpy(&u, &v, 4); return u; }

/* resolve a row's value base from the live scale ctx.
   At 4:3 (hud_scale derives A=640, C=480, B=128, D=128, AR=4/3) every base
   returns its stock-equivalent, so coeff*base+offset == the stock value and the
   value-guard turns the write into a no-op — the whole table is 4:3-identical. */
static float resolve_base(uint8_t base, const ws_scale_ctx *s)
{
    switch (base) {
        case B_A:        return s->A;
        case B_C:        return s->C;
        case B_B:        return s->B;
        case B_BCEIL:    return (float)(DWORD)(s->B + 0.9999f);  /* == anzz1 Bi */
        case B_D:        return (float)s->D;
        case B_AR:       return s->game_aspect;
        case B_RW:       return (float)s->render_w;
        case B_RH:       return (float)s->render_h;
        case B_HUDSCALE: return s->hud_scale;
        case B_KX:       return s->hud_scale * ((16.0f / 9.0f) / (4.0f / 3.0f));
        case B_AFFINEY:  return (s->render_h > 0) ? ((float)s->render_h / 480.0f) : 1.0f;
        case B_ANATIVE:  return (s->hud_scale > 0.01f) ? (s->A / s->hud_scale) : s->A;
        case B_CNATIVE:  return (s->hud_scale > 0.01f) ? (s->C / s->hud_scale) : s->C;
        case B_LIT:
        default:         return 1.0f;
    }
}

/* a row's gate bit maps to one cfg toggle; GATE_ALWAYS is unconditional. */
static int gate_on(uint8_t gate)
{
    switch (gate) {
        case GATE_ALWAYS:       return 1;
        case GATE_CHARSELECT:   return g_cfg.patch_charselect;
        case GATE_MINIMAP:      return g_cfg.patch_minimap;
        case GATE_RUNE:         return g_cfg.patch_rune_scale;
        case GATE_STREAK_SCALE: return g_cfg.patch_streak_scale;
        case GATE_HANGAME:      return g_cfg.patch_hangame_title_menu;
        case GATE_SPLASH:       return g_cfg.patch_splash_phase2;
        default:                return 1;
    }
}

/* ADD/MUL rows are RMW and would double on re-run; we snapshot each one's stock
   on the FIRST apply, so the written value is the absolute stock+delta / stock*f.
   That makes the WHOLE table idempotent and lets the overwrite-guard re-run it. */
static float   g_stock_cache[ARRAYLEN(kBakes)];
static uint8_t g_stock_valid[ARRAYLEN(kBakes)];

/* apply_bakes — the ONE value-guarded pass over kBakes[]. Safe to re-run. */
static void apply_bakes(const ws_scale_ctx *s)
{
    int n_set = 0, n_delta = 0, n_skip = 0, n_guard = 0;
    for (size_t i = 0; i < ARRAYLEN(kBakes); i++) {
        const bake_t *b = &kBakes[i];
        if (!gate_on(b->gate)) continue;

        __try {
            float expr = (b->coeff * resolve_base(b->base, s) + b->offset)
                       * resolve_base(b->base2, s);   /* base2 == B_LIT (1.0) for most rows */
            uint32_t want;

            if (b->kind == K_NOP) {
                want = 0;  /* sentinel; handled below */
            } else if (b->kind == K_U32) {
                want = (uint32_t)expr;
            } else if (b->kind == K_ADD || b->kind == K_MUL) {
                if (!g_stock_valid[i]) {
                    /* first sight: optional sig-guard, then snapshot stock */
                    uint32_t live = *(volatile uint32_t *)(uintptr_t)b->va;
                    if (b->stock && live != b->stock) { n_guard++; continue; }
                    memcpy(&g_stock_cache[i], &live, 4);
                    g_stock_valid[i] = 1;
                }
                float stock = g_stock_cache[i];
                float v = (b->kind == K_ADD) ? (stock + expr) : (stock * expr);
                want = f32_bits(v);
            } else { /* K_SET / K_AR */
                want = f32_bits(expr);
            }

            uint32_t live = *(volatile uint32_t *)(uintptr_t)b->va;
            if (b->kind != K_NOP && live == want) { n_skip++; continue; }  /* idempotent */
            /* SET-class sig-guard: refuse a foreign site (neither stock nor ours) */
            if (b->stock && (b->kind == K_SET || b->kind == K_U32) &&
                live != b->stock && live != want) { n_guard++; continue; }

            DWORD old, tmp;
            size_t len = (b->kind == K_NOP) ? 5 : 4;
            if (!VirtualProtect((LPVOID)(uintptr_t)b->va, len, PAGE_EXECUTE_READWRITE, &old)) continue;
            if (b->kind == K_NOP) memset((void *)(uintptr_t)b->va, 0x90, 5);
            else                  *(volatile uint32_t *)(uintptr_t)b->va = want;
            VirtualProtect((LPVOID)(uintptr_t)b->va, len, old, &tmp);
            if (b->va < 0x008F8000u)  /* .text => flush icache (anzz1's split) */
                FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)b->va, len);

            if (b->kind == K_ADD || b->kind == K_MUL) n_delta++; else n_set++;
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    log_line("[ws] apply_bakes: %d set, %d delta, %d skip, %d guard (of %d rows)",
             n_set, n_delta, n_skip, n_guard, (int)ARRAYLEN(kBakes));
    apply_special(s);
}

/* sp_write_f32 — value-guarded float write (the apply_special companion). */
static void sp_write_f32(uint32_t va, float v)
{
    DWORD old, tmp;
    __try {
        if (*(volatile uint32_t *)(uintptr_t)va == f32_bits(v)) return;
        if (!VirtualProtect((LPVOID)(uintptr_t)va, 4, PAGE_EXECUTE_READWRITE, &old)) return;
        *(volatile float *)(uintptr_t)va = v;
        VirtualProtect((LPVOID)(uintptr_t)va, 4, old, &tmp);
        if (va < 0x008F8000u)
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)va, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

/* apply_special — the 9 rows that aren't (coeff*base+offset)*base2: they read
   live cfg (minimap), a runtime ratio (in-game affine), or branch on aspect
   (hangame). Called at the tail of apply_bakes. NOTE: verify the hangame and
   FVPRESET formulas against patch_hangame_title_menu_layout / apply_startup_bakes
   when folding (placeholder uses the documented widescreen forms). */
static void apply_special(const ws_scale_ctx *s)
{
    /* minimap (ours): positions = ui_coord(cfg coord); zoom = raw cfg literal. */
    if (g_cfg.patch_minimap) {
        sp_write_f32(0x00A113E8, ui_coord(g_cfg.minimap_bg_x));
        sp_write_f32(0x00A113EC, ui_coord(g_cfg.minimap_bg_y));
        sp_write_f32(0x00A11324, ui_coord(g_cfg.minimap_vp_x));
        sp_write_f32(0x00A11328, ui_coord(g_cfg.minimap_vp_y));
        sp_write_f32(0x00804A5D, g_cfg.minimap_zoom);   /* .text imm; not scaled */
    }
    /* in-game 2D affine preset (F_VPRESET) = render_w / design_w; design_w == s->A
       (set by the kBakes design_w row that ran just before us). */
    if (s->A > 1.0f) {
        float ig_aff = (float)s->render_w / s->A;
        sp_write_f32(0x0082F8F4, ig_aff);
        sp_write_f32(0x0082F914, ig_aff);
    }
    /* hangame title menu (Login/Start Game/...): widescreen places it from the
       wide extents; the 4:3 fallback never fires here (apply_* only run enabled). */
    if (g_cfg.patch_hangame_title_menu) {
        if (s->A > 640.0f) {
            sp_write_f32(0x00974E08, s->A * 0.5f - 110.0f);
            sp_write_f32(0x00974E0C, s->C - 140.0f);
        } else {
            sp_write_f32(0x00974E08, ui_coord(g_cfg.hangame_menu_x));
            sp_write_f32(0x00974E0C, ui_coord(g_cfg.hangame_menu_y));
        }
    }
}


static void apply_static_patches(const ws_scale_ctx *s)
{
    // (1) front-end startup source bakes — the char-create description-TEXT
    // bake (not a flat write). Runs FIRST; apply_bakes overwrites any overlap.
    apply_startup_bakes(s->render_w);

    // (2) THE unified table — authoritative coordinate source of truth (folds
    // anzz1's 6 lists + the ~20 patch_* flat-write helpers). Value-guarded,
    // 4:3-identity by construction; calls apply_special() internally.
    apply_bakes(s);

    // (3) char-create hex-backdrop 16:9 fill — install the tail-splice on
    // RenderChallengePanelBackground_004ed008 that emits extra static
    // background-tile column(s) at design X=640.. until the columns cover
    // design_w. NOT a per-frame composer poke: it drives the engine's own
    // SetChallengePanelScale with a static {X,Y} struct (the same emit path the
    // engine uses for its 8 tiles). Runtime-gated to char-create (screen-id==12)
    // AND design_w>640 (4:3 no-op), so the always-present .text splice is inert
    // everywhere else. Shares the PatchCharSelect escape hatch.
    if (g_cfg.patch_charselect) {
        // (1c) char-create BACKDROP fill — REWORKED from duplicate-column fill
        // to a true horizontal STRETCH (owner: "stretch quads, don't duplicate").
        // The duplicate-fill splice @0x004ED06D is RETIRED (cc_backdrop_install no
        // longer called); the stretch is folded into cc_box_x_install's stub, which
        // now scales each backdrop tile's X-position AND X-scale by kx=design_w/640
        // (id 0x0C..0x11, screen-id 12). That also SUBSUMES the old self-inflicted
        // +128 LOOP2 shift (the box splice was grabbing the design-X=384 backdrop
        // tiles), closing the internal 384..512 gap without any extra columns.
        // (1d) char-create class-select INFO BOX rigid right-shift + BACKDROP stretch —
        // one .text splice on RenderChallengePanelElement_004e9d8c. Box elements
        // (design-X>=300) shift by CC_RIGHTPANE_X_FACTOR*(design_w-640); backdrop
        // tiles (depth 0xC61B3C00) stretch X by design_w/640 AND height by
        // design_h/480. screen-id==12 gated; 4:3 (design_w<=640) install no-op.
        cc_box_x_install();
        // (1e) char-create hex BACKDROP vertical bottom-fill — companion
        // splice @0x004e9df2 that scales each backdrop tile's Y-POSITION by
        // design_h/480 so the 4:3-authored hex plane (design Y 0..576) reaches the
        // bottom of a 16:9 frame (was solid black below ~55%). Same screen-id==12 +
        // depth==0xC61B3C00 gate; reads g_cc_ky set by cc_box_x_install (must run
        // AFTER it); 4:3 / ky<=1.0 => install no-op. Title hex is a DIFFERENT path
        // (sub_00415ab0 -> RenderUIQuad 0x0082B440), so it is untouched.
        cc_box_y_install();
    } else {
        log_line("[pso_widescreen] cc-backdrop hex-fill: SKIP (PatchCharSelect=0)");
    }

    // (4) psobb.io patch-server news/status screen (Trinity AdDrawLineTask
    // @0x00408C9D): right/bottom-anchor the MOTD box geometry. This stays a
    // CALL/code patch (not a flat .data write), so it is NOT in kBakes[].
    patch_ad_draw_line(s);

    // (Char-create class-select layout is an authoritative-source bake now —
    // folded into apply_startup_bakes() above (called right after the anzz1 bake),
    // which rewrites the 2 .text scene-init immediates + 2 .rdata portrait floats
    // alongside the Scene-02 login seeds. No render-thread hook: 0x0082BB74 /
    // 0x0082B440 are byte-stock.)

    // (4) Ephinea in-game detour-handler ports (FUN_52da6e50 / FUN_52d8cb90),
    // . Default-ON with widescreen; value/byte-guarded 4:3 no-op.
    //
    // (4a) KILL-SCREEN HEXAGON-LAYER aspect-fit pre-pass (Ephinea FUN_52da6e50) —
    // one-shot boot CALL-site redirect of 0x0067C4ED (NOT a kStripSites
    // detour, so NOT wiped by FE->IG). Identity at true 4:3 (else-branch
    // k=1,off=0), so install only when the engine aspect is wider than 4:3.
    if (s->enabled && s->game_aspect > (4.0f / 3.0f) + 0.001f) {
        ks_hexfit_install();
    } else {
        log_line("[pso_widescreen] killscreen hexfit: SKIP (disabled or 4:3 identity)");
    }

    // (4b) IN-GAME LIST-WINDOW bottom-anchor (Ephinea FUN_52d8cb90) — boot CALL-
    // site redirect of 0x0073FF92 (`call 0x00737F80`, ==1 arm). Self-no-ops
    // at design_h<=480 (the Y term is 0), so it is safe to install always;
    // it bottom-anchors only when design_h>480 (in-game @hs>1.0). RE-ASSERTED
    // every in-game worker tick (reassert_ingame_hooks) since this is a
    // code-flow detour the FE->IG transition wipes.
    if (s->enabled) {
        if (lw_yanchor_redirect())
            log_line("[pso_widescreen] listwindow y-anchor: installed @0x0073FF92");
    } else {
        log_line("[pso_widescreen] listwindow y-anchor: SKIP (disabled)");
    }
}

// ---- In-game scale + hook primitives ----------------------------------
static float hs_peek_f32(uint32_t va)
{
    return *(volatile float *)(uintptr_t)va;
}

// Value-guarded float store to .data: writes only on change (idempotent),
// a single aligned 4-byte store (atomic on x86), VirtualProtect-wrapped.
static void hs_poke_f32(uint32_t va, float v)
{
    volatile float *p = (volatile float *)(uintptr_t)va;
    __try {
        if (*p == v) return;                       // value-guard (idempotent)
        DWORD old;
        if (!VirtualProtect((LPVOID)(uintptr_t)va, 4, PAGE_EXECUTE_READWRITE, &old))
            return;
        *p = v;
        DWORD tmp; VirtualProtect((LPVOID)(uintptr_t)va, 4, old, &tmp);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// ---- Scene/gate globals ------------------------------
#define HS_VA_PLAYER_ARRAY 0x00A94254u   // 12-slot in-game player array head
#define HS_VA_SCENE_IDX    0x00AAFC9Cu   // G_SCENE_IDX (0 = front-end)
#define HS_VA_QUEST_LOAD   0x00AAB378u   // quest/area-loading flag (!=0 while loading)
#define HS_VA_DESIGN_W     0x0098A4B8u   // design_w  (853.33 native / 1280 @hs1.5)
#define HS_VA_DESIGN_H     0x0098A4B4u   // design_h  (480 native / 720 @hs1.5)
#define HS_VA_AFFINE_X     0x00ACC0E8u   // 2D affine SCALE_X
#define HS_VA_AFFINE_Y     0x00ACC0ECu   // 2D affine SCALE_Y
#define HS_VA_UIQUAD       0x0082B440u   // RenderUIQuad entry (stock first byte 0x83)
#define HS_UIQUAD_STOCK_B0 0x83u

// hs_ingame() — true iff we are in a live lobby/area (player object exists).
// 12-slot scan of the player array 0x00A94254: any slot holding a heap-range
// ptr [0x00400000,0x40000000) means a live player object exists -> in-game.
// Reads are SEH-guarded (the array head can be garbage very early in a scene).
static int hs_ingame(void)
{
    __try {
        const volatile uint32_t *pa = (const volatile uint32_t *)(uintptr_t)HS_VA_PLAYER_ARRAY;
        int i;
        for (i = 0; i < 12; i++) {
            uint32_t p = pa[i];
            if (p >= 0x00400000u && p < 0x40000000u)   /* live player object ptr */
                return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return 0;
}

// Data-only in-game scale pin: writes design_w/h + 2D affine derived from
// HudScale, value-guarded. Called once per FE->IG transition, and only when
// hud_scale != 1.0 (the front-end is owned by the static bake).
static void worker_scale_poke(int in_game)
{
    if (!in_game) return;                          // front-end: leave to W1 + static bake

    float hs = g_cfg.hud_scale;
    if (!(hs > 0.1f && hs < 10.0f)) hs = 1.0f;

    const float aspect = 16.0f / 9.0f;
    float dw  = (aspect / (4.0f / 3.0f)) * 640.0f * hs;  // 1280 @hs1.5
    float dh  = 480.0f * hs;                             //  720 @hs1.5
    float aff = 1920.0f / dw;                            //  1.5 @hs1.5
    hs_poke_f32(HS_VA_DESIGN_W, dw);
    hs_poke_f32(HS_VA_DESIGN_H, dh);
    hs_poke_f32(HS_VA_AFFINE_X, aff);
    hs_poke_f32(HS_VA_AFFINE_Y, aff);              // SCALE_Y clamped = SCALE_X
}

// ---- reassert_ingame_hooks — re-install wiped detours ------
// Re-installs the effect-deanchor CALL redirects (kStripSites -> rb_deanchor_shim)
// that the FE->IG transition WIPES back to stock (`call 0x0082B158`). Idempotent +
// steady-state quiet: we only attempt the redirect on a site that has been wiped
// BACK to the stock target (current call dest == 0x0082B158); a site already
// pointing at our shim is silently skipped (no redirect_call mismatch-log spam).
// Returns a bitmask of the sites (re)installed THIS call (0 in steady state).
//
// This is the missing-piece for the in-game 16:9 wide-fill: the affine source
// bakes (apply_startup_bakes) make the HUD wide and survive the transition, but
// the deanchor that keeps photon/screen-space effects from inheriting that wide
// affine is a code-flow detour the transition reverts — so it must be re-asserted
// from a context the transition can't kill (the worker), every in-game tick.
static unsigned reassert_ingame_hooks(void)
{
    unsigned did = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t site = kStripSites[i];
        uint8_t *p = (uint8_t *)(uintptr_t)site;
        __try {
            if (p[0] != 0xE8) continue;                         // not a CALL (unexpected) -> leave
            uint32_t cur = (uint32_t)(site + 5 + *(int32_t *)(p + 1));
            if (cur != 0x0082B158u) continue;                   // already ours (or foreign) -> skip quietly
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        // site is stock (`call 0x0082B158`): (re)install our deanchor wrap.
        if (redirect_call(site, 0x0082B158u, (void *)&rb_deanchor_shim))
            did |= (1u << i);
    }
    // ALSO re-assert the in-game list-window bottom-anchor (Ephinea FUN_52d8cb90
    // port @0x0073FF92) — it is a code-flow detour wiped by the FE->IG transition.
    // lw_yanchor_redirect() is itself stock-guarded + steady-state quiet, and the
    // shim self-no-ops at design_h<=480 (so it is harmless when re-asserted at
    // hud_scale 1.0 in-game). bit 3 reports a (re)install this tick.
    if (g_cfg.enabled && lw_yanchor_redirect())
        did |= (1u << 3);
    return did;
}

// In-game re-assert. The FE->IG scene transition reverts our deanchor + ListWindow-Y
// CALL-site redirects to stock and resets the in-game design_w/h + affine globals.
// Re-applies them ONCE per transition: armed on the FE->IG rising edge, fired after
// the area-load flag clears. No polling, no per-frame writes.
static void ingame_reassert_on_transition(void)
{
    static int s_prev_ig = 0;
    static int s_pending = 0;
    __try {
        int ig      = hs_ingame();
        int loading = (*(volatile uint32_t *)(uintptr_t)HS_VA_QUEST_LOAD != 0u);
        if (ig && !s_prev_ig) s_pending = 1;   // FE->IG rising edge: arm
        if (!ig)              s_pending = 0;    // back to front-end: disarm
        if (s_pending && ig && !loading) {      // fire once, after the area loads
            reassert_ingame_hooks();            // deanchor + ListWindow-Y redirects
            if (!(g_cfg.hud_scale > 0.999f && g_cfg.hud_scale < 1.001f))
                worker_scale_poke(1);           // design_w/h + affine pin
            s_pending = 0;
        }
        s_prev_ig = ig;

        /* clobber re-assert: a d3d8 wrapper or another ASI may re-init .data and
           revert our design_w (0x0098A4B8) to stock 640.0. One read/frame; re-run
           the whole table only on an actual revert (apply_bakes is idempotent). */
        if (g_scale.enabled && g_scale.A != 640.0f &&
            *(volatile float *)0x0098A4B8 == 640.0f) {
            log_line("[ws] clobber: design_w reverted to 640 -> re-asserting kBakes");
            apply_bakes(&g_scale);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// apply_engine_patches — every code-flow detour (CALL-site rewrite, IAT thunk,
// MinHook). Mechanism-classified, NOT by any anzz1/sodaboy label.
static void apply_engine_patches(const ws_scale_ctx *s)
{
    (void)s;  // reads g_cfg.hud_scale/hud_compress live inside the installers
    // Cursor-warp fix: keep the OS pointer on the DRAWN menu element under HUD
    // scale. IAT-hooks SetCursorPos. Identity no-op when both knobs are 1.0.
    install_cursor_warp_fix();
    // Char-select 3D model pan: E8 redirect of the scene's ResetNPCCamera call.
    // World-space; survives PatchCharSelect=0 (orthogonal to the 2D layout).
    patch_charselect_model_pan();
    // Char-create class-select layout: AUTHORITATIVE-SOURCE bake .
    // The two render-thread MinHooks (composer 0x0082BB74 + info-text 0x0082B440)
    // and their leaking negative gate are DELETED; every char-create content
    // element is now relocated at its TRUE static source (2 .text scene-init bake
    // immediates + 2 .rdata portrait floats), value-guarded so 4:3 is a byte-
    // identical no-op.
    //
    // NOTE startup-bake consolidation): the 4 STATIC char-create writes
    // are no longer poked from here — they were folded into apply_startup_bakes(),
    // which runs from apply_static_patches immediately after apply_anzz1_widescreen
    // (also after anzz1 owns design_w).
    //
    // NO per-frame char-create patching. The former per-frame
    // content-stride composer hook (cc_quad_fix_c @0x0082BB74) was DELETED per the
    // owner's no-per-frame-patching mandate; 0x0082BB74 is byte-stock. Char-create
    // is owned entirely by the static apply_startup_bakes() pass + stock geometry.
    // The class-select right-pane CONTENT rows are runtime-only widget data with no
    // static source, so they are intentionally not strided (no allowed mechanism).
}

// ---- Entry ----

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        load_config();
        // ---- DIAG BUILD audio-diag): FORCE file logging ON,
        // unconditionally, regardless of INI/DebugLogsEnabled. This build exists
        // to capture the real char-create intro audio/scene lifecycle from a live
        // user drive, so the log must always be written + flushed per line. This
        // is the ONLY behavioral delta vs the shipping build (md5 b890c5cc):
        // logging is forced, every other gate/suppression path is byte-identical.
        g_cfg.debug_log = 1;
        resolve_log_path();
        // Truncate log on each launch so we don't accumulate noise.
        if (g_cfg.debug_log && g_cfg.log_path[0]) {
            DeleteFileA(g_cfg.log_path);
        }
        log_line("[pso_widescreen] attach pid=%lu cfg enabled=%d windowed=%d %dx%d debug=%d verbose=%d",
                 GetCurrentProcessId(), g_cfg.enabled, g_cfg.windowed,
                 g_cfg.width, g_cfg.height, g_cfg.debug_log,
                 g_cfg.debug_log_verbose);

        // Stack detection: classify which d3d8.dll is currently providing
        // Direct3DCreate8 so downstream behavior can adapt. The d3d8to11
        // wrapper (>= handles RHW-vert NDC mapping correctly
        // via its physical-viewport divide; the canvas-hint mechanism is
        // gone. Sodaboy / native / d3d8to9 wrappers expect their own
        // submission-space rules; we don't try to second-guess them.
        detect_d3d8_stack();
        // Boot-poster init runs first and is INDEPENDENT of the master
        // Enabled switch. The widescreen patches and the boot poster are
        // separate features that share this ASI's D3D8 plumbing only —
        // a user can disable widescreen and still get the poster, or vice
        // versa. The PNG decode happens here (before any D3D init) so the
        // texture upload on first Present is just a CreateTexture+memcpy.
        boot_poster_init_from_cfg(g_cfg.boot_poster_path,
                                  g_cfg.boot_poster_enabled,
                                  g_cfg.boot_poster_disable_after_floor,
                                  g_cfg.boot_poster_disable_after_splash,
                                  g_cfg.boot_poster_max_seconds,
                                  g_cfg.boot_poster_max_screen_pct);

        // P3 video init (Stage 1). Independent of the widescreen master
        // switch, same as the boot poster. VideoEnable=0 (default) =>
        // mod_video_init early-returns dormant and on_present is a no-op.
        mod_video_init(g_cfg.video_path,
                       g_cfg.video_enabled,
                       g_cfg.video_skippable,
                       g_cfg.video_ffmpeg,
                       g_cfg.video_max_seconds,
                       g_cfg.video_skip_debounce_ms,
                       g_cfg.video_audio,
                       g_cfg.video_diag,
                       g_cfg.video_trigger,
                       g_cfg.video_decoder);

        const int needs_iat_hook =
            g_cfg.enabled || g_cfg.boot_poster_enabled || g_cfg.video_enabled;

        if (!g_cfg.enabled && !g_cfg.boot_poster_enabled && !g_cfg.video_enabled) {
            // All overlay features off — leave the IAT alone entirely.
            log_line("[pso_widescreen] Enabled=0, BootPosterEnabled=0, VideoEnable=0, staying dormant");
            return TRUE;
        }
        if (!g_cfg.enabled) {
            // Widescreen master off but boot poster on — IAT hook still
            // needed so we can patch the device vtable Present slot.
            log_line("[pso_widescreen] Enabled=0; boot poster only path");
        }
        else if (g_cfg.width <= 0 && g_cfg.height <= 0 && g_cfg.windowed == 1) {
            // Enabled=1 but no actual override values configured. Boot
            // poster / video may still want the IAT hook to install Present.
            if (!g_cfg.boot_poster_enabled && !g_cfg.video_enabled) {
                log_line("[pso_widescreen] no overrides configured, idle");
                return TRUE;
            }
            log_line("[pso_widescreen] no widescreen overrides; running for boot poster / video only");
        }
        (void)needs_iat_hook;
        // ---- THE ONE GATE refactor) ----
        // ONE master gate (g_scale.enabled, == g_cfg.enabled). No
        // widescreen_engine selector, no else branch. Everything inside runs
        // TOGETHER, de-conflicted: apply_static_patches (anzz1 bake one-shot ->
        // minimap bake-on-top -> engine-independent static companions, each
        // keeping its per-feature knob), then apply_engine_patches (cursor-warp
        // IAT, model-pan E8 redirect; the char-create composer MinHooks ride
        // along via the static anchor pass). detect_d3d8_stack() above logged
        // the wrapper for diagnostics; no behavior gates on it (wrapper-agnostic).
        if (g_scale.enabled) {
            log_line("[pso_widescreen] apply: anzz1 authoritative coords + companions (one gate, no engine selector)");
            apply_static_patches(&g_scale);
            apply_engine_patches(&g_scale);
        }
        // d3d8.dll might already be loaded (pulled in via the static
        // import on psobb.exe by the time DllMain runs for an ASI),
        // but we don't actually need its handle — we patch the IAT slot
        // in psobb.exe's image, which already points into d3d8.dll.
        HMODULE psobb = GetModuleHandleA(NULL);  // = psobb.exe base
        if (patch_iat(psobb, "d3d8.dll", "Direct3DCreate8",
                      (void *)&Hook_Direct3DCreate8,
                      (void **)&real_Direct3DCreate8)) {
            log_line("[pso_widescreen] IAT patched: real=0x%p",
                     (void *)real_Direct3DCreate8);
        } else {
            log_line("[pso_widescreen] IAT patch FAILED — d3d8.dll!Direct3DCreate8 not found in psobb.exe import table");
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_cfg.boot_poster_enabled) {
            boot_poster_log_summary();
        }
        if (g_cfg.video_enabled) {
            // Logs diagnostics AND tears down any live ffmpeg child so we
            // never leave a zombie decoder on exit (Job KILL_ON_JOB_CLOSE
            // is the backstop; this is the clean path).
            mod_video_log_summary();
        }
    }
    return TRUE;
}
