// mod_boot_poster.c — boot-time poster overlay for PSOBB.
//
// Renders a user-supplied PNG centered on screen during the very first
// "NOW LOADING" black screen (before SEGA splash). Auto-disables the
// instant any of these become true:
//   - splash atlas pointer at 0x00A3B080 becomes non-zero (SEGA splash
//     started)
//   - current floor at 0x00AAFCA0 becomes non-zero (player reached lobby
//     / floor 1+)
//   - configured MaxSeconds elapsed since attach
//   - PNG load failed at init (poster permanently disabled)
//   - texture upload failed on first Present (poster permanently disabled)
//
// Wrapper-agnostic: we render via IDirect3DDevice8::CreateTexture +
// DrawPrimitiveUP. d3d8to11 / d3d8to9 / native d3d8 all expose the same
// IDirect3DDevice8 interface.
//
// Exposed contract (called from pso_widescreen.c):
//   void boot_poster_init_from_cfg(const char *path, int enabled,
//                                  int disable_after_floor,
//                                  int disable_after_splash_atlas,
//                                  int max_seconds, float max_screen_pct);
//   void boot_poster_on_present(void *device, int viewport_w, int viewport_h);
//   void boot_poster_log_summary(void);
//
// All of these are SAFE to call before init (no-op) or after disable
// (no-op). Internal state is module-local; no shared static state with
// pso_widescreen.c apart from the explicit function calls.

#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <Windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

// Logging hook: pso_widescreen.c owns the log handle / file writer; we
// route through it. Forward-declared, defined in pso_widescreen.c.
extern void log_line(const char *fmt, ...);

// ---- Minimal D3D8 ABI we touch (same layout as pso_widescreen.c) ----
typedef struct IDirect3DDevice8  IDirect3DDevice8;
typedef struct IDirect3DTexture8 IDirect3DTexture8;
typedef struct IDirect3DSurface8 IDirect3DSurface8;

#ifndef D3DFMT_A8R8G8B8
#define D3DFMT_A8R8G8B8 21
#endif
#ifndef D3DPOOL_MANAGED
#define D3DPOOL_MANAGED 1
#endif
#ifndef D3DUSAGE_DYNAMIC
#define D3DUSAGE_DYNAMIC 0x00000200L
#endif
#ifndef D3DPT_TRIANGLEFAN
#define D3DPT_TRIANGLEFAN 6
#endif

// IDirect3DDevice8::CreateTexture — vtable slot 20.
typedef HRESULT (STDMETHODCALLTYPE *CreateTexture_t)(
    IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Levels,
    DWORD Usage, DWORD Format, DWORD Pool, IDirect3DTexture8 **ppTexture);

// IDirect3DTexture8 vtable layout (same prefix as IDirect3DBaseTexture8):
//   0..2: IUnknown
//   3..7: GetDevice / SetPrivateData / GetPrivateData / FreePrivateData /
//         SetPriority
//   8: GetPriority
//   9: PreLoad
//   10: GetType
//   11: SetLOD
//   12: GetLOD
//   13: GetLevelCount
//   14: GetLevelDesc
//   15: GetSurfaceLevel
//   16: LockRect
//   17: UnlockRect
//   18: AddDirtyRect
typedef struct {
    DWORD Format;
    DWORD Type;
    DWORD Usage;
    DWORD Pool;
    UINT  Size;
    DWORD MultiSampleType;
    UINT  Width;
    UINT  Height;
} D3DSURFACE_DESC8_X;

typedef struct {
    void *pBits;
    int   Pitch;
} D3DLOCKED_RECT_X;

typedef HRESULT (STDMETHODCALLTYPE *Tex_LockRect_t)(
    IDirect3DTexture8 *self, UINT Level, D3DLOCKED_RECT_X *pLockedRect,
    const RECT *pRect, DWORD Flags);
typedef HRESULT (STDMETHODCALLTYPE *Tex_UnlockRect_t)(
    IDirect3DTexture8 *self, UINT Level);
typedef ULONG (STDMETHODCALLTYPE *IUnknown_Release_t)(void *self);

// IDirect3DDevice8::SetTexture — vtable slot 61.
typedef HRESULT (STDMETHODCALLTYPE *SetTexture_t)(
    IDirect3DDevice8 *self, DWORD Stage, IDirect3DTexture8 *pTexture);

// IDirect3DDevice8::SetVertexShader — slot 76.
typedef HRESULT (STDMETHODCALLTYPE *SetVertexShader_t)(
    IDirect3DDevice8 *self, DWORD Handle);

// IDirect3DDevice8::SetRenderState — slot 50.
typedef HRESULT (STDMETHODCALLTYPE *SetRenderState_t)(
    IDirect3DDevice8 *self, DWORD State, DWORD Value);

// IDirect3DDevice8::SetTextureStageState — slot 63.
typedef HRESULT (STDMETHODCALLTYPE *SetTextureStageState_t)(
    IDirect3DDevice8 *self, DWORD Stage, DWORD Type, DWORD Value);

// IDirect3DDevice8::DrawPrimitiveUP — slot 72.
typedef HRESULT (STDMETHODCALLTYPE *DrawPrimitiveUP_t)(
    IDirect3DDevice8 *self, DWORD PrimitiveType, UINT PrimitiveCount,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride);

// IDirect3DDevice8::SetStreamSource — slot 83. Used for "decouple our
// DrawPrimitiveUP from any active VB" when re-entering Present hook.
typedef HRESULT (STDMETHODCALLTYPE *SetStreamSource_t)(
    IDirect3DDevice8 *self, UINT StreamNumber, void *pStreamData,
    UINT Stride);

// IDirect3DDevice8::GetViewport — slot 41 (sibling of SetViewport).
typedef struct {
    DWORD X;
    DWORD Y;
    DWORD Width;
    DWORD Height;
    float MinZ;
    float MaxZ;
} D3DVIEWPORT8_X;
typedef HRESULT (STDMETHODCALLTYPE *GetViewport_t)(
    IDirect3DDevice8 *self, D3DVIEWPORT8_X *pViewport);

// FVF for our vertex stream: pre-transformed RHW + 1 tex coord.
#define D3DFVF_XYZRHW   0x0004
#define D3DFVF_TEX1     0x0100
#define POSTER_FVF      (D3DFVF_XYZRHW | D3DFVF_TEX1)

// Render state subset we touch (only the ones likely to interfere).
#define D3DRS_LIGHTING               137
#define D3DRS_CULLMODE               22
#define D3DRS_ZENABLE                7
#define D3DRS_ZWRITEENABLE           14
#define D3DRS_ALPHABLENDENABLE       27
#define D3DRS_SRCBLEND               19
#define D3DRS_DESTBLEND              20
#define D3DRS_BLENDOP                171
#define D3DRS_ALPHATESTENABLE        15
#define D3DRS_FOGENABLE              28
#define D3DRS_STENCILENABLE          52
#define D3DRS_COLORVERTEX            141
#define D3DRS_CLIPPING               136

#define D3DBLEND_SRCALPHA            5
#define D3DBLEND_INVSRCALPHA         6
#define D3DBLEND_ONE                 2
#define D3DBLENDOP_ADD               1
#define D3DCULL_NONE                 1

#define D3DTSS_COLOROP               1
#define D3DTSS_COLORARG1             2
#define D3DTSS_COLORARG2             3
#define D3DTSS_ALPHAOP               4
#define D3DTSS_ALPHAARG1             5
#define D3DTSS_ALPHAARG2             6
#define D3DTOP_MODULATE              4
#define D3DTOP_SELECTARG1            2
#define D3DTOP_DISABLE               1
#define D3DTA_TEXTURE                0
#define D3DTA_DIFFUSE                0x00000000

// PSOBB state addresses (rebase only; same as pso_widescreen.c).
#define ADDR_SPLASH_ATLAS  0x00A3B080u  // becomes non-zero once SEGA splash starts
#define ADDR_CURRENT_FLOOR 0x00AAFCA0u  // becomes non-zero once a real floor loads

// ---- Module state ----

typedef struct {
    float x, y, z, rhw;
    DWORD color;       // diffuse — 0xFFFFFFFF for opaque white tint
    float u, v;
} PosterVtx;

static struct {
    int     enabled;                  // master switch from cfg
    int     disable_after_floor;      // gate flag from cfg
    int     disable_after_splash;     // gate flag from cfg
    int     max_seconds;              // hard timeout from cfg
    float   max_screen_pct;           // 80 = 80% of screen
    char    path[MAX_PATH];           // resolved (absolute) PNG path

    // Decoded asset (RGBA 8-bit, 4 channels)
    unsigned char *rgba;
    int     img_w;
    int     img_h;

    // Lifecycle / disable state
    int     disabled_perm;            // 1 = never try again (load fail, upload fail, conditions met)
    int     init_done;                // load_config + decode have run
    DWORD   t_attach_ms;              // GetTickCount on init
    LONG    present_calls;            // monotonic counter for diagnostics

    // D3D resources (lazy-created on first Present once we have a device)
    IDirect3DTexture8 *texture;
    int     tex_uploaded;
} g_bp;

// ---- Helpers ----

static void bp_disable_perm(const char *reason)
{
    if (g_bp.disabled_perm) return;
    g_bp.disabled_perm = 1;
    log_line("[boot_poster] disabled: %s", reason ? reason : "(unknown)");
}

static int bp_should_disable_now(void)
{
    if (g_bp.disabled_perm) return 1;
    if (g_bp.max_seconds > 0) {
        DWORD elapsed = GetTickCount() - g_bp.t_attach_ms;
        if (elapsed > (DWORD)g_bp.max_seconds * 1000u) {
            bp_disable_perm("MaxSeconds elapsed");
            return 1;
        }
    }
    if (g_bp.disable_after_splash) {
        // SEH-safe read (these addresses are valid for the entire process
        // lifetime in psobb.exe, so a raw deref is fine, but stay defensive).
        __try {
            uint32_t v = *(volatile uint32_t *)ADDR_SPLASH_ATLAS;
            if (v != 0u) {
                bp_disable_perm("splash atlas became non-zero");
                return 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* leave alive */ }
    }
    if (g_bp.disable_after_floor) {
        __try {
            uint32_t v = *(volatile uint32_t *)ADDR_CURRENT_FLOOR;
            if (v != 0u) {
                bp_disable_perm("current floor became non-zero");
                return 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* leave alive */ }
    }
    return 0;
}

// Resolve a possibly-relative PNG path to an absolute path next to
// psobb.exe. Mirrors the pattern pso_widescreen.c uses for widescreen.cfg.
static void bp_resolve_path(const char *cfg_path, char *out, size_t outlen)
{
    out[0] = 0;
    if (!cfg_path || !cfg_path[0]) return;
    // Already absolute? (drive letter or UNC)
    if ((cfg_path[0] && cfg_path[1] == ':') ||
        (cfg_path[0] == '\\' && cfg_path[1] == '\\')) {
        _snprintf_s(out, outlen, _TRUNCATE, "%s", cfg_path);
        return;
    }
    char exe[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, exe, MAX_PATH) == 0) {
        _snprintf_s(out, outlen, _TRUNCATE, "%s", cfg_path);
        return;
    }
    char *slash = strrchr(exe, '\\');
    if (slash) *(slash + 1) = 0;
    _snprintf_s(out, outlen, _TRUNCATE, "%s%s", exe, cfg_path);
    // Normalize forward slashes -> backslashes for Win32 file APIs.
    for (char *p = out; *p; ++p) if (*p == '/') *p = '\\';
}

static void bp_decode_png(void)
{
    if (!g_bp.path[0]) {
        bp_disable_perm("no Path configured");
        return;
    }
    int w = 0, h = 0, ch = 0;
    unsigned char *rgba = stbi_load(g_bp.path, &w, &h, &ch, 4);
    if (!rgba || w <= 0 || h <= 0) {
        log_line("[boot_poster] stbi_load failed for '%s'", g_bp.path);
        if (rgba) stbi_image_free(rgba);
        bp_disable_perm("stbi_load failed");
        return;
    }
    // Pre-swizzle RGBA (stb output) -> ARGB byte order = 0xAARRGGBB on
    // little-endian = D3DFMT_A8R8G8B8. stb yields R G B A bytes in memory;
    // D3DFMT_A8R8G8B8 wants B G R A bytes. Swap R<->B in place.
    size_t pixels = (size_t)w * (size_t)h;
    unsigned char *p = rgba;
    for (size_t i = 0; i < pixels; ++i, p += 4) {
        unsigned char r = p[0];
        p[0] = p[2];
        p[2] = r;
    }
    g_bp.rgba = rgba;
    g_bp.img_w = w;
    g_bp.img_h = h;
    log_line("[boot_poster] decoded PNG '%s' %dx%d (orig channels=%d)",
             g_bp.path, w, h, ch);
}

// Upload decoded RGBA into a managed-pool D3D8 texture once we have a
// device. Sets g_bp.tex_uploaded on success. Disables permanently on
// hard failure.
static int bp_upload_texture(IDirect3DDevice8 *dev)
{
    if (g_bp.tex_uploaded) return 1;
    if (!g_bp.rgba || g_bp.img_w <= 0 || g_bp.img_h <= 0) return 0;
    void **vt = *(void ***)dev;
    CreateTexture_t fnCreateTexture = (CreateTexture_t)vt[20];
    HRESULT hr = fnCreateTexture(dev, (UINT)g_bp.img_w, (UINT)g_bp.img_h,
                                 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                 &g_bp.texture);
    if (FAILED(hr) || !g_bp.texture) {
        log_line("[boot_poster] CreateTexture FAILED hr=0x%08x", hr);
        bp_disable_perm("CreateTexture failed");
        return 0;
    }
    void **tvt = *(void ***)g_bp.texture;
    Tex_LockRect_t   fnLockRect   = (Tex_LockRect_t)tvt[16];
    Tex_UnlockRect_t fnUnlockRect = (Tex_UnlockRect_t)tvt[17];
    D3DLOCKED_RECT_X lr; lr.pBits = NULL; lr.Pitch = 0;
    hr = fnLockRect(g_bp.texture, 0, &lr, NULL, 0);
    if (FAILED(hr) || !lr.pBits) {
        log_line("[boot_poster] LockRect FAILED hr=0x%08x", hr);
        IUnknown_Release_t fnRelease = (IUnknown_Release_t)tvt[2];
        fnRelease(g_bp.texture);
        g_bp.texture = NULL;
        bp_disable_perm("LockRect failed");
        return 0;
    }
    // Copy row by row to honor the surface pitch (which may exceed
    // width*4 due to driver alignment).
    const int row_bytes = g_bp.img_w * 4;
    for (int y = 0; y < g_bp.img_h; ++y) {
        unsigned char *dst = (unsigned char *)lr.pBits + (size_t)y * (size_t)lr.Pitch;
        const unsigned char *src = g_bp.rgba + (size_t)y * (size_t)row_bytes;
        memcpy(dst, src, (size_t)row_bytes);
    }
    fnUnlockRect(g_bp.texture, 0);
    g_bp.tex_uploaded = 1;
    log_line("[boot_poster] uploaded %dx%d texture to D3D8 (managed pool)",
             g_bp.img_w, g_bp.img_h);
    // The CPU-side pixel buffer is no longer needed (managed pool keeps a
    // backing copy itself). Release it to free 1MB+ of RGBA.
    stbi_image_free(g_bp.rgba);
    g_bp.rgba = NULL;
    return 1;
}

// Compute the centered, aspect-preserved rect within the viewport.
static void bp_compute_rect(int vp_w, int vp_h, float pct,
                            float *x1, float *y1, float *x2, float *y2)
{
    if (vp_w <= 0 || vp_h <= 0) {
        *x1 = *y1 = *x2 = *y2 = 0.0f;
        return;
    }
    float pw = (float)g_bp.img_w;
    float ph = (float)g_bp.img_h;
    if (pw <= 0.0f || ph <= 0.0f) {
        *x1 = *y1 = *x2 = *y2 = 0.0f;
        return;
    }
    float ap = pw / ph;                          // poster aspect
    float as = (float)vp_w / (float)vp_h;        // screen aspect
    float maxw = (float)vp_w * pct;
    float maxh = (float)vp_h * pct;
    float w, h;
    if (ap > as) {
        // Poster is wider than screen — width-limited.
        w = maxw;
        h = w / ap;
        if (h > maxh) { h = maxh; w = h * ap; }
    } else {
        // Poster is taller / equal — height-limited.
        h = maxh;
        w = h * ap;
        if (w > maxw) { w = maxw; h = w / ap; }
    }
    float cx = (float)vp_w * 0.5f;
    float cy = (float)vp_h * 0.5f;
    *x1 = cx - w * 0.5f;
    *y1 = cy - h * 0.5f;
    *x2 = cx + w * 0.5f;
    *y2 = cy + h * 0.5f;
}

// State snapshot bundle for save/restore around our draw. We don't try
// to snapshot ALL render state — too noisy and the engine resets most of
// it next frame anyway. We only restore the fields most likely to leak
// out and visibly corrupt the splash (texture binding, FVF, lighting).
typedef struct {
    DWORD vs;
} BPRSSnap;

static void bp_render_quad(IDirect3DDevice8 *dev,
                           float x1, float y1, float x2, float y2)
{
    void **vt = *(void ***)dev;
    SetTexture_t           fnSetTexture           = (SetTexture_t)vt[61];
    SetVertexShader_t      fnSetVertexShader      = (SetVertexShader_t)vt[76];
    SetRenderState_t       fnSetRenderState       = (SetRenderState_t)vt[50];
    SetTextureStageState_t fnSetTextureStageState = (SetTextureStageState_t)vt[63];
    DrawPrimitiveUP_t      fnDrawPrimitiveUP      = (DrawPrimitiveUP_t)vt[72];

    // Configure pipeline for screen-space textured-quad render.
    fnSetRenderState(dev, D3DRS_ZENABLE,         0);
    fnSetRenderState(dev, D3DRS_ZWRITEENABLE,    0);
    fnSetRenderState(dev, D3DRS_LIGHTING,        0);
    fnSetRenderState(dev, D3DRS_FOGENABLE,       0);
    fnSetRenderState(dev, D3DRS_STENCILENABLE,   0);
    fnSetRenderState(dev, D3DRS_CULLMODE,        D3DCULL_NONE);
    fnSetRenderState(dev, D3DRS_CLIPPING,        0);
    fnSetRenderState(dev, D3DRS_COLORVERTEX,     1);
    fnSetRenderState(dev, D3DRS_ALPHATESTENABLE, 0);
    fnSetRenderState(dev, D3DRS_ALPHABLENDENABLE, 1);
    fnSetRenderState(dev, D3DRS_SRCBLEND,        D3DBLEND_SRCALPHA);
    fnSetRenderState(dev, D3DRS_DESTBLEND,       D3DBLEND_INVSRCALPHA);
    fnSetRenderState(dev, D3DRS_BLENDOP,         D3DBLENDOP_ADD);

    fnSetTextureStageState(dev, 0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
    fnSetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    fnSetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    fnSetTextureStageState(dev, 0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
    fnSetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    fnSetTextureStageState(dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    fnSetTextureStageState(dev, 1, D3DTSS_COLOROP,   D3DTOP_DISABLE);
    fnSetTextureStageState(dev, 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);

    fnSetTexture(dev, 0, g_bp.texture);
    fnSetVertexShader(dev, POSTER_FVF);

    // Half-pixel offset on RHW: D3D8 maps pixel (x,y) center at integer
    // coords + 0.5; subtract 0.5 from each vertex so our 0..w-1, 0..h-1
    // pixel-aligned quad samples texel centers cleanly. Without this the
    // edges blur into adjacent texels at non-1:1 magnification.
    PosterVtx v[4] = {
        { x1 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f },
        { x2 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f },
        { x2 - 0.5f, y2 - 0.5f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f },
        { x1 - 0.5f, y2 - 0.5f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f },
    };
    fnDrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, v, sizeof(PosterVtx));

    // Best-effort cleanup so we don't leak our texture binding into
    // engine draws this frame. Engine re-binds its own texture before its
    // next sprite-primitive call anyway, but defensive belts + braces.
    fnSetTexture(dev, 0, NULL);
    fnSetRenderState(dev, D3DRS_ALPHABLENDENABLE, 0);
}

// ---- Public API ----

void boot_poster_init_from_cfg(const char *path, int enabled,
                               int disable_after_floor,
                               int disable_after_splash_atlas,
                               int max_seconds, float max_screen_pct)
{
    if (g_bp.init_done) return;
    g_bp.init_done             = 1;
    g_bp.enabled               = enabled;
    g_bp.disable_after_floor   = disable_after_floor;
    g_bp.disable_after_splash  = disable_after_splash_atlas;
    g_bp.max_seconds           = (max_seconds > 0) ? max_seconds : 30;
    g_bp.max_screen_pct        = (max_screen_pct > 0.0f && max_screen_pct <= 100.0f)
                                 ? (max_screen_pct / 100.0f) : 0.8f;
    g_bp.t_attach_ms           = GetTickCount();
    bp_resolve_path(path, g_bp.path, sizeof(g_bp.path));
    log_line("[boot_poster] init enabled=%d path='%s' floor_gate=%d splash_gate=%d "
             "max_s=%d pct=%.2f",
             enabled, g_bp.path, disable_after_floor,
             disable_after_splash_atlas, g_bp.max_seconds, g_bp.max_screen_pct);
    if (!enabled) {
        bp_disable_perm("Enabled=0");
        return;
    }
    bp_decode_png();
}

void boot_poster_on_present(void *device, int viewport_w, int viewport_h)
{
    if (!g_bp.init_done || g_bp.disabled_perm || !g_bp.enabled) return;
    if (!device) return;
    InterlockedIncrement(&g_bp.present_calls);

    // Re-check disable conditions every Present — cheap (3 mem reads + a
    // tick subtract). Once we cross any threshold we permanently disable.
    if (bp_should_disable_now()) return;

    IDirect3DDevice8 *dev = (IDirect3DDevice8 *)device;

    // Pull a viewport from the device if the caller didn't have one cached.
    int vp_w = viewport_w;
    int vp_h = viewport_h;
    if (vp_w <= 0 || vp_h <= 0) {
        __try {
            void **vt = *(void ***)dev;
            GetViewport_t fnGetViewport = (GetViewport_t)vt[41];
            D3DVIEWPORT8_X vp; memset(&vp, 0, sizeof(vp));
            if (SUCCEEDED(fnGetViewport(dev, &vp))) {
                vp_w = (int)vp.Width;
                vp_h = (int)vp.Height;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
    }
    if (vp_w <= 0 || vp_h <= 0) return;

    // Lazy texture upload on first valid Present.
    if (!g_bp.tex_uploaded) {
        __try {
            if (!bp_upload_texture(dev)) return;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            bp_disable_perm("upload SEH");
            return;
        }
    }

    float x1, y1, x2, y2;
    bp_compute_rect(vp_w, vp_h, g_bp.max_screen_pct, &x1, &y1, &x2, &y2);
    if (x2 <= x1 || y2 <= y1) return;

    __try {
        bp_render_quad(dev, x1, y1, x2, y2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bp_disable_perm("render SEH");
    }
}

// ---- Lost-device discipline (alt-tab / dgVoodoo2 Reset) -------------------
//
// Called from the IDirect3DDevice8::Reset hook (Hook_Reset in pso_widescreen.c)
// around the engine's device Reset. The boot poster's ONLY device resource is
// g_bp.texture, created in D3DPOOL_MANAGED. A managed-pool texture is
// Reset-RESILIENT: the D3D runtime keeps a system-memory backing copy and
// re-uploads it to VRAM automatically after a Reset, so it stays valid across an
// alt-tab without any action from us.
//
// Crucially we must NOT release it on device-lost: bp_upload_texture frees the
// CPU-side pixel buffer (g_bp.rgba) right after the one-time upload (to reclaim
// the RGBA), so once uploaded there is no source left to re-upload from — a
// release would permanently lose the poster with no recreate path. So
// on_device_lost is a deliberate no-op for the managed texture; we only clear a
// transient if one ever existed. on_device_reset is likewise a no-op (managed
// re-upload is automatic). Both bridges exist for symmetry with mod_video and so
// the Reset hook can drive every overlay module uniformly.
void boot_poster_on_device_lost(void)
{
    if (!g_bp.enabled) return;
    // Managed-pool texture survives Reset; releasing it would lose the only copy
    // (g_bp.rgba was freed after upload). Intentionally nothing to release here.
    log_line("[boot_poster] on_device_lost: managed texture is Reset-resilient (no-op)");
}

void boot_poster_on_device_reset(void)
{
    if (!g_bp.enabled) return;
    // Managed-pool re-upload is automatic after a successful Reset; nothing to do.
    log_line("[boot_poster] on_device_reset: managed texture re-uploads automatically (no-op)");
}

void boot_poster_log_summary(void)
{
    log_line("[boot_poster] summary: present_calls=%ld disabled_perm=%d "
             "tex_uploaded=%d img=%dx%d",
             (long)g_bp.present_calls, g_bp.disabled_perm,
             g_bp.tex_uploaded, g_bp.img_w, g_bp.img_h);
}
