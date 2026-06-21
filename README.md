# psobb-widescreen

A faithful **widescreen + HudScale** mod for *Phantasy Star Online: Blue Burst*
(`PsoBB.exe`, build MTethVer12513 / "59NL"), shipped as a single drop-in ASI.
The goal is to make the whole front-end and HUD fill a 16:9 (or wider) canvas at
any HudScale **the way Ephinea does it** — not a pile of per-element pokes, but the
same anchor/scale model the official client uses, reverse-engineered and ported.

This repo is open so others can read the RE, check the math, and help close the
remaining gaps. PRs and issues welcome.

---

## How it works (one paragraph)

PSOBB lays out its entire 2D UI in a **640×480 design space** and multiplies it
through a single 2D affine at draw time. Widescreen = (1) widen the design size to
`design_w = 853.33 × HudScale`, `design_h = 480 × HudScale`; (2) re-anchor each UI
element to the correct edge/center of the wider canvas; (3) keep screen-space
effects (photon/vignette/etc.) from inheriting the wide affine. Ephinea does this
with a **3-stage runtime cascade** (config → render-size → bulk anchor apply) plus a
**4-handler per-draw detour layer**. We reproduce the same *result* with a
**single-boot bake** of the design size + anchor table and the same four per-draw
detours — installed once, **no per-frame rebakes**.

The complete reverse-engineering of Ephinea's implementation — every one of the
**956 hooks** it installs, named and classified — is in
[`docs/EPHINEA_HOOKS_FULL_RE.md`](docs/EPHINEA_HOOKS_FULL_RE.md). That document is
the spec this mod is ported from.

---

## Repository layout

```
asi/                  # the mod source — the pso_widescreen ASI (C + inline x86)
                      #   build.bat -> pso_widescreen.asi ; pso_widescreen.ini config
docs/                 # the reverse-engineering: how Ephinea's widescreen works
  EPHINEA_HOOKS_FULL_RE.md      # ⭐ full RE of all 956 Ephinea->PsoBB hooks (the port spec)
  hooks_re/                     # the 956-hook inventory table, cascade deep-dives, raw data
  EPHINEA_DLL_COMPLETE_RE.md    # earlier complete-RE writeup of the DLL
  EPHINEA_FULL_RE_PORT_PROMPT.md
  LISTWINDOW_YANCHOR_EPHINEA_RE.md   # the in-game ListWindow bottom-anchor (FUN_52d8cb90) port
  WIDESCREEN_PORT_STATUS_VERIFIED.md
  qol-deepdive/                 # deep-dive chapters: widescreen math, render pipeline, char-create
ephinea-reference/    # llama-bob's clean-C reconstruction of Ephinea's widescreen engine
  src/                #   stage_a/b/c, the 92-VA apply, the 4 per-draw detours, etc.
  docs/PATCHES_OVERVIEW.md
  reference/          #   raw Ghidra decompile of a near-identical build
```

## The RE, in short

Ephinea's widescreen is a runtime cascade, not static patched constants:

| Stage | Function | Job |
|---|---|---|
| A | `FUN_52da9930` | load display size + SSAA from config, tail-call B |
| B | `FUN_52dabbd0` | `gameRenderW = (640·S·aspectMult)/(4/3)`, `gameRenderH = 480·S` (7-level aspect ladder) |
| C | `FUN_52da7ff0` | bulk-apply render size to ~700 UI-anchor floats (6 loops) + call the 92-VA leaf |
| — | `FUN_52da9280` | the "92-VA" arithmetic apply over the front-end / char-select anchor block |

Plus 4 per-draw detours (the only visually load-bearing of ~350): `FUN_52da6e50`
(2D-vertex aspect-correct + recenter), `FUN_52dc4f60` (HUD/text glyph scaler),
`FUN_52d8cb90` (in-game ListWindow bottom-anchor), `FUN_52db2240` (deanchor pin).
It re-runs **only on config reload** — there is no `IDirect3DDevice::Reset` hook and
no per-frame work. Full details, formulas, and a per-hook table with adversarial
verdicts are in [`docs/`](docs/).

## Build

The ASI is built with the MSVC toolchain (32-bit). From `asi/`:

```
build.bat
```

This produces `pso_widescreen.asi`. Drop it (plus `pso_widescreen.ini`) into the
client's ASI loader directory (e.g. `<client>/patches/`). Widescreen auto-enables
from the display aspect; `HUDScale` in the ini sets the UI scale.

## Credits & references

- **anzz1** — the original PSOBB widescreen anchor tables this builds on.
- **llama-bob** — the clean-C reconstruction of Ephinea's widescreen engine in
  [`ephinea-reference/`](ephinea-reference/) (upstream:
  `github.com/llama-bob/psobb-ephinea-re`).
- **Trinity DLL** — the same-build static widescreen patch set used to name and
  cross-check every anchor VA and alignment mode.
- The hook RE is mirrored in the dedicated
  [`therealpixelated/psobb-ephinea-re`](https://github.com/therealpixelated/psobb-ephinea-re)
  repo (the Ephinea-DLL-specific reverse-engineering home).

## Legal / ethics

*Phantasy Star Online* and its assets are © SEGA. The Ephinea client is the property
of the Ephinea project. This repository is reverse-engineering for interoperability,
study, and education only, and ships **no** copyrighted game or client binaries.
