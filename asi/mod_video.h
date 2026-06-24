// mod_video.h — P3 Stage 1 video player contract (FFmpeg-decoded pipe + Present blit).
//
// Self-contained companion module mirroring mod_boot_poster.c's contract.
// All entry points are SAFE to call before init (no-op) and after disable
// (no-op). The on_present path is a cheap `if(!g_video.enabled) return;`
// when disabled, so VideoEnable=0 (default) is byte-identical to no module.
//
// Stage 1 scope: build the player + present path ONLY. No engine-state
// hooks, no intro suppression (Stage 2). The Stage-1 test trigger plays a
// configured video once on the first N present frames after device create.
//
// Stage 2 (2026-06-11): the Stage-1 boot-test trigger is replaced by a
// VideoTrigger mode. The video is now driven by a per-frame STATE POLL — the
// SPEC's "cover, don't detour" path (no engine-state-machine MinHook):
//   * VID_TRIGGER_BOOT       — start once at boot/title (the old present-count
//                              one-shot, now mode-gated).
//   * VID_TRIGGER_CHARCREATE — start on the RISING edge of the char-create
//                              scene gate: the engine current-scene id
//                              @0x00AAB384 ∈ {3 (scripted intro), 5 (class-
//                              select)} (RE 2026-06-11; replaces the old
//                              0x00A3A93C-nonzero predicate, which falsely fired
//                              on an existing-char Confirm because 0x00A3A93C is
//                              the game-list socket buffer, not a char-create
//                              root). The Present overlay COVERS the engine's scripted
//                              starfield intro for the clip's duration; on
//                              skip/EOF teardown reveals whatever the engine is
//                              showing underneath (class-select once the script
//                              has finished, else the tail of the scripted
//                              intro). The engine state machine is NOT touched.
//                              Re-armed on the gate's falling edge so re-entering
//                              char-create replays.
//   * VID_TRIGGER_OFF        — never auto-start (the default when no key set).

#ifndef MOD_VIDEO_H
#define MOD_VIDEO_H

// VideoTrigger modes (parsed from the INI string in pso_widescreen.c).
#define VID_TRIGGER_OFF         0
#define VID_TRIGGER_BOOT        1
#define VID_TRIGGER_CHARCREATE  2

// VideoDecoder modes (parsed from the INI string in pso_widescreen.c).
//   VID_DECODER_MF     — in-process Windows Media Foundation Source Reader
//                        (H.264 -> RGB32 -> ring). Ships nothing; decode runs
//                        on the OS in-box H.264 MFT. The DEFAULT.
//   VID_DECODER_FFMPEG — external ffmpeg.exe -> rawvideo BGRA pipe (the legacy
//                        Stage-1 path, kept as a config-selectable fallback).
#define VID_DECODER_MF          0
#define VID_DECODER_FFMPEG      1

// Initialize from parsed INI knobs. Resolves paths, but spawns NOTHING and
// touches no D3D until the first qualifying on_present. enabled=0 => the
// module stays fully dormant (every other entry point becomes a no-op).
//
//   enabled        VideoEnable        (0 = OFF, the default)
//   path           VideoPath          (resolved relative to psobb.exe dir)
//   skippable      VideoSkippable     (1 = Enter/Esc skip with debounce)
//   ffmpeg_path    VideoFfmpeg        ("" => "ffmpeg" on PATH / next to exe)
//   max_seconds    VideoMaxSeconds    (hard watchdog per playback)
//   skip_debounce  VideoSkipDebounceMs(release-then-press arm delay)
//   audio          VideoAudio         (1 = play sibling .wav via mci)
//   diag           VideoDiag          (ACCEPTED but INERT as of 2026-06-17: the
//                                       VideoDiag RED-quad isolation render path was
//                                       pruned; the param is kept for ABI stability)
//   trigger        VideoTrigger       (VID_TRIGGER_OFF/BOOT/CHARCREATE — what
//                                       event starts playback; OFF = never)
//   decoder        VideoDecoder       (VID_DECODER_MF [default, in-process MF]
//                                       or VID_DECODER_FFMPEG [external pipe])
void mod_video_init(const char *path, int enabled, int skippable,
                    const char *ffmpeg_path, int max_seconds,
                    int skip_debounce_ms, int audio, int diag, int trigger,
                    int decoder);

// Called once per Present from Hook_Present, alongside boot_poster_on_present.
// No-op when disabled. When a video is playing it locks the newest ready
// frame, uploads to a dynamic texture, and blits it fullscreen (letterboxed).
//   device      the IDirect3DDevice8* (opaque here)
//   viewport_w  cached backbuffer width  (g_last_vp_w)
//   viewport_h  cached backbuffer height (g_last_vp_h)
void mod_video_on_present(void *device, int viewport_w, int viewport_h);

// Lost-device discipline. Called from the IDirect3DDevice8::Reset hook
// (Hook_Reset in pso_widescreen.c) around the engine's device Reset on an
// alt-tab / fullscreen-toggle under dgVoodoo2. on_device_lost runs BEFORE the
// real Reset (releases the device-bound state block + frame texture);
// on_device_reset runs AFTER a SUCCESSFUL Reset (resources recreate lazily on
// the next on_present). Both are no-ops when disabled and idempotent.
void mod_video_on_device_lost(void);
void mod_video_on_device_reset(void);

// Closing diagnostics on DLL_PROCESS_DETACH (safe under DllMain).
void mod_video_log_summary(void);

#endif // MOD_VIDEO_H
