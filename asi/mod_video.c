// mod_video.c — skippable video player + Present blit (Media Foundation or FFmpeg).
//
// Decode architecture:
//   - FFmpeg path: spawn ffmpeg.exe as a child with its stdout piped to us,
//     decoding the source to rawvideo BGRA at the backbuffer size:
//       ffmpeg -loglevel error -i "<path>" -f rawvideo -pix_fmt bgra -s <bbW>x<bbH> -
//     BGRA byte order == D3DFMT_A8R8G8B8 little-endian exactly (B,G,R,A), so there
//     is ZERO CPU swizzle. The child runs in a Job object with KILL_ON_JOB_CLOSE so
//     it dies with us no matter how we exit. A reader thread does blocking ReadFile
//     of exactly w*h*4 per frame into a 3-slot ring buffer and publishes the newest
//     frame index with InterlockedExchange; the render thread never blocks on the pipe.
//   - mod_video_on_present(): if playing, pick the latest DUE frame by wall-clock
//     (QPC vs video fps), upload to a D3D8 texture, and blit it fullscreen
//     letterboxed via the same quad path boot_poster uses (device vtable slot 72
//     DrawPrimitiveUP, RHW FVF). An opaque black fullscreen underlay provides bars.
//   - Skip: GetAsyncKeyState(VK_RETURN/VK_ESCAPE) with release-then-press debounce;
//     on skip stop playback + kill the decoder.
//
// VideoTrigger modes:
//   * VID_TRIGGER_BOOT       — one-shot start after device warm-up (a per-frame
//                              present-count one-shot, no engine hook). Covers the
//                              boot splash; ends/skips to the title.
//   * VID_TRIGGER_CHARCREATE — driven by a SOURCE-EVENT hook, NOT a scene poll. We
//                              MinHook the engine transition-request setter
//                              sub_007A60DC @0x007A60DC and read ONLY its `target`
//                              arg: request(3) (the unique scripted-intro start) arms
//                              the cover (cc_session); request(5|0|2|0xB) while
//                              cc_session is set tears the cover down (the cover spans
//                              scene-3 ONLY). No reads of the current-scene global
//                              0x00AAB384 / the 0x00A3A93C buffer / the player array.
//                              We also take OWNERSHIP of the scene-3 per-frame update
//                              0x007C1588 via a SECOND MinHook (stub_7c1588). While
//                              cc_session is set the hook runs a replica that KEEPS
//                              the shared device snapshot (0x007BECD4), the input
//                              AGGREGATION + skip READ (0x007BFBA0 /
//                              0x007BFEDC->0x007A6174), the 3D-audio listener
//                              (0x00814168), and the shared frame epilogue
//                              (0x0080030C), and SKIPS only the 3 intro DRAW calls
//                              (0x0081745C/0x00817604/0x00818F80) + the intro state
//                              machine (0x005BA748) -> the intro draws NOTHING under
//                              our overlay but a deliberate skip / ESC still works
//                              (the engine's own skip read drives it). Engine audio is
//                              silenced via Option K (save+zero the four SOUNDCTRL
//                              module-enable dwords 0x00A46C88/8C/90/94; restore the
//                              SAVED values on exit). Because under full ownership the
//                              intro ADX never plays, the engine's natural ADX-done
//                              exit never fires, so on video EOF/skip WE arm the
//                              engine's own 3->5 exit (byte 0xAAE988=1, 0xAAE980=1);
//                              the owned replica then issues the engine's OWN
//                              request(5) -> cc_on_request(5) tears down the cover ->
//                              live class-select. Create-NEW-only: scene 3 has a
//                              single confirm-gated requester; pick-EXISTING goes
//                              straight to 0xB and never hits 3/5, so the cover never
//                              arms for it.
//   * VID_TRIGGER_OFF        — never auto-start.
//
// OWNERSHIP vs COVER: the scene-3 update is replaced by our replica while cc_session
// is set, so the intro renders nothing of its own (no starfield/crawl behind the
// video). It does NOT steal input: the replica keeps the engine's input aggregation +
// skip read so a deliberate skip and ESC behave exactly as stock. WE own the duration
// (the ADX is muted under ownership); the video plays for its full length and the
// engine's own request(5) fires at our EOF/SKIP. With VideoTrigger!=charcreate (or
// cc_session==0) the 0x007C1588 stub is byte-identical passthrough — no behavior change.
//
// HARD CONSTRAINT: VideoEnable=0 (default) => mod_video_init early-returns
// and every on_present is `if(!g_video.enabled) return;`. The build is then
// byte-identical in behavior to the boot_poster-only client.
//
// Exposed contract (mod_video.h) mirrors mod_boot_poster.c's shape exactly.

#include <Windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include <MinHook.h>          // CHARCREATE source-event detour on 0x007A60DC.

// stb_image DECLARATIONS only (the IMPLEMENTATION lives in mod_boot_poster.c,
// same build/link). The boot-cover still loads patches/psobb_boot_poster.png
// via stbi_load + an RGBA->BGRA swizzle, mirroring bp_decode_png.
#include "stb_image.h"

#include "mod_video.h"

// ---- In-process Windows Media Foundation decode (VID_DECODER_MF, default) ----
//
// Guarded by VID_NO_MF: a /DVID_NO_MF build compiles ffmpeg-only (vid_mf_open
// becomes a stub returning 0) and links without the four MF import libs. The MF
// C-bindings (COBJMACROS C-vtable form, e.g. IMFSourceReader_ReadSample(rdr,...))
// are exactly the patterns the validated mf_spike.c proved compile + link + run
// under vcvars32 x86 MSVC against the real intro clip. CINTERFACE/COBJMACROS are
// defined here (NOT globally) so these headers emit the C vtable form; the D3D8
// vtable code in this file uses raw *(void***) indexing and is unaffected.
//
// NOTE: the mfapi.h inline helpers MFSetAttributeSize/MFGetAttributeSize/
// MFGetAttributeRatio DO NOT LINK in C mode (proven in the spike). We pack/unpack
// the UINT64 FRAME_SIZE/FRAME_RATE pairs ourselves via SetUINT64/GetUINT64 (the
// mt_set_pair/mt_get_pair helpers below) — same pattern the spike validated.
#ifndef VID_NO_MF
#  define COBJMACROS
#  define CINTERFACE
#  include <mfapi.h>
#  include <mfidl.h>
#  include <mfreadwrite.h>
#  include <mferror.h>
   // ---- XAudio2 (DYNAMICALLY loaded: no lib pragma, no static dep) ----
   // xaudio2.h gives the COBJMACROS C-call macros (IXAudio2_*, IXAudio2SourceVoice_*)
   // and the XAUDIO2_BUFFER / XAUDIO2_VOICE_STATE structs. We resolve the
   // xaudio2_9.dll!XAudio2Create export ourselves (the legacy 2-extra-arg form),
   // bypassing the SDK's NTDDI-gated inline XAudio2Create wrapper -> NO link dep,
   // so a VideoEnable=0 / machine-without-xaudio2_9 build degrades gracefully.
#  include <xaudio2.h>
#  include <mmreg.h>            // WAVEFORMATEX (also via Windows.h; explicit for clarity)
#  pragma comment(lib, "mfplat.lib")
#  pragma comment(lib, "mfreadwrite.lib")
#  pragma comment(lib, "mfuuid.lib")
#  pragma comment(lib, "ole32.lib")

// The XAudio2Create export resolved dynamically has the desktop-legacy signature
// HRESULT __stdcall(IXAudio2**, UINT32 Flags, XAUDIO2_PROCESSOR). Typedef it
// ourselves so we never depend on the SDK's inline wrapper.
typedef HRESULT (STDAPICALLTYPE *XAudio2Create_t)(
    IXAudio2 **ppXAudio2, UINT32 Flags, XAUDIO2_PROCESSOR XAudio2Processor);
#endif

// pso_widescreen.c owns the log file writer; route through it.
extern void log_line(const char *fmt, ...);

// ---- Minimal D3D8 ABI we touch (identical layout to mod_boot_poster.c) ----
typedef struct IDirect3DDevice8  IDirect3DDevice8;
typedef struct IDirect3DTexture8 IDirect3DTexture8;

#ifndef D3DFMT_A8R8G8B8
#define D3DFMT_A8R8G8B8 21
#endif
#ifndef D3DPOOL_MANAGED
#define D3DPOOL_MANAGED 1
#endif
#ifndef D3DPOOL_DEFAULT
#define D3DPOOL_DEFAULT 0
#endif
#ifndef D3DUSAGE_DYNAMIC
#define D3DUSAGE_DYNAMIC 0x00000200L
#endif
#ifndef D3DPT_TRIANGLEFAN
#define D3DPT_TRIANGLEFAN 6
#endif
#ifndef D3DLOCK_DISCARD
#define D3DLOCK_DISCARD 0x00002000L
#endif

typedef HRESULT (STDMETHODCALLTYPE *CreateTexture_t)(
    IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Levels,
    DWORD Usage, DWORD Format, DWORD Pool, IDirect3DTexture8 **ppTexture);

typedef struct { int Pitch; void *pBits; } D3DLOCKED_RECT_X;  /* real D3D8 order: Pitch FIRST, then pBits */

typedef HRESULT (STDMETHODCALLTYPE *Tex_LockRect_t)(
    IDirect3DTexture8 *self, UINT Level, D3DLOCKED_RECT_X *pLockedRect,
    const RECT *pRect, DWORD Flags);
typedef HRESULT (STDMETHODCALLTYPE *Tex_UnlockRect_t)(
    IDirect3DTexture8 *self, UINT Level);
typedef ULONG (STDMETHODCALLTYPE *IUnknown_Release_t)(void *self);

typedef HRESULT (STDMETHODCALLTYPE *SetTexture_t)(
    IDirect3DDevice8 *self, DWORD Stage, IDirect3DTexture8 *pTexture);
typedef HRESULT (STDMETHODCALLTYPE *SetVertexShader_t)(
    IDirect3DDevice8 *self, DWORD Handle);
typedef HRESULT (STDMETHODCALLTYPE *SetRenderState_t)(
    IDirect3DDevice8 *self, DWORD State, DWORD Value);
typedef HRESULT (STDMETHODCALLTYPE *SetTextureStageState_t)(
    IDirect3DDevice8 *self, DWORD Stage, DWORD Type, DWORD Value);
typedef HRESULT (STDMETHODCALLTYPE *DrawPrimitiveUP_t)(
    IDirect3DDevice8 *self, DWORD PrimitiveType, UINT PrimitiveCount,
    const void *pVertexStreamZeroData, UINT VertexStreamZeroStride);

typedef struct {
    DWORD X, Y, Width, Height;
    float MinZ, MaxZ;
} D3DVIEWPORT8_X;
// Standard IDirect3DDevice8 vtable order (after the 3 IUnknown methods):
//   ... 37 SetTransform, 38 GetTransform, 39 MultiplyTransform,
//   40 SetViewport, 41 GetViewport, 42 SetMaterial ...
// CONFIRMED authoritative against pso_widescreen.c, which patches
//   vt[40] = Hook_SetViewport  ("device vtable[40] patched: real SetViewport")
// and whose SetViewport_t signature is HRESULT(self, const D3DVIEWPORT8_X*).
// So: SetViewport = slot 40, GetViewport = slot 41.
//
// NOTE: pso_widescreen.c HOOKS vt[40]. Calling vt[40] therefore enters
// Hook_SetViewport, which (when override_viewport is on) re-stretches any
// "main scene" viewport to the logical backbuffer. That is harmless for our
// FORCE call (we pass full-BB dims, which the hook passes through / re-sets to
// the same logical BB) and is self-consistent for our RESTORE call (feeding the
// saved viewport back through the hook reproduces exactly the device state the
// hook would have left for that input — i.e. as if we never touched it).
typedef HRESULT (STDMETHODCALLTYPE *GetViewport_t)(
    IDirect3DDevice8 *self, D3DVIEWPORT8_X *pViewport);
typedef HRESULT (STDMETHODCALLTYPE *SetViewport_t)(
    IDirect3DDevice8 *self, const D3DVIEWPORT8_X *pViewport);

// ---- backbuffer-size query (device slot 16 -> surface slot 8) ----
// The Present hook hands us the engine's last SetViewport dims, which at boot
// is a sub-region (the 3D viewport), NOT the swap-chain backbuffer. To size the
// fullscreen black underlay + the aspect-fit video rect correctly we ask the
// device for the real backbuffer surface and read its desc.
//   IDirect3DDevice8::GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE Type,
//                                   IDirect3DSurface8 **ppBackBuffer)  [slot 16]
//   IDirect3DSurface8::GetDesc(D3DSURFACE_DESC8 *pDesc)               [slot 8]
//   IDirect3DSurface8::Release()                                      [slot 2]
typedef struct IDirect3DSurface8 IDirect3DSurface8;
#ifndef D3DBACKBUFFER_TYPE_MONO
#define D3DBACKBUFFER_TYPE_MONO 0
#endif
// D3D8 D3DSURFACE_DESC layout: Format, Type, Usage, Pool, Size, MultiSampleType,
// Width, Height. (D3D8's desc carries Size before the WxH; this differs from
// D3D9 — keep the full D3D8 order so the Width/Height offsets are correct.)
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
typedef HRESULT (STDMETHODCALLTYPE *GetBackBuffer_t)(
    IDirect3DDevice8 *self, UINT BackBuffer, DWORD Type,
    IDirect3DSurface8 **ppBackBuffer);
typedef HRESULT (STDMETHODCALLTYPE *Surf_GetDesc_t)(
    IDirect3DSurface8 *self, D3DSURFACE_DESC8_X *pDesc);

// (GetBackBuffer (slot 16) above is used by vid_query_backbuffer to read the real
//  backbuffer W/H/Format.)

#ifndef D3DFMT_X8R8G8B8
#define D3DFMT_X8R8G8B8 22
#endif

// dgVoodoo2 (D3D8->D3D11/12 wrapper) throws when DrawPrimitiveUP fires outside
// a BeginScene/EndScene pair. Hook_Present calls us AFTER the engine's
// EndScene, so we MUST open our own scene around the overlay draws. Standard
// D3D8 device vtable slots: BeginScene=34, EndScene=35 (consistent with the
// other indices we already use: CreateTexture=20, SetRenderState=50,
// SetTexture=61, DrawPrimitiveUP=72).
typedef HRESULT (STDMETHODCALLTYPE *BeginScene_t)(IDirect3DDevice8 *self);
typedef HRESULT (STDMETHODCALLTYPE *EndScene_t)(IDirect3DDevice8 *self);

// ---- D3D8 state save/restore (FIX 1: state leak) ----
//
// Hook_Present calls us AFTER the engine's EndScene; our overlay draw mutates a
// pile of device state (render states, texture-stage states, the bound texture,
// FVF, TEXTUREFACTOR, the viewport) and the engine assumes that state is
// untouched on its next frame. Without restoring, the TITLE screen rendered
// after a clip tears down inherits our last states (the final video frame burns
// in, the hex background breaks). So we snapshot the engine's state before our
// draw and restore it exactly afterward.
//
// PREFERRED: a D3D8 state block (D3DSBT_ALL) captures EVERYTHING in one call.
// CRITICAL D3D8-vs-D3D9 difference: in D3D8 a state block is a DWORD TOKEN
// (handle), NOT a COM object — CreateStateBlock(type, DWORD* pToken);
// CaptureStateBlock/ApplyStateBlock/DeleteStateBlock take that DWORD token.
//
// Vtable slots (derived from the canonical IDirect3DDevice8 method order and
// cross-checked against every slot this file ALREADY uses live — Present=15,
// CreateTexture=20, BeginScene=34, SetViewport=40, GetViewport=41,
// SetRenderState=50, SetTexture=61, SetTextureStageState=63, DrawPrimitiveUP=72,
// SetVertexShader=76 — which all line up exactly, giving high confidence):
//   51 GetRenderState
//   52 BeginStateBlock   53 EndStateBlock     54 ApplyStateBlock
//   55 CaptureStateBlock 56 DeleteStateBlock  57 CreateStateBlock
//   60 GetTexture        62 GetTextureStageState
//   (SetClipStatus=58 / GetClipStatus=59 sit between the state-block group and
//    GetTexture=60, the correct d3d8.h order — this anchors the group.)
#ifndef D3DSBT_ALL
#define D3DSBT_ALL 1
#endif

typedef HRESULT (STDMETHODCALLTYPE *CreateStateBlock_t)(
    IDirect3DDevice8 *self, DWORD Type, DWORD *pToken);
typedef HRESULT (STDMETHODCALLTYPE *CaptureStateBlock_t)(
    IDirect3DDevice8 *self, DWORD Token);
typedef HRESULT (STDMETHODCALLTYPE *ApplyStateBlock_t)(
    IDirect3DDevice8 *self, DWORD Token);
typedef HRESULT (STDMETHODCALLTYPE *DeleteStateBlock_t)(
    IDirect3DDevice8 *self, DWORD Token);

// Manual-fallback getters (used only if CreateStateBlock fails under a wrapper).
typedef HRESULT (STDMETHODCALLTYPE *GetRenderState_t)(
    IDirect3DDevice8 *self, DWORD State, DWORD *pValue);
typedef HRESULT (STDMETHODCALLTYPE *GetTextureStageState_t)(
    IDirect3DDevice8 *self, DWORD Stage, DWORD Type, DWORD *pValue);
typedef HRESULT (STDMETHODCALLTYPE *GetTexture_t)(
    IDirect3DDevice8 *self, DWORD Stage, IDirect3DTexture8 **ppTexture);
typedef HRESULT (STDMETHODCALLTYPE *GetVertexShader_t)(
    IDirect3DDevice8 *self, DWORD *pHandle);

// FVF: pre-transformed RHW + diffuse + 1 tex coord.
#define D3DFVF_XYZRHW   0x0004
#define D3DFVF_DIFFUSE  0x0040
#define D3DFVF_TEX1     0x0100
#define VIDEO_FVF       (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

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
#define D3DRS_SHADEMODE              9
#define D3DRS_TEXTUREFACTOR          60
#define D3DRS_DIFFUSEMATERIALSOURCE  145

#define D3DSHADE_FLAT                1
#define D3DSHADE_GOURAUD            2

#define D3DBLEND_SRCALPHA            5
#define D3DBLEND_INVSRCALPHA         6
#define D3DBLENDOP_ADD               1
#define D3DCULL_NONE                 1

#define D3DTSS_COLOROP               1
#define D3DTSS_COLORARG1             2
#define D3DTSS_COLORARG2             3
#define D3DTSS_ALPHAOP               4
#define D3DTSS_ALPHAARG1             5
#define D3DTSS_ALPHAARG2             6
// Sampler/filter + addressing states. These live in the SAME
// SetTextureStageState(stage,Type,Value) table in D3D8 (D3DTEXTURESTAGESTATETYPE);
// the sampler-state split is a D3D9 thing. Values per d3d8types.h.
#define D3DTSS_ADDRESSU             13
#define D3DTSS_ADDRESSV             14
#define D3DTSS_MAGFILTER           16
#define D3DTSS_MINFILTER           17
#define D3DTSS_MIPFILTER           18
#define D3DTSS_TEXCOORDINDEX       11
#define D3DTOP_MODULATE              4
#define D3DTOP_SELECTARG1            2
#define D3DTOP_DISABLE               1
#define D3DTA_TEXTURE                0x00000002
#define D3DTA_DIFFUSE                0x00000000
#define D3DTA_TFACTOR                0x00000003
#define D3DTEXF_NONE                 0
#define D3DTEXF_POINT                1
#define D3DTEXF_LINEAR               2
#define D3DTADDRESS_WRAP             1
#define D3DTADDRESS_CLAMP            3

// ---- Tunables ----
#define VID_RING_SLOTS     3        // spec: 3-slot ring
// BOOT-trigger warm-up: minimum Presents after a device appears before we even
// begin testing the start gate. A tiny delay lets the wrapper settle the swap
// chain before we hammer it with a fullscreen quad. (Unused by the CHARCREATE
// trigger, which starts off the live scene gate's rising edge, not a count.)
#define VID_START_AFTER_PRESENTS 8
// BOOT start: after the warm-up, hold the meteor a fixed wall-clock delay so the
// wide-cover boot poster (mod_boot_poster.c) gets a visible splash window first,
// then hand off. Wall-clock, NOT a present count and NOT the engine's "NOW
// LOADING done" signal (which on a heavy mod stack runs ~30s) -- so it lands at
// the same ~4s on every machine.
#define VID_BOOT_POSTER_MS 4000
// Absolute fallback: if the cover never becomes visible (device never goes
// texture-able), start the meteor anyway this long after the first present so the
// boot can't stall on a black screen forever.
#define VID_BOOT_HARD_CAP_MS 12000

// ---- BOOT COVER still image (the meteor's pre-roll splash) ----
// Replaces the separate mod_boot_poster.c draw: the boot leg shows
// patches/psobb_boot_poster.png fullscreen (through vid_draw_overlay's state
// capture/restore) opaque for VID_COVER_HOLD_MS, then ramps its diffuse alpha
// 255->0 over VID_COVER_FADE_MS (fades to the overlay's black underlay), then
// starts the meteor. A skip during the opaque hold jumps straight to the fade.
#define VID_COVER_HOLD_MS  3500     // opaque hold before the fade
#define VID_COVER_FADE_MS  600      // cover fade-out (alpha 255->0)
#define VID_COVER_MAX_DIM  1280     // texture-dim cap (== bp's BP_MAX_TEX_DIM)
// Meteor fade-IN (boot leg only): when the meteor starts, ramp its quad's diffuse
// alpha 0->255 over this window so it dissolves up from black (no hard cut). The
// char-create intros leg keeps a constant 0xFFFFFFFF diffuse (no fade).
#define VID_METEOR_FADEIN_MS 600

// Implemented in mod_boot_poster.c: force-disable the poster when the meteor takes
// over (kept; the cover path now lives here, so we still disable the legacy poster).
extern void boot_poster_force_disable(void);

// HARD startup grace (ms) before any skip is honored. The Enter that confirmed
// the "create new Play Character? YES/NO" dialog is still down (and bounces) at
// the cc-gate rising edge that starts char-create playback, so a pure debounce
// instantly tore the clip down before it was ever visible. This floor
// guarantees the clip plays VISIBLY for at least this long; only a deliberate
// FRESH key press after it can skip. Effective grace = max(this, debounce).
#define VID_SKIP_GRACE_MS 2000

// ---- Module state ----

typedef struct {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
} VidVtx;

typedef struct {
    // ---- config (set once in init) ----
    int   enabled;                 // master switch (VideoEnable)
    int   skippable;               // VideoSkippable
    int   max_seconds;             // VideoMaxSeconds watchdog
    int   skip_debounce_ms;        // VideoSkipDebounceMs
    int   audio;                   // VideoAudio (sibling .wav via mci)
    int   diag;                    // VideoDiag (1 = draw a single RED half-width
                                   //   untextured quad instead of the video, to
                                   //   isolate draw+diffuse from texture sampling)
    char  path[MAX_PATH];          // resolved (absolute) ACTIVE source path (what
                                   //   the next vid_start decodes). For trigger=
                                   //   both this is boot_path during the boot leg
                                   //   and char_path after the boot leg ends.
    char  ffmpeg[MAX_PATH];        // resolved ffmpeg.exe path (or "ffmpeg")
    char  wav[MAX_PATH];           // sibling "<path>.wav" if audio==1

    // ---- VID_TRIGGER_BOTH: the two source paths + leg tracking ----
    // For trigger=both ONE instance plays boot_path at boot, then char_path at
    // char-create. `path` (above) is swapped between them as the active source.
    // For trigger=boot/charcreate, char_path mirrors `path` and boot_path is "".
    char  char_path[MAX_PATH];     // the CHAR-CREATE leg source (VideoPath)
    char  boot_path[MAX_PATH];     // the BOOT leg source (VideoBootPath; both-only)
    int   boot_leg_started;        // 1 once the both-mode boot leg actually began
                                   //   playing (so we can detect its end)
    int   boot_leg_done;           // 1 once the boot leg (both-mode) has ended and
                                   //   the active path was switched to char_path

    int   init_done;
    int   disabled_perm;           // 1 = never try again
    LONG  present_calls;           // diagnostics

    // ---- Stage-2 trigger mode (VID_TRIGGER_OFF/BOOT/CHARCREATE) ----
    int   trigger;                 // VideoTrigger

    // ---- decode backend (VID_DECODER_MF [default] / VID_DECODER_FFMPEG) ----
    int   decoder;                 // VideoDecoder

    // ---- boot trigger (mode==BOOT): one-shot present-count start ----
    int   start_requested;         // 1 until we've kicked playback once
    LONG  present_seen;            // present count since a valid device
    DWORD boot_first_ms;           // GetTickCount() at the boot leg's first present

    // ---- BOOT COVER still (the meteor pre-roll splash) ----
    // Lifecycle: 0=not started, 1=loading/loaded, 2=opaque hold, 3=fading,
    // 4=done (meteor may start). Owns the boot-leg timing now (the meteor starts
    // when the still's fade completes, replacing the boot_poster_shown_ms gate).
    int   cover_phase;             // 0 idle, 1 loaded, 2 hold, 3 fade, 4 done
    int   cover_load_tried;        // 1 once we've attempted the PNG load (success or fail)
    int   cover_ok;                // 1 if the cover texture is uploaded + drawable
    int   cover_w, cover_h;        // cover image dims (possibly downscaled)
    unsigned char *cover_rgba;     // BGRA pixels (freed after upload)
    IDirect3DTexture8 *cover_tex;  // cover texture (A8R8G8B8 managed)
    DWORD cover_phase_ms;          // GetTickCount() when the current phase began
    int   cover_skip_seen_up;      // skip-edge debounce: both keys seen UP once
    int   cover_skip_prev_down;    // prev Enter|Esc down-state (edge detect)

    // ---- meteor fade-IN (boot leg only) ----
    DWORD meteor_fadein_ms;        // GetTickCount() at meteor start; 0 = no fade-in

    // ---- char-create trigger (mode==CHARCREATE): SOURCE-EVENT driven ----
    // No scene-id poll. The transition-request setter sub_007A60DC @0x007A60DC
    // is MinHook'd (cc_on_request) and reads ONLY the request target. It sets
    // start_requested on request(3) (the unique scripted-intro start) and
    // stop_requested on the real exits (request(0|2|0xB)), bracketed by its own
    // cc_session latch so the 3->5 intro->class-select hand-off is HELD (single
    // continuous cover). The Present body consumes these one-shots; it NEVER
    // reads 0x00AAB384 / 0x00A3A93C / the player array for the CHARCREATE
    // trigger. See _cc_scene_entry_exit_hooks.md §5. start_requested is shared
    // with the BOOT one-shot (a CHARCREATE build never arms BOOT and vice
    // versa, so there is no collision).
    volatile LONG cc_session;      // OUR latch (set/cleared only by cc_on_request).
                                   // Also the SOLE discriminator for the scene-3
                                   // ownership replica (0x007C1588 runs the replica
                                   // while set, else byte-identical passthrough) and
                                   // for the engine-exit arm. The audio mute is the
                                   // separate g_soundctrl_armed latch (Option K).
                                   // Our XAudio2 video audio is a separate device/
                                   // session, never on the engine audio path.
    volatile LONG stop_requested;  // one-shot: cc_on_request asks Present to tear down
    volatile LONG cc_exit_armed;   // one-shot: at video EOF/skip the owned scene-3 frame
                                   //   RETURNS THROUGH the real 0x007C1588 (no replica
                                   //   request(5)); consumed in cc_scene3_owned_frame.

    // ---- playback session ----
    volatile LONG playing;         // 1 while a session is live
    int   frame_w, frame_h;        // decode size (== backbuffer when started)
    double fps;                    // video frame rate
    LONGLONG qpc_freq;             // QueryPerformanceFrequency
    LONGLONG qpc_start;            // playback t0
    int   audio_open;              // mci alias open

    // ffmpeg child + pipe + reader thread
    HANDLE hJob;
    HANDLE hProc;
    HANDLE hThread;                // child main thread (from CreateProcess)
    HANDLE hPipeRead;              // our read end of ffmpeg stdout
    HANDLE hReader;                // reader thread handle
    volatile LONG reader_run;      // 1 while reader should keep reading
    volatile LONG eof;             // set on EOF / broken pipe

#ifndef VID_NO_MF
    // ---- in-process Media Foundation decode (VID_DECODER_MF) ----
    // mf_reader is the IMFSourceReader created on the engine thread in
    // vid_mf_open and consumed (ReadSample) by vid_mf_reader_thread; released in
    // vid_mf_close_handles AFTER the reader joins. mf_stride is the negotiated
    // MF_MT_DEFAULT_STRIDE (the spike observed +7680 / top-down for the real
    // clip, but it CAN be negative => MFCopyImage handles the sign). mf_started
    // is 1 once MFStartup succeeded so MFShutdown is balanced exactly once.
    struct IMFSourceReader *mf_reader;
    LONG  mf_stride;               // MF_MT_DEFAULT_STRIDE (may be negative)
    int   mf_started;              // 1 once MFStartup succeeded

    // ---- in-process MF AUDIO -> XAudio2 render (audio-clock master) ----
    HMODULE   xa_dll;              // xaudio2_9.dll (FreeLibrary on teardown)
    IXAudio2 *xa;                  // engine (Release on teardown)
    IXAudio2MasteringVoice *xa_master;  // DestroyVoice on teardown
    IXAudio2SourceVoice    *xa_voice;   // DestroyVoice BEFORE master
    int    audio_mf_ok;           // 1 once the voice is live (else silent video)
    int    audio_stream_idx;      // the reader's audio stream index (for dispatch)
    UINT32 audio_chans, audio_rate, audio_bits, audio_block;  // negotiated PCM fmt
    volatile LONG audio_started;  // 1 once the FIRST audio buffer was submitted
                                  // (seeds qpc_start -> A/V share one timeline)
    // in-flight PCM block FIFO (submitted buffers we must free after the voice
    // consumes them). Sized so we never block the reader; reaped each loop.
    #define VID_AUDIO_QMAX 64
    // Cushion the reader keeps queued AHEAD of the voice (< QMAX so we never overfill
    // -> never drop). ~32 PCM buffers (~0.6-1.3s) ride out slow video frames so the
    // XAudio2 voice never underruns — the audio-crackle fix. The reader tops the FIFO
    // up to this, then reads paced video.
    #define VID_AUDIO_TARGET 32
    BYTE  *audio_q[VID_AUDIO_QMAX];
    LONG   audio_q_head, audio_q_tail;   // ring indices (head=submit, tail=reap)
#endif

    // 3-slot ring of frame buffers (each frame_w*frame_h*4 bytes)
    unsigned char *ring[VID_RING_SLOTS];
    size_t ring_bytes;             // size of each ring buffer
    volatile LONG ring_filled;     // count of frames the reader has produced
    volatile LONG newest_slot;     // slot index of the newest complete frame
    volatile LONG newest_index;    // frame index of the newest complete frame

    // ---- skip debounce ----
    int   skip_armed;              // becomes 1 once both keys seen UP + delay
    int   keys_seen_up;            // both VK_RETURN and VK_ESCAPE were UP once
    int   skip_prev_down;          // prev Enter|Esc down-state (edge detect)
    int   skip_draining;           // BOOT-leg only: a skip fired but the skip key is
                                   // still physically DOWN. Keep the cover up (redraw
                                   // last frame) until BOTH VK_RETURN and VK_ESCAPE
                                   // read UP, THEN teardown — so the revealed title
                                   // doesn't poll the held Enter and pop its menu.

    // ---- D3D resources ----
    IDirect3DTexture8 *texture;
    int   tex_w, tex_h;            // texture dims (== frame dims)
    LONG  last_uploaded_index;     // frame index last copied into the texture

    // frame index last drawn by the textured-quad overlay (diagnostic marker).
    LONG  last_copied_index;

    // ---- state save/restore (FIX 1) ----
    int   sb_tried;                // 1 once we've attempted CreateStateBlock
    int   sb_have;                 // 1 if the state-block path is usable
    DWORD sb_token;                // D3DSBT_ALL state-block token (D3D8 = DWORD)
    void *sb_dev;                  // device the state block belongs to (for Delete)
    int   save_method_logged;      // one-shot "[video] state-save = ..." guard
} VidState;

static VidState g_video;

// ---- FIX 1: state-save tables + snapshot (declared here, before vid_teardown,
// which references g_vid_saved for cleanup). The state-block path uses none of
// these; they back only the manual fallback. ----

// The full set of render states vid_render_quad writes (phase-5 block).
static const DWORD VID_SAVED_RS[] = {
    D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_LIGHTING, D3DRS_FOGENABLE,
    D3DRS_STENCILENABLE, D3DRS_CULLMODE, D3DRS_CLIPPING, D3DRS_COLORVERTEX,
    D3DRS_ALPHATESTENABLE, D3DRS_SHADEMODE, D3DRS_TEXTUREFACTOR,
    D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_ALPHABLENDENABLE,
    // The blend path (cover fade / meteor fade-in) also writes these; save them so
    // the manual-fallback restore can't leak them into the engine's next frame.
    D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
};
#define VID_NUM_SAVED_RS (sizeof(VID_SAVED_RS)/sizeof(VID_SAVED_RS[0]))

// The full set of stage-0 texture-stage states vid_render_quad writes across
// BOTH the textured and untextured paths (their union).
static const DWORD VID_SAVED_TSS0[] = {
    D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2,
    D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
    D3DTSS_MAGFILTER, D3DTSS_MINFILTER, D3DTSS_MIPFILTER,
    D3DTSS_ADDRESSU, D3DTSS_ADDRESSV, D3DTSS_TEXCOORDINDEX,
};
#define VID_NUM_SAVED_TSS0 (sizeof(VID_SAVED_TSS0)/sizeof(VID_SAVED_TSS0[0]))
// Stage-1 states it writes (COLOROP/ALPHAOP -> DISABLE).
static const DWORD VID_SAVED_TSS1[] = { D3DTSS_COLOROP, D3DTSS_ALPHAOP };
#define VID_NUM_SAVED_TSS1 (sizeof(VID_SAVED_TSS1)/sizeof(VID_SAVED_TSS1[0]))

// Manual-fallback saved snapshot.
typedef struct {
    DWORD rs[VID_NUM_SAVED_RS];
    DWORD tss0[VID_NUM_SAVED_TSS0];
    DWORD tss1[VID_NUM_SAVED_TSS1];
    IDirect3DTexture8 *tex0;       // bound stage-0 texture (AddRef'd by GetTexture)
    DWORD vshader;                 // current vertex shader / FVF handle
    int   valid;
} VidSavedState;

static VidSavedState g_vid_saved;

// Granular present-path phase, set immediately BEFORE each D3D op so a single
// SEH-caught run names the exact failing call. Legend:
//   1=ensure_texture/CreateTexture  2=LockRect  3=row memcpy  4=BeginScene
//   5=SetRenderState block  6=SetTextureStageState/SetTexture  7=SetVertexShader
//   8=DrawPrimitiveUP  9=EndScene
static volatile long g_vid_phase;

// ---- helpers ----

static void vid_disable_perm(const char *reason)
{
    if (g_video.disabled_perm) return;
    g_video.disabled_perm = 1;
    log_line("[video] disabled: %s", reason ? reason : "(unknown)");
}

// Resolve a possibly-relative path to an absolute path next to psobb.exe.
// Mirrors mod_boot_poster.c's bp_resolve_path exactly.
static void vid_resolve_path(const char *cfg_path, char *out, size_t outlen)
{
    out[0] = 0;
    if (!cfg_path || !cfg_path[0]) return;
    if ((cfg_path[0] && cfg_path[1] == ':') ||
        (cfg_path[0] == '\\' && cfg_path[1] == '\\')) {
        _snprintf_s(out, outlen, _TRUNCATE, "%s", cfg_path);
        for (char *p = out; *p; ++p) if (*p == '/') *p = '\\';
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
    for (char *p = out; *p; ++p) if (*p == '/') *p = '\\';
}

static int vid_file_exists(const char *path)
{
    if (!path || !path[0]) return 0;
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---- CHARCREATE source-event hook (NO scene-id reads) ----
//
// Per _cc_scene_entry_exit_hooks.md §5: the char-create cover is driven purely
// from the engine's transition-request setter `sub_007A60DC` @0x007A60DC, which
// EVERY scene transition flows through. We MinHook it and read ONLY its single
// __cdecl arg (`int target` = the scene the engine is about to switch to). We
// NEVER read the current-scene global 0x00AAB384, the (mis-named) 0x00A3A93C
// game-list buffer, or the player array for the CHARCREATE trigger.
//
// ENTRY/EXIT logic (OWNERSHIP MODEL: cover scene-3 ONLY):
//   target == 3                          -> cc_session=1; start the cover.
//                                           (0x004143E5 is the ONLY request(3)
//                                            site in the image — the confirm-
//                                            gated "create new char? YES" path;
//                                            picking an EXISTING char goes
//                                            straight to 0xB and never hits 3.)
//   cc_session && target == 5            -> INTRO OVER: TEAR DOWN the cover. The
//                                           cover spans scene-3 ONLY now; the 3->5
//                                           transition (fired by the engine after
//                                           WE arm 0xAAE988/0xAAE980 at video EOF/
//                                           skip) lands here and uncovers the live,
//                                           now-running class-select.
//   cc_session && target in {0,2,0xB}    -> cc_session=0; stop the cover + re-arm
//                                           (created -> 0xB, cancel -> 2/0).
//   otherwise                            -> ignored.
//
// LIVE-RE CONFIRMED (harness disasm on the attached io client):
//   0x007A60DC: 8B 44 24 04  mov eax,[esp+4]   (arg0 at [esp+4], __cdecl)
//               C7 05 94 B3 AA 00 01 00 00 00  mov [0xAAB394],1 (MinHook-relocatable)
//   0x004143E5: 6A 03        push 3
//   0x004143E7: E8 F0 1C 39 00  call 0x007A60DC (unique request(3) site).
//
// Re-arm is automatic: a later request(3) re-sets cc_session and re-requests
// start. The handler is branch-only (no allocations, no D3D); it runs at most a
// few times/sec, only on transitions.
#define CC_REQUEST_SETTER_VA  0x007A60DCu

static LPVOID g_tramp_7a60dc = NULL;   // MinHook trampoline -> the real setter
static int    g_cc_hook_installed = 0;

// C handler — receives the target scene id the engine is about to switch to.
// Trigger-gated: a pure no-op unless this is a CHARCREATE-trigger build, so it
// is byte-identical to today when the cover is not configured for char-create.
// Forward decls — the audio-mute (Option K) is armed here on request(3) (engine
// thread, BEFORE the first scene-3 dispatch) but is defined further down with the
// rest of the cover machinery. The scene-3 ownership hook is installed eagerly at
// mod_video_init (not here), and is a byte-identical passthrough until cc_session
// is set — so by the time request(3) sets the latch, the replica is already armed.
static void cc_audio_arm(void);
static void cc_audio_disarm(void);
// Read-only diagnostic helpers (defined in the diag block below). adiag_scene()
// is SEH-guarded and used ONLY for log context here — never as a discriminator.
static int adiag_scene(void);
static volatile LONG adiag_cc(void);

// Does this trigger drive the CHAR-CREATE leg (the request(3) ownership cover)?
// True for CHARCREATE and for BOTH (whose char leg is identical to CHARCREATE).
// The BOOT one-shot leg of BOTH is a SEPARATE path that never sets cc_session, so
// the ownership writes here are reached ONLY via request(3) — never by the boot leg.
static int cc_is_charcreate_trigger(void)
{
    return g_video.trigger == VID_TRIGGER_CHARCREATE ||
           g_video.trigger == VID_TRIGGER_BOTH;
}

static void __cdecl cc_on_request(int target)
{
    // DIAG: log EVERY transition request (before any trigger gate) so the real
    // scene/cover lifecycle is captured even on a non-charcreate build. The scene
    // id is read via the SEH-guarded adiag_scene() ONLY for log context — it is
    // NEVER a discriminator. The COVER PATH decisions below key STRICTLY off the
    // call's `target` arg + the cc_session latch (no scene-id read in any branch).
    log_line("[adiag] request target=%d scene=%d cc_session=%ld",
             target, adiag_scene(),
             (long)InterlockedCompareExchange(&g_video.cc_session, 0, 0));

    if (!cc_is_charcreate_trigger()) return;

    if (target == 3) {                                  // ENTRY: scripted intro
        // The scene-3 ownership hook is already installed (eagerly at init) as a
        // passthrough; setting cc_session here flips it into the replica BEFORE the
        // pump dispatches scene-3 (we run inside the request setter, ahead of the
        // transition commit). Then arm the audio mute + ask Present to start.
        // BOTH mode: ensure the active source is the CHAR-CREATE path before we
        // kick the char leg. The boot-leg teardown normally already switched it
        // (boot_leg_done), but if request(3) somehow arrives while the boot leg is
        // still the active path (e.g. boot leg never ran), force the swap here so
        // the char leg never accidentally re-decodes the boot video. char_path is
        // mirrored to `path` for boot/charcreate triggers, so this is a no-op there.
        if (g_video.char_path[0])
            _snprintf_s(g_video.path, sizeof(g_video.path), _TRUNCATE,
                        "%s", g_video.char_path);
        g_video.boot_leg_done = 1;                      // the char leg owns playback now
        InterlockedExchange(&g_video.cc_session, 1);    // the replica now owns scene-3
        // (No audio mute on char-create: the intro ADX is prevented at its SOURCE by
        //  the play_bgm 0x00814AFC cc_session gate — no SOUNDCTRL zeroing, so engine
        //  audio is untouched and there is no "BGM gone after skip".)
        g_video.start_requested = 1;                    // Present body kicks vid_start
    } else if (InterlockedCompareExchange(&g_video.cc_session, 0, 0)) {
        if (target == 5 || target == 0 || target == 2 || target == 0xB) {
            // 5  = intro over (the replica drove the 3->5 we armed) -> uncover to the
            //      live class-select. 0/2/0xB = a real exit (created/cancelled).
            // Clear cc_session BEFORE scene-5's first dispatch (we run inside the
            // request setter, before the pump commits the transition) so the
            // 0x007C1588 stub reverts to passthrough and scene-5 runs fully live.
            InterlockedExchange(&g_video.cc_session, 0);
            cc_audio_disarm();                          // restore the SAVED SOUNDCTRL
            InterlockedExchange(&g_video.stop_requested, 1);
        }
    }
}

// naked trampoline — __cdecl, arg0 at [esp+4]; preserve all regs+flags. After
// pushad(0x20)+pushfd(4) the original [esp+4] sits at [esp+0x28] (the same
// convention pso_widescreen.c's stub_70d53c_cbp_bake uses for its [esp+4] arg).
static __declspec(naked) void stub_7a60dc(void)
{
    __asm {
        pushad
        pushfd
        mov  eax, [esp + 0x28]        // arg0 = target (orig [esp+4] after pushad+pushfd)
        push eax
        call cc_on_request
        add  esp, 4
        popfd
        popad
        jmp  [g_tramp_7a60dc]         // continue into the real request setter
    }
}

// Install the CHARCREATE source-event hook. Idempotent; MH_Initialize tolerates
// ALREADY_INITIALIZED (pso_widescreen.c's char-create hooks share the MinHook
// instance). Returns 1 on success.
static int cc_request_hook_install(void)
{
    if (g_cc_hook_installed) return 1;
    MH_STATUS rc = MH_Initialize();
    if (rc != MH_OK && rc != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("[video] cc-hook MH_Initialize failed rc=%d", (int)rc);
        return 0;
    }
    rc = MH_CreateHook((LPVOID)(uintptr_t)CC_REQUEST_SETTER_VA,
                       (LPVOID)stub_7a60dc, &g_tramp_7a60dc);
    if (rc != MH_OK) {
        log_line("[video] cc-hook MH_CreateHook(0x%08x) failed rc=%d",
                 CC_REQUEST_SETTER_VA, (int)rc);
        return 0;
    }
    rc = MH_EnableHook((LPVOID)(uintptr_t)CC_REQUEST_SETTER_VA);
    if (rc != MH_OK) {
        log_line("[video] cc-hook MH_EnableHook failed rc=%d", (int)rc);
        return 0;
    }
    g_cc_hook_installed = 1;
    log_line("[video] cc source-event hook installed @0x%08x tramp=%p",
             CC_REQUEST_SETTER_VA, g_tramp_7a60dc);
    return 1;
}

static void cc_request_hook_uninstall(void)
{
    if (!g_cc_hook_installed) return;
    MH_DisableHook((LPVOID)(uintptr_t)CC_REQUEST_SETTER_VA);
    MH_RemoveHook((LPVOID)(uintptr_t)CC_REQUEST_SETTER_VA);
    g_cc_hook_installed = 0;
    g_tramp_7a60dc = NULL;
}

// Pointer-hoist of the session latch (matches the asi's static-pointer idiom) so
// the naked scene-3 ownership stub reads the LONG via a stable global, not a
// struct-field operand the inline assembler may choke on.
static volatile LONG *g_cc_session_p = &g_video.cc_session;

// (adiag_scene/adiag_cc forward-declared above, before cc_on_request.)

// The two scene-3 exit-gate bytes (VERIFIED disasm 0x007C15C4..0x007C1614). The
// scene-3 update reads byte[0xAAE988] (substate) then byte[0xAAE980] (==1 -> the
// 3->5 request). The ownership replica READS these; cc_arm_engine_exit (below)
// WRITES them at video EOF/SKIP to drive the engine's own exit. One definition,
// used by both (the later cc_arm_engine_exit block reuses these macros).
#define CC_EXIT_FLAG_988_R  ((volatile unsigned char *)(uintptr_t)0x00AAE988u)  // substate gate
#define CC_EXIT_FLAG_980_R  ((volatile unsigned char *)(uintptr_t)0x00AAE980u)  // ==1 -> request(5)

// ---- ENGINE-AUDIO suppression: Option K — the SOUNDCTRL module-enable flags ----
//
// The char-create intro BGM (CUBE_OPENING.adx) is a software-decoded ADX STREAM, NOT
// a by-id 0x00ACB388 secondary buffer — so it does NOT pass through the play-by-id
// core 0x00828E4C and is NOT in the table STOP-ALL 0x00829AD0 walks. THREE prior
// levers (the 0x004D80A4 3D-pump, the 0x00828E4C by-id gate, STOP-ALL alone) each
// hit a wrong subsystem. The ONE choke that ALL THREE audio subsystems honor is the
// engine's own "SOUNDCTRL" module-enable flags 0x00A46C88/8C/90/94 (getter
// 0x00483740 = [0x00A46C8C]; checked at the TOP of every service/play/feed path).
//
// Live-proven: writing 0 to all four dwords silences the engine BGM immediately
// (no ring-out; the engine does NOT auto-restore), and restoring resumes audio. So
// the lever is: SAVE the four dwords + WRITE 0 at ARM; RESTORE the SAVED values at
// DISARM.
//
//   * RESTORE the SAVED values, NOT a hardcoded 1, and NOT the setter 0x00483760
//     (which FORCES all four to 1) — a user who disabled sound in-game must stay
//     disabled after the cover tears down.
//   * NO MinHook, no detour: just a 4-dword save / zero / restore. Reversible,
//     SEH-safe (aligned dword writes the mixer thread tolerates frame-to-frame),
//     reachable from our cover latch with NO scene-id read.
//   * Cannot touch our XAudio2 video audio: these flags gate ONLY the engine's
//     DirectSound mixer; XAudio2 (g_video.xa / xa_voice / xa_master, a separate
//     engine + WASAPI endpoint) never reads them.
//
// TODO: if a residual ring-out of the in-flight ADX stream past one decode block is
// observed (i.e. the stream feed 0x0070EF28/0x00839838 also gates on SOUNDCTRL), add
// an explicit stream tear at ARM (decoder dtor 0x00829DE0 with the live stream handle
// from [*0x00A9CD54+0x24], SEH-guarded). Under FULL scene-3 ownership (the 0x007C1588
// replica) the ADX-start blocks are never entered, so the stream should never start
// and the tear is likely unnecessary; SOUNDCTRL=0 silences engine audio with no
// observed ring-out.
#define CC_SOUNDCTRL_VA   0x00A46C88u   // first of four contiguous "SOUNDCTRL" dwords

static volatile DWORD *const g_soundctrl = (volatile DWORD *)(uintptr_t)CC_SOUNDCTRL_VA;
static DWORD g_soundctrl_saved[4];      // saved 0x00A46C88/8C/90/94 (valid iff armed)
static volatile LONG g_soundctrl_armed = 0;  // 1 while the four flags are forced 0

// ---- ENGINE-BGM / streamed-ADX suppression: StopAllAudio_00815434 ------------
//
// AUDIO-LEAK FIX (disasm-verified). The SOUNDCTRL flags above gate
// ONLY the by-id DirectSound SE/BGM mixer (getter 0x00483740 = [0x00A46C8C],
// checked at the top of the play-by-id core 0x00828E4C + STOP-ALL 0x00829AD0).
// They DO NOT gate the char-create intro BGM: CUBE_OPENING.adx is started in the
// scene-3 ENTRY ctor 0x007C1640 (via 0x0070F41C -> play_bgm, the BGM player pool)
// and serviced every frame by 0x0086DBB0 (channel table 0x00AE7F20) with NO
// SOUNDCTRL check. So zeroing SOUNDCTRL left the ADX audible UNDER the video.
// Worse, zeroing [0x00A46C8C] turns STOP-ALL into a no-op.
//
// The engine's OWN BGM teardown is StopAllAudio_00815434 (__cdecl, no args): it
// walks the two BGM players (one holds CUBE_OPENING), stops/resets each, and
// clears the BGM fade globals. It is the primitive ~15 scene-exit sites call, so
// it is the well-trodden, reversible cut (the next scene's play_bgm restarts BGM
// normally). It IS gated by get_settings_audio_bgm (one of the SOUNDCTRL flags),
// so we must call it BEFORE we zero SOUNDCTRL, and we must call it on each owned
// frame: the ADX starts in the scene-3 ENTRY ctor 0x007C1714 (NOT hooked — the
// replica only owns the per-frame tick 0x007C1588), which runs AFTER request(3)
// commits, i.e. AFTER cc_audio_arm. A one-shot at arm would fire before the ADX
// even starts; the per-owned-frame call (cc_audio_kill_bgm in the replica) catches
// it on the first cover frame and keeps it silenced for the cover's duration.
#define CC_STOP_ALL_AUDIO_VA  0x00815434u   // StopAllAudio_00815434 (__cdecl, void)

typedef void (__cdecl *cc_stop_all_audio_fn_t)(void);

// Stop the engine BGM player pool (the only lever that silences the streamed ADX
// intro). SEH-guarded; safe to call repeatedly (idempotent reset of already-stopped
// players). Trigger-gated so a non-charcreate build never calls into engine audio.
// (BOTH counts as charcreate-capable; this only ever runs from the cc_session-set
// owned-frame replica, so the boot leg — which never sets cc_session — never reaches it.)
static void cc_audio_kill_bgm(void)
{
    if (!cc_is_charcreate_trigger()) return;
    __try {
        cc_stop_all_audio_fn_t stop_all =
            (cc_stop_all_audio_fn_t)(uintptr_t)CC_STOP_ALL_AUDIO_VA;
        stop_all();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Engine BGM teardown must never crash the cover; swallow.
    }
}

// ARM: (1) stop the engine BGM pool — the ONLY lever that silences the streamed
// ADX intro (SOUNDCTRL does not gate it; see CC_STOP_ALL_AUDIO_VA), called FIRST
// while SOUNDCTRL is still at its saved values (StopAllAudio is gated by an
// SOUNDCTRL flag), then (2) save the four SOUNDCTRL dwords + write 0 to silence
// the by-id DirectSound SE/BGM mixer. One-shot per cover (re-arm is a no-op while
// already armed, so we never overwrite the saved values with our own 0s). NOTE the
// per-owned-frame cc_audio_kill_bgm() in the replica is what actually catches the
// ADX (it starts on scene-3 ENTRY, AFTER this arm); this one-shot only kills any
// BGM still ringing out from char-select at request(3) time.
static void cc_audio_arm(void)
{
    if (!cc_is_charcreate_trigger()) return;                   // only for the cover (incl. BOTH char leg)
    if (InterlockedCompareExchange(&g_soundctrl_armed, 1, 0) != 0) return; // already armed
    cc_audio_kill_bgm();   // (1) stop BGM pool BEFORE zeroing SOUNDCTRL (which would gate it out)
    __try {
        g_soundctrl_saved[0] = g_soundctrl[0];
        g_soundctrl_saved[1] = g_soundctrl[1];
        g_soundctrl_saved[2] = g_soundctrl[2];
        g_soundctrl_saved[3] = g_soundctrl[3];
        g_soundctrl[0] = 0; g_soundctrl[1] = 0;
        g_soundctrl[2] = 0; g_soundctrl[3] = 0;
        log_line("[video] engine audio MUTED (cover up): SOUNDCTRL saved=%lu/%lu/%lu/%lu -> 0",
                 g_soundctrl_saved[0], g_soundctrl_saved[1],
                 g_soundctrl_saved[2], g_soundctrl_saved[3]);
        log_line("[adiag] SOUNDCTRL arm scene=%d cc_session=%ld", adiag_scene(),
                 (long)adiag_cc());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Should never fault (committed engine globals) — but a mute must never
        // crash the cover. Leave armed=1 (the disarm restore is a no-op if we
        // never wrote) and log.
        log_line("[video] SOUNDCTRL arm SEH (mute skipped)");
    }
}

// DISARM: restore the SAVED four dwords (NOT a forced 1). No-op if not armed.
static void cc_audio_disarm(void)
{
    if (InterlockedCompareExchange(&g_soundctrl_armed, 0, 1) != 1) return; // not armed
    __try {
        g_soundctrl[0] = g_soundctrl_saved[0];
        g_soundctrl[1] = g_soundctrl_saved[1];
        g_soundctrl[2] = g_soundctrl_saved[2];
        g_soundctrl[3] = g_soundctrl_saved[3];
        log_line("[video] engine audio RESTORED (cover down): SOUNDCTRL <- %lu/%lu/%lu/%lu",
                 g_soundctrl_saved[0], g_soundctrl_saved[1],
                 g_soundctrl_saved[2], g_soundctrl_saved[3]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("[video] SOUNDCTRL disarm SEH (restore skipped)");
    }
}

// ---- INTRO-BGM PREVENTION: the "no mute" replacement for char-create ----------
// Rather than zeroing SOUNDCTRL / per-frame StopAllAudio to silence the intro ADX
// UNDER the video, PREVENT it from starting at its source (inserting the video as a
// proper frame should not require muting anything).
// The intro CUBE_OPENING.adx is started by play_bgm (void __cdecl, byte* filename) at
// 0x00814AFC (chain: scene-3 entry ctor 0x007C1714 -> 0x007C16D4 -> 0x0070F41C ->
// 0x0070F50C -> call 0x00814AFC), which runs AFTER request(3) sets cc_session. We
// MinHook play_bgm: while cc_session owns scene-3 the detour RETURNS WITHOUT starting
// any BGM; otherwise it passes through to the real player. cc_session is the sole gate
// (no scene-id read) and the cover is the only window we want zero engine BGM, so it is
// surgical + reversible. __cdecl -> a plain C detour cleans the stack on the skip path.
#define CC_PLAY_BGM_VA   0x00814AFCu   // void __cdecl play_bgm(byte *filename)
typedef void (__cdecl *cc_play_bgm_fn_t)(const void *filename);
static LPVOID g_tramp_814afc = NULL;
static int    g_play_bgm_hook_installed = 0;

static void __cdecl cc_play_bgm_detour(const void *filename)
{
    if (InterlockedCompareExchange(&g_video.cc_session, 0, 0)) return;  // cover up -> no BGM
    if (g_tramp_814afc) ((cc_play_bgm_fn_t)g_tramp_814afc)(filename);   // else pass through
}

static int cc_play_bgm_hook_install(void)
{
    if (g_play_bgm_hook_installed) return 1;
    MH_STATUS rc = MH_Initialize();
    if (rc != MH_OK && rc != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("[video] play_bgm MH_Initialize failed rc=%d", (int)rc);
        return 0;
    }
    rc = MH_CreateHook((LPVOID)(uintptr_t)CC_PLAY_BGM_VA,
                       (LPVOID)cc_play_bgm_detour, &g_tramp_814afc);
    if (rc != MH_OK) {
        log_line("[video] play_bgm MH_CreateHook(0x%08x) failed rc=%d",
                 CC_PLAY_BGM_VA, (int)rc);
        return 0;
    }
    rc = MH_EnableHook((LPVOID)(uintptr_t)CC_PLAY_BGM_VA);
    if (rc != MH_OK) {
        log_line("[video] play_bgm MH_EnableHook failed rc=%d", (int)rc);
        return 0;
    }
    g_play_bgm_hook_installed = 1;
    log_line("[video] intro-BGM-prevent hook installed @0x%08x tramp=%p",
             CC_PLAY_BGM_VA, g_tramp_814afc);
    return 1;
}

static void cc_play_bgm_hook_uninstall(void)
{
    if (!g_play_bgm_hook_installed) return;
    MH_DisableHook((LPVOID)(uintptr_t)CC_PLAY_BGM_VA);
    MH_RemoveHook((LPVOID)(uintptr_t)CC_PLAY_BGM_VA);
    g_play_bgm_hook_installed = 0;
    g_tramp_814afc = NULL;
}

// ---- BOOT TITLE-DEFER: the boot leg's "video as a proper frame" --------------
// The boot video must play FIRST, THEN the title fades in. Without this the title
// (scene 1) builds UNDER the video overlay: its objects are made
// in .init 0x007A45D0 and its .update 0x007A4418 fires a one-shot fade-in + BGM on
// the first call after the build — all consumed invisibly under the cover, so EOF
// hardcuts to an already-built, muted title.
//
// FIX (no manual frame-driving — the engine keeps driving its own loop):
//   .init (0x007A45D0): while the boot leg is live, DEFER (return without building).
//       The scene loop leaves current_scene_type==0 and proceeds to .update.
//   .update (0x007A4418): while deferred + video not done, run the REAL update — it
//       renders the EMPTY title container (nothing) + Present, so the video overlay
//       shows. The instant the boot leg ends (boot_leg_done), call the real .init
//       ONCE to build the title, then the real update — its first call fires the
//       fade-in + BGM with audio on. Subsequent title entries (boot_leg_done already
//       set, or non-boot trigger) are byte-identical passthrough.
// No-arg scene handlers -> __cdecl/stdcall are identical (no stack args), so plain C
// detours are ABI-clean. Live-confirmed: .init=0x007A45D0, .update=0x007A4418 (its
// sibling .update matches SinowberillScene_Update_007a4418 in the decompile).
#define BOOT_TITLE_INIT_VA    0x007A45D0u   // scene_handlers[1].init
#define BOOT_TITLE_UPDATE_VA  0x007A4418u   // scene_handlers[1].update
#define BOOT_PAD_AGG_VA       0x007BFBA0u   // front-end pad aggregation (input pump + action masks)

static LPVOID g_tramp_title_init   = NULL;
static LPVOID g_tramp_title_update = NULL;
static LPVOID g_tramp_pad_agg      = NULL;
static int    g_boot_title_hook_installed = 0;
static volatile LONG g_boot_title_deferred = 0;

static void __cdecl boot_title_init_detour(void)
{
    // Defer the build while the boot leg is live (boot video not yet done).
    if ((g_video.trigger == VID_TRIGGER_BOOT || g_video.trigger == VID_TRIGGER_BOTH)
        && !g_video.boot_leg_done) {
        InterlockedExchange(&g_boot_title_deferred, 1);
        return;   // DEFER — do not build the title yet (current_scene_type stays 0)
    }
    if (g_tramp_title_init) ((void (__cdecl *)(void))g_tramp_title_init)();
}

static void __cdecl boot_title_update_detour(void)
{
    if (InterlockedCompareExchange(&g_boot_title_deferred, 0, 0)) {
        if (g_video.boot_leg_done) {
            // Boot video finished -> build the title NOW so the real update below is
            // its FIRST update and fires the fade-in + BGM with audio on.
            if (g_tramp_title_init) ((void (__cdecl *)(void))g_tramp_title_init)();
            InterlockedExchange(&g_boot_title_deferred, 0);
        }
        // else: video still playing -> the real update below renders the empty title
        //       container + Present so the overlay shows; nothing else is built.
    }
    if (g_tramp_title_update) ((void (__cdecl *)(void))g_tramp_title_update)();
}

// INPUT-LEAK FIX (the "skip-Enter counts for multiples" bug): the title scene's
// .update (0x007A4418) KEEPS RUNNING under the boot cover — we only defer its .init.
// That update pumps the shared front-end input object via 0x007BFBA0 -> 0x0078EEDC
// and builds the CONFIRM/CANCEL action masks (0xAAE934/938) the title reads to advance
// the scene. So a single skip-Enter held under the cover advances the title once PER
// FRAME the update runs -> "counts for multiples". While a boot cover is up (boot leg
// only: cc_session==0), DROP the aggregation entirely so the title sees NO input and
// cannot advance. The mod's own skip read is GetAsyncKeyState (separate from the
// 0x00A9CAA0 queue), so skip detection is unaffected; once the cover tears down — which
// the skip-drain delays until the held key releases — the title reads input normally.
// 0x007BFBA0 is int __cdecl(void): `sub esp,0xC` ... bare `ret`, no stack args
// (disasm-verified), so a plain C detour is ABI-clean. During char-create the scene-3
// owned-frame omits this call, so the hook only ever fires on the boot/front-end path.
static int __cdecl detour_pad_agg(void)
{
    if (InterlockedCompareExchange(&g_video.playing, 0, 0) &&
        !InterlockedCompareExchange(&g_video.cc_session, 0, 0)) {
        return 0;   // boot cover up -> swallow the front-end input this frame
    }
    if (g_tramp_pad_agg) return ((int (__cdecl *)(void))g_tramp_pad_agg)();
    return 0;
}

static int boot_title_hook_install(void)
{
    if (g_boot_title_hook_installed) return 1;
    MH_STATUS rc = MH_Initialize();
    if (rc != MH_OK && rc != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("[video] boot-title MH_Initialize failed rc=%d", (int)rc);
        return 0;
    }
    if (MH_CreateHook((LPVOID)(uintptr_t)BOOT_TITLE_INIT_VA,
                      (LPVOID)boot_title_init_detour, &g_tramp_title_init) != MH_OK) {
        log_line("[video] boot-title .init hook create failed"); return 0;
    }
    if (MH_CreateHook((LPVOID)(uintptr_t)BOOT_TITLE_UPDATE_VA,
                      (LPVOID)boot_title_update_detour, &g_tramp_title_update) != MH_OK) {
        log_line("[video] boot-title .update hook create failed"); return 0;
    }
    // INPUT-LEAK FIX: hook the front-end pad aggregation too, so detour_pad_agg can
    // swallow title input while the boot cover is up. Non-fatal — a failure leaves the
    // cover working, the title just keeps leaking the skip-Enter (old behavior).
    if (MH_CreateHook((LPVOID)(uintptr_t)BOOT_PAD_AGG_VA,
                      (LPVOID)detour_pad_agg, &g_tramp_pad_agg) != MH_OK) {
        log_line("[video] boot pad-agg hook create failed -> title input may leak");
        g_tramp_pad_agg = NULL;
    }
    if (MH_EnableHook((LPVOID)(uintptr_t)BOOT_TITLE_INIT_VA) != MH_OK ||
        MH_EnableHook((LPVOID)(uintptr_t)BOOT_TITLE_UPDATE_VA) != MH_OK) {
        log_line("[video] boot-title MH_EnableHook failed"); return 0;
    }
    if (g_tramp_pad_agg &&
        MH_EnableHook((LPVOID)(uintptr_t)BOOT_PAD_AGG_VA) != MH_OK) {
        log_line("[video] boot pad-agg MH_EnableHook failed -> title input may leak");
        g_tramp_pad_agg = NULL;
    }
    g_boot_title_hook_installed = 1;
    log_line("[video] boot title-defer hooks installed (.init=0x%08x .update=0x%08x)",
             BOOT_TITLE_INIT_VA, BOOT_TITLE_UPDATE_VA);
    return 1;
}

static void boot_title_hook_uninstall(void)
{
    if (!g_boot_title_hook_installed) return;
    MH_DisableHook((LPVOID)(uintptr_t)BOOT_TITLE_INIT_VA);
    MH_DisableHook((LPVOID)(uintptr_t)BOOT_TITLE_UPDATE_VA);
    MH_RemoveHook((LPVOID)(uintptr_t)BOOT_TITLE_INIT_VA);
    MH_RemoveHook((LPVOID)(uintptr_t)BOOT_TITLE_UPDATE_VA);
    if (g_tramp_pad_agg) {
        MH_DisableHook((LPVOID)(uintptr_t)BOOT_PAD_AGG_VA);
        MH_RemoveHook((LPVOID)(uintptr_t)BOOT_PAD_AGG_VA);
    }
    g_boot_title_hook_installed = 0;
    g_tramp_title_init = NULL;
    g_tramp_title_update = NULL;
    g_tramp_pad_agg = NULL;
}

// ---- OWNERSHIP replica: MinHook the scene-3 update 0x007C1588 ----
//
// While our cc_session latch is set we run a hand-written replica of the scene-3
// per-frame UPDATE (GameLoop_MainFrameUpdate_007c1588, the scripted-intro body)
// that DRAWS NOTHING but keeps every non-visual side effect, so our video overlay
// is the only thing on screen, no menu is actuated behind the cover, and a
// deliberate skip / ESC still works exactly as stock.
//
// Disasm-derived (harness disasm on the io client, base 0x00400000 — every VA below
// confirmed). The replica must KEEP the input reads: skipping the pad aggregation
// (0x007BFBA0) or the skip read (0x007BFEDC) means the engine skip can never fire.
//
//   0x7C1588  call 0x007BECD4    [DEV]   KEEP  shared input-DEVICE snapshot.
//   0x7C158D  call 0x007BFBA0    [INPUT] KEEP  pad/keyboard AGGREGATION. Runs the
//                                              front-end input pump (this=0xA9CAA0;
//                                              0x0078EEDC/0x0078F3B8/0x0078F3C0) and
//                                              ProcessKeyboardInput_007bfcf8, which
//                                              produces the per-action edge flags
//                                              ([0xAAE934/938] + the START/skip flag
//                                              the skip read below consumes). THIS
//                                              WAS THE EAT-REGRESSION: the prior
//                                              replica SKIPPED this, so the skip flag
//                                              was never produced; combined with
//                                              skipping 0x007BFEDC, the engine skip
//                                              could never fire. GetAsyncKeyState is
//                                              non-consuming; this engine read is the
//                                              one that drives a deliberate skip.
//   0x7C1592  mov ecx,0xACA2E4 ; call 0x0061CDB0   SKIP  — 0x0061CDB0 is a bare `ret`
//                                              stub (_SinowberillPureVirtualPlaceholder);
//                                              skipping it is a no-op either way.
//   0x7C159C  mov ecx,0xACA2E4 ; call 0x0081745C   SKIP  [DRAW] container-list render
//                                              (walks 0xACA2A8/0xACA2A4 child list).
//   0x7C15A6  mov ecx,0xACA2E4 ; call 0x00817604   SKIP  [DRAW] container-list render
//                                              (walks 0xACA2A0/0xACA29C child list).
//   0x7C15B0  mov ecx,0xACA2E4 ; call 0x00818F80   SKIP  [DRAW] container flush/render
//                                              (tests [ebx+0x1C]&0x100 -> render+clear).
//   0x7C15BA  call 0x005BA748    SKIP  [DRAW] intro starfield/crawl STATE MACHINE
//                                              (switch on [0xA74B74]; all visual).
//   0x7C15BF  call 0x00814168    [AUDIO] KEEP  3D-audio LISTENER position update
//                                              (reads list [0xA9C4F4]; writes the
//                                              listener delta into [0xAB59C0..C8]).
//                                              Non-visual; keep so the audio engine
//                                              stays coherent across the cover.
//   0x7C15C4..0x7C161B  the SUBSTATE STATE MACHINE — replicated verbatim below,
//                       INCLUDING the skip read. KEY: every branch falls through to
//                       the sub0 tail (`jmp 0x7C15D0`), which runs the skip read and
//                       the epilogue. Stock flow:
//                         movsx eax,[0xAAE988]; cmp 1; je sub1
//                         sub0 (0x7C15D0):
//                           al = 0x007BFEDC()           ; [INPUT] KEEP — skip/START read
//                           if (al) 0x007A6174()        ; KEEP — engine-driven skip
//                                                         ([0xAAB388]=0,[0xAAB394]=3)
//                           if (0x006DC358()) request(0); ; 0x006DC358 is a ret-0 stub
//                                                         (xor eax,eax;ret) -> this
//                                                         request(0) is DEAD; omitted.
//                           0x0080030C()                ; [EPILOGUE] KEEP (MANDATORY)
//                           ret
//                         sub1 (0x7C15F8):
//                           movsx eax,[0xAAE980]
//                           if (==0) { request(1); jmp sub0 }   ; stock parity
//                           else if (==1) { request(5); jmp sub0 } ; engine 3->5 EXIT
//                           else jmp sub0
//
//   0x7C15F2  call 0x0080030C    [EPILOGUE] KEEP — shared frame epilogue: frame
//                                              bookkeeping + the scene-request PUMP
//                                              (0x007A6268, the mechanism that lands
//                                              us in scene 5). NOT intro geometry.
//
// DURATION: under full ownership the intro ADX never plays, so the engine's natural
// end ([0xAAE988]=1) never fires on its own. WE own duration: at video EOF/SKIP,
// cc_arm_engine_exit writes byte[0xAAE980]=1 FIRST (discriminator), THEN
// byte[0xAAE988]=1 (gate). Write order is critical: if 988 were written first, an
// interleaved replica frame could observe 988=1,980=0 and issue request(1) (login)
// instead of request(5) (class-select). Reversed order closes that race. The next
// OWNED frame takes the sub1 branch, sees [0xAAE980]==1, and issues request(5).
//
// NOTE on the engine's OWN end: the stock scene-3 also sets [0xAAE988]=1 from the
// intro CONTAINER constructor path (0x007C16FF, when [0xA3B0A4]==1) — i.e. the
// engine itself can end the intro. Because we keep the epilogue + the substate
// machine verbatim, if the engine ever sets 988 on its own the replica issues the
// same request(5) and exits cleanly; our explicit arm just guarantees an end since
// the ADX clock is muted.
//
// PASSTHROUGH: when cc_session==0 the stub is a byte-identical `jmp [tramp]` to the
// stock update — so a non-charcreate build, the EXISTING-char path, and scene-5 are
// all unaffected. Hook is install-eager + trigger-gated; removed on detach.
//
//   0x007C1588: e8 47 d7 ff ff  call 0x7becd4   (rel32 -> MinHook relocates;
//                                                trampoline resumes at 0x007C158D)
// The slot fns take NO args + use no `this` for our purposes (they read globals);
// the kept draws are skipped so the `mov ecx,0xACA2E4` ECX setup is irrelevant.
#define CC_SCENE3_UPDATE_VA   0x007C1588u
#define CC_S3_DEV_SNAPSHOT    0x007BECD4u   // [DEV]   KEEP — shared input-device snapshot (input-inert)
#define CC_S3_PAD_AGG         0x007BFBA0u   // [INPUT] DROPPED — pumps shared input obj 0x00A9CAA0 -> the leak
#define CC_S3_AUDIO_LISTENER  0x00814168u   // [AUDIO] KEEP — 3D-audio listener position
#define CC_S3_SKIP_READ       0x007BFEDCu   // [INPUT] DROPPED — redundant w/ the mod's GetAsyncKeyState skip
#define CC_S3_SKIP_APPLY      0x007A6174u   // [INPUT] DROPPED — engine skip apply, no longer driven
#define CC_S3_EPILOGUE        0x0080030Cu   // [EPILOGUE] KEEP — shared frame epilogue (scene pump)
#define CC_REQUEST_SETTER_CALL 0x007A60DCu  // engine request(target) — the SM tail uses it

static LPVOID g_tramp_7c1588 = NULL;
static int    g_scene3_hook_installed = 0;

// The owned-frame replica, called by stub_7c1588 ONLY when cc_session is set.
// Plain C — the kept engine calls are arg-less leaf calls and request(target) is
// __cdecl(int). Mirrors the stock control flow with the 3 draws + the intro state
// machine + the ret-0 stubs omitted, but the input reads / audio / epilogue KEPT.
typedef void (__cdecl *cc_void_fn_t)(void);
typedef int  (__cdecl *cc_int_fn_t)(void);
typedef void (__cdecl *cc_request_fn_t)(int target);

static void cc_scene3_owned_frame(void)
{
    cc_void_fn_t    dev_snapshot  = (cc_void_fn_t)(uintptr_t)CC_S3_DEV_SNAPSHOT;
    cc_void_fn_t    audio_listen  = (cc_void_fn_t)(uintptr_t)CC_S3_AUDIO_LISTENER;
    cc_void_fn_t    epilogue      = (cc_void_fn_t)(uintptr_t)CC_S3_EPILOGUE;
    cc_request_fn_t request       = (cc_request_fn_t)(uintptr_t)CC_REQUEST_SETTER_CALL;

    // INPUT-LEAK FIX: DRAIN the front-end keyboard queue every owned
    // frame. The skip-Enter is BUFFERED in this queue (0x00A9CAA0) and survives the
    // cover -> class-select reads it right after the 3->5 handoff and auto-picks
    // Hunter. Waiting for key-release didn't help because the EVENT is buffered, not
    // the held key (our own RE note). Zeroing the processed-key COUNT at +3 (the same
    // field agent/kbd_inject increments to INJECT) makes the engine's
    // ProcessKeyboardInput_0078eedc early-return (num_keys_processed < 1 ->
    // num_new_keypresses=false), so the next scene sees no buffered keypress. The
    // mod's own skip read is GetAsyncKeyState (separate), so skip detection is
    // unaffected. Drains continuously while the cover owns scene-3, so the queue is
    // empty at the handoff.
    *(volatile unsigned char *)(uintptr_t)0x00A9CAA3u = 0;  // 0x00A9CAA0+3 = num_keys_processed

    // INPUT-LEAK FIX (2026-06-24, disasm-verified on the io client):
    //   The prior replica KEPT the input AGGREGATION 0x007BFBA0 (and the engine
    //   skip read 0x007BFEDC / skip apply 0x007A6174). That was the leak: 0x007BFBA0
    //   pumps the SHARED front-end input object 0x00A9CAA0 (via 0x0078EEDC) and
    //   builds the logical-action masks. Those masks (and the advanced 0x00A9CAA0
    //   queue) are read by OTHER live consumers (cursor/menu/camera) — so while the
    //   video covered the screen, real pad/keyboard input still drove the engine
    //   underneath. The action-flag globals 0x00AAE934/938 have NO readers; the
    //   coupling is purely that 0x007BFBA0 advances the shared input queue.
    //   => DROP 0x007BFBA0 entirely. With the aggregation gone, no engine input is
    //      produced this frame, so nothing behind the cover can be actuated.
    //
    //   The engine skip path (0x007BFEDC read -> 0x007A6174 apply) is now also
    //   DROPPED: it is redundant. The mod reads a deliberate skip itself via
    //   GetAsyncKeyState (vid_skip_pressed, render thread, non-consuming) and on a
    //   fresh Enter/Esc edge tears the cover down + arms the engine 3->5 exit
    //   (cc_arm_engine_exit writes 0xAAE980=1 then 0xAAE988=1). That arm routes the
    //   substate machine below straight through request(5) WITHOUT touching the
    //   input object — DISASM-VERIFIED that the 3->5 handoff (substate -> request(5)
    //   -> commit pump 0x007A6268 inside the epilogue) has NO dependency on the
    //   input aggregation having run. So dropping all three input calls is safe AND
    //   is what stops the leak. 0x007BFEDC additionally calls into the input object,
    //   so keeping it would partially re-leak — another reason to drop it.
    //
    //   KEPT: 0x007BECD4 (device snapshot) — it sets the skip-key/byte[0xAAE93C]
    //   flags but does NOT pump the shared 0x00A9CAA0 queue, and nothing consumes
    //   those flags now (the skip read is gone), so it is input-inert; we keep it
    //   only so the device-state bookkeeping stays coherent across the cover.
    //   0x00814168 (3D-audio listener) and 0x0080030C (epilogue/scene pump) are
    //   mandatory non-visual side effects, kept verbatim.
    dev_snapshot();   // KEEP 0x007BECD4 (device snapshot; input-inert without the agg/read)
    // DROP 0x007BFBA0 (input aggregation) — see INPUT-LEAK FIX above.
    // SKIP the 3 DRAW calls 0x0081745C / 0x00817604 / 0x00818F80 (+ the 0x0061CDB0
    // ret stub at 0x7C1592 and the 0x005BA748 intro state machine) -> intro draws nothing.
    audio_listen();   // KEEP 0x00814168 (3D-audio listener position)

    // AUDIO-LEAK FIX (2026-06-24): silence the streamed CUBE_OPENING.adx intro BGM.
    // It is started in the scene-3 ENTRY ctor 0x007C1714 (NOT hooked) AFTER request(3),
    // and is NOT gated by SOUNDCTRL — so the one-shot mute at arm time can't catch it.
    // Stop the BGM pool on EVERY owned frame: idempotent (resets already-stopped
    // players), engine-thread, only runs while the cover is up. This is the lever
    // that actually keeps the intro audio off under the video.
    // (No per-frame BGM kill: the intro ADX is now prevented at its SOURCE — the
    //  play_bgm 0x00814AFC detour returns early while cc_session owns scene-3 — so
    //  there is nothing to silence and NO mute is needed. 2026-06-28.)

    // --- substate state machine (the 3->5 handoff; input-INDEPENDENT) ---
    // We arm 0xAAE988=1 + 0xAAE980=1 at video EOF/skip (cc_arm_engine_exit); on the
    // next owned frame this fires request(5) -> the engine's natural 3->5 exit. It does
    // NOT run the input pump, so the skip Enter never cascades into class-select.
    if (*CC_EXIT_FLAG_988_R == 1) {               // sub1: ADX-done / WE armed it
        signed char f980 = (signed char)*CC_EXIT_FLAG_980_R;
        if (f980 == 0) {
            request(1);                           // stock parity: ==0 -> request(1)
        } else if (f980 == 1) {
            request(5);                           // ==1 -> the engine's own 3->5 EXIT
        }
    }

    // sub0 tail: the engine skip read 0x007BFEDC + skip apply 0x007A6174 are DROPPED
    // (input-leak fix above). The skip is driven by the mod's own GetAsyncKeyState
    // edge + EOF arm; the ret-0 timer 0x006DC358 + its dead request(0) were never
    // taken and remain omitted.
    epilogue();       // KEEP 0x0080030C (shared frame epilogue / scene pump) — MANDATORY
}

// naked stub: pass through to the stock update when cc_session==0 (byte-identical);
// otherwise run the replica (preserving caller regs). The pump dispatches via
// `call eax`/`call esi`; we use pushad/pushfd around the C replica so no caller reg
// is clobbered, then `ret` (the replica reproduces the whole update body).
static __declspec(naked) void stub_7c1588(void)
{
    __asm {
        mov   eax, dword ptr [g_cc_session_p]   // eax = &cc_session
        mov   eax, dword ptr [eax]              // eax = cc_session
        test  eax, eax
        jne   owned
        jmp   dword ptr [g_tramp_7c1588]        // cc_session==0 -> stock update (ECX intact)
    owned:
        pushad
        pushfd
        call  cc_scene3_owned_frame
        popfd
        popad
        ret                                     // replica IS the whole update body
    }
}

static int cc_scene3_hook_install(void)
{
    if (g_scene3_hook_installed) return 1;
    MH_STATUS rc = MH_Initialize();
    if (rc != MH_OK && rc != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("[video] scene3-own MH_Initialize failed rc=%d", (int)rc);
        return 0;
    }
    rc = MH_CreateHook((LPVOID)(uintptr_t)CC_SCENE3_UPDATE_VA,
                       (LPVOID)stub_7c1588, &g_tramp_7c1588);
    if (rc != MH_OK) {
        log_line("[video] scene3-own MH_CreateHook(0x%08x) failed rc=%d",
                 CC_SCENE3_UPDATE_VA, (int)rc);
        return 0;
    }
    rc = MH_EnableHook((LPVOID)(uintptr_t)CC_SCENE3_UPDATE_VA);
    if (rc != MH_OK) {
        log_line("[video] scene3-own MH_EnableHook failed rc=%d", (int)rc);
        return 0;
    }
    g_scene3_hook_installed = 1;
    log_line("[video] scene-3 ownership hook installed @0x%08x tramp=%p",
             CC_SCENE3_UPDATE_VA, g_tramp_7c1588);
    return 1;
}

static void cc_scene3_hook_uninstall(void)
{
    if (!g_scene3_hook_installed) return;
    MH_DisableHook((LPVOID)(uintptr_t)CC_SCENE3_UPDATE_VA);
    MH_RemoveHook((LPVOID)(uintptr_t)CC_SCENE3_UPDATE_VA);
    g_scene3_hook_installed = 0;
    g_tramp_7c1588 = NULL;
}

// =====================================================================
// ==== DIAGNOSTIC SCENE/LATCH READERS (log-only, no hooks) ============
// =====================================================================
//
// adiag_scene()/adiag_cc() are read-only SEH-guarded helpers used by the
// audio-mute + engine-exit log lines. They install NO hooks and patch NO
// engine memory — they only READ the scene-id global (for log context) and
// our own cover-session latch.
//
// scene-id global 0x00AAB384 is READ ONLY for log context — it is NEVER a
// discriminator in the cover path (the request `target` arg + the cc_session
// latch are the only discriminators).
#define ADIAG_SCENE_VA      0x00AAB384u   // current-scene id global (int) — log context only

// Read the engine scene-id behind an SEH firewall (the global is always
// committed once the engine is up, but a diag tap must never fault).
static int adiag_scene(void)
{
    int s = -1;
    __try { s = *(volatile int *)(uintptr_t)ADIAG_SCENE_VA; }
    __except (EXCEPTION_EXECUTE_HANDLER) { s = -2; }
    return s;
}

static volatile LONG adiag_cc(void)   // snapshot of OUR cover-session latch
{
    return InterlockedCompareExchange(&g_video.cc_session, 0, 0);
}

// =====================================================================
// ==== end diagnostic taps ============================================
// =====================================================================

// ---- reader thread: blocking ReadFile of full frames into the ring ----

static DWORD WINAPI vid_reader_thread(LPVOID arg)
{
    (void)arg;
    const size_t frame_bytes = g_video.ring_bytes;
    LONG produced = 0;
    while (InterlockedCompareExchange(&g_video.reader_run, 1, 1) == 1) {
        LONG slot = produced % VID_RING_SLOTS;
        unsigned char *buf = g_video.ring[slot];
        if (!buf) break;
        size_t got = 0;
        int broke = 0;
        // Read exactly one frame (the pipe may hand it to us in chunks).
        while (got < frame_bytes) {
            DWORD n = 0;
            BOOL ok = ReadFile(g_video.hPipeRead, buf + got,
                               (DWORD)(frame_bytes - got), &n, NULL);
            if (!ok || n == 0) { broke = 1; break; }
            got += n;
            if (InterlockedCompareExchange(&g_video.reader_run, 1, 1) != 1) {
                broke = 1; break;
            }
        }
        if (broke) {
            InterlockedExchange(&g_video.eof, 1);
            break;
        }
        // Publish the freshly-completed frame.
        InterlockedExchange(&g_video.newest_slot, slot);
        InterlockedExchange(&g_video.newest_index, produced);
        InterlockedExchange(&g_video.ring_filled, produced + 1);
        produced++;
    }
    return 0;
}

// ---- ffprobe-free fps: read from ffmpeg stderr is overkill for Stage 1.
// We pass an explicit fps to ffmpeg's output instead (so the cadence is
// known a priori) and pace by that. Default 30 if unknown. ----

// ---- spawn ffmpeg ----
//
// Returns 1 on success (child running, pipe + reader thread live), 0 on
// any failure (all partial state cleaned up, never leaves a zombie).
static int vid_spawn_ffmpeg(int bb_w, int bb_h)
{
    // Job object: KILL_ON_JOB_CLOSE guarantees ffmpeg dies with us.
    g_video.hJob = CreateJobObjectA(NULL, NULL);
    if (g_video.hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        memset(&jeli, 0, sizeof(jeli));
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_video.hJob,
                                JobObjectExtendedLimitInformation,
                                &jeli, sizeof(jeli));
    }

    // Anonymous pipe for ffmpeg stdout -> our read end. The WRITE end is
    // inheritable (child stdout); the READ end is NOT inherited.
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hWrite = NULL, hRead = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        log_line("[video] CreatePipe failed err=%lu", GetLastError());
        return 0;
    }
    // Make our read end non-inheritable so the child can't keep it open
    // (which would prevent a clean EOF when the child exits).
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    // stderr -> a log file (truncated each launch), so ffmpeg diagnostics
    // are visible without polluting our frame pipe. If that file can't be
    // created (e.g. patches\video\ dir missing), fall back to NUL — NEVER to
    // the stdout pipe, or ffmpeg's error text would corrupt the rawvideo
    // frame stream.
    char errlog[MAX_PATH];
    vid_resolve_path("patches\\video\\ffmpeg_err.log", errlog, sizeof(errlog));
    HANDLE hErr = CreateFileA(errlog, GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hErr == INVALID_HANDLE_VALUE) {
        hErr = CreateFileA("NUL", GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    // hErr may still be INVALID_HANDLE_VALUE in a pathological case; then we
    // point the child's stderr at our own stderr (inherited), still never the
    // frame pipe.

    // Build command line. -re (BEFORE -i, an INPUT option) makes ffmpeg read
    // the source at its native frame rate, so decoded frames arrive at ~realtime
    // (~30fps) instead of as-fast-as-possible (~4.5x ahead). With the reader
    // pacing throttled at the source, "present the newest frame" is correctly
    // timed for Stage 1 — no ring scheduler rework needed. -loglevel error keeps
    // stderr terse; -an drops audio (we play a sibling .wav separately); -s forces
    // decode-to-backbuffer so the texture is exactly backbuffer-sized (full-screen,
    // no scale at blit time). -r forces a known output cadence we pace by.
    // Quote both the ffmpeg path and the input path.
    char cmd[1024];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
        "\"%s\" -re -loglevel error -i \"%s\" -f rawvideo -pix_fmt bgra "
        "-an -s %dx%d -r 30 -",
        g_video.ffmpeg, g_video.path, bb_w, bb_h);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hWrite;
    // Never route stderr into the stdout frame pipe — use the log/NUL handle,
    // or the inherited parent stderr as a last resort.
    si.hStdError  = (hErr != INVALID_HANDLE_VALUE) ? hErr
                                                   : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    BOOL ok = CreateProcessA(
        NULL, cmd, NULL, NULL,
        TRUE,                                   // inherit handles (the pipe)
        CREATE_NO_WINDOW | CREATE_SUSPENDED,    // suspended: assign to job first
        NULL, NULL, &si, &pi);

    // We are done with the child's copies of the write-end handles.
    if (hWrite) CloseHandle(hWrite);
    if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);

    if (!ok) {
        log_line("[video] CreateProcess failed err=%lu cmd=%s",
                 GetLastError(), cmd);
        CloseHandle(hRead);
        if (g_video.hJob) { CloseHandle(g_video.hJob); g_video.hJob = NULL; }
        return 0;
    }

    // Assign to the job BEFORE resuming so the kill-on-close guarantee holds
    // for the whole child lifetime.
    if (g_video.hJob) AssignProcessToJobObject(g_video.hJob, pi.hProcess);
    ResumeThread(pi.hThread);

    g_video.hProc     = pi.hProcess;
    g_video.hThread   = pi.hThread;
    g_video.hPipeRead = hRead;
    g_video.fps       = 30.0;   // matches the forced -r 30 above

    // Allocate the ring (frame_bytes each).
    g_video.frame_w    = bb_w;
    g_video.frame_h    = bb_h;
    g_video.ring_bytes = (size_t)bb_w * (size_t)bb_h * 4u;
    for (int i = 0; i < VID_RING_SLOTS; ++i) {
        g_video.ring[i] = (unsigned char *)VirtualAlloc(
            NULL, g_video.ring_bytes, MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE);
        if (!g_video.ring[i]) {
            log_line("[video] ring alloc failed (%zu bytes)", g_video.ring_bytes);
            // Unwind: kill child + free what we got.
            for (int j = 0; j < i; ++j) {
                VirtualFree(g_video.ring[j], 0, MEM_RELEASE);
                g_video.ring[j] = NULL;
            }
            TerminateProcess(g_video.hProc, 0);
            CloseHandle(g_video.hThread);  g_video.hThread = NULL;
            CloseHandle(g_video.hProc);    g_video.hProc = NULL;
            CloseHandle(g_video.hPipeRead);g_video.hPipeRead = NULL;
            if (g_video.hJob) { CloseHandle(g_video.hJob); g_video.hJob = NULL; }
            return 0;
        }
    }

    g_video.newest_slot   = -1;
    g_video.newest_index  = -1;
    g_video.ring_filled   = 0;
    g_video.eof           = 0;
    g_video.reader_run    = 1;
    g_video.hReader = CreateThread(NULL, 0, vid_reader_thread, NULL, 0, NULL);
    if (!g_video.hReader) {
        log_line("[video] reader thread create failed err=%lu", GetLastError());
        InterlockedExchange(&g_video.reader_run, 0);
        TerminateProcess(g_video.hProc, 0);
        for (int i = 0; i < VID_RING_SLOTS; ++i) {
            if (g_video.ring[i]) { VirtualFree(g_video.ring[i], 0, MEM_RELEASE); g_video.ring[i] = NULL; }
        }
        CloseHandle(g_video.hThread);   g_video.hThread = NULL;
        CloseHandle(g_video.hProc);     g_video.hProc = NULL;
        CloseHandle(g_video.hPipeRead); g_video.hPipeRead = NULL;
        if (g_video.hJob) { CloseHandle(g_video.hJob); g_video.hJob = NULL; }
        return 0;
    }

    log_line("[video] ffmpeg spawned: %dx%d fps=%.2f src='%s'",
             bb_w, bb_h, g_video.fps, g_video.path);
    return 1;
}

// ============================================================================
// In-process Media Foundation decode (VID_DECODER_MF, default) — mirrors the
// VALIDATED mf_spike.c exactly. Same ring contract / publish protocol as the
// ffmpeg pipe reader above; the entire consumer side is untouched.
//
// COM/MF LIFETIME (documented per task):
//   * MFStartup() runs on the ENGINE thread inside vid_mf_open (so an open
//     failure surfaces synchronously, exactly like a failed CreateProcess —
//     vid_start's 0-return -> vid_disable_perm contract is preserved).
//   * The IMFSourceReader is created (synchronous mode) on the engine thread
//     and read (ReadSample) from the dedicated reader thread. A synchronous
//     Source Reader is documented free-threaded for ReadSample; the reader
//     thread additionally CoInitializeEx(MTA)/CoUninitialize for its own
//     apartment hygiene (balanced on that thread).
//   * MFShutdown() runs in vid_mf_close_handles on the TEARDOWN thread, which
//     is the SAME engine thread that called MFStartup. The reader is JOINED
//     before vid_mf_close_handles releases mf_reader + MFShutdown, so the
//     reader never touches a freed reader. mf_started gates exactly-one
//     MFShutdown.
// ============================================================================
#ifndef VID_NO_MF

static void vid_mf_close_handles(void);  // fwd
static int  vid_audio_mf_open(IMFSourceReader *rdr);   // fwd
static void vid_audio_mf_stop(void);     // fwd

// MF_MT_FRAME_SIZE / FRAME_RATE are packed UINT64 (hi32|lo32). The mfapi.h
// inline helpers (MFSetAttributeSize/...) do NOT link in C mode (proven in the
// spike) -> pack/unpack directly via SetUINT64/GetUINT64. SAME pattern the spike
// validated; do NOT call the MF*Attribute* inline helpers.
static void mt_set_pair(IMFMediaType *mt, const GUID *key, UINT32 a, UINT32 b)
{
    IMFMediaType_SetUINT64(mt, key, ((UINT64)a << 32) | (UINT64)b);
}
static HRESULT mt_get_pair(IMFMediaType *mt, const GUID *key, UINT32 *a, UINT32 *b)
{
    UINT64 v = 0;
    HRESULT hr = IMFMediaType_GetUINT64(mt, key, &v);
    if (SUCCEEDED(hr)) { *a = (UINT32)(v >> 32); *b = (UINT32)(v & 0xFFFFFFFFu); }
    return hr;
}

// ---- Embedded-video resources: the compressed clips are RCDATA in this .asi, so
//      NO loose .mp4 is read at runtime (no temp file, no fallback). ----
// Resolve `path`'s basename (sans dir + extension, upper-cased) to its RCDATA
// resource in THIS module; returns a pointer to the in-image bytes (no copy) and
// the size, or NULL if there is no matching embedded clip.
static const void *vid_embedded_bytes(const char *path, DWORD *out_size)
{
    char name[64];
    const char *base = path, *p;
    for (p = path; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    size_t i = 0;
    for (; base[i] && base[i] != '.' && i + 1 < sizeof(name); ++i) {
        char c = base[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        name[i] = c;
    }
    name[i] = 0;
    if (!name[0]) return NULL;

    HMODULE self = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(void *)&vid_embedded_bytes, &self) || !self)
        return NULL;
    HRSRC res = FindResourceA(self, name, RT_RCDATA);
    if (!res) return NULL;
    HGLOBAL hg = LoadResource(self, res);
    if (!hg) return NULL;
    const void *bytes = LockResource(hg);
    DWORD sz = SizeofResource(self, res);
    if (!bytes || !sz) return NULL;
    if (out_size) *out_size = sz;
    return bytes;
}

static int vid_embedded_exists(const char *path)
{
    DWORD sz = 0;
    return vid_embedded_bytes(path, &sz) != NULL;
}

// Open + negotiate on the engine thread. POD/pointer-only body inside the SEH
// firewall (no C++ unwinding objects -> no C2712, per the file's existing
// pattern). Sets mf_reader/mf_stride/fps/frame_w/frame_h. Returns 1 on success
// (reader ready to read), 0 on any failure (vid_mf_open's wrapper unwinds).
static int vid_mf_open_inner(int bb_w, int bb_h)
{
    g_video.mf_reader  = NULL;
    g_video.mf_stride  = 0;
    g_video.mf_started = 0;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        log_line("[video] mf: MFStartup failed hr=0x%08x", hr);
        return 0;
    }
    g_video.mf_started = 1;

    // Play from the EMBEDDED clip bytes: this .asi carries the compressed mp4s as
    // RCDATA, so there is NO loose .mp4 read at runtime and no temp file.
    DWORD vsz = 0;
    const void *vbytes = vid_embedded_bytes(g_video.path, &vsz);
    if (!vbytes || !vsz) {
        log_line("[video] mf: no embedded clip for '%s'", g_video.path);
        return 0;
    }

    // ENABLE_VIDEO_PROCESSING lets the reader insert a converter MFT so it can
    // satisfy the RGB32 request (we do NOT request a size, so it only converts
    // format; the frame stays at the video's native resolution).
    IMFAttributes *attrs = NULL;
    if (FAILED(MFCreateAttributes(&attrs, 1)) || !attrs) {
        log_line("[video] mf: MFCreateAttributes failed");
        return 0;
    }
    IMFAttributes_SetUINT32(attrs, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    // Wrap the embedded bytes in an HGLOBAL-backed IStream the MF byte stream owns
    // for the playback (one transient copy, freed when the reader is released).
    IStream *istream = NULL;
    IMFByteStream *mfbs = NULL;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, vsz);
    void *hgp = hg ? GlobalLock(hg) : NULL;
    if (hgp) { memcpy(hgp, vbytes, vsz); GlobalUnlock(hg); }
    if (!hgp || FAILED(CreateStreamOnHGlobal(hg, TRUE, &istream)) || !istream) {
        if (hg) GlobalFree(hg);
        IMFAttributes_Release(attrs);
        log_line("[video] mf: in-memory stream alloc failed (size=%lu)", vsz);
        return 0;
    }
    hr = MFCreateMFByteStreamOnStream(istream, &mfbs);
    istream->lpVtbl->Release(istream);                 // mfbs holds its own ref
    if (FAILED(hr) || !mfbs) {
        IMFAttributes_Release(attrs);
        log_line("[video] mf: MFCreateMFByteStreamOnStream failed hr=0x%08x", hr);
        return 0;
    }

    hr = MFCreateSourceReaderFromByteStream(mfbs, attrs, &g_video.mf_reader);
    mfbs->lpVtbl->Release(mfbs);                        // reader holds its own ref
    IMFAttributes_Release(attrs);
    if (FAILED(hr) || !g_video.mf_reader) {
        log_line("[video] mf: CreateSourceReaderFromByteStream failed hr=0x%08x", hr);
        return 0;
    }
    IMFSourceReader *rdr = g_video.mf_reader;

    IMFSourceReader_SetStreamSelection(rdr, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(rdr, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    // Demand RGB32 (== D3DFMT_A8R8G8B8 byte order == our BGRA ring) but DO NOT
    // force a FRAME_SIZE: let the reader negotiate the video's NATIVE decode size
    // (the auto-inserted Color Converter DSP converts format but does NOT resize,
    // so forcing bb dims => MF_E_INVALIDMEDIATYPE). We read the negotiated dims
    // back below and the GPU stretches the native texture to fill the backbuffer
    // at present time (fullscreen textured quad).
    IMFMediaType *mt = NULL;
    if (FAILED(MFCreateMediaType(&mt)) || !mt) {
        log_line("[video] mf: MFCreateMediaType failed");
        return 0;
    }
    IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE,    &MFVideoFormat_RGB32);
    hr = IMFSourceReader_SetCurrentMediaType(
            rdr, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, mt);
    IMFMediaType_Release(mt);
    if (FAILED(hr)) {
        log_line("[video] mf: SetCurrentMediaType(RGB32) failed hr=0x%08x "
                 "-> fallback", hr);
        return 0;
    }

    // Read back the negotiated type for true W/H, stride sign, and frame rate.
    IMFMediaType *cur = NULL;
    if (FAILED(IMFSourceReader_GetCurrentMediaType(
            rdr, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) || !cur) {
        log_line("[video] mf: GetCurrentMediaType failed");
        return 0;
    }
    UINT32 negW = (UINT32)bb_w, negH = (UINT32)bb_h;
    mt_get_pair(cur, &MF_MT_FRAME_SIZE, &negW, &negH);
    LONG stride = 0;
    if (FAILED(IMFMediaType_GetUINT32(cur, &MF_MT_DEFAULT_STRIDE, (UINT32 *)&stride))
        || stride == 0)
        stride = (LONG)negW * 4;   // absent -> top-down default
    g_video.mf_stride = stride;
    // MF_MT_FRAME_RATE came back ABSENT on the RGB32 output in the spike (fps=0)
    // -> fall back to 30.0 (the ffmpeg path also hardcodes 30.0). Acceptable.
    UINT32 fr_num = 0, fr_den = 0; double fps = 30.0;
    if (SUCCEEDED(mt_get_pair(cur, &MF_MT_FRAME_RATE, &fr_num, &fr_den))
        && fr_den > 0 && fr_num > 0)
        fps = (double)fr_num / (double)fr_den;
    if (fps <= 0.0 || fps > 1000.0) fps = 30.0;
    g_video.fps = fps;
    IMFMediaType_Release(cur);

    // Decode-at-native: honor the negotiated dims (the video's true frame size).
    // The ring + D3D texture are sized to these negW/negH and the present-time
    // fullscreen quad (vid_compute_rect) aspect-fits/stretches them to the bb.
    g_video.frame_w = (int)negW;
    g_video.frame_h = (int)negH;

    log_line("[video] mf open ok: %dx%d RGB32 stride=%ld (%s) fps=%.3f '%s'",
             g_video.frame_w, g_video.frame_h, g_video.mf_stride,
             (g_video.mf_stride < 0 ? "bottom-up" : "top-down"),
             g_video.fps, g_video.path);

    // Best-effort: enable + negotiate the audio stream on the SAME reader and
    // stand up XAudio2. Failure => silent video (NEVER fails the session). Runs
    // inside vid_mf_open's SEH firewall.
    vid_audio_mf_open(rdr);

    return 1;
}

// Enable + negotiate the source's FIRST_AUDIO_STREAM as 16-bit PCM on the SAME
// reader, then stand up XAudio2 (dynamically loaded). Best-effort: returns 0 to
// mean "no audio, play video silently" and NEVER fails vid_mf_open. SEH is
// handled by the vid_mf_open_inner SEH firewall that wraps the whole open.
static int vid_audio_mf_open(IMFSourceReader *rdr)
{
    g_video.audio_stream_idx = -1;
    g_video.audio_mf_ok = 0;
    g_video.audio_started = 0;
    g_video.audio_q_head = g_video.audio_q_tail = 0;

    // 1) Select the audio stream on the same reader and force PCM s16.
    if (FAILED(IMFSourceReader_SetStreamSelection(
            rdr, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) {
        log_line("[video] mf: no audio stream (silent)");
        return 0;
    }
    IMFMediaType *amt = NULL;
    if (FAILED(MFCreateMediaType(&amt)) || !amt) return 0;
    IMFMediaType_SetGUID(amt, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(amt, &MF_MT_SUBTYPE,    &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(amt, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    HRESULT hr = IMFSourceReader_SetCurrentMediaType(
            rdr, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, amt);
    IMFMediaType_Release(amt);
    if (FAILED(hr)) {
        log_line("[video] mf: audio SetCurrentMediaType(PCM16) failed hr=0x%08x (silent)", hr);
        IMFSourceReader_SetStreamSelection(
            rdr, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, FALSE);
        return 0;
    }

    // 2) Read back the negotiated PCM format (rate/channels chosen by the MFT).
    IMFMediaType *acur = NULL;
    if (FAILED(IMFSourceReader_GetCurrentMediaType(
            rdr, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &acur)) || !acur)
        return 0;
    UINT32 chans = 2, rate = 48000, bits = 16, block = 4;
    IMFMediaType_GetUINT32(acur, &MF_MT_AUDIO_NUM_CHANNELS,      &chans);
    IMFMediaType_GetUINT32(acur, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    IMFMediaType_GetUINT32(acur, &MF_MT_AUDIO_BITS_PER_SAMPLE,    &bits);
    if (FAILED(IMFMediaType_GetUINT32(acur, &MF_MT_AUDIO_BLOCK_ALIGNMENT, &block))
        || block == 0)
        block = (chans * bits) / 8;
    IMFMediaType_Release(acur);
    if (chans == 0 || rate == 0 || bits != 16) {
        log_line("[video] mf: audio fmt unusable ch=%u rate=%u bits=%u (silent)",
                 chans, rate, bits);
        return 0;
    }
    g_video.audio_chans = chans; g_video.audio_rate = rate;
    g_video.audio_bits  = bits;  g_video.audio_block = block;

    // 3) Dynamically load XAudio2 (NO static lib -> byte-identical when disabled,
    //    graceful degrade on a machine without xaudio2_9.dll). Resolve the legacy
    //    2-extra-arg XAudio2Create export directly, bypassing the SDK inline.
    g_video.xa_dll = LoadLibraryW(L"xaudio2_9.dll");
    if (!g_video.xa_dll) {
        log_line("[video] audio: xaudio2_9.dll not present (silent)");
        return 0;
    }
    XAudio2Create_t pXAudio2Create =
        (XAudio2Create_t)GetProcAddress(g_video.xa_dll, "XAudio2Create");
    if (!pXAudio2Create) {
        log_line("[video] audio: XAudio2Create export missing (silent)");
        FreeLibrary(g_video.xa_dll); g_video.xa_dll = NULL;
        return 0;
    }

    // 4) Engine + mastering voice + source voice. CoInitializeEx already done by
    //    the reader thread; vid_mf_open_inner runs on the engine thread which is
    //    also COM-init'd by the host. XAudio2Create is apartment-agnostic.
    hr = pXAudio2Create(&g_video.xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_video.xa) {
        log_line("[video] audio: XAudio2Create failed hr=0x%08x (silent)", hr);
        g_video.xa = NULL;
        FreeLibrary(g_video.xa_dll); g_video.xa_dll = NULL;
        return 0;
    }
    hr = IXAudio2_CreateMasteringVoice(g_video.xa, &g_video.xa_master,
            XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, NULL, NULL,
            AudioCategory_GameEffects);
    if (FAILED(hr) || !g_video.xa_master) {
        log_line("[video] audio: CreateMasteringVoice failed hr=0x%08x (silent)", hr);
        vid_audio_mf_stop();
        return 0;
    }

    WAVEFORMATEX wfx; memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = (WORD)chans;
    wfx.nSamplesPerSec  = rate;
    wfx.wBitsPerSample  = (WORD)bits;
    wfx.nBlockAlign     = (WORD)block;
    wfx.nAvgBytesPerSec = rate * block;
    wfx.cbSize          = 0;
    hr = IXAudio2_CreateSourceVoice(g_video.xa, &g_video.xa_voice, &wfx,
            0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr) || !g_video.xa_voice) {
        log_line("[video] audio: CreateSourceVoice failed hr=0x%08x (silent)", hr);
        vid_audio_mf_stop();
        return 0;
    }
    // Start the voice now; it idles (BuffersQueued=0) until we submit PCM.
    IXAudio2SourceVoice_Start(g_video.xa_voice, 0, XAUDIO2_COMMIT_NOW);

    g_video.audio_mf_ok = 1;
    log_line("[video] audio open: PCM s16 ch=%u rate=%u block=%u (XAudio2)",
             chans, rate, block);
    return 1;
}

// Is reader stream `idx` the audio stream? Cached after first resolve. Reads the
// stream's current media-type MAJOR_TYPE once.
static int vid_mf_stream_is_audio(IMFSourceReader *rdr, DWORD idx)
{
    if (g_video.audio_stream_idx >= 0) return idx == (DWORD)g_video.audio_stream_idx;
    IMFMediaType *mt = NULL;
    int is_audio = 0;
    if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(rdr, idx, &mt)) && mt) {
        GUID mj;
        if (SUCCEEDED(IMFMediaType_GetGUID(mt, &MF_MT_MAJOR_TYPE, &mj)))
            is_audio = (memcmp(&mj, &MFMediaType_Audio, sizeof(GUID)) == 0);
        IMFMediaType_Release(mt);
    }
    if (is_audio) g_video.audio_stream_idx = (int)idx;
    return is_audio;
}

// Reap finished PCM blocks: free everything the voice has already consumed.
// BuffersQueued tells us how many are still owned by XAudio2; the rest at the
// FIFO tail are done. Called each reader loop. SEH-guarded.
static void vid_audio_mf_reap(void)
{
    if (!g_video.audio_mf_ok || !g_video.xa_voice) return;
    XAUDIO2_VOICE_STATE st; st.BuffersQueued = 0;
    __try {
        IXAudio2SourceVoice_GetState(g_video.xa_voice, &st,
                                     XAUDIO2_VOICE_NOSAMPLESPLAYED);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    // queued count = head-tail (mod). Free until in-flight == BuffersQueued.
    LONG inflight = (g_video.audio_q_head - g_video.audio_q_tail + VID_AUDIO_QMAX)
                    % VID_AUDIO_QMAX;
    while (inflight > (LONG)st.BuffersQueued) {
        BYTE *done = g_video.audio_q[g_video.audio_q_tail];
        if (done) { free(done); g_video.audio_q[g_video.audio_q_tail] = NULL; }
        g_video.audio_q_tail = (g_video.audio_q_tail + 1) % VID_AUDIO_QMAX;
        inflight--;
    }
}

// Copy one audio IMFSample's PCM into a heap block and submit it to the voice.
// Seeds qpc_start on the FIRST submit so audio is the master clock and video is
// paced to it. SEH-guarded around the XAudio2 call. Drops the sample (no submit)
// if the FIFO is full (the voice is behind) -> bounded memory, never blocks.
static void vid_audio_mf_submit(IMFSample *sample)
{
    if (!g_video.audio_mf_ok || !g_video.xa_voice) return;
    LONG next = (g_video.audio_q_head + 1) % VID_AUDIO_QMAX;
    if (next == g_video.audio_q_tail) { vid_audio_mf_reap();
        next = (g_video.audio_q_head + 1) % VID_AUDIO_QMAX;
        if (next == g_video.audio_q_tail) return;   // still full -> drop
    }
    IMFMediaBuffer *buf = NULL;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(sample, &buf)) || !buf) return;
    BYTE *p = NULL; DWORD maxlen = 0, curlen = 0;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, &maxlen, &curlen)) && curlen > 0) {
        BYTE *pcm = (BYTE *)malloc(curlen);
        if (pcm) {
            memcpy(pcm, p, curlen);
            XAUDIO2_BUFFER xb; memset(&xb, 0, sizeof(xb));
            xb.AudioBytes = curlen;
            xb.pAudioData = pcm;
            xb.pContext   = pcm;   // (diagnostic; we reap via BuffersQueued)
            int submitted = 0;
            __try {
                if (SUCCEEDED(IXAudio2SourceVoice_SubmitSourceBuffer(
                        g_video.xa_voice, &xb, NULL)))
                    submitted = 1;
            } __except (EXCEPTION_EXECUTE_HANDLER) { submitted = 0; }
            if (submitted) {
                g_video.audio_q[g_video.audio_q_head] = pcm;
                g_video.audio_q_head = next;
                // A/V SYNC SEED: the FIRST audio buffer establishes t0 so video
                // frames are paced to the audio clock (audio-clock master).
                if (InterlockedCompareExchange(&g_video.audio_started, 1, 0) == 0) {
                    LARGE_INTEGER now; QueryPerformanceCounter(&now);
                    g_video.qpc_start = now.QuadPart;
                    log_line("[video] audio clock t0 seeded at first submit");
                }
            } else {
                free(pcm);
            }
        }
        IMFMediaBuffer_Unlock(buf);
    }
    IMFMediaBuffer_Release(buf);
}

// Stop + release the entire XAudio2 audio render chain. Idempotent. SEH-guarded
// (a wrapper/driver quirk in DestroyVoice/StopEngine must never poison teardown).
// Frees any in-flight PCM blocks still owned by the FIFO. Mirrors the file's
// teardown discipline (release children before parents; null after free).
static void vid_audio_mf_stop(void)
{
    __try {
        if (g_video.xa_voice) {
            IXAudio2SourceVoice_Stop(g_video.xa_voice, 0, XAUDIO2_COMMIT_NOW);
            IXAudio2SourceVoice_FlushSourceBuffers(g_video.xa_voice);
            IXAudio2SourceVoice_DestroyVoice(g_video.xa_voice);
            g_video.xa_voice = NULL;
        }
        if (g_video.xa_master) {
            IXAudio2MasteringVoice_DestroyVoice(g_video.xa_master);
            g_video.xa_master = NULL;
        }
        if (g_video.xa) {
            IXAudio2_StopEngine(g_video.xa);
            IXAudio2_Release(g_video.xa);
            g_video.xa = NULL;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("[video] audio xa stop SEH");
    }
    // Free any PCM blocks the voice never finished (FlushSourceBuffers dropped them).
    for (LONG i = g_video.audio_q_tail; i != g_video.audio_q_head;
         i = (i + 1) % VID_AUDIO_QMAX) {
        if (g_video.audio_q[i]) { free(g_video.audio_q[i]); g_video.audio_q[i] = NULL; }
    }
    g_video.audio_q_head = g_video.audio_q_tail = 0;
    if (g_video.xa_dll) { FreeLibrary(g_video.xa_dll); g_video.xa_dll = NULL; }
    g_video.audio_mf_ok = 0;
    g_video.audio_started = 0;
    g_video.audio_stream_idx = -1;
}

// Reader thread: pull RGB32 samples and publish into the ring with the EXACT
// SAME protocol as vid_reader_thread (the pipe reader). MFCopyImage is sign-
// aware so the ring is always top-down packed BGRA regardless of mf_stride.
static DWORD WINAPI vid_mf_reader_thread(LPVOID arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);   // MTA on our own thread
    IMFSourceReader *rdr = g_video.mf_reader;
    LONG produced = 0;
    __try {
        int audio_eof = 0;
        while (InterlockedCompareExchange(&g_video.reader_run, 1, 1) == 1) {
            // CUSHION ACCOUNTING FIX (2026-06-28): reap consumed PCM EVERY iteration so
            // audio_q_tail advances as the voice plays and the adepth below stays TRUE.
            // The no-audio bug: once the cushion filled we read VIDEO (not audio) and so
            // never reaped -> tail never advanced -> adepth stuck at TARGET -> the reader
            // stopped topping up -> the voice underran to SILENCE after ~0.7s. Reaping
            // here every loop keeps adepth accurate so the cushion refills as it drains.
            if (g_video.audio_mf_ok) vid_audio_mf_reap();

            // STREAM PICK (audio-crackle fix): keep the audio FIFO topped to a cushion
            // so the XAudio2 voice never underruns at a slow video frame. The old
            // ANY_STREAM read produced audio at real-time density with NO cushion, so
            // the voice ran dry whenever a heavy MFCopyImage stalled the feed ->
            // repeated mini-underruns = crackle. Now: if the cushion is below target,
            // read the AUDIO stream specifically to top it up; otherwise read VIDEO.
            // target < QMAX => we never overfill, so we never drop a sample.
            DWORD pick = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
            if (g_video.audio_mf_ok && !audio_eof) {
                LONG adepth = (g_video.audio_q_head - g_video.audio_q_tail
                               + VID_AUDIO_QMAX) % VID_AUDIO_QMAX;
                if (adepth < VID_AUDIO_TARGET)
                    pick = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
            }

            // VIDEO PACING: the present shows the NEWEST frame, so newest must stay
            // ~= the audio playback time. If the last produced frame is already >50ms
            // ahead of the audio clock, nap (and let the cushion top up next loop)
            // instead of racing more frames into the ring and desyncing.
            if (pick == (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM &&
                g_video.audio_started && g_video.qpc_freq && g_video.fps > 0.0) {
                LARGE_INTEGER now; QueryPerformanceCounter(&now);
                double play_t  = (double)(now.QuadPart - g_video.qpc_start)
                                 / (double)g_video.qpc_freq;
                double frame_t = (double)produced / g_video.fps;
                if (frame_t - play_t > 0.05) { Sleep(5); continue; }
            }

            DWORD streamIdx = 0, flags = 0; LONGLONG ts = 0;
            IMFSample *sample = NULL;
            HRESULT hr = IMFSourceReader_ReadSample(rdr, pick, 0,
                            &streamIdx, &flags, &ts, &sample);
            if (FAILED(hr)) { InterlockedExchange(&g_video.eof, 1); break; }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (sample) IMFSample_Release(sample);
                if (pick == (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM) {
                    audio_eof = 1;          // audio track done -> keep draining video
                    continue;
                }
                InterlockedExchange(&g_video.eof, 1);   // video done -> end of clip
                break;
            }
            if (!sample) continue;          // stream gap / format change, not EOS

            // --- AUDIO stream -> XAudio2 submit ---
            if (pick == (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM) {
                if (g_video.audio_mf_ok) {
                    vid_audio_mf_submit(sample);
                    vid_audio_mf_reap();
                }
                IMFSample_Release(sample);
                continue;
            }

            // --- VIDEO stream -> ring ---
            IMFMediaBuffer *buf = NULL;
            if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buf)) && buf) {
                BYTE *p = NULL; DWORD maxlen = 0, curlen = 0;
                if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, &maxlen, &curlen))) {
                    LONG slot = produced % VID_RING_SLOTS;
                    unsigned char *dst = g_video.ring[slot];
                    if (dst) {
                        // Sign-aware: honors NEGATIVE mf_stride (bottom-up) ->
                        // always top-down packed BGRA (== the ffmpeg ring).
                        MFCopyImage(dst, g_video.frame_w * 4,
                                    p, g_video.mf_stride,
                                    (DWORD)(g_video.frame_w * 4),
                                    (DWORD)g_video.frame_h);
                        // SAME publish order/protocol as the pipe reader.
                        InterlockedExchange(&g_video.newest_slot,  slot);
                        InterlockedExchange(&g_video.newest_index, produced);
                        InterlockedExchange(&g_video.ring_filled,  produced + 1);
                        produced++;
                    }
                    IMFMediaBuffer_Unlock(buf);
                }
                IMFMediaBuffer_Release(buf);
            }
            IMFSample_Release(sample);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("[video] mf reader SEH");
        InterlockedExchange(&g_video.eof, 1);
    }
    // drain: reap any remaining consumed blocks (full free happens in stop)
    vid_audio_mf_reap();
    CoUninitialize();
    return 0;
}

// Release the reader (caller MUST have joined the reader thread first) and
// balance MFStartup. Idempotent.
static void vid_mf_close_handles(void)
{
    vid_audio_mf_stop();          // stop+free XAudio2 chain + in-flight PCM
    if (g_video.mf_reader) {
        IMFSourceReader_Release(g_video.mf_reader);
        g_video.mf_reader = NULL;
    }
    if (g_video.mf_started) {
        MFShutdown();
        g_video.mf_started = 0;
    }
    g_video.mf_stride = 0;
}

// MF producer. Same return contract as vid_spawn_ffmpeg: 1 with ring allocated +
// reader thread live + fps/frame_w/frame_h/ring_bytes set, OR 0 with ALL partial
// state freed. Open/negotiate is SEH-firewalled (vid_mf_open_inner) so a wrapper/
// codec quirk can never take down vid_start.
static int vid_mf_open(int bb_w, int bb_h)
{
    int opened = 0;
    __try {
        opened = vid_mf_open_inner(bb_w, bb_h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("[video] mf open SEH");
        opened = 0;
    }
    if (!opened) {
        // Unwind anything vid_mf_open_inner created (reader + MFStartup).
        vid_mf_close_handles();
        return 0;
    }

    // Allocate the ring (frame_bytes each) at the negotiated decode size.
    g_video.ring_bytes = (size_t)g_video.frame_w * (size_t)g_video.frame_h * 4u;
    for (int i = 0; i < VID_RING_SLOTS; ++i) {
        g_video.ring[i] = (unsigned char *)VirtualAlloc(
            NULL, g_video.ring_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_video.ring[i]) {
            log_line("[video] mf: ring alloc failed (%zu bytes)", g_video.ring_bytes);
            for (int j = 0; j < i; ++j) {
                VirtualFree(g_video.ring[j], 0, MEM_RELEASE);
                g_video.ring[j] = NULL;
            }
            vid_mf_close_handles();
            return 0;
        }
    }

    g_video.newest_slot   = -1;
    g_video.newest_index  = -1;
    g_video.ring_filled   = 0;
    g_video.eof           = 0;
    g_video.reader_run    = 1;
    g_video.hReader = CreateThread(NULL, 0, vid_mf_reader_thread, NULL, 0, NULL);
    if (!g_video.hReader) {
        log_line("[video] mf: reader thread create failed err=%lu", GetLastError());
        InterlockedExchange(&g_video.reader_run, 0);
        for (int i = 0; i < VID_RING_SLOTS; ++i) {
            if (g_video.ring[i]) {
                VirtualFree(g_video.ring[i], 0, MEM_RELEASE);
                g_video.ring[i] = NULL;
            }
        }
        vid_mf_close_handles();
        return 0;
    }

    log_line("[video] mf open complete: %dx%d fps=%.2f src='%s'",
             g_video.frame_w, g_video.frame_h, g_video.fps, g_video.path);
    return 1;
}

#else  // VID_NO_MF: ffmpeg-only build. Stub so VideoDecoder=mf falls back/disables.

static int vid_mf_open(int bb_w, int bb_h)
{
    (void)bb_w; (void)bb_h;
    log_line("[video] built without MF (VID_NO_MF); use VideoDecoder=ffmpeg");
    return 0;
}
static void vid_mf_close_handles(void) {}

#endif // VID_NO_MF

// ---- audio (optional, sibling .wav via mci) ----

static void vid_audio_start(void)
{
    if (!g_video.audio || !g_video.wav[0]) return;
    if (!vid_file_exists(g_video.wav)) {
        log_line("[video] audio: sibling wav not found '%s' (silent)", g_video.wav);
        return;
    }
    char open_cmd[MAX_PATH + 64];
    _snprintf_s(open_cmd, sizeof(open_cmd), _TRUNCATE,
                "open \"%s\" type waveaudio alias p3v", g_video.wav);
    if (mciSendStringA(open_cmd, NULL, 0, NULL) == 0) {
        g_video.audio_open = 1;
        mciSendStringA("play p3v", NULL, 0, NULL);
        log_line("[video] audio playing '%s'", g_video.wav);
    } else {
        log_line("[video] audio mci open failed '%s'", g_video.wav);
    }
}

static void vid_audio_stop(void)
{
    if (g_video.audio_open) {
        mciSendStringA("close p3v", NULL, 0, NULL);
        g_video.audio_open = 0;
    }
}

// ---- teardown (idempotent across every exit path) ----

static void vid_teardown(const char *why)
{
    if (!InterlockedCompareExchange(&g_video.playing, 0, 1)) {
        // Wasn't playing; nothing to tear down (but still let audio close).
        vid_audio_stop();
        return;
    }
    log_line("[video] teardown (%s)", why ? why : "?");
    log_line("[adiag] teardown (%s) scene=%d cc_session=%ld",
             why ? why : "?", adiag_scene(), (long)adiag_cc());

    // Stop the reader.
    InterlockedExchange(&g_video.reader_run, 0);

#ifndef VID_NO_MF
    if (g_video.decoder == VID_DECODER_MF) {
        // No child to kill. ReadSample on a local file returns promptly; the
        // reader exits once reader_run flips. JOIN the reader, THEN free the
        // source (the reader owns mf_reader -> close ONLY after the join).
        if (g_video.hReader) {
            WaitForSingleObject(g_video.hReader, 1000);  // 1s cap
            CloseHandle(g_video.hReader);
            g_video.hReader = NULL;
        }
        vid_mf_close_handles();   // releases mf_reader + balances MFShutdown
    } else
#endif
    {
        // ---- ffmpeg-child teardown (unchanged) ----
        // Break the reader out of any blocking ReadFile by killing the child
        // (which closes the pipe write end -> ReadFile returns 0).
        if (g_video.hProc) {
            TerminateProcess(g_video.hProc, 0);   // idempotent; Job backstops
        }
        if (g_video.hReader) {
            WaitForSingleObject(g_video.hReader, 1000);  // 1s cap
            CloseHandle(g_video.hReader);
            g_video.hReader = NULL;
        }
        if (g_video.hPipeRead) { CloseHandle(g_video.hPipeRead); g_video.hPipeRead = NULL; }
        if (g_video.hThread)   { CloseHandle(g_video.hThread);   g_video.hThread = NULL; }
        if (g_video.hProc)     { CloseHandle(g_video.hProc);     g_video.hProc = NULL; }
        // Closing the job handle enforces KILL_ON_JOB_CLOSE on anything we missed.
        if (g_video.hJob)      { CloseHandle(g_video.hJob);      g_video.hJob = NULL; }
    }

    for (int i = 0; i < VID_RING_SLOTS; ++i) {
        if (g_video.ring[i]) { VirtualFree(g_video.ring[i], 0, MEM_RELEASE); g_video.ring[i] = NULL; }
    }

    if (g_video.texture) {
        void **tvt = *(void ***)g_video.texture;
        IUnknown_Release_t fnRelease = (IUnknown_Release_t)tvt[2];
        fnRelease(g_video.texture);
        g_video.texture = NULL;
        g_video.tex_w = g_video.tex_h = 0;
        g_video.last_uploaded_index = -1;
    }

    // FIX 1: release the D3DSBT_ALL state block (a DWORD token in D3D8). The
    // device that created it is still alive at teardown (we tear down on skip/
    // watchdog/eof while the engine keeps rendering), so DeleteStateBlock is the
    // matched free. Re-created lazily on the next session. SEH-guarded; needs the
    // device, which teardown doesn't carry — so we delete via the cached token on
    // the device pointer captured at draw time. If we have no device handle here
    // we simply clear the flags so the next session re-creates a fresh block.
    if (g_video.sb_have && g_video.sb_dev) {
        __try {
            void **dvt = *(void ***)g_video.sb_dev;
            DeleteStateBlock_t fnDelete = (DeleteStateBlock_t)dvt[56];
            fnDelete((IDirect3DDevice8 *)g_video.sb_dev, g_video.sb_token);
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* swallow */ }
    }
    g_video.sb_have   = 0;
    g_video.sb_tried  = 0;
    g_video.sb_token  = 0;
    g_video.sb_dev    = NULL;
    g_video.save_method_logged = 0;
    g_vid_saved.valid = 0;
    // Defensive: in the normal flow capture+restore bracket one vid_draw_overlay
    // call (restore already released tex0), so this is NULL here. Release any
    // stray AddRef from a half-completed manual capture just in case.
    if (g_vid_saved.tex0) {
        __try {
            void **tvt = *(void ***)g_vid_saved.tex0;
            IUnknown_Release_t fnRelease = (IUnknown_Release_t)tvt[2];
            fnRelease(g_vid_saved.tex0);
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* swallow */ }
        g_vid_saved.tex0 = NULL;
    }

    g_video.last_copied_index = -1;

    vid_audio_stop();
    // Belt-and-suspenders: restore the engine's SOUNDCTRL flags (Option K) on EVERY
    // teardown path (EOF / skip / watchdog / cc-exit / present-SEH / detach). This
    // is idempotent — a no-op if not armed — and guarantees we never leave engine
    // audio muted even if a teardown happens without a cc_on_request real-exit
    // (e.g. present-SEH disable, or DLL detach). cc_on_request already disarms on
    // the normal request(5|0|2|0xB) exit; this is the safety net.
    cc_audio_disarm();

    g_video.meteor_fadein_ms = 0;   // any meteor fade-in ends with the session
    g_video.newest_slot  = -1;
    g_video.newest_index = -1;
    g_video.ring_filled  = 0;
}

// ---- start a playback session at backbuffer size ----

static int vid_start(IDirect3DDevice8 *dev, int bb_w, int bb_h)
{
    if (g_video.disabled_perm) return 0;
    if (InterlockedCompareExchange(&g_video.playing, 1, 0) != 0) return 1; // already playing
    if (bb_w <= 0 || bb_h <= 0) { InterlockedExchange(&g_video.playing, 0); return 0; }
    (void)dev;

    // FFMPEG mode only: resolve ffmpeg.exe (if not configured / not present,
    // fall back to a bare "ffmpeg" on PATH). MF mode NEVER needs ffmpeg.exe.
    if (g_video.decoder == VID_DECODER_FFMPEG) {
        if (!g_video.ffmpeg[0] || !vid_file_exists(g_video.ffmpeg)) {
            _snprintf_s(g_video.ffmpeg, sizeof(g_video.ffmpeg), _TRUNCATE, "ffmpeg");
        }
    }

    // Producer dispatch: in-process MF (default) or external ffmpeg pipe.
    // Both return 1 with the ring + reader thread live (identical contract),
    // 0 with all partial state freed.
    int opened = (g_video.decoder == VID_DECODER_FFMPEG)
                     ? vid_spawn_ffmpeg(bb_w, bb_h)
                     : vid_mf_open(bb_w, bb_h);

    // MF auto-fallback: if the in-process MF open failed (Windows N / no h264
    // MFT — hr=0xc00d36b4), retry via external ffmpeg.exe BEFORE giving up to
    // the stock intro. On success we flip g_video.decoder to FFMPEG so the
    // audio-start gate (below) and teardown branch both take the ffmpeg path.
    // (This also covers the VID_NO_MF build, where vid_mf_open is a stub that
    // returns 0.)
    if (!opened && g_video.decoder == VID_DECODER_MF) {
        log_line("[video] MF open failed -> auto-fallback to ffmpeg");
        if (!g_video.ffmpeg[0] || !vid_file_exists(g_video.ffmpeg)) {
            _snprintf_s(g_video.ffmpeg, sizeof(g_video.ffmpeg), _TRUNCATE, "ffmpeg");
        }
        opened = vid_spawn_ffmpeg(bb_w, bb_h);
        if (opened) g_video.decoder = VID_DECODER_FFMPEG;
    }

    if (!opened) {
        InterlockedExchange(&g_video.playing, 0);
        vid_disable_perm("decode open failed (mf+ffmpeg)");
        return 0;
    }

    QueryPerformanceFrequency((LARGE_INTEGER *)&g_video.qpc_freq);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    g_video.qpc_start = now.QuadPart;
    g_video.last_uploaded_index = -1;

    // Reset skip debounce: require both keys observed UP at least once and
    // the debounce delay before a skip is honored (Enter on slot 11 enters
    // char-create; the same press must not instantly skip in Stage 2 use).
    g_video.skip_armed     = 0;
    g_video.keys_seen_up   = 0;
    g_video.skip_prev_down = 1;  // assume confirm key still down at start
    g_video.skip_draining  = 0;  // no skip-key-drain in progress
    // Default: no meteor fade-in. The BOOT-leg caller sets meteor_fadein_ms right
    // after this returns; the char-create leg leaves it 0 (intro is never faded in).
    g_video.meteor_fadein_ms = 0;

    // mci sibling-.wav is the FFMPEG fallback only; MF carries embedded audio.
    if (g_video.decoder == VID_DECODER_FFMPEG) vid_audio_start();

    log_line("[video] start %dx%d (trigger=%s)", bb_w, bb_h,
             g_video.trigger == VID_TRIGGER_CHARCREATE ? "charcreate"
           : g_video.trigger == VID_TRIGGER_BOOT       ? "boot"
                                                       : "off");
    log_line("[adiag] vid_start scene=%d cc_session=%ld %dx%d",
             adiag_scene(), (long)adiag_cc(), bb_w, bb_h);
    return 1;
}

// ---- skip input (release-then-press debounce) ----

// TRUE iff one of OUR process's windows is the foreground window. The skip read is
// GetAsyncKeyState (system-wide / focus-independent), so without this gate an
// Enter/Esc pressed in ANOTHER app — or held from launch while the client sits in
// the background — would skip the intro. Honour the skip only when the game is
// actually focused.
static int vid_app_focused(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static int vid_skip_pressed(void)
{
    if (!g_video.skippable) return 0;
    // GetAsyncKeyState high bit = currently down.
    SHORT ret = GetAsyncKeyState(VK_RETURN);
    SHORT esc = GetAsyncKeyState(VK_ESCAPE);
    int down = ((ret & 0x8000) != 0) || ((esc & 0x8000) != 0);

    // GetAsyncKeyState is system-wide, so only honor the skip when the game is the
    // FOREGROUND app — otherwise a key pressed in another window skips the intro.
    // Keep tracking the key state while unfocused so a press after refocusing still
    // reads as a clean up->down edge (a key held across the focus change is not a
    // fresh skip).
    if (!vid_app_focused()) {
        g_video.skip_prev_down = down;
        return 0;
    }

    // Effective startup grace = max(hard floor, configured debounce). The clip
    // MUST be visible this long before ANY skip is honored. Without it the
    // create-confirm Enter (still down at the cc-gate rising edge) tore the
    // clip down before a single frame showed.
    //
    // BOOT LEG ONLY (2026-06-28): zero the grace. The grace exists for the
    // char-create dialog-bounce (the YES/NO confirm Enter is still down when the
    // char-create clip starts); the BOOT meteor has no such dialog, so the grace
    // just swallowed the user's natural eager Enter right when the meteor starts,
    // forcing a SECOND press. With grace=0 a single fresh focus-gated Enter/Esc
    // skips the meteor immediately. The release-then-press debounce below is KEPT
    // for both legs — that is what correctly stops a still-held cover-skip Enter
    // from also skipping the meteor (preserving the intended two-press behavior
    // when you DO skip the cover). cc_session==0 == the boot leg (the char-create
    // leg sets cc_session; see cc_on_request).
    int boot_leg = (InterlockedCompareExchange(&g_video.cc_session, 0, 0) == 0);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double elapsed_ms = 0.0;
    if (g_video.qpc_freq)
        elapsed_ms = (double)(now.QuadPart - g_video.qpc_start) * 1000.0
                     / (double)g_video.qpc_freq;
    double grace = 0.0;
    if (!boot_leg) {
        grace = (double)g_video.skip_debounce_ms;
        if (grace < (double)VID_SKIP_GRACE_MS) grace = (double)VID_SKIP_GRACE_MS;
    }

    if (elapsed_ms < grace) {
        // Inside the grace window: never skip. Track the key state so the
        // first post-grace press is detected as a clean up->down edge.
        g_video.skip_prev_down = down;
        return 0;
    }

    // Past grace: also require the confirm key to have been observed UP at
    // least once (covers a key held continuously past the grace window).
    if (!g_video.keys_seen_up) {
        if (!down) g_video.keys_seen_up = 1;
        g_video.skip_prev_down = down;
        return 0;
    }

    // Skip ONLY on a deliberate fresh press: an up->down edge.
    int edge = down && !g_video.skip_prev_down;
    g_video.skip_prev_down = down;
    if (edge) g_video.skip_armed = 1;  // (kept for diag/log compatibility)
    return edge ? 1 : 0;
}

// TRUE iff BOTH skip keys (Enter and Esc) are currently physically UP. Used by the
// BOOT-leg skip-drain: after a skip fires we keep the cover up until this reads TRUE
// so the revealed title never sees the still-held Enter as a fresh menu-open press.
static int vid_skip_keys_up(void)
{
    SHORT ret = GetAsyncKeyState(VK_RETURN);
    SHORT esc = GetAsyncKeyState(VK_ESCAPE);
    return ((ret & 0x8000) == 0) && ((esc & 0x8000) == 0);
}

// ---- texture upload of one frame slot ----

static int vid_ensure_texture(IDirect3DDevice8 *dev)
{
    if (g_video.texture && g_video.tex_w == g_video.frame_w &&
        g_video.tex_h == g_video.frame_h)
        return 1;
    if (g_video.texture) {
        void **tvt = *(void ***)g_video.texture;
        IUnknown_Release_t fnRelease = (IUnknown_Release_t)tvt[2];
        fnRelease(g_video.texture);
        g_video.texture = NULL;
    }
    void **vt = *(void ***)dev;
    CreateTexture_t fnCreateTexture = (CreateTexture_t)vt[20];
    // D3DPOOL_MANAGED + Usage 0: MANAGED LockRect is universally supported
    // across native / d3d8to9 / d3d8to11 / dgVoodoo2. The previous round
    // switched MANAGED->DYNAMIC+DEFAULT on a hunch; that combo gives a
    // LockRect whose Pitch is inconsistent with a (Pitch*frame_h) writable
    // buffer under the wrapper (locked region is a partial/staging buffer),
    // which AV'd in the row memcpy (phase=3). Revert to the bulletproof
    // MANAGED path; LockRect Flags are 0 (no D3DLOCK_DISCARD on MANAGED).
    g_vid_phase = 1;
    HRESULT hr = fnCreateTexture(dev, (UINT)g_video.frame_w,
                                 (UINT)g_video.frame_h, 1, 0,
                                 D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                 &g_video.texture);
    if (FAILED(hr) || !g_video.texture) {
        log_line("[video] CreateTexture FAILED hr=0x%08x", hr);
        g_video.texture = NULL;
        return 0;
    }
    g_video.tex_w = g_video.frame_w;
    g_video.tex_h = g_video.frame_h;
    g_video.last_uploaded_index = -1;
    log_line("[video] CreateTexture ok tex=%p %dx%d fmt=A8R8G8B8 pool=MANAGED",
             (void *)g_video.texture, g_video.tex_w, g_video.tex_h);
    return 1;
}

// SEH-firewalled pitch-aware row blit. Isolated in its own function (no C++
// objects -> no C2712) so a bad Pitch from a wrapper LockRect can never take
// down Present: we log the exact failing row and bail. Per-row size is clamped
// to min(row_bytes, pitch) so a short/negative pitch cannot overrun dst.
// Returns 1 on a clean copy, 0 if an AV was caught mid-blit.
static int vid_blit_rows(void *dst_bits, int pitch,
                         const unsigned char *src, int row_bytes, int height)
{
    size_t copy = (size_t)row_bytes;
    if ((size_t)pitch < copy) copy = (size_t)pitch;  // clamp; pitch>0 ensured by caller
    int y = 0;
    __try {
        for (; y < height; ++y) {
            unsigned char *dst = (unsigned char *)dst_bits + (size_t)y * (size_t)pitch;
            memcpy(dst, src + (size_t)y * (size_t)row_bytes, copy);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("[video] memcpy AV pitch=%ld y=%d", (long)pitch, y);
        return 0;
    }
    return 1;
}

// Copy ring slot `slot` (frame index `index`) into the texture. BGRA bytes
// == D3DFMT_A8R8G8B8: a straight pitch-aware row memcpy, no swizzle.
static int vid_upload_frame(IDirect3DDevice8 *dev, LONG slot, LONG index)
{
    if (index == g_video.last_uploaded_index) return 1;  // already current
    if (slot < 0 || slot >= VID_RING_SLOTS || !g_video.ring[slot]) return 0;
    if (!vid_ensure_texture(dev)) return 0;

    void **tvt = *(void ***)g_video.texture;
    Tex_LockRect_t   fnLockRect   = (Tex_LockRect_t)tvt[16];
    Tex_UnlockRect_t fnUnlockRect = (Tex_UnlockRect_t)tvt[17];
    D3DLOCKED_RECT_X lr; lr.pBits = NULL; lr.Pitch = 0;
    // MANAGED texture: LockRect Flags = 0 (D3DLOCK_DISCARD is only valid on
    // DYNAMIC textures; passing it on MANAGED is what the wrapper choked on).
    g_vid_phase = 2;
    HRESULT hr = fnLockRect(g_video.texture, 0, &lr, NULL, 0);
    if (FAILED(hr) || !lr.pBits) return 0;

    g_vid_phase = 3;
    const int row_bytes = g_video.frame_w * 4;
    // ONE-SHOT diagnostic, written to disk BEFORE any memcpy so lr.Pitch
    // survives even a crash. This is the value we need to SEE.
    {
        static long s_pitch_logged = 0;
        if (!s_pitch_logged) {
            s_pitch_logged = 1;
            log_line("[video] lock ok: pitch=%ld row_bytes=%d dims=%dx%d",
                     (long)lr.Pitch, row_bytes,
                     g_video.frame_w, g_video.frame_h);
        }
    }
    // A non-positive pitch means the locked region is unusable; never index
    // off it. Unlock and bail cleanly (no AV, no Present teardown).
    if (lr.Pitch <= 0) {
        fnUnlockRect(g_video.texture, 0);
        return 0;
    }
    const unsigned char *src = g_video.ring[slot];
    int ok = vid_blit_rows(lr.pBits, lr.Pitch, src, row_bytes, g_video.frame_h);
    fnUnlockRect(g_video.texture, 0);
    if (!ok) return 0;
    g_video.last_uploaded_index = index;
    return 1;
}

// ---- quad blit (mirror of boot_poster's bp_render_quad) ----

// Callers: the opaque-BLACK fullscreen underlay (textured=0, diffuse=0xFF000000),
// the aspect-fit textured video quad (textured=1, blend=0, diffuse=0xFFFFFFFF), and
// the boot-cover / meteor-fade quad (textured=1, blend=1, diffuse alpha < 0xFF).
//   `tex`   = the texture to bind for the textured path (g_video.texture for the
//             video, cover_tex for the cover; ignored when !textured).
//   `blend` = 1 -> ALPHABLENDENABLE on (SRCALPHA/INVSRCALPHA) and the texture's
//             ALPHAOP MODULATEs texture.a * diffuse.a, so the per-vertex diffuse
//             ALPHA fades the whole quad over the black underlay (the cover fade-
//             out + the meteor fade-in). 0 -> opaque (the original video path).
// The untextured underlay draws its colour from the per-vertex DIFFUSE.
static void vid_render_quad(IDirect3DDevice8 *dev,
                            float x1, float y1, float x2, float y2,
                            int textured, DWORD diffuse,
                            IDirect3DTexture8 *tex, int blend)
{
    void **vt = *(void ***)dev;
    SetTexture_t           fnSetTexture           = (SetTexture_t)vt[61];
    SetVertexShader_t      fnSetVertexShader      = (SetVertexShader_t)vt[76];
    SetRenderState_t       fnSetRenderState       = (SetRenderState_t)vt[50];
    SetTextureStageState_t fnSetTextureStageState = (SetTextureStageState_t)vt[63];
    DrawPrimitiveUP_t      fnDrawPrimitiveUP      = (DrawPrimitiveUP_t)vt[72];

    g_vid_phase = 5;
    fnSetRenderState(dev, D3DRS_ZENABLE,          0);
    fnSetRenderState(dev, D3DRS_ZWRITEENABLE,     0);
    fnSetRenderState(dev, D3DRS_LIGHTING,         0);
    fnSetRenderState(dev, D3DRS_FOGENABLE,        0);
    fnSetRenderState(dev, D3DRS_STENCILENABLE,    0);
    fnSetRenderState(dev, D3DRS_CULLMODE,         D3DCULL_NONE);
    fnSetRenderState(dev, D3DRS_CLIPPING,         0);
    fnSetRenderState(dev, D3DRS_COLORVERTEX,      1);
    fnSetRenderState(dev, D3DRS_ALPHATESTENABLE,  0);
    // Gouraud shade so the per-vertex DIFFUSE actually colours the quad
    // (dgVoodoo's fixed-function path can leave SHADEMODE in a state where a
    // flat default wins). TEXTUREFACTOR is forced opaque-white so a stale stage
    // state referencing D3DTA_TFACTOR can't tint us.
    fnSetRenderState(dev, D3DRS_SHADEMODE,        D3DSHADE_GOURAUD);
    fnSetRenderState(dev, D3DRS_TEXTUREFACTOR,    0xFFFFFFFFu);
    // For XYZRHW geometry the material/diffuse source is moot (pre-lit), but
    // make the intent explicit: diffuse comes from the vertex color (source 1).
    fnSetRenderState(dev, D3DRS_DIFFUSEMATERIALSOURCE, 1);
    // blend=1 (cover fade / meteor fade-in): alpha-blend over the black underlay so
    // a diffuse alpha < 0xFF dissolves the quad to black. blend=0: opaque (original
    // video path; BGRA alpha is 0xFF anyway).
    if (blend) {
        fnSetRenderState(dev, D3DRS_ALPHABLENDENABLE, 1);
        fnSetRenderState(dev, D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
        fnSetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        fnSetRenderState(dev, D3DRS_BLENDOP,   D3DBLENDOP_ADD);
    } else {
        fnSetRenderState(dev, D3DRS_ALPHABLENDENABLE, 0);
    }

    g_vid_phase = 6;
    if (textured) {
        // dgVoodoo's fixed-function sampler defaults are NOT guaranteed; set
        // filtering + clamp addressing + texcoord index 0 explicitly or the
        // stage can sample the default white texel for every pixel.
        fnSetTextureStageState(dev, 0, D3DTSS_MAGFILTER,    D3DTEXF_LINEAR);
        fnSetTextureStageState(dev, 0, D3DTSS_MINFILTER,    D3DTEXF_LINEAR);
        fnSetTextureStageState(dev, 0, D3DTSS_MIPFILTER,    D3DTEXF_NONE);
        fnSetTextureStageState(dev, 0, D3DTSS_ADDRESSU,     D3DTADDRESS_CLAMP);
        fnSetTextureStageState(dev, 0, D3DTSS_ADDRESSV,     D3DTADDRESS_CLAMP);
        fnSetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, 0);

        fnSetTextureStageState(dev, 0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
        fnSetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        fnSetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        if (blend) {
            // Fade path: final alpha = texture.a * diffuse.a so the ramping diffuse
            // ALPHA drives the dissolve (texture alpha is 0xFF for the cover PNG /
            // the meteor BGRA, so this is effectively the diffuse alpha).
            fnSetTextureStageState(dev, 0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
            fnSetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            fnSetTextureStageState(dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        } else {
            fnSetTextureStageState(dev, 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
            fnSetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        }
        fnSetTextureStageState(dev, 1, D3DTSS_COLOROP,   D3DTOP_DISABLE);
        fnSetTextureStageState(dev, 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);
        fnSetTexture(dev, 0, tex);
        {   // one-shot: confirm the bound texture == the CreateTexture'd ptr.
            static long s_bind_logged = 0;
            if (!s_bind_logged) {
                s_bind_logged = 1;
                log_line("[video] SetTexture stage0 tex=%p op=MODULATE "
                         "arg1=TEXTURE(0x2) arg2=DIFFUSE blend=%d",
                         (void *)tex, blend);
            }
        }
    } else {
        // Untextured solid quad (the opaque black letterbox underlay). Colour +
        // alpha come from the per-vertex DIFFUSE (0xFF000000 = opaque black).
        fnSetTextureStageState(dev, 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
        fnSetTextureStageState(dev, 0, D3DTSS_COLORARG1, (DWORD)D3DTA_DIFFUSE);
        fnSetTextureStageState(dev, 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
        fnSetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, (DWORD)D3DTA_DIFFUSE);
        fnSetTextureStageState(dev, 1, D3DTSS_COLOROP,   D3DTOP_DISABLE);
        fnSetTextureStageState(dev, 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);
        fnSetTexture(dev, 0, NULL);
    }
    g_vid_phase = 7;
    fnSetVertexShader(dev, VIDEO_FVF);

    // Half-pixel offset (-0.5) per vertex: D3D8 pixel-center alignment.
    VidVtx v[4] = {
        { x1 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, diffuse, 0.0f, 0.0f },
        { x2 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, diffuse, 1.0f, 0.0f },
        { x2 - 0.5f, y2 - 0.5f, 0.0f, 1.0f, diffuse, 1.0f, 1.0f },
        { x1 - 0.5f, y2 - 0.5f, 0.0f, 1.0f, diffuse, 0.0f, 1.0f },
    };
    g_vid_phase = 8;
    fnDrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, v, sizeof(VidVtx));

    if (textured) fnSetTexture(dev, 0, NULL);
}

// Aspect-fit a (fw x fh) frame inside the viewport (letterbox/pillarbox).
static void vid_compute_rect_dims(int vp_w, int vp_h, int fw, int fh,
                                  float *x1, float *y1, float *x2, float *y2)
{
    *x1 = *y1 = *x2 = *y2 = 0.0f;
    if (vp_w <= 0 || vp_h <= 0 || fw <= 0 || fh <= 0)
        return;
    float fa = (float)fw / (float)fh;        // frame aspect
    float sa = (float)vp_w / (float)vp_h;    // screen aspect
    float w, h;
    if (fa > sa) {                 // frame wider -> width-limited (pillarbox vertical)
        w = (float)vp_w;
        h = w / fa;
    } else {                       // frame taller/equal -> height-limited
        h = (float)vp_h;
        w = h * fa;
    }
    float cx = (float)vp_w * 0.5f;
    float cy = (float)vp_h * 0.5f;
    *x1 = cx - w * 0.5f; *y1 = cy - h * 0.5f;
    *x2 = cx + w * 0.5f; *y2 = cy + h * 0.5f;
}

// Query the REAL backbuffer dimensions from the device. The Present hook's
// vp_w/vp_h is the engine's last SetViewport (a boot-time sub-region), not the
// swap chain — so we ignore it and ask the device. SEH-firewalled: any wrapper
// quirk here must never poison Present; on failure we return 0 and the caller
// falls back to the passed viewport. One-shot logged so a live capture can
// confirm the backbuffer size vs. the (wrong) passed viewport.
static int vid_query_backbuffer(IDirect3DDevice8 *dev, int *out_w, int *out_h,
                                DWORD *out_fmt)
{
    int ok = 0;
    int w = 0, h = 0;
    DWORD fmt = 0;
    __try {
        void **vt = *(void ***)dev;
        GetBackBuffer_t fnGetBackBuffer = (GetBackBuffer_t)vt[16];
        IDirect3DSurface8 *surf = NULL;
        HRESULT hr = fnGetBackBuffer(dev, 0, D3DBACKBUFFER_TYPE_MONO, &surf);
        if (SUCCEEDED(hr) && surf) {
            void **svt = *(void ***)surf;
            Surf_GetDesc_t    fnGetDesc = (Surf_GetDesc_t)svt[8];
            IUnknown_Release_t fnRelease = (IUnknown_Release_t)svt[2];
            D3DSURFACE_DESC8_X desc; memset(&desc, 0, sizeof(desc));
            HRESULT dhr = fnGetDesc(surf, &desc);
            if (SUCCEEDED(dhr) && desc.Width > 0 && desc.Height > 0) {
                w = (int)desc.Width;
                h = (int)desc.Height;
                fmt = desc.Format;
                ok = 1;
            }
            fnRelease(surf);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (ok) { *out_w = w; *out_h = h; if (out_fmt) *out_fmt = fmt; }
    return ok;
}

// NOTE: the only blit path is the textured-quad (vid_draw_overlay) — there is no
// CopyRects SYSTEMMEM->backbuffer path (dgVoodoo2's D3D8->D3D11 wrapper does not
// pick up a CopyRects to the emulated backbuffer at Present time).

// ---- viewport force/restore around our Present-time draw ----
//
// The textured-quad blit DID reach the screen (real video frames showed), but
// rendered into a SUB-RECTANGLE (~80% width) with the title bleeding around it,
// even though vid_compute_rect produced (0,0,bbW,bbH) and the backbuffer query
// returned the full size. RHW vertices are clipped to the device's ACTIVE
// VIEWPORT, and at our Present-time draw that viewport is the engine's LAST
// SetViewport — a sub-region (either override_viewport is off, or the engine's
// last call before Present was a sub-render under the hook's 60% threshold). So
// we save the current viewport, force it to the full backbuffer for our draw,
// then restore it so the engine's next frame is undisturbed.
//
// SEH-firewalled (a wrapper quirk in Get/SetViewport must never poison Present).
// vid_save_viewport returns 1 and fills *saved on success; vid_set_viewport
// returns 1 on success. Slots: GetViewport=41, SetViewport=40 (see typedefs).
static int vid_save_viewport(IDirect3DDevice8 *dev, D3DVIEWPORT8_X *saved)
{
    int ok = 0;
    __try {
        void **vt = *(void ***)dev;
        GetViewport_t fnGetViewport = (GetViewport_t)vt[41];
        D3DVIEWPORT8_X vp; memset(&vp, 0, sizeof(vp));
        if (SUCCEEDED(fnGetViewport(dev, &vp))) {
            *saved = vp;
            ok = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return ok;
}

static int vid_set_viewport(IDirect3DDevice8 *dev, const D3DVIEWPORT8_X *vp)
{
    int ok = 0;
    __try {
        void **vt = *(void ***)dev;
        SetViewport_t fnSetViewport = (SetViewport_t)vt[40];
        if (SUCCEEDED(fnSetViewport(dev, vp))) ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return ok;
}

// ---- FIX 1: device-state save/restore around our overlay draw ----
//
// Our draw must leave the device EXACTLY as the engine left it. Two paths:
//   * state block (preferred): one CaptureStateBlock snapshots EVERYTHING our
//     draw could touch; one ApplyStateBlock restores it verbatim.
//   * manual (fallback, for wrappers whose CreateStateBlock fails): save+restore
//     exactly the render states / texture-stage states / texture / FVF that
//     vid_render_quad mutates. (The viewport is saved/restored separately, in
//     vid_draw_overlay, on both paths.)
// (VID_SAVED_* tables and g_vid_saved live up by g_video so vid_teardown can
//  reach them for cleanup.)

// One-shot log of which save path is in use.
static void vid_log_save_method(const char *m)
{
    if (g_video.save_method_logged) return;
    g_video.save_method_logged = 1;
    log_line("[video] state-save = %s", m);
}

// Lazily create the D3DSBT_ALL state block once a device is ready. Returns 1 if
// the state-block path is usable, 0 if we should use the manual fallback.
static int vid_ensure_stateblock(IDirect3DDevice8 *dev)
{
    if (g_video.sb_tried) return g_video.sb_have;
    g_video.sb_tried = 1;
    __try {
        void **vt = *(void ***)dev;
        CreateStateBlock_t fnCreate = (CreateStateBlock_t)vt[57];
        DWORD token = 0;
        HRESULT hr = fnCreate(dev, D3DSBT_ALL, &token);
        if (SUCCEEDED(hr) && token != 0) {
            g_video.sb_token = token;
            g_video.sb_dev   = dev;   // remembered so teardown can DeleteStateBlock
            g_video.sb_have  = 1;
        } else {
            log_line("[video] CreateStateBlock FAILED hr=0x%08x (manual fallback)", hr);
            g_video.sb_have = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("[video] CreateStateBlock SEH (manual fallback)");
        g_video.sb_have = 0;
    }
    return g_video.sb_have;
}

// Snapshot the engine's current device state. Returns 1 on success. On the
// state-block path this Captures the block; on the manual path it Gets every
// state vid_render_quad mutates. SEH-firewalled: a failure leaves *no* snapshot
// (valid=0) so vid_restore_state becomes a no-op rather than restoring garbage.
static int vid_capture_state(IDirect3DDevice8 *dev)
{
    g_vid_saved.valid = 0;
    g_vid_saved.tex0  = NULL;

    if (vid_ensure_stateblock(dev)) {
        int ok = 0;
        __try {
            void **vt = *(void ***)dev;
            CaptureStateBlock_t fnCapture = (CaptureStateBlock_t)vt[55];
            if (SUCCEEDED(fnCapture(dev, g_video.sb_token))) ok = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = 0;
        }
        if (ok) { vid_log_save_method("stateblock"); g_vid_saved.valid = 1; return 1; }
        // Capture failed mid-session: drop to manual for the rest of the run.
        g_video.sb_have = 0;
    }

    // Manual fallback: read back every state our draw touches.
    int ok = 0;
    __try {
        void **vt = *(void ***)dev;
        GetRenderState_t       fnGetRS  = (GetRenderState_t)vt[51];
        GetTextureStageState_t fnGetTSS = (GetTextureStageState_t)vt[62];
        GetTexture_t           fnGetTex = (GetTexture_t)vt[60];
        GetVertexShader_t      fnGetVS  = (GetVertexShader_t)vt[75];

        for (unsigned i = 0; i < VID_NUM_SAVED_RS; ++i) {
            g_vid_saved.rs[i] = 0;
            fnGetRS(dev, VID_SAVED_RS[i], &g_vid_saved.rs[i]);
        }
        for (unsigned i = 0; i < VID_NUM_SAVED_TSS0; ++i) {
            g_vid_saved.tss0[i] = 0;
            fnGetTSS(dev, 0, VID_SAVED_TSS0[i], &g_vid_saved.tss0[i]);
        }
        for (unsigned i = 0; i < VID_NUM_SAVED_TSS1; ++i) {
            g_vid_saved.tss1[i] = 0;
            fnGetTSS(dev, 1, VID_SAVED_TSS1[i], &g_vid_saved.tss1[i]);
        }
        g_vid_saved.tex0 = NULL;
        fnGetTex(dev, 0, &g_vid_saved.tex0);  // AddRef'd; released on restore
        g_vid_saved.vshader = 0;
        fnGetVS(dev, &g_vid_saved.vshader);
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    if (ok) { vid_log_save_method("manual"); g_vid_saved.valid = 1; }
    return ok;
}

// Restore the snapshot taken by vid_capture_state so the device is left EXACTLY
// as the engine had it. No-op if no valid snapshot exists. SEH-firewalled.
static void vid_restore_state(IDirect3DDevice8 *dev)
{
    if (!g_vid_saved.valid) return;
    g_vid_saved.valid = 0;

    if (g_video.sb_have) {
        __try {
            void **vt = *(void ***)dev;
            ApplyStateBlock_t fnApply = (ApplyStateBlock_t)vt[54];
            fnApply(dev, g_video.sb_token);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            /* swallow: a failed restore must never poison Present */
        }
        return;
    }

    // Manual restore: write every saved state back, rebind the saved texture and
    // FVF/vertex shader, then release the GetTexture AddRef.
    __try {
        void **vt = *(void ***)dev;
        SetRenderState_t       fnSetRS  = (SetRenderState_t)vt[50];
        SetTextureStageState_t fnSetTSS = (SetTextureStageState_t)vt[63];
        SetTexture_t           fnSetTex = (SetTexture_t)vt[61];
        SetVertexShader_t      fnSetVS  = (SetVertexShader_t)vt[76];

        for (unsigned i = 0; i < VID_NUM_SAVED_RS; ++i)
            fnSetRS(dev, VID_SAVED_RS[i], g_vid_saved.rs[i]);
        for (unsigned i = 0; i < VID_NUM_SAVED_TSS0; ++i)
            fnSetTSS(dev, 0, VID_SAVED_TSS0[i], g_vid_saved.tss0[i]);
        for (unsigned i = 0; i < VID_NUM_SAVED_TSS1; ++i)
            fnSetTSS(dev, 1, VID_SAVED_TSS1[i], g_vid_saved.tss1[i]);
        fnSetTex(dev, 0, g_vid_saved.tex0);   // rebind exactly what was bound
        fnSetVS(dev, g_vid_saved.vshader);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* swallow */
    }

    // Release the AddRef GetTexture took (the SetTexture rebind holds its own
    // ref). Done OUTSIDE the restore __try so even a restore fault frees it.
    if (g_vid_saved.tex0) {
        __try {
            void **tvt = *(void ***)g_vid_saved.tex0;
            IUnknown_Release_t fnRelease = (IUnknown_Release_t)tvt[2];
            fnRelease(g_vid_saved.tex0);
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* swallow */ }
        g_vid_saved.tex0 = NULL;
    }
}

// Draw a fullscreen overlay (a black underlay + an aspect-fit textured quad)
// inside our own BeginScene/EndScene pair (dgVoodoo2 throws on DrawPrimitiveUP
// outside a scene; Hook_Present runs AFTER the engine's EndScene). The active
// viewport is forced to the FULL backbuffer for the draw and restored after, so
// RHW vertices are not clipped to the engine's last (sub-region) SetViewport.
// EVERY draw goes through this so the engine state is captured + restored verbatim
// (the white-out bug was a draw that leaked render state — this is why the video
// never corrupts the screen, and the boot cover reuses it for the same guarantee).
//
//   `tex`     = the texture to draw (g_video.texture for the meteor, cover_tex for
//               the boot cover still).
//   `diffuse` = per-vertex diffuse color/alpha. 0xFFFFFFFF = opaque untinted;
//               a ramping alpha drives the cover fade-out / meteor fade-in.
//   `blend`   = 1 -> the quad alpha-blends over the black underlay (so a diffuse
//               alpha < 0xFF fades it to black). 0 -> opaque (the meteor's normal
//               full-alpha path).
//
// vp_w/vp_h are the FULL backbuffer dims. Returns 1 if the draw was issued.
static int vid_draw_overlay_tex(IDirect3DDevice8 *dev, int vp_w, int vp_h,
                                IDirect3DTexture8 *tex, DWORD diffuse, int blend)
{
    void **vt = *(void ***)dev;
    BeginScene_t fnBeginScene = (BeginScene_t)vt[34];
    EndScene_t   fnEndScene   = (EndScene_t)vt[35];

    // FIX 1: snapshot the ENGINE's device state BEFORE we touch anything (the
    // D3DSBT_ALL state block also captures the viewport), so we can restore it
    // verbatim after the draw and leave the device exactly as the engine left it.
    vid_capture_state(dev);

    // Save the engine's current viewport and force the full backbuffer.
    D3DVIEWPORT8_X saved; memset(&saved, 0, sizeof(saved));
    int have_saved = vid_save_viewport(dev, &saved);
    D3DVIEWPORT8_X full;
    full.X = 0; full.Y = 0;
    full.Width  = (DWORD)vp_w;
    full.Height = (DWORD)vp_h;
    full.MinZ = 0.0f; full.MaxZ = 1.0f;
    vid_set_viewport(dev, &full);
    {   // one-shot: SEE what the engine's viewport was, confirming the clip.
        static long s_vp_logged = 0;
        if (!s_vp_logged) {
            s_vp_logged = 1;
            if (have_saved)
                log_line("[video] viewport was {X=%lu Y=%lu W=%lu H=%lu} -> forced 0,0,%d,%d",
                         (unsigned long)saved.X, (unsigned long)saved.Y,
                         (unsigned long)saved.Width, (unsigned long)saved.Height,
                         vp_w, vp_h);
            else
                log_line("[video] viewport was {GetViewport FAILED} -> forced 0,0,%d,%d",
                         vp_w, vp_h);
        }
    }

    g_vid_phase = 4;
    fnBeginScene(dev);

    // Opaque black fullscreen underlay (letterbox bars + clean black behind the
    // video / the fade target), then the aspect-fit textured quad. For the meteor
    // diffuse=0xFFFFFFFF: MODULATE texture*white == texture. For the cover fade /
    // meteor fade-in the diffuse alpha < 0xFF dissolves the quad over this black.
    vid_render_quad(dev, 0.0f, 0.0f, (float)vp_w, (float)vp_h,
                    /*textured=*/0, /*diffuse=*/0xFF000000u, /*tex=*/NULL, /*blend=*/0);
    // Aspect-fit the texture: the cover uses its own dims, the meteor uses the
    // decode dims. (cover_tex is non-NULL only for the boot cover still.)
    int fw = g_video.frame_w, fh = g_video.frame_h;
    if (tex && tex == g_video.cover_tex) { fw = g_video.cover_w; fh = g_video.cover_h; }
    float x1, y1, x2, y2;
    vid_compute_rect_dims(vp_w, vp_h, fw, fh, &x1, &y1, &x2, &y2);
    vid_render_quad(dev, x1, y1, x2, y2,
                    /*textured=*/1, diffuse, tex, blend);

    g_vid_phase = 9;
    fnEndScene(dev);

    // Restore the engine's viewport so its next frame is undisturbed. (On the
    // state-block path vid_restore_state below also restores the viewport;
    // restoring the same saved viewport twice is an idempotent no-op.)
    if (have_saved) vid_set_viewport(dev, &saved);

    // FIX 1: restore the ENGINE's full device state captured above, so the next
    // frame (e.g. the TITLE after the clip tears down) is byte-clean — no burned-in
    // last frame, no broken hex background from leaked render/texture-stage states.
    vid_restore_state(dev);
    return 1;
}

// Meteor/video convenience wrapper: draw g_video.texture. `diffuse`/`blend` carry
// the optional boot-leg fade-IN (diffuse alpha 0->255, blend=1); the char-create
// leg and the steady meteor pass 0xFFFFFFFF + blend=0 (opaque, original behavior).
static int vid_draw_overlay(IDirect3DDevice8 *dev, int vp_w, int vp_h,
                            DWORD diffuse, int blend)
{
    return vid_draw_overlay_tex(dev, vp_w, vp_h, g_video.texture, diffuse, blend);
}

// =====================================================================
// ==== BOOT COVER still image (replaces mod_boot_poster.c's draw) =====
// =====================================================================
//
// Loads patches/psobb_boot_poster.png (mirroring bp_decode_png: stbi_load(...,4)
// then RGBA->BGRA swizzle for A8R8G8B8, area-downscale anything over the texture
// cap), uploads it to a MANAGED texture, and draws it fullscreen through
// vid_draw_overlay_tex so it goes through the same state capture/restore. The boot
// leg owns the cover timing: opaque hold -> diffuse-alpha fade-out -> meteor start.

// Decode the cover PNG into BGRA (g_video.cover_rgba + cover_w/h). One-shot
// (cover_load_tried). On any failure cover_ok stays 0 and the boot leg just skips
// straight to the meteor (no cover). Mirrors bp_decode_png.
static void vid_cover_load_png(void)
{
    if (g_video.cover_load_tried) return;
    g_video.cover_load_tried = 1;

    char path[MAX_PATH];
    vid_resolve_path("patches\\psobb_boot_poster.png", path, sizeof(path));
    if (!path[0] || !vid_file_exists(path)) {
        log_line("[video] cover: no PNG at '%s' (skip cover, straight to meteor)", path);
        return;
    }
    int w = 0, h = 0, ch = 0;
    unsigned char *rgba = stbi_load(path, &w, &h, &ch, 4);
    if (!rgba || w <= 0 || h <= 0) {
        log_line("[video] cover: stbi_load failed for '%s'", path);
        if (rgba) stbi_image_free(rgba);
        return;
    }
    // RGBA (stb) -> BGRA (D3DFMT_A8R8G8B8): swap R<->B in place.
    size_t pixels = (size_t)w * (size_t)h;
    unsigned char *p = rgba;
    for (size_t i = 0; i < pixels; ++i, p += 4) {
        unsigned char r = p[0]; p[0] = p[2]; p[2] = r;
    }
    // Area-downscale anything over the texture cap (aspect-preserved), mirroring
    // bp_downscale_rgba — some wrappers SEH on an oversized texture upload.
    if (w > VID_COVER_MAX_DIM || h > VID_COVER_MAX_DIM) {
        int nw, nh;
        if (w >= h) { nw = VID_COVER_MAX_DIM; nh = (int)((long long)h * VID_COVER_MAX_DIM / w); }
        else        { nh = VID_COVER_MAX_DIM; nw = (int)((long long)w * VID_COVER_MAX_DIM / h); }
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        unsigned char *dst = (unsigned char *)malloc((size_t)nw * (size_t)nh * 4);
        if (dst) {
            for (int dy = 0; dy < nh; ++dy) {
                int sy0 = (int)((long long)dy * h / nh);
                int sy1 = (int)((long long)(dy + 1) * h / nh);
                if (sy1 <= sy0) sy1 = sy0 + 1;
                for (int dx = 0; dx < nw; ++dx) {
                    int sx0 = (int)((long long)dx * w / nw);
                    int sx1 = (int)((long long)(dx + 1) * w / nw);
                    if (sx1 <= sx0) sx1 = sx0 + 1;
                    unsigned a0 = 0, a1 = 0, a2 = 0, a3 = 0, n = 0;
                    for (int sy = sy0; sy < sy1; ++sy)
                        for (int sx = sx0; sx < sx1; ++sx) {
                            const unsigned char *s = rgba + ((size_t)sy * w + sx) * 4;
                            a0 += s[0]; a1 += s[1]; a2 += s[2]; a3 += s[3]; ++n;
                        }
                    unsigned char *d = dst + ((size_t)dy * nw + dx) * 4;
                    d[0] = (unsigned char)(a0 / n); d[1] = (unsigned char)(a1 / n);
                    d[2] = (unsigned char)(a2 / n); d[3] = (unsigned char)(a3 / n);
                }
            }
            log_line("[video] cover: downscaled %dx%d -> %dx%d (cap %d)",
                     w, h, nw, nh, VID_COVER_MAX_DIM);
            stbi_image_free(rgba);
            rgba = dst; w = nw; h = nh;
        }
    }
    g_video.cover_rgba = rgba;
    g_video.cover_w = w;
    g_video.cover_h = h;
    log_line("[video] cover: decoded PNG %dx%d (orig channels=%d)", w, h, ch);
}

// Create + upload the cover texture (A8R8G8B8 MANAGED). Frees cover_rgba after
// upload. Pitch-CLAMPED row blit (vid_blit_rows). Returns 1 once cover_ok.
static int vid_cover_upload(IDirect3DDevice8 *dev)
{
    if (g_video.cover_ok) return 1;
    if (!g_video.cover_rgba || g_video.cover_w <= 0 || g_video.cover_h <= 0) return 0;
    void **vt = *(void ***)dev;
    CreateTexture_t fnCreateTexture = (CreateTexture_t)vt[20];
    HRESULT hr = fnCreateTexture(dev, (UINT)g_video.cover_w, (UINT)g_video.cover_h,
                                 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                 &g_video.cover_tex);
    if (FAILED(hr) || !g_video.cover_tex) {
        log_line("[video] cover: CreateTexture FAILED hr=0x%08x", hr);
        g_video.cover_tex = NULL;
        return 0;
    }
    void **tvt = *(void ***)g_video.cover_tex;
    Tex_LockRect_t   fnLockRect   = (Tex_LockRect_t)tvt[16];
    Tex_UnlockRect_t fnUnlockRect = (Tex_UnlockRect_t)tvt[17];
    D3DLOCKED_RECT_X lr; lr.pBits = NULL; lr.Pitch = 0;
    hr = fnLockRect(g_video.cover_tex, 0, &lr, NULL, 0);
    if (FAILED(hr) || !lr.pBits || lr.Pitch <= 0) {
        log_line("[video] cover: LockRect FAILED hr=0x%08x pitch=%ld", hr, (long)lr.Pitch);
        if (lr.pBits) fnUnlockRect(g_video.cover_tex, 0);
        IUnknown_Release_t fnRelease = (IUnknown_Release_t)tvt[2];
        fnRelease(g_video.cover_tex);
        g_video.cover_tex = NULL;
        return 0;
    }
    int ok = vid_blit_rows(lr.pBits, lr.Pitch, g_video.cover_rgba,
                           g_video.cover_w * 4, g_video.cover_h);
    fnUnlockRect(g_video.cover_tex, 0);
    if (!ok) {
        IUnknown_Release_t fnRelease = (IUnknown_Release_t)tvt[2];
        fnRelease(g_video.cover_tex);
        g_video.cover_tex = NULL;
        return 0;
    }
    // Managed pool keeps its own backing copy; free the CPU buffer.
    stbi_image_free(g_video.cover_rgba);
    g_video.cover_rgba = NULL;
    g_video.cover_ok = 1;
    log_line("[video] cover: uploaded %dx%d (managed)", g_video.cover_w, g_video.cover_h);
    return 1;
}

// Release the cover texture + any CPU buffer (teardown / detach). Idempotent.
static void vid_cover_release(void)
{
    if (g_video.cover_tex) {
        __try {
            void **tvt = *(void ***)g_video.cover_tex;
            IUnknown_Release_t fnRelease = (IUnknown_Release_t)tvt[2];
            fnRelease(g_video.cover_tex);
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* swallow */ }
        g_video.cover_tex = NULL;
    }
    if (g_video.cover_rgba) { stbi_image_free(g_video.cover_rgba); g_video.cover_rgba = NULL; }
    g_video.cover_ok = 0;
}

// Focus-gated skip read for the cover (mirrors vid_skip_pressed's debounce/focus
// discipline, but tracks its OWN edge state — vid_skip_pressed's debounce is
// session-scoped and not yet seeded during the cover phase). Returns 1 on a fresh
// Enter/Esc up->down edge while the game is focused. Only meaningful during the
// OPAQUE hold (the caller jumps to the fade); ignored once fading.
static int vid_cover_skip_pressed(void)
{
    if (!g_video.skippable) return 0;
    SHORT ret = GetAsyncKeyState(VK_RETURN);
    SHORT esc = GetAsyncKeyState(VK_ESCAPE);
    int down = ((ret & 0x8000) != 0) || ((esc & 0x8000) != 0);
    if (!vid_app_focused()) { g_video.cover_skip_prev_down = down; return 0; }
    // Require both keys seen UP once (covers a key held from launch / the slot-
    // confirm Enter), then skip ONLY on a fresh up->down edge.
    if (!g_video.cover_skip_seen_up) {
        if (!down) g_video.cover_skip_seen_up = 1;
        g_video.cover_skip_prev_down = down;
        return 0;
    }
    int edge = down && !g_video.cover_skip_prev_down;
    g_video.cover_skip_prev_down = down;
    return edge ? 1 : 0;
}

// Advance the boot-cover phase + draw it. Returns 1 while the cover OWNS the
// screen (caller must NOT start the meteor yet), 0 once the cover is DONE (fade
// finished or no cover) so the boot leg proceeds to vid_start the meteor.
//
// Phase flow (wall-clock, GetTickCount):
//   load -> phase=2 OPAQUE hold (VID_COVER_HOLD_MS, diffuse alpha 255).
//           Enter/Esc during the hold -> jump to phase=3 (start the fade NOW).
//        -> phase=3 FADE: diffuse alpha ramps 255->0 over VID_COVER_FADE_MS.
//        -> phase=4 DONE: return 0; the boot leg starts the meteor (which then
//           fades IN from black).
static int vid_cover_present(IDirect3DDevice8 *dev, int vp_w, int vp_h)
{
    if (g_video.cover_phase >= 4) return 0;   // already done

    // Lazy load on first call.
    if (g_video.cover_phase == 0) {
        vid_cover_load_png();
        if (!g_video.cover_rgba) {            // no PNG / decode failed -> no cover
            g_video.cover_phase = 4;
            return 0;
        }
        g_video.cover_phase = 1;              // loaded; upload happens below
    }

    // Upload (retry until the device is texture-able; bounded by the meteor's hard
    // cap in the caller). If it permanently can't upload, give up on the cover.
    if (!g_video.cover_ok) {
        int up = 0;
        __try { up = vid_cover_upload(dev); }
        __except (EXCEPTION_EXECUTE_HANDLER) { up = 0; vid_cover_release(); }
        if (!up) return 1;                    // hold (black) until it uploads
        // Uploaded this frame -> begin the opaque hold.
        g_video.cover_phase    = 2;
        g_video.cover_phase_ms = GetTickCount();
        log_line("[video] cover: hold begins (%ums opaque, then %ums fade)",
                 (unsigned)VID_COVER_HOLD_MS, (unsigned)VID_COVER_FADE_MS);
    }

    DWORD now = GetTickCount();

    // OPAQUE hold: full alpha. Enter/Esc -> jump straight to the fade (never a hard
    // cut). Otherwise transition to the fade after VID_COVER_HOLD_MS.
    if (g_video.cover_phase == 2) {
        if (vid_cover_skip_pressed() ||
            (now - g_video.cover_phase_ms) >= (DWORD)VID_COVER_HOLD_MS) {
            g_video.cover_phase    = 3;
            g_video.cover_phase_ms = now;
            log_line("[video] cover: fade-out begins (%ums)", (unsigned)VID_COVER_FADE_MS);
        }
        __try {
            vid_draw_overlay_tex(dev, vp_w, vp_h, g_video.cover_tex,
                                 0xFFFFFFFFu, /*blend=*/1);
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* swallow */ }
        return 1;
    }

    // FADE: diffuse alpha 255->0 over VID_COVER_FADE_MS. At end -> DONE (meteor).
    if (g_video.cover_phase == 3) {
        DWORD dt = now - g_video.cover_phase_ms;
        if (dt >= (DWORD)VID_COVER_FADE_MS) {
            g_video.cover_phase = 4;          // fade complete -> meteor starts
            return 0;
        }
        float t = 1.0f - (float)dt / (float)VID_COVER_FADE_MS;  // 1 -> 0
        DWORD a = (DWORD)(t * 255.0f + 0.5f);
        if (a > 255) a = 255;
        DWORD diffuse = (a << 24) | 0x00FFFFFFu;               // ARGB, alpha=a
        __try {
            vid_draw_overlay_tex(dev, vp_w, vp_h, g_video.cover_tex, diffuse, /*blend=*/1);
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* swallow */ }
        return 1;
    }

    return 1;   // phase 1 with cover_ok not yet set is handled above; safety hold
}

// ---- ENGINE-EXIT ARM (CHARCREATE trigger) -------------------------------
//
// OWNERSHIP FINAL 2026-06-11 / DURATION (FINAL.6): under FULL scene-3 ownership
// (the 0x007C1588 replica) the intro ADX never plays, so the engine's natural
// ADX-done end (which would set 0xAAE988=1) never fires on its own. WE own the
// duration. So WHEN OUR VIDEO ENDS (EOF or user skip), while cc_session is set, we
// ARM the engine's OWN exit by writing the two scene-3 exit-gate flags:
//   byte[0x00AAE988] = 1   (substate gate: the replica takes its owned_sub1 branch)
//   byte[0x00AAE980] = 1   (the ==1 path -> the replica's request(5))
// On its next OWNED frame the replica sees both flags and issues the engine's OWN
// request(5) (the same code path as 0x007C1612) -> normal 3->5 transition (the
// engine releases the intro container at the 0x007C161C post-step we do NOT hook)
// -> our cc_on_request(5) clears cc_session + raises stop_requested -> Present tears
// down the cover -> LIVE class-select. We never invent a transition: we drive the
// engine's natural end, NOT the abort path 0x007A6174 (that is user-CANCEL).
//
// GATE: cc_session is the SOLE discriminator (our cover session) — NO read of the
// scene-id global 0x00AAB384. One-shot per cover (idempotent: the flags are 0/1
// bytes; re-writing 1 is harmless, and the engine/replica consumes them next frame).
// VERIFIED disasm of the scene-3 exit (0x007C15C4..0x007C1614) this session. The
// CC_EXIT_FLAG_988_R / _980_R pointers are defined once near the ownership block.
static void cc_arm_engine_exit(const char *why)
{
    // POD-only SEH firewall (two byte stores, no C++ unwinding objects -> no C2712).
    // Arm the engine 3->5 exit: the owned-frame replica reads these next frame and
    // issues request(5) WITHOUT running the input pump, so the skip Enter never
    // cascades into class-select (the trampoline DID run the pump -> auto-Hunter).
    __try {
        *CC_EXIT_FLAG_980_R = 1;   // discriminator first: ==1 -> replica takes request(5) path
        *CC_EXIT_FLAG_988_R = 1;   // gate second: substate -> owned_sub1 (triggers replica read)
        log_line("[video] cc engine-exit armed (%s): 0xAAE980=1 0xAAE988=1 -> request(5)",
                 why ? why : "?");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("[video] cc engine-exit arm SEH (flag write skipped)");
    }
}

// Called from the EOF/skip/watchdog teardown points. While our cover session is
// set, ARM the engine's own 3->5 exit (instead of tearing the video down here) and
// return 1 so the caller HOLDS the cover up: the engine then fires its own
// request(5) -> cc_on_request(5) -> stop_requested -> a clean teardown next frame
// (no flash of the live scene-3 underneath). Returns 0 when there is no cover
// session (BOOT, or already torn down) -> the caller proceeds with its own
// vid_teardown. The flag-arm is one-shot-idempotent (re-writing 1 is harmless; the
// engine consumes the flags on its next frame).
static int cc_session_hold_for_exit(const char *why)
{
    // cc_session is the REAL discriminator: the BOOT leg of trigger=both never sets
    // it, so a boot-leg EOF/skip/watchdog returns 0 here and tears down directly
    // (no engine 3->5 exit armed — that is char-create-only). Accept BOTH so the
    // char leg of trigger=both still holds-for-exit exactly like CHARCREATE.
    if (!cc_is_charcreate_trigger()) return 0;
    if (!InterlockedCompareExchange(&g_video.cc_session, 0, 0)) return 0;  // gate = our session
    cc_arm_engine_exit(why);
    return 1;   // hold the cover; the engine drives the teardown via request(5)
}

// The per-present body, called inside the SEH firewall.
static void vid_present_body(IDirect3DDevice8 *dev, int vp_w, int vp_h)
{
    // ---- DIAG (log-only): emit a one-line marker on every scene-id change.
    // Pure logging via the read-only adiag_scene() (NOT a discriminator; the
    // cover path keys off the request target + cc_session latch only).
    {
        static int s_adiag_last_scene = -123456;
        int sc = adiag_scene();
        if (sc != s_adiag_last_scene) {
            log_line("[adiag] scene %d->%d cc_session=%ld playing=%ld",
                     s_adiag_last_scene, sc, (long)adiag_cc(),
                     (long)InterlockedCompareExchange(&g_video.playing, 0, 0));
            s_adiag_last_scene = sc;
        }
    }

    // The vp_w/vp_h handed to us is the engine's last SetViewport, which at boot
    // is a small 3D-viewport sub-region — NOT the backbuffer. Ask the device for
    // the real backbuffer size + FORMAT and use them for the CopyRects path (the
    // scratch surface must match the backbuffer's W/H/Format). Fall back to the
    // passed viewport only if the query fails. One-shot log of the values.
    int bb_w = 0, bb_h = 0;
    DWORD bb_fmt = 0;
    int have_bb = vid_query_backbuffer(dev, &bb_w, &bb_h, &bb_fmt);
    if (have_bb) {
        static long s_bb_logged = 0;
        if (!s_bb_logged) {
            s_bb_logged = 1;
            log_line("[video] backbuffer WxH from device = %dx%d fmt=%lu (vp passed = %dx%d)",
                     bb_w, bb_h, bb_fmt, vp_w, vp_h);
        }
        vp_w = bb_w;
        vp_h = bb_h;
    }

    // --- Stage-2 trigger dispatch (cover-don't-detour; SOURCE-EVENT driven). ---
    // CHARCREATE honors a stop request from cc_on_request even mid-playback (the
    // engine left char-create -> uncover immediately). Consume it before the
    // start check so an exit that lands in the same Present can't immediately
    // re-trigger.
    if (InterlockedExchange(&g_video.stop_requested, 0)) {
        if (InterlockedCompareExchange(&g_video.playing, 0, 0)) {
            log_line("[video] cc source-event: stop -> teardown");
            vid_teardown("cc exit");
        }
        // start_requested is NOT set here (the exit hook clears the session),
        // so we just fall through; nothing to draw.
    }

    // VID_TRIGGER_BOTH boot-leg END detector: the boot leg always tears down via
    // vid_teardown (cc_session is never set for it, so cc_session_hold_for_exit
    // returns 0). vid_teardown already did the FULL reset — MF reader/ffmpeg child
    // closed, playing=0, eof=0, ring cleared, texture freed. Here we finish the
    // hand-off: SWITCH the active source boot_path -> char_path, CONSUME the boot
    // one-shot (start_requested=0, present_seen=0), and latch boot_leg_done so the
    // leg select below routes to the request(3)-driven CHAR leg. One-shot: keys off
    // (boot_leg_started && !boot_leg_done && !playing).
    if (g_video.trigger == VID_TRIGGER_BOTH &&
        g_video.boot_leg_started && !g_video.boot_leg_done &&
        !InterlockedCompareExchange(&g_video.playing, 0, 0)) {
        g_video.boot_leg_done   = 1;
        g_video.start_requested = 0;   // consume any stray boot one-shot
        g_video.present_seen    = 0;
        if (g_video.char_path[0])
            _snprintf_s(g_video.path, sizeof(g_video.path), _TRUNCATE,
                        "%s", g_video.char_path);
        log_line("[video] both: boot leg ended -> active path now char='%s'",
                 g_video.path);
    }

    // VID_TRIGGER_BOTH leg select: the BOOT one-shot leg runs first (present-count
    // start, boot_path). Once it ends (boot_leg_done set at its teardown, or by an
    // early request(3)), the request(3)-driven CHAR leg takes over (char_path). We
    // express this by treating BOTH as BOOT while !boot_leg_done, and as CHARCREATE
    // after — reusing the exact same one-shot paths below, so each leg behaves
    // byte-identically to the standalone trigger it mirrors.
    int eff_trigger = g_video.trigger;
    if (g_video.trigger == VID_TRIGGER_BOTH)
        eff_trigger = g_video.boot_leg_done ? VID_TRIGGER_CHARCREATE
                                            : VID_TRIGGER_BOOT;

    // We never start a new session while one is already playing.
    if (!InterlockedCompareExchange(&g_video.playing, 0, 0)) {
        if (eff_trigger == VID_TRIGGER_BOOT) {
            // BOOT: one-shot start after a short device warm-up (the former
            // Stage-1 boot-test path, now mode-gated). The clip covers the boot
            // splash / lands on the title once it ends or is skipped. For BOTH this
            // is the BOOT leg (boot_path); the char leg below takes over after.
            if (g_video.start_requested) {
                LONG seen = InterlockedIncrement(&g_video.present_seen);
                if (seen == 1) g_video.boot_first_ms = GetTickCount();
                DWORD held = g_video.boot_first_ms ? (GetTickCount() - g_video.boot_first_ms) : 0;

                // BOOT COVER: after a short warm-up, show the still cover (load +
                // upload + opaque hold + fade-out, all through vid_draw_overlay_tex's
                // state capture/restore). vid_cover_present returns 1 while the cover
                // OWNS the screen -> draw it + return (meteor waits). It returns 0 once
                // the fade completes (or there is no cover PNG) -> start the meteor,
                // which then fades IN from black. The hard cap force-starts the meteor
                // if the cover never finishes (device never texture-able), so the boot
                // can't stall on black. This REPLACES the old boot_poster_shown_ms gate.
                if (seen >= VID_START_AFTER_PRESENTS) {
                    int cover_busy = vid_cover_present(dev, vp_w, vp_h);
                    int cap_hit = (held >= VID_BOOT_HARD_CAP_MS);
                    if (cover_busy && !cap_hit)
                        return;                    // cover on screen; meteor waits
                    // Cover done (or hard cap) -> start the meteor with a fade-in.
                    g_video.start_requested = 0;   // one-shot
                    if (vid_start(dev, vp_w, vp_h)) {
                        g_video.meteor_fadein_ms = GetTickCount();  // boot leg: fade IN
                        vid_cover_release();           // cover's job is done; free its texture
                        boot_poster_force_disable();   // legacy poster: don't let it re-show
                        if (g_video.trigger == VID_TRIGGER_BOTH) {
                            g_video.boot_leg_started = 1;  // both-mode: mark boot leg live
                            // Silence the engine/title BGM while the boot leg plays (the
                            // title boots live underneath). cc_audio_arm zeros SOUNDCTRL
                            // (engine master) but NOT our MF video audio; vid_teardown
                            // disarms + restores it when the boot leg ends.
                            cc_audio_arm();
                        }
                    }
                }
            }
        } else if (eff_trigger == VID_TRIGGER_CHARCREATE) {
            // CHARCREATE: start when cc_on_request (the 0x007A60DC source-event
            // hook) saw request(3) and raised start_requested. NO scene poll.
            // The overlay COVERS the engine's scripted starfield intro (the
            // engine keeps running its script underneath; we merely hide it for
            // the clip's duration) and is HELD across the 3->5 intro->class-
            // select hand-off by the hook's cc_session latch. On skip/EOF the
            // teardown uncovers whatever the engine is now showing. Re-arm is
            // automatic: a later request(3) raises start_requested again.
            if (g_video.start_requested) {
                g_video.start_requested = 0;   // one-shot per request(3)
                log_line("[video] cc source-event: request(3) -> start");
                // NB: audio mute (cc_audio_arm) + the scene-3 ownership replica are
                // already armed by cc_on_request at request(3) time (engine thread,
                // BEFORE the first scene-3 dispatch). This start path only stands up
                // the video graph. SAFETY: if vid_start FAILS we'd own scene-3 (draws
                // nothing) + mute with no video -> a black/silent intro. So on
                // failure, RELEASE the cover: clear cc_session (replica reverts to
                // passthrough -> the live intro plays), restore SOUNDCTRL, and arm
                // the engine's own 3->5 exit isn't needed (the live intro runs).
                if (!vid_start(dev, vp_w, vp_h)) {
                    log_line("[video] vid_start FAILED -> release cover (live intro)");
                    InterlockedExchange(&g_video.cc_session, 0);
                    cc_audio_disarm();
                }
            }
        }
        // If still not playing after the trigger check, nothing to draw.
        if (!InterlockedCompareExchange(&g_video.playing, 0, 0)) return;
    }

    // --- watchdog ---
    if (g_video.max_seconds > 0 && g_video.qpc_freq) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - g_video.qpc_start)
                         / (double)g_video.qpc_freq;
        if (elapsed > (double)g_video.max_seconds) {
            // CHARCREATE: arm the engine's own 3->5 exit and HOLD the cover; the
            // engine's request(5) lands in cc_on_request -> stop_requested ->
            // teardown (no scene-3 flash). Otherwise tear down directly (BOOT).
            if (cc_session_hold_for_exit("watchdog")) return;
            vid_teardown("watchdog");
            return;
        }
    }

    // --- skip-drain (BOOT leg only): a skip already fired but the key is still
    // held. Hold the cover up (fall through to redraw the last frame) until BOTH
    // Enter and Esc read UP, THEN teardown. This stops the revealed title from
    // polling the still-down Enter as a fresh press and popping its menu — one
    // Enter skips cleanly, a SECOND Enter opens the menu. cc_session is never set
    // on the boot leg, so this never touches the char-create path. Checked BEFORE
    // the EOF logic below so the cover holds even if the reader hits EOF mid-drain.
    if (g_video.skip_draining) {
        if (vid_skip_keys_up()) {
            g_video.skip_draining = 0;
            // RELEASED -> NOW hand off. Char-create: arm the engine's 3->5 exit (the
            // replica issues request(5) next frame). Boot: tear down. Because we waited
            // for the release, the held Enter is GONE before the next scene reads input,
            // so it cannot cascade (char-create class-select auto-Hunter / boot menu).
            if (cc_session_hold_for_exit("skip (key released)")) return;
            vid_teardown("skip (key released)");
            return;
        }
        // keys still held -> fall through and redraw the last frame (cover up).
    } else if (vid_skip_pressed()) {
        // BOTH legs WAIT for the skip key to RELEASE before handing off, so a held
        // Enter never cascades into the next scene (handing off on a still-down Enter
        // lets class-select read it and auto-pick Hunter — this is the input-leak fix).
        if (!vid_skip_keys_up()) {
            g_video.skip_draining = 1;
            log_line("[video] skip: key still held -> draining (hold cover until release)");
            return;   // hold the cover (redraw last frame) until release
        }
        // keys already up this Present -> hand off now.
        if (cc_session_hold_for_exit("skip")) return;   // char-create: arm engine 3->5 exit
        vid_teardown("skip");                            // boot: tear down
        return;
    }

    // (The diag config field is accepted by mod_video_init for ABI stability but
    //  drives no render — the textured video path is the only render path.)

    // --- pick the latest DUE frame by wall-clock ---
    LONG filled = InterlockedCompareExchange(&g_video.ring_filled, 0, 0);
    LONG eof    = InterlockedCompareExchange(&g_video.eof, 0, 0);
    if (filled <= 0) {
        // No frame yet. If the reader already hit EOF with zero frames the
        // source was empty/unreadable -> end cleanly. While a cover session is
        // up, arm the engine 3->5 exit instead (no frame to hold, just hand off).
        if (eof) {
            if (!cc_session_hold_for_exit("eof (no frames)"))
                vid_teardown("eof (no frames)");
        }
        return;
    }

    LONG newest_slot  = InterlockedCompareExchange(&g_video.newest_slot, -1, -1);
    LONG newest_index = InterlockedCompareExchange(&g_video.newest_index, -1, -1);

    // Frame-advance heartbeat, throttled to ~1/sec via QPC (NOT every present).
    // Lets a live capture confirm newest_index is climbing (reader/pipe healthy)
    // vs. stuck (reader/pipe stalled). last_uploaded_index shows the texture is
    // tracking; ring_filled is the producer count; eof flags end-of-stream.
    if (g_video.qpc_freq) {
        static LONGLONG s_last_fa_log = 0;
        LARGE_INTEGER fnow; QueryPerformanceCounter(&fnow);
        if (s_last_fa_log == 0) s_last_fa_log = fnow.QuadPart;
        if ((fnow.QuadPart - s_last_fa_log) >= g_video.qpc_freq) {  // >= 1s
            s_last_fa_log = fnow.QuadPart;
            log_line("[video] frame: newest_index=%ld uploaded=%ld ring_filled=%ld eof=%ld",
                     (long)newest_index, (long)g_video.last_uploaded_index,
                     (long)filled, (long)eof);
        }
    }

    // Compute the frame index that is DUE now (t / frame_period).
    LONGLONG due_index = newest_index;
    if (g_video.fps > 0.0 && g_video.qpc_freq) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - g_video.qpc_start)
                   / (double)g_video.qpc_freq;
        LONGLONG di = (LONGLONG)(t * g_video.fps);
        // End of stream: once we've shown the last produced frame and the reader
        // reports EOF, tear down. While a cover session is up, instead ARM the
        // engine's 3->5 exit and HOLD the last frame (cover stays up) until the
        // engine fires request(5) -> cc_on_request(5) -> stop_requested -> teardown.
        // Boot leg (non-cover): tear down the instant the reader signals EOF so the
        // title is revealed and (trigger=both) the char leg can take over. Do NOT
        // wait for the presentation clock to pass the last frame (di > newest_index)
        // — that gate can lag seconds, which was freezing the boot logo on screen.
        // While a BOOT-leg skip is draining (key still held) the cover MUST stay
        // up even if the reader hits EOF — otherwise EOF would tear down and the
        // held Enter would leak to the title. The drain handler above owns the
        // teardown once both keys release.
        if (eof && !g_video.skip_draining &&
            !InterlockedCompareExchange(&g_video.cc_session, 0, 0)) {
            vid_teardown("eof (boot leg)");
            return;
        }
        if (eof && di > newest_index) {
            if (!cc_session_hold_for_exit("eof")) {
                vid_teardown("eof");
                return;
            }
            // held: fall through, clamp to newest, redraw the last frame.
        }
        // Hold on fast Present, skip-ahead on slow Present, but never past
        // the newest the reader has actually produced.
        if (di > newest_index) di = newest_index;
        due_index = di;
    }

    // The reader only keeps the NEWEST slot pointer; for Stage 1 we present
    // the newest available frame (the 3-slot ring tolerates the small skew
    // between due_index and newest_index). due_index just gates EOF + pacing.
    (void)due_index;

    if (newest_slot < 0 || newest_slot >= VID_RING_SLOTS ||
        !g_video.ring[newest_slot])
        return;

    // --- textured-quad blit (the path that DID reach the screen) ---
    // Upload the newest frame into the MANAGED A8R8G8B8 texture (BGRA == that
    // format byte-for-byte, no swizzle), then draw the two-quad overlay (opaque
    // black underlay + aspect-fit textured video) inside our own BeginScene/
    // EndScene with the viewport forced to the full backbuffer. The earlier
    // textured-quad version showed real frame content but clipped to a sub-rect
    // because the engine's last SetViewport was a sub-region — the viewport
    // force inside vid_draw_overlay is the fix.
    if (!vid_upload_frame(dev, newest_slot, newest_index)) {
        // Texture create/lock/blit failed for this frame; skip it. The next
        // Present retries; a hard fault is caught by the SEH firewall.
        return;
    }

    // Meteor diffuse: the BOOT leg fades the meteor IN from black over
    // VID_METEOR_FADEIN_MS (meteor_fadein_ms is set at vid_start; cleared once the
    // window elapses). The char-create intros leg never sets it -> constant
    // 0xFFFFFFFF opaque (no fade-in). Fully opaque after the window -> blend off,
    // identical to the original meteor path.
    DWORD diffuse = 0xFFFFFFFFu;
    int   blend   = 0;
    if (g_video.meteor_fadein_ms) {
        DWORD dt = GetTickCount() - g_video.meteor_fadein_ms;
        if (dt < (DWORD)VID_METEOR_FADEIN_MS) {
            float t = (float)dt / (float)VID_METEOR_FADEIN_MS;    // 0 -> 1
            DWORD a = (DWORD)(t * 255.0f + 0.5f);
            if (a > 255) a = 255;
            diffuse = (a << 24) | 0x00FFFFFFu;                    // alpha ramps up
            blend   = 1;
        } else {
            g_video.meteor_fadein_ms = 0;                         // fade-in done
        }
    }

    if (vid_draw_overlay(dev, vp_w, vp_h, diffuse, blend)) {
        g_video.last_copied_index = newest_index;
    }
}

// ---- public API ----

void mod_video_init(const char *path, const char *boot_path, int enabled,
                    int skippable, const char *ffmpeg_path, int max_seconds,
                    int skip_debounce_ms, int audio, int diag, int trigger,
                    int decoder)
{
    if (g_video.init_done) return;
    g_video.init_done        = 1;
    g_video.enabled          = enabled;
    g_video.skippable        = skippable;
    // 0 (or negative) => NO watchdog: the clip plays to its natural EOF or until
    // the user skips with Enter. A positive value is an optional safety cap only.
    g_video.max_seconds      = (max_seconds > 0) ? max_seconds : 0;
    g_video.skip_debounce_ms = (skip_debounce_ms > 0) ? skip_debounce_ms : 300;
    g_video.audio            = audio;
    g_video.diag             = diag;
    g_video.trigger          = trigger;
    g_video.decoder          = decoder;
    g_video.cc_session       = 0;
    g_video.cc_exit_armed    = 0;
    g_video.stop_requested   = 0;
    g_video.newest_slot      = -1;
    g_video.newest_index     = -1;
    g_video.last_uploaded_index = -1;
    g_video.last_copied_index   = -1;
    // BOOT cover still + meteor fade-in state (zeroed; cover loads lazily at boot).
    g_video.cover_phase       = 0;
    g_video.cover_load_tried  = 0;
    g_video.cover_ok          = 0;
    g_video.cover_w = g_video.cover_h = 0;
    g_video.cover_rgba        = NULL;
    g_video.cover_tex         = NULL;
    g_video.cover_skip_seen_up = 0;
    g_video.cover_skip_prev_down = 0;
    g_video.meteor_fadein_ms  = 0;

    if (!enabled) {
        // Dormant: do not even resolve paths. on_present is a pure no-op.
        log_line("[video] init: VideoEnable=0 (dormant)");
        return;
    }

    // path = CHAR-CREATE leg source; boot_path = BOOT leg source (trigger=both).
    vid_resolve_path(path, g_video.char_path, sizeof(g_video.char_path));
    if (boot_path && boot_path[0])
        vid_resolve_path(boot_path, g_video.boot_path, sizeof(g_video.boot_path));
    // Active source g_video.path: BOTH plays the boot leg FIRST (boot_path), then
    // its teardown swaps the active path to char_path; boot/charcreate mirror the
    // single source into both char_path and the active path.
    if (trigger == VID_TRIGGER_BOTH && g_video.boot_path[0])
        _snprintf_s(g_video.path, sizeof(g_video.path), _TRUNCATE, "%s", g_video.boot_path);
    else
        _snprintf_s(g_video.path, sizeof(g_video.path), _TRUNCATE, "%s", g_video.char_path);
    vid_resolve_path(ffmpeg_path, g_video.ffmpeg, sizeof(g_video.ffmpeg));
    if (audio && g_video.path[0]) {
        _snprintf_s(g_video.wav, sizeof(g_video.wav), _TRUNCATE,
                    "%s.wav", g_video.path);
    }

    log_line("[video] init enabled=1 path='%s' ffmpeg='%s' skippable=%d "
             "max_s=%d debounce=%dms audio=%d diag=%d trigger=%s decoder=%s",
             g_video.path, g_video.ffmpeg, skippable,
             g_video.max_seconds, g_video.skip_debounce_ms, audio,
             g_video.diag,
             trigger == VID_TRIGGER_CHARCREATE ? "charcreate"
           : trigger == VID_TRIGGER_BOOT       ? "boot"
           : trigger == VID_TRIGGER_BOTH       ? "both"
                                               : "off",
             g_video.decoder == VID_DECODER_FFMPEG ? "ffmpeg" : "mf");

    if (!g_video.path[0] || !vid_embedded_exists(g_video.path)) {
        log_line("[video] no embedded clip for '%s' -> disabled (stock path)", g_video.path);
        vid_disable_perm("embedded clip missing");
        return;
    }

    if (trigger == VID_TRIGGER_OFF) {
        // Enabled + a valid source, but no trigger event configured: stay
        // armed for nothing. on_present runs (the no-op trigger dispatch falls
        // through) but never starts a session. This is the "loaded but idle"
        // state — useful for A/B without auto-play. Logged so it's not mistaken
        // for a misconfiguration.
        log_line("[video] trigger=off: source ready but no auto-start event");
        return;
    }

    // Arm the BOOT one-shot (mode==BOOT only). CHARCREATE arms off the
    // 0x007A60DC source-event hook's request(3) -> start_requested, so it starts
    // at 0 here; the hook raises it.
    g_video.start_requested = (trigger == VID_TRIGGER_BOOT ||
                               trigger == VID_TRIGGER_BOTH) ? 1 : 0;
    g_video.present_seen    = 0;

    // CHARCREATE: install the two source-event hooks (the ONLY trigger that needs
    // them; BOOT/OFF never touch the engine transition setter / scene-3 update).
    //   1) the request-setter hook 0x007A60DC (cc_on_request) — session discriminator
    //   2) the scene-3 update hook 0x007C1588 (stub_7c1588) — OWNERSHIP replica.
    // We install BOTH here at init (not lazily in cc_on_request) so MH_EnableHook
    // never runs from inside an engine callback. The scene-3 stub is a byte-identical
    // passthrough until cc_session is set, so installing it eagerly is inert until a
    // real request(3) arms the cover. Failure is non-fatal (logged); the cover simply
    // never arms (no scene poll exists to fall back to). With trigger!=charcreate
    // NEITHER hook is installed -> the client is byte-identical to today.
    if (trigger == VID_TRIGGER_CHARCREATE || trigger == VID_TRIGGER_BOTH) {
        if (!cc_request_hook_install())
            log_line("[video] cc source-event hook NOT installed -> cover disarmed");
        if (!cc_scene3_hook_install())
            log_line("[video] cc scene-3 ownership hook NOT installed -> overlay-only");
        if (!cc_play_bgm_hook_install())
            log_line("[video] intro-BGM-prevent hook NOT installed -> intro ADX may bleed");
    }
    if (trigger == VID_TRIGGER_BOOT || trigger == VID_TRIGGER_BOTH) {
        if (!boot_title_hook_install())
            log_line("[video] boot title-defer hooks NOT installed -> title builds under video (old behavior)");
    }
}

void mod_video_on_present(void *device, int viewport_w, int viewport_h)
{
    // The cheap no-op that keeps VideoEnable=0 byte-identical to today.
    if (!g_video.enabled || g_video.disabled_perm) return;
    if (!device) return;
    InterlockedIncrement(&g_video.present_calls);

    IDirect3DDevice8 *dev = (IDirect3DDevice8 *)device;

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

    __try {
        vid_present_body(dev, vp_w, vp_h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Never poison Present. Name the exact failing D3D op (phase legend at
        // g_vid_phase's declaration) BEFORE tearing down, then permanently
        // disable so a faulting frame can't recur.
        log_line("[video] present SEH at phase=%ld", g_vid_phase);
        vid_teardown("present SEH");
        vid_disable_perm("present SEH");
    }
}

// ---- Lost-device discipline (alt-tab / dgVoodoo2 Reset) -------------------
//
// On a device-lost event (alt-tab in virtual fullscreen drops the D3D8 device;
// dgVoodoo2 then Resets it) the engine's IDirect3DDevice8::Reset hook
// (Hook_Reset in pso_widescreen.c) calls these bridges. Reset INVALIDATES every
// device-state object and every D3DPOOL_DEFAULT resource. Our overlay holds two
// device-bound things:
//   * the D3DSBT_ALL state block (sb_token, created by CreateStateBlock) — a
//     device-state object that is INVALID after a Reset; Capturing/Applying a
//     stale token is exactly the kind of op that AVs inside the wrapper.
//   * the frame texture (g_video.texture) — D3DPOOL_MANAGED, so the runtime
//     would re-upload it itself, but we drop+recreate it anyway so we never
//     hand the wrapper a handle from across a Reset boundary, and so a
//     backbuffer-size change re-sizes it cleanly on the next session.
//
// We do NOT tear down the decode session (ffmpeg/MF reader, ring buffers, audio)
// — none of that is device-bound; the next vid_present_body re-creates the
// texture and state block lazily (sb_tried=0 -> recreate; texture==NULL ->
// recreate) and continues the clip across the alt-tab.
//
// on_device_lost runs BEFORE real_Reset (resources must be gone before Reset);
// on_device_reset runs AFTER a SUCCESSFUL Reset. Both are SEH-firewalled and
// idempotent — safe to call when nothing is allocated.
void mod_video_on_device_lost(void)
{
    if (!g_video.enabled) return;
    log_line("[video] on_device_lost: releasing state block + frame texture");
    // Release the D3DSBT_ALL state block via its owning device (sb_dev), which
    // is still the live COM object across a Reset (Reset keeps the same device).
    if (g_video.sb_have && g_video.sb_dev) {
        __try {
            void **dvt = *(void ***)g_video.sb_dev;
            DeleteStateBlock_t fnDelete = (DeleteStateBlock_t)dvt[56];
            fnDelete((IDirect3DDevice8 *)g_video.sb_dev, g_video.sb_token);
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* swallow */ }
    }
    g_video.sb_have  = 0;
    g_video.sb_tried = 0;     // force lazy re-create on the next capture
    g_video.sb_token = 0;
    g_video.sb_dev   = NULL;
    g_video.save_method_logged = 0;
    g_vid_saved.valid = 0;
    // Release any stray AddRef the manual save path may hold (normally NULL).
    if (g_vid_saved.tex0) {
        __try {
            void **tvt = *(void ***)g_vid_saved.tex0;
            IUnknown_Release_t fnRelease = (IUnknown_Release_t)tvt[2];
            fnRelease(g_vid_saved.tex0);
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* swallow */ }
        g_vid_saved.tex0 = NULL;
    }
    // Drop the frame texture so it is recreated (and re-sized to the post-Reset
    // backbuffer if the dims changed) on the next vid_ensure_texture.
    if (g_video.texture) {
        __try {
            void **tvt = *(void ***)g_video.texture;
            IUnknown_Release_t fnRelease = (IUnknown_Release_t)tvt[2];
            fnRelease(g_video.texture);
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* swallow */ }
        g_video.texture = NULL;
        g_video.tex_w = g_video.tex_h = 0;
        g_video.last_uploaded_index = -1;
    }
    // Drop the boot-cover texture too (if a Reset lands mid-cover). The CPU buffer
    // was freed at upload, so force a clean re-load+re-upload on the next cover
    // frame: only re-load if the cover was still active (not past its fade).
    vid_cover_release();
    if (g_video.cover_phase > 0 && g_video.cover_phase < 4) {
        g_video.cover_load_tried = 0;   // re-decode the PNG (cover_rgba was freed)
        g_video.cover_phase      = 0;   // restart the cover lifecycle cleanly
    }
}

void mod_video_on_device_reset(void)
{
    if (!g_video.enabled) return;
    // Nothing to allocate here: the texture and state block are recreated lazily
    // by the next vid_present_body (texture==NULL -> recreate; sb_tried==0 ->
    // recreate). This bridge exists so the Reset hook has a symmetric "after"
    // call and so future device-bound resources have a home. The lazy paths
    // re-fetch the device pointer fresh from the Present hook each frame, so a
    // changed device is picked up without caching anything stale here.
    log_line("[video] on_device_reset: resources will recreate lazily next frame");
}

void mod_video_log_summary(void)
{
    log_line("[video] summary: present_calls=%ld disabled_perm=%d playing=%ld "
             "frame=%dx%d eof=%ld filled=%ld",
             (long)g_video.present_calls, g_video.disabled_perm,
             (long)g_video.playing, g_video.frame_w, g_video.frame_h,
             (long)g_video.eof, (long)g_video.ring_filled);
    // Best-effort final teardown so we never leave ffmpeg running on detach.
    // (vid_teardown also disarms the SOUNDCTRL mute as a safety net.)
    vid_teardown("detach");
    vid_cover_release();   // free the boot-cover texture / CPU buffer (idempotent)
    // Restore the SOUNDCTRL flags explicitly too (idempotent) before removing hooks.
    cc_audio_disarm();
    // Remove the CHARCREATE source-event detour (mirrors pso_widescreen.c's
    // other hooks; no-op if it was never installed).
    cc_request_hook_uninstall();
    // Remove the scene-3 OWNERSHIP replica detour (no-op if it was never installed).
    // Restores 0x007C1588 to byte-identical stock.
    cc_scene3_hook_uninstall();
    cc_play_bgm_hook_uninstall();   // remove the intro-BGM-prevent detour
    boot_title_hook_uninstall();    // remove the boot title-defer detours
}
