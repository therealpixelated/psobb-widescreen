# PSOBB ReShade package

Adds six post-processing effects to Phantasy Star Online: Blue Burst via
[ReShade](https://reshade.me) — **MSAA, SMAA, SSAO, Cel Shading, Depth of Field,
HDR Tone Mapping** — matching the anzz1 widescreen look, **without baking
anything into the game executable or the widescreen ASI**. Effects are toggled
through the same six `widescreen.cfg` flags the ASI already reads.

ReShade composites on top of the rendered frame, so this works alongside the
existing widescreen ASI (HudScale, layout) untouched. `<gamedir>\patches` is
never modified.

---

## What this is, in one breath

ReShade is loaded by sitting next to `psobb.exe` under a DLL name the game's
graphics wrapper already imports, then it runs `.fx` shaders each frame. PSOBB
ships a D3D8 game with a wrapper that upgrades it to D3D9 or D3D11. We detect
which wrapper you have and install ReShade as the matching proxy DLL.

| Your `d3d8.dll` wrapper | Chain | ReShade installs as | Script |
|---|---|---|---|
| **crosire d3d8to9** (→ D3D9) | A | `d3d9.dll` | `install/install_crosire.ps1` |
| **dgVoodoo2** (→ host D3D11) | B | `dxgi.dll` | `install/install_dgvoodoo.ps1` |

Run `install/detect_wrapper.ps1` first if unsure — it tells you which one you have.

---

## Prerequisites

- A working PSOBB install with **either** crosire d3d8to9 **or** dgVoodoo2 already
  present as `d3d8.dll` (the widescreen package installs one of these).
- **PowerShell 5.1+ or PowerShell 7** (the scripts are cross-edition).
- **7-Zip on PATH** (`7z.exe`) so the script can extract the ReShade DLL from the
  setup archive without running the GUI installer. If you don't have it, the
  script tells you to run the ReShade setup once manually; everything else still
  applies idempotently.
- Internet access for the one-time ReShade download (and SMAA fetch).
- **Chain A only:** `d3dx9_43.dll` must be resolvable (DirectX End-User Runtime),
  or crosire's `d3d8.dll` fails at startup. The script checks and warns.

---

## How to run

From this `reshade/` folder. The scripts default to the conventional PSOBB.IO
location; pass `-GameDir` for anything else.

```powershell
# 0. (optional) confirm which wrapper you have
pwsh -File .\install\detect_wrapper.ps1

# 1a. dgVoodoo2 base (this machine's default)
pwsh -File .\install\install_dgvoodoo.ps1

# 1b. crosire d3d8to9 base
pwsh -File .\install\install_crosire.ps1

# Common switches:
#   -GameDir 'D:\Games\PSOBB'   point at a non-default install
#   -DryRun                     show the detection + plan, write nothing
#   -NoDepth                    ship the color-only preset (drop SSAO + DOF)
#   -Force                      re-download / re-extract ReShade
#   -DllName d3d11.dll          (dgVoodoo) use d3d11.dll if dxgi.dll is taken
```

Each script:
1. **Refuses** if the wrong base wrapper is present (crosire script won't touch a
   dgVoodoo install and vice-versa).
2. Checks for an ASI-loader proxy-DLL name collision and refuses if it would clash.
3. Reads the six `widescreen.cfg` flags.
4. Downloads ReShade from reshade.me (pinned version) — **printing the exact URL,
   destination, and SHA256** — and extracts only the DLL.
5. Copies the bundled `PSO_*` shaders + headers + `DisplayDepth.fx`, and fetches
   the redistributable SMAA (BSD-3) if SMAA is enabled.
6. Generates the preset (filtering effects by flag) and `ReShade.ini`.

Everything is **idempotent** (re-running changes nothing if already current) and
**non-destructive** (any file it would overwrite is first copied to
`<name>.reshade-bak`).

---

## The six effects ↔ `widescreen.cfg`

| `widescreen.cfg` | ReShade technique | Depth needed? | When flag = 0 |
|---|---|---|---|
| `MSAA` | *(wrapper setting, not a `.fx`)* | n/a | wrapper AA forced off |
| `SMAA` | `SMAA` | no | omitted from preset |
| `SSAO` | `PSO_SSAO` | **yes** | omitted |
| `CelShader` | `PSO_CelShader` | no | omitted |
| `DOF` | `PSO_DOF` | **yes** | omitted |
| `HDR` | `PSO_HDRToneMap` | no | omitted |

**MSAA is not a shader.** It's an anti-aliasing mode in the wrapper. Critically,
**wrapper MSAA disables ReShade's depth buffer**, which would kill SSAO + DOF. So:

- **Chain A (crosire):** crosire doesn't force MSAA; AA comes from **SMAA**. Don't
  enable D3D9 MSAA.
- **Chain B (dgVoodoo):** the installer sets dgVoodoo `Antialiasing = off` (a
  backup of your `dgVoodoo.conf` is made first). AA comes from **SMAA**. If you
  want wrapper AA, use **SSAA** (depth-safe), never MSAA. Use `-NoAAEdit` to keep
  your conf as-is.

The generated preset's `Techniques=` line includes only the enabled effects, in
the fixed anzz1 order: SMAA → SSAO → Cel → DOF → HDR.

---

## Depth caveat — the acceptance gate (read this before trusting SSAO/DOF)

SSAO and DOF need a working depth buffer. SMAA, Cel and HDR are color-only and
work on any chain. The depth path under **dgVoodoo (D3D8→D3D11→ReShade)** is the
historically fragile one; the crosire (D3D9) path is better-tested.

After installing, **verify depth before relying on SSAO/DOF:**

1. Launch PSOBB, press **Home** to open the ReShade overlay.
2. Confirm the renderer reads **Direct3D 9** (Chain A) or **Direct3D 11** (Chain B),
   and that the Generic Depth buffer list is populated.
3. Enable **DisplayDepth** in the technique list. You want a smooth grayscale
   gradient — **near = dark, far = light**.
   - Correct gradient → depth works → SSAO/DOF are valid.
   - Flat gray / pure white/black / colored noise / static garbage → depth is
     broken on your chain. Re-run the installer with **`-NoDepth`** to ship the
     color-only preset (`PSOBBIO_no_depth.ini`).
4. If the gradient is inverted or flat, tune the depth defines via the overlay's
   "Edit global preprocessor definitions": start with
   `RESHADE_DEPTH_INPUT_IS_REVERSED`, then lower
   `RESHADE_DEPTH_LINEARIZATION_FAR_PLANE` (default 1000).

Note: anzz1 injects SSAO/DOF *before* the 2D HUD via a render-target hook; ReShade
can only run on the final frame, so SSAO/DOF will lightly affect the HUD. This is
a known, minor divergence from the anzz1 look.

---

## Repository layout

```
reshade/
  README.md
  install/
    _common.ps1            shared helpers (config parse, detect, fetch, preset gen)
    detect_wrapper.ps1     read-only: report base wrapper + collisions + flags
    install_crosire.ps1    Chain A installer (ReShade as d3d9.dll)
    install_dgvoodoo.ps1   Chain B installer (ReShade as dxgi.dll)
    fetch_smaa.ps1         fetch ReShade SMAA (BSD-3) + textures (auto-called)
    fetch_mxao.ps1         OPT-IN qUINT MXAO download (never committed)
  shaders/Shaders/
    PSO_SSAO.fx  PSO_DOF.fx  PSO_CelShader.fx  PSO_HDRToneMap.fx
    DisplayDepth.fx  ReShade.fxh  ReShadeUI.fxh
  shaders/Textures/        (SMAA textures land in <gamedir>, fetched at install)
  presets/
    PSOBBIO.ini            curated anzz1-matched preset (base for generation)
    PSOBBIO_no_depth.ini   color-only fallback (depth gate failed / -NoDepth)
  config/
    ReShade.ini.template     filled with the preset path at install
    dgVoodoo.conf.template   reference (Chain B); installer only patches keys
  LICENSES/
    CREDITS-anzz1-lineage.md  BSD3-ReShade.txt  MIT-SMAA.txt
```

---

## Licensing

- **PSO_SSAO / PSO_DOF:** free techniques (Arkano22/martinsh; WrinklyNinja/Sodaboy),
  attributed in each shader header.
- **PSO_CelShader / PSO_HDRToneMap:** ports of Asmodean's GSFx suite, which has
  **no formal OSS license** — bundling these is the repo owner's call. MIT
  fallbacks: SweetFX `Cartoon.fx` (cel) and `Tonemap.fx` + `LevelsPlus.fx` (HDR).
- **SMAA / ReShade / DisplayDepth upstream:** BSD-3 / MIT, fetched at install,
  never committed.
- **qUINT MXAO:** "All rights reserved", **never committed**; `fetch_mxao.ps1`
  downloads it only on explicit consent.

See `LICENSES/CREDITS-anzz1-lineage.md`.

---

## Uninstall

1. Delete the ReShade proxy DLL: `<gamedir>\d3d9.dll` (Chain A) or
   `<gamedir>\dxgi.dll` (Chain B).
2. Delete `<gamedir>\ReShade.ini`, `<gamedir>\ReShade.log`, and
   `<gamedir>\reshade-shaders\`.
3. Restore any `*.reshade-bak` files the installer made (notably
   `dgVoodoo.conf.reshade-bak` → `dgVoodoo.conf` for Chain B) by removing the
   `.reshade-bak` suffix.

The base wrapper (`d3d8.dll`) and the widescreen ASI are untouched by install or
uninstall.
