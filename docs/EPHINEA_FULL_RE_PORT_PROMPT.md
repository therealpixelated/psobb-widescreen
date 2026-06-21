# Ephinea Full Reverse-Engineering & Widescreen-Parity Port — Master Prompt

**Created:** 2026-05-28
**Owner:** therealcloudviking
**Driver tool:** `psoharness` (first-class MCP — already registered, use it for everything)
**Target client to change:** `C:/Users/u03a9/PSOBB.IO/psobb.exe` (+ `patches/pso_widescreen.asi`)
**Reference client (read-only ground truth):** `C:/Users/u03a9/EphineaPSO/PsoBB.exe` + `ephinea.dll`
**Deliverable of the work this prompt kicks off:** PSOBB.IO that is **pixel-perfect-equal to Ephinea at 16:9**, plus a **complete, documented RE of every ephinea.dll patch** (excluding ffmpeg internals — see scope).

---

## 0. How to use this document

This is the **single starting prompt** for an agent (you, next session) to drive the entire
effort. Read it top to bottom, then **start at §7 (First Moves)**. Do not trust any prior
"complete / byte-identical" claim in the repo — **verify everything from scratch** against the
live Ephinea process and against your own eyes (screenshots). The on-paper VA match is already
"100%" and the user *still sees slop*, so the paper is not the territory.

The user's exact words on the gap:

> "Overall the scaling is just sloppy compared to Ephinea even when it's working.
> Theirs is crisp and pixel perfect, ours has slop."

That sentence is the acceptance test. "92/92 VAs patched" is **not** the acceptance test.

---

## 1. Mission (what "done" means)

**Done = Parity + documented mechanism.** Both halves are required:

1. **Visual parity.** PSOBB.IO at 16:9 (1920×1080) is indistinguishable from Ephinea at 16:9
   on every screen the user can reach: title, login (Hangame + regular), ship select, **char
   select**, lobby, **dungeon gameplay**, menus, minimap/map, and **effects** (techniques,
   photon blasts, traps, weapon trails, fullscreen FX/transitions). Crisp, pixel-aligned, no
   sub-rect, no stretch, no float-off-object, no "slop."
2. **Documented mechanism.** A durable RE reference that explains *every* ephinea.dll patch
   (what VA, what it does, why, how we replicate it) — built as we go, not hand-waved.

### User-confirmed scope decisions (2026-05-28)

| Decision | Answer |
|---|---|
| Definition of done | **Parity + documented mechanism** |
| ephinea.dll RE depth | **Complete RE of ALL patches** via live-diff **and** a real dump + static/dynamic analysis. The dll is packed (VMProtect/Enigma, `.banana`) — defeat the packer enough to get a clean, analyzable image. **Exception: ffmpeg.** Do not RE ffmpeg internals, but **do document how Ephinea uses ffmpeg** (where it's called from, for what). |
| dgVoodoo / DXVK / ReShade wrapper + post-FX layer | **OUT OF SCOPE.** Do not try to match Ephinea's wrapper stack. |
| Trust existing "complete" port docs | **Verify from scratch.** Treat all prior claims as unverified. |
| psoharness MCP tooling | **Fine as-is — just use it.** (Don't go build new harness features unless something is genuinely blocking; the user does not want a tooling detour.) |
| Areas that look wrong today | **3D world / camera / FOV**, **HUD / menus / text**, **Map / minimap / screen FX**, and a **general "sloppy scaling"** across all of it. |

### Critical user insight (drives the whole approach)

> "Ephinea also supports custom DLLs for the d3dX stack, so their widescreen patch is
> **independent of the pipeline DLLs**."

**Implication:** Ephinea's widescreen is implemented **100% at the engine level inside
ephinea.dll** and works regardless of whether the d3d8/d3d9 path is native, dgVoodoo, or DXVK.
Therefore:
- Our port must **also** be fully engine-level (in our `.asi`), independent of the wrapper.
- The wrapper layer is **not** the cause of the slop and is **not** the fix. Do not chase it.
- The "slop" is an **engine-patch fidelity** problem, not a rendering-backend problem.

---

## 2. The terrain (verified facts — re-verify, don't assume)

### 2.1 The two clients
| | PSOBB.IO | Ephinea |
|---|---|---|
| Exe | `C:/Users/u03a9/PSOBB.IO/psobb.exe` (md5 `e5bdb901…`) | `C:/Users/u03a9/EphineaPSO/PsoBB.exe` (md5 `e89d53b6…`) |
| Engine base | MTethVer12513 (1.25.13) | **same engine, byte-identical in all code regions** |
| Patch delivery | Our ASIs (dinput8 loader) | Runtime, by `online.exe` (640 KB) + **`ephinea.dll` (12 MB, packed)** |
| Widescreen | `patches/pso_widescreen.asi` (our port) | inside `ephinea.dll` (the thing we're matching) |

**Ground truth:** every Ephinea behavior is a **runtime patch** over the identical engine. The
on-disk exe md5 differs only by an embedded build/resource string. So the difference between the
two clients == (the set of runtime patches Ephinea applies) minus (the set we apply).

### 2.2 ephinea.dll
- **Packed** (Enigma/VMProtect, `.banana` section), 12.05 MB. Image base observed ~`0x52000000`;
  all detour targets land in `0x52F3xxxx–0x52F8xxxx`.
- Static disasm of the packed image is blocked → **the live process is the source of truth** for
  behavior, **and** a runtime dump (post-unpack) is the source of truth for the *code*.

### 2.3 ffmpeg (scope: document usage only, do NOT RE internals)
Ephinea ships a full FFmpeg next to the exe:
`avcodec-61.dll` (73.5 MB), `avformat-61.dll`, `avutil-59.dll`, `swscale-8.dll`, `swresample-5.dll`.
→ Find **where ephinea.dll calls into these** and **what for** (almost certainly video/cutscene
playback or capture/streaming). Write one section documenting the *integration points* (import
thunks, call sites, the feature it powers). Do not disassemble libav internals.

### 2.4 Render-pipeline layer — OUT OF SCOPE (documented for awareness only)
Both clients run on **dgVoodoo2** (D3D8→D3D11). Ephinea pins `OutputAPI=d3d11_fl11_0`, ships
**DXVK** (`dxvk_d3d9.dll`) and forces `ExtraEnumeratedResolutions=1920x1080`. PSOBB.IO uses
`OutputAPI=bestavailable`, **ReShade**, and `widescreen.cfg` (`HUDScale=Auto`, MSAA/SMAA/SSAO/
CelShader/DOF/HDR). **Per the user's d3dX-independence insight, none of this is the widescreen
mechanism. Leave it alone.**

### 2.5 What the prior work already claims (TREAT AS UNVERIFIED)
- `pixelateds-psobb-mods/psobb-patches/pso_widescreen-asi/pso_widescreen.c` (~5251 lines):
  D3D8 vtable hooks (CreateDevice/SetTransform/SetViewport/DrawPrimitiveUP/DrawIndexedPrimitiveUP),
  the **92-VA HUD-anchor recipe** (`FUN_52da9280`: L1 23×W-mul, L2 17×H-mul, L3 16×W-add,
  L4 16×H-add, L5 7×H-add-raw, + 12 inline), 10/10 `FUN_52dabbd0` extras, font-atlas plan
  (592px atlas / 32px cells), cursor-warp (SetCursorPos IAT) fix.
- Docs: `pixelateds-psobb-mods/docs/EPHINEA_RE_CATALOG.md`, `EPHINEA_92VA_PATCH_LIST.md`,
  `EPHINEA_CASCADE_RPM.md`, `EPHINEA_DETOUR_RE.md`, `EPHINEA_WIDESCREEN_DECOMPILED.md`,
  `WIDESCREEN_PORT_STATUS.md`.
- **These docs already contain a leading hypothesis for the slop** (see §4). Use them as leads,
  not as gospel.

---

## 3. The formula (Ephinea's widescreen cascade — confirmed live)

From `EPHINEA_CASCADE_RPM.md` / `EPHINEA_WIDESCREEN_DECOMPILED.md` (live-captured from
ephinea.dll, 2026-05-28):

```c
// Re-runs on EVERY device reset / resolution change (this re-run cadence matters — see §4).
aspect      = display_w / display_h;          // 16:9 -> 1.77778
aspectMult  = aspect / (4.0/3.0);             // 16:9 -> 1.33333
S           = launcher_hud_scale;             // default 1.0 in the table; user S multiplied in
gameRenderW = 640.0 * S * aspectMult;         // == 480 * S * aspect
gameRenderH = 480.0 * S;
apply_92va(gameRenderW, gameRenderH, S);      // FUN_52da9280 + FUN_52dabbd0 extras
```

Aspect breakpoint table (ephinea.dll .rdata `0x52C8xxxx`): 5:4=1.25, 4:3=1.3333, 3:2=1.5,
16:10=1.6, 16:9=1.7778, 21:9=2.3333, 32:9=3.5556. Paired (aspectMult,S): 0.9375/1, 1.0/1,
1.125/1, 1.2/1, 1.3333/1, 1.75/1, 2.6667/1.

Our port reportedly computes the byte-identical `gameRenderW/H`. **If the math matches and it
still looks sloppy, the bug is in WHEN/HOW we apply it or in sites we don't cover — not the
arithmetic.** See §4.

---

## 4. Leading hypotheses for the "slop" (test these first, falsify ruthlessly)

The existing `EPHINEA_WIDESCREEN_DECOMPILED.md` already names the prime suspect. Rank-ordered:

1. **One-shot vs cascade re-run (TOP SUSPECT).** Our port writes `stock × factor` **once** at
   CreateDevice. Ephinea's cascade **re-runs on every device reset** and re-derives from live
   `(aspectMult, S)`. Any of the 92 anchors that live in `.data` the engine itself rewrites
   between resets will go **stale** in our build (correct only until the engine touches it),
   while Ephinea stays correct every frame. → **Test:** breakpoint/watch the 92 anchor VAs in
   our live process over time and across an alt-tab/resolution-change/device-reset; diff against
   Ephinea's same VAs over the same events. If ours drift and theirs don't, this is it. **Fix:**
   re-apply on device reset (hook Reset / re-run the recipe), or hook the engine's writers.
2. **Rounding / order-of-operations slop.** `640*S*aspectMult` vs `480*S*aspect` are equal in
   real numbers but **not** bit-identical in float depending on operation order and intermediate
   rounding. Pixel-snapping ("crisp") can require matching Ephinea's exact float sequence and any
   `floor/round` it applies before writing each anchor. → **Test:** read the *post-write* float
   at each of the 92 VAs in both processes and compare **bitwise** (not "≈"). Any 1-ULP diff at a
   position anchor = visible half-pixel shimmer/slop.
3. **Missing or mis-typed sites.** The 92 are the ones we *found*. Ephinea may touch more (the
   RE_CATALOG notes HUDScale>1 touches *more* float sites: 92 vs 64 in one region, 170 vs 120 in
   another, 82 vs 38 in another). At S=1 those may coincide, but verify there are no anchors we
   write as the wrong **type** (u32 vs f32) or with the wrong **factor class** (W-mul vs W-add).
4. **Sub-pixel / texel-center alignment.** "Crisp and pixel-perfect" often means Ephinea aligns
   2D/HUD quads to texel centers (the `+0.5` half-texel) and/or snaps to integer pixels after
   scaling. Check the inline bespoke sites and the font UV constants (`0x0097DB80=32/592`, etc.)
   and the DrawPrimitiveUP RHW path for half-pixel handling.
5. **Font atlas not actually swapped.** The RE_CATALOG warns: writing the 6 font constants
   (592/32) **without** shipping the matching 592px atlas samples a 256px texture at 592px coords
   = garbage/blur. If "text looks sloppy," confirm the larger atlas is actually bound on every
   text path (HUD, menu, char-screen `0x007AB554`), not just constants written.

Document the outcome of each test (confirmed / falsified / partial) in the RE reference.

---

## 5. Method (the loop you will actually run)

Everything is driven through **psoharness MCP** (`mcp__psobb__*` in-game ASI server +
host debugger on `127.0.0.1:8731`). The MCP is fine as-is — **use it, don't rebuild it.**

### 5.1 Stand up both clients
- Launch **Ephinea** via `online.exe` (it injects ephinea.dll) and **PSOBB.IO** via the
  psoharness launcher / `psobb.exe`. PSOBB.IO already loads `psobb_mcp.asi` (the in-game MCP).
  `mcp__psobb__ping` returns `{ok:false}` when no client is up — a green ping is your "ready."
- For Ephinea, use the **host-side debugger / RPM** path (attach by PID) for reads; it does not
  need the in-game ASI.

### 5.2 Static + dynamic RE of ephinea.dll (full, per user scope)
1. **Dump the unpacked image.** ephinea.dll is packed — get a clean dump:
   - Let it fully unpack at runtime, find OEP, dump the image, rebuild imports (Scylla-style),
     fix the section table (`.banana` + the real code section). The psoharness debugger
     (HW breakpoints, RPM, module map) is your primary tool; radare2 MCP is available for the
     dumped image. Save the dump under `psoharness/_ephinea_unpacked.bin` (or similar) and
     record base, OEP, and the import map.
   - **Cross-check** every static finding against live memory — the dump is for *reading code*,
     live RPM is for *confirming behavior*.
2. **Enumerate every patch** ephinea.dll applies to the engine (the runtime delta):
   - Regenerate the section-mapped live-vs-disk diff for BOTH processes
     (`_secdiff.ps1` → `_eph_diff.csv`, `_io_diff.csv`), then set-difference to get
     **Ephinea-only** runs. Prior run: 3113 Ephinea runs, 2451 Ephinea-only, 372 detours.
   - For **every** Ephinea-only run: classify (detour `E9/E8`→`0x52Fxxxxx` / data float / u32 /
     vtable-pointer-reloc / string-localization), identify the hooked engine function, read the
     detour target's behavior, and **write it down**. This is the "complete RE" deliverable.
   - **Exclude ffmpeg** internals; **include** an "ffmpeg integration" section (call sites + feature).
3. **Categorize** into: widescreen/scaling (the priority), font/text, menu/UI, position-precision
   (`+0x334`/`+0x2BC`, 49 detours — known, not widescreen), network/quest, localization strings,
   ffmpeg-integration, misc. Widescreen first.

### 5.3 Live A/B comparison (the parity engine)
- For each screen and each suspected VA: **read the same VA in both live processes** and compare.
  Bitwise for floats. Watch over time and across device resets (§4.1).
- **Screenshot both** at the same screen (`mcp__psobb__screenshot`) and eyeball + pixel-diff.
  The user can't always describe the slop — capture it. Drive navigation with
  `mcp__psobb__input_key` / `input_mouse` to reach matching screens.

### 5.4 Patch → test → verify loop
- Change `pso_widescreen.c`, rebuild the ASI (MSVC 2025; see the repo's build/deploy scripts),
  deploy to `C:/Users/u03a9/PSOBB.IO/patches/pso_widescreen.asi`, relaunch PSOBB.IO, screenshot,
  compare to Ephinea. (No hot-reload — relaunch per iteration; the user accepted current tooling.)
- For fast hypothesis tests you can `mcp__psobb__write_memory` the candidate values into the live
  PSOBB.IO process and eyeball **before** baking them into the ASI — this is the fast inner loop.
- **Acceptance per screen:** side-by-side screenshot is pixel-indistinguishable from Ephinea.

---

## 6. Concrete anchor data (starting coordinates — re-verify each)

**ephinea.dll:** image base ~`0x52000000`; detours → `0x52F3xxxx–0x52F8xxxx`; cascade tables
~`0x52C8xxxx`; widescreen apply `FUN_52da9280`, extras `FUN_52dabbd0`, deanchor `FUN_52db2240`.

**Engine VAs (byte-identical in both exes):**
| VA | Role |
|---|---|
| `0x0082EF4C`, `0x0082EC74` | 3D projection aspect immediate — **16:9 (1.77778) in BOTH**; do NOT revert to 4:3 |
| `0x00892565` | perspective builder (`m00=cot(fovY/2)/aspect`, `m11=cot(fovY/2)`, RH m23=-1) — NOT hooked in either |
| `0x0082EEA0`, family `0x0082EC58/EE4C/EE78/EF30/EF78/F0D7/F6E4/F733` | projection builders (>=6, per-context) |
| `0x00ACBF40` | live 4×4 projection matrix (sample both, both screens) |
| `0x00ACBE7C/80`, `0x00AF0350/54` | engine W/H storage slots |
| `0x009006F4` | engine resolution table (6× u32, must == backbuffer) |
| `0x00ACC0E8/EC` | live 2D design scale anchors |
| font block `0x0097DB60..8C` | UV/metric constants; Ephinea: cell=32, atlas=592, step=32/592=0.054054 |
| `0x007AB554` → `0x52F75AE0` | char-screen glyph render (Ephinea binds 592px font here) |
| `0x006F4CC4` → `0x52F60150` | title/splash render (Ephinea branded title) |
| HUDScale denoms `0x0098A4A8`, `0x004011D2`, `0x00972000`, `0x006F4922`, `0x009A3840` | scale with S |

**Our port:** `pso_widescreen.c` 92-VA recipe (L1–L5 + 12 inline), inline.H scratch float
`g_ws_inline_h_scratch`, FUN_52dabbd0 extras, SetCursorPos IAT warp fix. Engine W/H redirect +
res table + BB-line NOPs (`0x719F96`, `0x719FD4`).

---

## 7. First Moves (do these, in order, next session)

1. **Confirm tooling is live.** Launch PSOBB.IO; `mcp__psobb__ping` until green; `mcp__psobb__modules`
   to confirm base + our ASI loaded. Launch Ephinea; confirm host-debugger RPM can read its PID.
2. **Capture the baseline divergence visually.** Navigate both to the **same** screens
   (title → login → ship → char-select → lobby → a dungeon room) and screenshot each. Build a
   side-by-side gallery. This is the "slop" made concrete — refer back to it as the test.
3. **Test hypothesis §4.1 (one-shot vs cascade re-run) first.** Read all 92 anchor VAs in both
   live processes; watch them across a device reset / alt-tab / resolution change. If ours drift,
   implement re-apply-on-reset and re-test. This is the single most likely root cause.
4. **Then §4.2 (bitwise float compare)** of every post-write anchor. Fix any 1-ULP / rounding /
   op-order diffs and any pixel-snap Ephinea does that we don't.
5. **Full ephinea.dll RE in parallel** (§5.2): dump+unpack, enumerate every Ephinea-only patch,
   classify, document. Widescreen/scaling category first; ffmpeg = usage-only.
6. **Iterate** the patch→build→deploy→screenshot→compare loop (§5.4) screen by screen until each
   is pixel-indistinguishable from Ephinea.

---

## 8. Deliverables this effort must produce

1. **PSOBB.IO at 16:9 that is pixel-equal to Ephinea** on every reachable screen + effects.
2. **`EPHINEA_DLL_COMPLETE_RE.md`** — every ephinea.dll patch documented (VA, mechanism, why,
   how replicated / why not), categorized, with the ffmpeg-integration section. Built from the
   unpacked dump + live diff, cross-verified.
3. **An unpacked, analyzable ephinea.dll dump** (`_ephinea_unpacked.bin`) with base/OEP/import map.
4. **Updated `WIDESCREEN_PORT_STATUS.md`** reflecting *verified-from-scratch* state (not the prior
   "byte-identical complete" claim) — including the resolved root cause of the slop.
5. **A/B screenshot gallery** proving parity per screen.

---

## 9. Guardrails

- **Do not** chase the dgVoodoo/DXVK/ReShade layer — user ruled it out; Ephinea's widescreen is
  d3dX-independent (engine-level).
- **Do not** trust prior "complete/byte-identical" docs — verify from scratch.
- **Do not** revert the 3D projection aspect to 4:3 — Ephinea keeps it at 16:9 too (live-verified).
- **Do not** rebuild psoharness tooling unless genuinely blocked — use it as-is.
- **Do** make the user's eyes the acceptance test: "crisp, pixel-perfect, no slop," proven by
  side-by-side screenshots, not by VA-count.
- **Path hygiene (this machine):** the real username is the literal ASCII `u03a9`. Never put a
  backslash before `u03a9` (it escape-decodes to Ω). Use forward slashes (`C:/Users/u03a9/...`)
  or the Bash tool.

---

## Appendix — source docs to mine (leads, not gospel)
- `psoharness/EPHINEA_FULL_RE_PORT_PROMPT.md` (this file)
- `psoharness/HARNESS_ARCHITECTURE.md`, `GRAPHICS_PIPELINE.md`, `DISPLAY_FINAL.md`
- `pixelateds-psobb-mods/docs/EPHINEA_RE_CATALOG.md` (the 3113/1272 diff, HUDScale, font block)
- `pixelateds-psobb-mods/docs/EPHINEA_92VA_PATCH_LIST.md` (the recipe spec)
- `pixelateds-psobb-mods/docs/EPHINEA_CASCADE_RPM.md` (breakpoints + scale floats, live)
- `pixelateds-psobb-mods/docs/EPHINEA_DETOUR_RE.md` (engine→dll detour map)
- `pixelateds-psobb-mods/docs/EPHINEA_WIDESCREEN_DECOMPILED.md` (cascade pseudocode + slop hypothesis)
- `pixelateds-psobb-mods/docs/WIDESCREEN_PORT_STATUS.md` (prior "complete" claim — verify)
- `pixelateds-psobb-mods/psobb-patches/pso_widescreen-asi/pso_widescreen.c` (the port itself)
- Diff artifacts: `psoharness/_eph_diff.csv`, `_io_diff.csv`; scripts `_secdiff.ps1`, `_rpm2.ps1`, `_rpm4.ps1`
