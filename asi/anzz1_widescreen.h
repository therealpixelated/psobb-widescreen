// anzz1_widescreen.h — faithful port of anzz1/tofuman WideScreenPatch
// (WideScreen.c, target MTethVer12513 / 1.25.13) as the authoritative
// engine-memory widescreen apply path for pso_widescreen.asi.
//
// This is a pure engine-memory patch (no D3D wrapper involvement). It
// rewrites psobb.exe .text immediates and .data floats to relocate the
// HUD/UI to a non-4:3 canvas. See anzz1_widescreen.c for the algorithm.
//
// Differences from the upstream reference (deliberate adaptations):
//   * Aspect comes from the caller (our INI GameAspect), NOT the monitor.
//   * No fullscreen/borderless window hook (memset 0x00482E20 +
//     myCreateWindowExA @ 0x0082D1D8) — we are windowed + reparented.
//   * No printscreen-disable (0x007C1424).
//   * Version mismatch logs a warning instead of MessageBox + ExitProcess.
//   * Every write goes through VirtualProtect RW (many targets are .text).
//   * The ADD-style coordinate lists run EXACTLY ONCE per process
//     (static applied guard) to avoid double-offsetting.

#ifndef ANZZ1_WIDESCREEN_H
#define ANZZ1_WIDESCREEN_H

// Apply the anzz1/tofuman widescreen coordinate patches.
//
// 2026-06-09 REFACTOR: the A/B/C/D extents + render_w/render_h are now computed
// ONCE in pso_widescreen.c's ws_compute_scale() and passed in (MINOR-3), so the
// magnitude lives in exactly one place. This fn no longer derives them.
//   aspect        — target display aspect ratio (logging / 12 AR writes).
//   A/B/C         — anzz1 horizontal / atlas / vertical extents (= ws_scale_ctx).
//   D             — anzz1 integer tile-height unit.
//   render_w/render_h — the ACTUAL render resolution (backbuffer size, e.g.
//                       1920x1080) for the engine's 6-entry resolution table —
//                       must match the device backbuffer. NOT the A/C extents.
// Idempotent: the ADD-style lists are guarded (one-shot) so calling twice is safe.
void apply_anzz1_widescreen(float aspect, float A, float B, float C,
                            unsigned long D, int render_w, int render_h);

#endif // ANZZ1_WIDESCREEN_H
