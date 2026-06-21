# Ephinea.dll — Complete Reverse-Engineering of the Widescreen Engine Patch Set

**Status:** PHASE 1, fully static. Verified from on-disk artifacts only (no live process). All prior
"complete / byte-identical" claims were treated as UNVERIFIED and re-checked; every load-bearing
fact below was independently reproduced (and adversarially re-verified) from `_ephinea_decompiled.c`,
`_ephinea_hooks.txt`, `_ephinea_delta.csv`, `_eph_diff.csv`, `_io_diff.csv`, `_ephinea_dump.bin`, and
radare2 disassembly of `PsoBB.exe` / `psobb.exe`.

Path hygiene note: the real username is the literal 7-char ASCII string `u03a9`; all paths use forward
slashes (`C:/Users/u03a9/...`).

Phase-1 caveats that bound every numeric claim:
- The dll `.data`/`.rdata` at `0x5314xxxx`–`0x5318xxxx` is **outside** the 1.5 MB code dump. Every
  `_DAT_5317xxxx`/`_DAT_5318xxxx` constant (640, 480, 4/3, 2.0, 1.0, 100, the breakpoint table, the
  aspectMult table) is **referenced-only, never assigned** in the decompile. Formula *structure* is
  statically verified; the literal *values* are trusted from `EPHINEA_CASCADE_RPM.md` and are
  RPM-only / unconfirmable in Phase 1.
- Two cascade functions (`FUN_52da9930`, `FUN_52da7ff0`) have **zero callers and zero data-refs** in
  the entire 4.9 MB decompile, and there is **no visible IDirect3DDevice Reset hook**. Re-run on
  config reload is *proven* (tail-call chain); re-run on D3D Reset is *plausible but unconfirmed*.

---

## 1. Executive Summary

**Verified mechanism (one paragraph).** Ephinea's widescreen is **not** a static set of patched 4:3
constants and it is **not** a single "92-VA" function. It is a **three-stage runtime cascade** plus a
**runtime per-draw detour layer**. Stage A (`FUN_52da9930`) reads `displayW`/`displayH` from the
registry or the resolution table and applies SSAA, then tail-calls Stage B. Stage B
(`FUN_52dabbd0`) computes `gameRenderW = _DAT_5318deec = (640 * S * aspectMult) / (4/3)` and
`gameRenderH = _DAT_5318d124 = 480 * S` (where `S` is the HUDScale/widescreen factor), choosing
`aspectMult` from a 7-level aspect-breakpoint ladder, plus 10 extra `*= S` writes. Stage C
(`FUN_52da7ff0`) is the **real bulk apply**: ~80 scalar writes and six bulk loops
(122/90/35/28/235/12 iterations) over the engine's UI-anchor `.data`, and only *then* does it call
`FUN_52da9280` (the doc's "92-VA", really **97 arithmetic writes across 93 distinct VAs** in five
factor classes L1–L5 + 11 inline + 2 non-arith). On top of this, ephinea.dll installs **~350 runtime
detours** (two installer families) into the engine `.text`, of which only a handful are widescreen
(a 2D-vertex aspect-correct+recenter transform `FUN_52da6e50`, a HUD-quad design-anchor scaler
`FUN_52dc4f60`, a UI edge-reanchor `FUN_52d8cb90`, and the design-scale deanchor pin `FUN_52db2240`);
the overwhelming majority are localization, local co-op / network, input, and camera tuning —
**not visual**.

**Leading slop root cause (resolved).** The acceptance failure ("not pixel-perfect off-16:9") is the
port stacking **three contradictory apply layers** and running them **once** at `CreateDevice`:
(1) the per-aspect arithmetic cascade (L1–L5 + inline + WMul/WAddExtra); (2) **verbatim-duplicated**
diff tables (`kEphineaWMulExtra` 96 entries/70 unique, `kEphineaWAddExtra` 57/31) that double-apply
26+26 VAs (giving `L1mul^2 ≈ 1.78` instead of `1.333`, and `+213` instead of `+106`); and (3) a
**hardcoded 16:9 / S=1.0 absolute snapshot** `kEphineaDataTable` (337 entries, `0x44555556` = 853.333
everywhere) applied **LAST with no aspect/scale guard**. The W-axis VAs (all 70 WMul, all 31 WAdd, all
16 L3, 12/23 L1) are **clobbered** by the frozen data table; the H-axis (L2 0/17, L4 0/16, L5 0/7)
**survives**. At exactly 16:9 @ S=1.0 all three layers coincidentally agree, masking the bug; at *any*
other aspect or `hud_scale != 1.0` the frozen data table fights the cascade and freezes half the
anchors at 16:9 while the rest scale → **visible slop**. Compounded by: one-shot apply with no Reset /
config-reload re-derive (anchors mutated in place via `*=`/`+=`, **not idempotent**), and L3/L4
**undercount** (port has 16+16; truth is 19+20). **Adversarial correction:** the WMul/WAdd
double-apply is a real defect but has **zero observable effect** (100% of those VAs are subsequently
overwritten by the absolute data table), so the dominant defect is the **frozen 16:9 data-table
clobber**, not the duplication. The `inline.A/B/M` "W-add written as multiply" sub-claim is **not
supported** — those inline VAs are genuinely multiplies and the port codes them correctly.

**Headline fix:** delete the 337-entry absolute 16:9 data table; de-dup the WMul/WAdd tables; collapse
to **one ordered arithmetic pass** keyed on live `W/H/S`, each VA written exactly once with its
correct factor class (§4); add the missing L3/L4 VAs; and **hook Reset (vtable slot 14)** to re-run an
**idempotent** pass from absolute stock baselines on every device (re)create / resolution change.

---

## 2. The Verified Cascade

### Functions (decompile addresses confirmed)

| Function | Addr | Role |
|---|---|---|
| `FUN_52da9930` | `0x52DA9930` | **Stage A** — registry/config + resolution-table loader. Reads `displayW=_DAT_5318d210`, `displayH=_DAT_5318dcdc` from registry (resx/resy) or `0x53146240 + idx*8`; if SSAA `_DAT_5387b938 != 0` → W,H ×= SSAA. Tail-calls Stage B at `0x52DAB9BB` (right after `RegCloseKey`). |
| `FUN_52dabbd0` | `0x52DABBD0` | **Stage B** — render-size calc + 7-level aspect-breakpoint bucket select. Writes `gameRenderW`/`gameRenderH` + 10 `*=S` extras. |
| `FUN_52da7ff0` | `0x52DA7FF0` | **Stage C** — the REAL bulk anchor apply: ~80 scalars + six bulk loops, then calls `FUN_52da9280` at `0x52DA9062`. |
| `FUN_52da9280` | `0x52DA9280` | The doc's "92-VA": 97 arithmetic writes / 93 distinct VAs, L1–L5 + 11 inline + 2 non-arith. |
| `FUN_52dac110` | `0x52DAC110` | Per-device vertex-buffer anchor patch (guarded `renderH != 480`). **Body verified; detour wiring UNVERIFIED — see correction below.** |
| `FUN_52db2240` | `0x52DB2240` | Deanchor: pins `0xACC0E8/EC` to `_DAT_53175e64` around engine draw `0x82B158`. Hooked at `0x4A9C0C / 0x4A9D28 / 0x5474D4`. |

### Stage B formula (decompile `_ephinea_decompiled.c` lines 133394–133492)

```
aspect = displayW / displayH                                  // 133402
// 7-level nested-<= breakpoint ladder picks aspectMult=_DAT_5318f038 and S=local_8:
//   if(_DAT_53175ee8<=aspect){ if(f00<=aspect){ if(f10<=aspect){ if(f18<=aspect){
//      if(f30<=aspect){ if(f40<=aspect){ if(f50<=aspect) ... }}}}}}      // 133403-133409
// widescreen OFF (_DAT_53857a5c==0): S = local_8 = _DAT_53175e64 (==1.0)         // 133463-467
// widescreen ON:  if HUD_SCALE(_DAT_5382fe9c)!=0: S = HUD_SCALE / _DAT_53175fc0  // (==/100) 133471
gameRenderW = _DAT_5318deec = (_DAT_53176128 * S * _DAT_5318f038) / _DAT_53175e8c // =(640*S*aspectMult)/(4/3)  133477
gameRenderH = _DAT_5318d124 = _DAT_53176120 * S                                   // =480*S                     133478
// +10 extras (133476-133492): 0x0097A910 = dVar3/100;  *=S on 0x0096E114, 0x006F49FD,
//   0x006F4A57, 0x0096E168/16C/170/174/178/17C;  _DAT_5387b918 = S.
```

This is **byte-equivalent** to the port's `640*S*(aspect/(4/3))` form (= `480*S*aspect`) **only under
the live constants** `_DAT_53176128=640`, `_DAT_53176120=480`, `_DAT_53175e8c=4/3`. Structure verified;
equality rests on RPM-only constants.

### Stage C / `FUN_52da9280` op-order (the load-bearing recipe — `_ephinea_decompiled.c:131837` and `:132130`)

`FUN_52da7ff0` runs six bulk loops `0x7a/0x5a/0x23/0x1c/0xeb/0xc` = **122 / 90 / 35 / 28 / 235 / 12**
iterations over engine globals (`0x5318F040`×122=renderW, `0x5318E6E0`×90=renderH,
`0x5318E650`×35 `+=(renderW-640)/2`, `0x5318F2A0`×28 `+=(renderW-640)`, `0x5318E9C8`×235
`+=(renderH-480)`, `0x5318EF00`×12) **plus** ~80 scalar writes (lines 131885–132121), **then** calls
`FUN_52da9280` at `0x52DA9062`, and **keeps writing afterward**. *A port that implements only the
92-VA function will be visibly wrong* — this is the single biggest doc error, and it is **confirmed**.

`FUN_52da9280` writes, in order:

| Pass | Count | Factor | Formula |
|---|---|---|---|
| **L1** W-mul | 23 | `renderW/640` | `*x = (renderW/640) * x` |
| **L2** H-mul | 17 | `renderH/480` | `*x = (renderH/480) * x` |
| **L3** W-add | **19** | `(renderW-640)/2` | `*x += (renderW-640) / _DAT_53175f20`  (`f20=2`) |
| **L4** H-add | **20** | `(renderH-480)/2` | `*x += (renderH-480) / _DAT_53175f20` |
| **L5** H-add-raw | 7 | `(renderH-480)` | `*x += (renderH-480)`  **NO /2** |
| inline | 11 | mixed | `(displayW/renderW)*x`, `(displayH/renderH)*x`, etc. |
| non-arith | 2 | — | `0x40D04E` ptr-store `=&DAT_5318e980`; `FUN_52dc3460(0x40D305, 0xB)` 11-byte code patch |

**Doc discrepancies confirmed against the decompile** (these supersede `EPHINEA_92VA_PATCH_LIST.md`):
- **L3 is 19, not 16** — doc omits `0x008FA1D8`, `0x008F9F18`, `0x008F9F20` (loop bound
  `local_100 → local_b4` includes `local_100[0x10]` + scalars `local_bc`/`local_b8`).
- **L4 is 20, not 16** — doc omits `0x008FA1D0`, `0x008FA1AC`, `0x008F9F1C`, `0x008F9F24`.
- L3/L4 **divide the delta by 2** (`_DAT_53175f20`); L5 is **raw** (no /2). The doc's L3/L4 formulas
  omitted the `/2`. (Port was later corrected to `*0.5f` at `pso_widescreen.c:3354`; doc not updated.)
- inline is **11 arithmetic** writes, not 12 — the doc folded in the non-arithmetic `0x40D04E`
  pointer-redirect and `0x40D305` code patch.

### Rerun-on-reset behavior

- **PROVEN:** Stage A (`FUN_52da9930`) tail-calls Stage B (`FUN_52dabbd0`) at `0x52DAB9BB` after
  `RegCloseKey`, and Stage A re-reads `displayW`/`displayH` fresh each call → **any config /
  resolution re-apply recomputes `renderW`/`renderH`**, and everything downstream is re-derived from
  live `displayW`/`displayH` with **no baked constants**.
- **CORRECTION (adversarial):** `reruns_on_device_reset=true` overstates Phase-1 certainty. Stage A
  and Stage C each appear **only as their own definitions** in the 4.9 MB decompile — zero call sites,
  zero data references — and there is **no visible `IDirect3DDevice9::Reset` hook**. The mechanism is
  proven **config-reload-driven**; a D3D-Reset rerun is **plausible but UNCONFIRMED** in Phase 1.

---

## 3. Complete Patch Inventory (the 2451-row delta)

`_ephinea_delta.csv` = **2451 rows = 1978 `eph_only` + 473 `differ`**. Sections: **1709 .text**,
**332 .data**, **404 unsectioned**, **6 .pseudo**. Two complementary capture systems exist and **both
are needed for parity**: the **delta CSV** (engine-VA static byte diffs) and **`_ephinea_hooks.txt`**
(120 site lines / 74 handlers, the runtime detour layer). They overlap by adjacency, not by VA:
0/119 hook VAs equal a delta VA, but 108/119 are within 16 bytes of one. The hooks file captures the
`0x52D8–0x52DE` trampoline/dispatcher handlers; the delta captures the `0x52F3–0x52F8` leaf handlers.

> **Two installer families** (not one): `FUN_52dc3240(handler, VA, 5)` = the 5-byte E9/E8 detours in
> the 120-line hooks file; `FUN_52dc3290(handler, VA, len)` = ~187 variable-length (5/6/8/10/0xB/0x1E)
> inline patches. True install surface ≈ **350**, not the 120 or the 371 either prior doc named.

### Category breakdown

The most reliable, **load-bearing** count is the .data/unsectioned analysis: of 736 .data+unsectioned
rows, the **actionable** number is **`eph_only = 430` exactly** (VAs Ephinea writes that the IO port
never touches). The raw `IoLive != EphLive` count (736/736) is **meaningless** — it is dominated by
image-base relocations (constant delta `-0xAD90000`) and 1-LSB float rounding. Categories below order
widescreen/scaling FIRST.

#### A. Widescreen / scaling — **the parity surface**

**Static layout anchors (.data + unsectioned f32):** ~51 fully-formed f32 scalar anchors implementing
**three transform classes** applied to the 4:3 (640×480) layout to reach 16:9 (853.33×480):
- **W-mul ×1.3333** (origin-anchored widths/scales): e.g. `0x008F9EC0` 100→133.333, `0x008FA124`
  410→546.667.
- **W-add +106.667** = half-letterbox `(853.33-640)/2` (X positions): e.g. `0x008FA0D8` 459→565.667,
  `0x008FADC8` 64→170.667. The constant itself lives at `0x00972048`.
- **W-add +213.333** = full width delta `853.33-640` (right-edge-anchored X): e.g. `0x008FA528`
  426→639.333, `0x0096F2B8` 400→613.333. The constant itself lives at `0x00978454`.

The virtual width **853.333** (= 640×1.3333) is present at `0x009B8D08` and `0x009B8D50` in **both**
clients, confirming all three factors derive from one design width. **PARITY VERDICT:** **zero** float
anchors genuinely diverge between Eph and IO — every `differ`-class 4-byte float decodes to the **same**
value in both clients; the byte "difference" is a 1-LSB mantissa rounding artifact (e.g. `433faaac` vs
`433faaa8`, both = 191.667). So the IO port already reproduces all *decodable* float anchors that it
writes — **the real gap is the 430 `eph_only` patches it never touches**, ≈16 `eph_only` f32 layout
anchors + the detour installs.

**Runtime widescreen detour handlers** (only 4 of 74 handlers are visual — see §3 detour buckets and
`_re/detours_*.md`):
- `FUN_52da6e50` @ site **`0x67C4ED`** (`detours_4.md`) — **core 2D-vertex aspect-correct + recenter**:
  computes `width/height` vs reference aspect, picks letterbox-vertical vs pillarbox-horizontal,
  uniformly scales each `(x,y)` vertex and re-centers on the long axis, then calls original draw
  `0x82B558`. **Must be matched for pixel parity.**
- `FUN_52dc4f60` @ site **`0x78A300`** (`detours_5.md`) — **HUD/UI quad design-anchor scaler**: reads
  `0xACC0E8/EC` via `FUN_52dd2610`, multiplies by master scale `_DAT_53175e64`, conditionally applies
  `_DAT_5318e34c`, forwards a scale ptr to quad renderer `FUN_52dc5b30`. **Two guard conditions
  (`scale<w && scale<h`, `_DAT_53175ec8 < _DAT_5318e34c`) — precedence is easy to get wrong.**
- `FUN_52d8cb90` @ site **`0x73FF92`** (`detours_3.md`) — **UI edge re-anchor**: when widescreen flag
  `0x00A95EDC != 0`, recomputes element Y = `(gameRenderH - 480) + offset` and clamps to live screen
  bounds, so HUD/menu elements track the widescreen viewport edge.
- `FUN_52db2240` @ sites **`0x4A9C0C / 0x4A9D28 / 0x5474D4`** (`detours_1.md`) — **deanchor pin**:
  forces `0xACC0E8/EC` to `_DAT_53175e64` (assumed 1.0; lives outside dump, **inferred not byte-
  verified**) around effect/title 2D-quad draw `0x82B158`, then restores. Already partially ported.

**.text widescreen detour clusters** (all `eph_only` unless noted; full list in `code_patches.md`):
- Title/splash & early 2D: `0x6F4CC4→0x52F60150`, `0x708238→0x52F812E0`, `0x709F5E→0x52F60130`,
  `0x7103B7→0x52F5FEF0` *(differ)*, `0x7103C0→0x52F5FEC0` *(differ)*.
- Char-select / glyph render: `0x7AB554→0x52F75AE0`, `0x7ABAA5→0x52F76CC0`, `0x7AC1A0→0x52F76BE0`,
  `0x7A3BC4→0x52F55F10`, `0x7A60DC→0x52F68EC0`.
- HUD/menu quad family (`0x77xxxx → 0x52F67xxx`): large contiguous block incl. `0x765529→0x52F67A50`,
  `0x770888→0x52F67AF0`, `0x77571C→0x52F68140`, …; the dense **`0x52F674xx–0x52F676xx`** family
  (handlers reused 2–6×, e.g. `0x52F4C2F0` ×12, `0x52F67300` ×6) is the strongest UI-coord-transform
  signal.
- 2D-anchor family `0x52F5Fxxx`: **8-site contiguous `differ` block `0x748944–0x748ABE` →
  `0x52F5FFxx/0x52F600xx`** — HIGH priority, both builds patch these to **different** bytes.
- Projection/matrix page (`0x82xxxx`): `0x829110→0x52F4FE30`, `0x82936D→0x52F502B0`,
  `0x82A827→0x52F63750`, `0x836E8F→0x52F62320`.

**.text widescreen counts:** **372 detours total** (271 E9 + 100 E8 + 1 intra-engine `0x7A03F1→
0x7A0483`); **371 land in ephinea.dll** at `0x52F30000–0x52F84220` (323 distinct handler targets);
**18 are `differ`** (diff exactly for parity). **29 NOP-out** runs (code removed).

#### B. Font / text

- **Font block `0x0097DB60..8C`** (verified byte-exact from delta): `0x0097DB88` atlas width 256→592;
  `0x0097DB68` 14→32 and `0x0097DB7C` 7→32 (glyph cell); `0x0097DB80` UV step bytes `00 00 60 3d`
  (=14/256) → `c9 67 5d 3d` (=32/592 = 0.054054). **PORT RISK (gap.md rank-5):** the port writes the
  592 constants via the data table but performs **no atlas texture swap** — if PSOBB.IO binds a 256/512
  font atlas while the engine is told 592-cell-step, glyph UVs sample the wrong region (garbled text).
  Gate the `0x0097DB6x..8x` writes on the *actually-bound* atlas width, or ship the 592 atlas.
- Glyph render-mode flag handler `FUN_52db36d0`/`FUN_52db36f0` (sites `0x594C86/98`, `0x5A15B2/C4`):
  pokes `_DAT_53835c68` byte-3 around text draw `0x51F6B0`.
- Char-screen glyph render detour `0x7AB554→0x52F75AE0` (also in §A). **CORRECTION:** installed via
  `FUN_52dc3290(FUN_52dc5ae0, 0x7ab554, 7)` (len=7, **second** installer family) — it is **absent
  from the 120-line hooks file**; the prior "5-byte hook in the 120-list" framing is wrong.

#### C. Menu / UI

- Co-op per-player cursor hit-test & widget plumbing: `FUN_52dd2880` (`0x790E16`), `FUN_52dd2a10`
  (`0x7915E1`), `FUN_52dd2a30` (`0x7537FC`), `FUN_52dd4f70` (`0x66E001`) — `detours_6.md`. Local co-op,
  not visual.
- Scene-state-machine callback gates: `FUN_52db8fc0` (`0x5E5510`), `FUN_52db8ff0` (`0x5E4DA8`) —
  `detours_4.md`. Gate engine init/teardown on active screen (states 8/0xc/0x12).
- Keyed resource lookup by u16 id: `FUN_52db3800` (sites `0x80EF00/0x80F2D3/0x80F732/0x80FA4B`) —
  `detours_1.md`. **Prior doc mislabeled this as projection-side / a de-anchor — REFUTED**; it is a
  hash/list lookup returning a record field, no float/matrix math.

#### D. Position-precision

- Screen-coord edge clamps: `FUN_52d8cca0` (`0x6D79EE/AB6/A9A4/AA37`) + gate `FUN_52d8cc60`
  (`0x6D78BD/0x6DA5D0`) — min-clamp UI X to camera bound, gated by `0x00A95EDC` (`detours_1.md`).
  Plausibly widescreen-adjacent (safe-area), but generic min-clamp, no aspect term.
- Camera / animation easing made config-driven: `FUN_52d87f60/80/a0/c0/fe0`
  (`0x561206/0x4A154D/0x446BF9/0x43F612/0x43D828`) replace hardcoded `0.1f/0.15f/0.08f` at
  `call 0x4D430C` with config-key values (`detours_2.md`). Camera-follow reimpl `FUN_52d8ba30`
  (`0x4D7947`). Aim/turn vector + raycast `FUN_52d8bc20` (`0x4D7939`) — `detours_3.md`. Object-field
  float fixup `FUN_52db98c0` (`0x5886F7`) — `detours_5.md`. **None affect projection or 2D layout.**

#### E. Network / quest

- Per-player data-context swap cluster (Cluster A, `detours_4.md`): `FUN_52db4380/43a0/43c0/43e0/
  4400/4430/4460` redirect `_DAT_00a72ccc` to per-player buffers (table `0x5388ecd0`, flag
  `0x538582f4`, selector slot index `0x00aafc9c`).
- Lobby/party plumbing: client-count overrides `FUN_52dd63b0` (`0x4DC211/0x4DD734`), `FUN_52dd6370`
  (`0x6CBA28`); packet command filter `FUN_52dd5040` (`0x5C9BAB`, drops `0x310xx`/type-5 items);
  quest/section gates `FUN_52dd19e0` (`0x78237F`), `FUN_52dd1d90` (`0x43E783`), `FUN_52dd40f0`
  (`0x74B6DD`); table-index remap `FUN_52dd0370` (`0x790CED/0x7925A1`, 0x48→0x57); idle keepalive
  `FUN_52de5e60` (`0x7BCCDA/0x7BD3A0`); per-player send wrapper `FUN_52de5f60` (`0x77FDFD`); server/room
  list manager `FUN_52dce9e0` (`0x79809A`). All `detours_2/4/5/6.md`. **None visual.**

#### F. Localization

- The dominant detour pattern: **6 handlers / 33 of 50 sites in bucket 1** are a **locale string-table
  swap** family — `FUN_52db4540` (10 sites, highest fan-out), `FUN_52db44e0/4500/4560/44c0/4520`. Each
  swaps the active string buffer `_DAT_00a72ccc` to a per-language buffer for the duration of an engine
  text draw (`0x509590/0x50B510/0x509DC4`), keyed by language index `0x00aafc9c` and tables
  `0x5388ecd0`/`0x538582f4` (47-entry, 0x2600-byte buffers). String/asset substitution:
  `FUN_52d9a4c0` (`0x415AA1`), `FUN_52d9a4e0` (`0x6FEBE1`), `FUN_52d9a660` (`0x7932DA`), `FUN_52d9b0a0`
  (`0x5E4836`). **CORRECTION: the prior doc speculation that the high-fan-out `FUN_52db4540` is a
  "shared aspect/coordinate transform" is REFUTED** by its body — it is localization.

#### G. FFmpeg integration

Not in the engine-VA delta at all (statically linked + packed inside ephinea.dll). See §5. **Not a
pixel-parity dependency — do not chase.**

#### H. Misc

- Config-file scale knob `FUN_52da5f60` (`0x7A365B/9F/DE`, `_DAT_53869078`); QPC frame-rate limiter
  `FUN_52da5130` (`0x83AE47`); registry MRU persistence `FUN_52dc37a0` (`0x4822C1`,
  RegOpenKeyEx/RegSetValueEx REG_MULTI_SZ under HKCU); input-map re-application cluster
  `FUN_52db9060/9080/90b0/90d0/91a0/9200/9230` (`detours_5.md`); 64-bit timestamp throttle
  `FUN_52da54a0` (`0x5D910E/0x5D9F6C`); distance transparency fade `FUN_52db09a0` (`0x72E6F5`,
  near-miss color register `0x00ACBA2C` — **NOT geometry**); multi-instance plumbing
  `FUN_52db36f0/3ee0/5e80`, entity-state save/restore `FUN_52db03c0` (`0x800347`).

#### `.text differ`-class is mostly RELOCATIONS — do NOT chase

Of the 167 `.text differ` rows, the dominant `(Eph-Io)` deltas are **+0x22FB0** (52×, a moved global),
**+2** (52×), **+0xC** (32×). The brief's "HUDScale denom" VAs **`0x004011D2`** (`Eph=565555 /
Io=545555`) and **`0x006F4922`** are a **+2 operand shift = reloc noise, NOT scale-constant rewrites**
— do not chase them. The genuinely-different non-pointer u32 is **`0x009F0B3B`** (`0x35555642` vs
`0x35555442`, exponent byte 56 vs 54) — flagged for a targeted decompile lookup.

---

## 4. Authoritative Anchor List (supersedes the docs' 92-VA recipe)

This list **supersedes** `EPHINEA_92VA_PATCH_LIST.md` / `WIDESCREEN_PORT_STATUS.md`. **97 arithmetic
writes / 93 distinct VAs**, plus 2 non-arith. `factor_class`: `W-mul`=`*(renderW/640)`,
`H-mul`=`*(renderH/480)`, `W-add`=`+(renderW-640)/2`, `H-add`=`+(renderH-480)/2`,
`H-add-raw`=`+(renderH-480)` (no /2), `inline`=mixed display/render ratios. **DOC OMITS** marks VAs the
prior recipe missed.

**L1 — W-mul (23):** `0x008FA154`(L1[0]) `0x008FA15C` `0x008FA144` `0x008FA14C` `0x008FA134`
`0x008FA13C` `0x008FA124` `0x008FA12C` `0x008FA0F8` `0x008FA098` `0x008FA068` `0x008FA0C8` `0x008FA1A4`
`0x008F9EC0` `0x008FA1C0`(also inline displayW/renderW) — plus 8 **imm32-in-engine-.text**: `0x0040CA07`
`0x0040CA40` `0x0040CA66` `0x0040CA8C` `0x0040CAE2` `0x0040CB1E` `0x0040CB44` `0x0040CB6A`.

**L2 — H-mul (17):** `0x008FA148`(L2[0]) `0x008FA150` `0x008FA128` `0x008FA130` `0x008FA0F4`
`0x008FA0C4` `0x008FA0D4` `0x008FA094` `0x008FA0A4` `0x008FA064` `0x008FA074` `0x008FA044` `0x008FA04C`
`0x008FA024` `0x008FA02C` `0x008FA104` `0x008F9EFC`.

**L3 — W-add (19, doc says 16):** `0x008FA108` `0x008FA110` `0x008FA118` `0x008FA0D8` `0x008FA0E0`
`0x008FA0E8` `0x008FA0A8` `0x008FA0B0` `0x008FA0B8` `0x008FA078` `0x008FA080` `0x008FA088` `0x008FA050`
`0x008FA058` `0x008FA030` `0x008FA038` — **+ DOC OMITS:** `0x008FA1D8` (local_100[0x10]), `0x008F9F18`
(local_bc), `0x008F9F20` (local_b8).

**L4 — H-add (20, doc says 16):** `0x008FA10C` `0x008FA114` `0x008FA11C` `0x008FA0DC` `0x008FA0E4`
`0x008FA0EC` `0x008FA0AC` `0x008FA0B4` `0x008FA0BC` `0x008FA07C` `0x008FA084` `0x008FA08C` `0x008FA054`
`0x008FA05C` `0x008FA034` `0x008FA03C` — **+ DOC OMITS:** `0x008FA1D0` (local_150[0x10]), `0x008FA1AC`
(local_10c, also inline), `0x008F9F1C` (local_108), `0x008F9F24` (local_104).

**L5 — H-add-raw, NO /2 (7):** `0x008FA138` `0x008FA140` `0x008F9F48` — plus 4 **imm32-in-.text**:
`0x0040CB07` `0x0040CB2C` `0x0040CB52` `0x0040CB78`.

**inline (11 arithmetic):** `0x008FA1D4`(displayW/renderW) `0x008FA1CC`(displayH/renderH)
`0x008FA1C4`(displayW/renderW) `0x5318E980`(displayW/640, dll-internal scratch)
`0x0040D039`(displayH/renderH, imm32) `0x0040D032`((displayH/renderH)*(x-480)+renderH)
`0x0040D1DD`((displayW/renderW)*((renderW-640)/2+x)) — and the L1/L4 dual-listed `0x008FA1C0`,
`0x008FA1AC`.

**non-arith (2):** `0x0040D04E` u32 pointer-store `=&DAT_5318e980` (NON-arith redirect);
`0x0040D305` 11-byte code patch via `FUN_52dc3460(0x40D305, 0xB)`.

**Discrepancies called out (vs prior docs):**
1. **92-VA is NOT the whole apply.** `FUN_52da7ff0` writes ~700+ engine globals (loops
   122/90/35/28/235/12 + ~80 scalars) **before and after** `FUN_52da9280`. Port only-92 = visibly wrong.
2. L3 = **19** (doc 16); L4 = **20** (doc 16) — six VAs omitted (listed above).
3. L3/L4 **divide delta by 2**; L5 is **raw**. Doc formulas dropped the /2 (port later fixed).
4. inline = **11 arithmetic**, not 12 (doc folded in the 2 non-arith).
5. Constants `0x53176128=640`, `0x53176120=480`, `0x53175e8c=4/3`, `0x53175f20=2`, `0x53175e64=1.0`,
   `0x53175fc0=100` are **referenced-only / undumped .rdata** — structure verified, literals RPM-only.

**`FUN_52dabbd0` 10 extra writes (verified, `_ephinea_decompiled.c:133476-491`):** `0x0097A910`
(=dVar3/100) and `*=S` on `0x0096E114`, `0x006F49FD`, `0x006F4A57`, `0x0096E168`, `0x0096E16C`,
`0x0096E170`, `0x0096E174`, `0x0096E178`, `0x0096E17C`.

---

## 5. FFmpeg Integration (usage only)

**Verdict (verified + adversarially re-verified):** the "ffmpeg ships next to the exe" claim is
**FALSE**. There is **no `av*.dll`/`sw*.dll` anywhere** in `EphineaPSO/` or `PSOBB.IO/`. FFmpeg
(**libavcodec 59.39.100 = FFmpeg 5.1.x**, built by "Terry" via msys64) is **statically linked and
packed inside the 12.6 MB `ephinea.dll`** in the `.banana` packer section — confirmed by PE parse:
section names stripped, a real `.banana` section at vRVA `0x02E05000`, and **Import directory RVA = 0**
(zeroed import table → no av*.dll dependency; imports resolved post-unpack). FFmpeg
code/strings appear **only after runtime unpack**, captured in `_ephinea_dump.bin`.

**Feature powered:** video / cutscene (intro/movie) playback. Decoders present include h264, hevc,
mpeg2/4, vp8/9, av1, theora, wmv, mjpeg, plus the classic game-video codecs **Bink** and **Smacker**;
audio: aac/ac3/mp3/vorbis/opus and **SEGA CRI ADX & CRI HCA**.

**Evidence anchors:** version string `Lavc59.39.100` at dump offset **`0x10BADC`**; format string
`FFmpeg v%d.%d.%d / libavcodec build: %d`; 5× `C:\msys64\home\Terry\ffmpeg\libav*\...` source paths;
74 libav* string occurrences; blob ends at `0x17AD0C`.

**Corrections to the FFmpeg pass (adversarial):** blob **start** is `0x0EBB24` (not `0xEBDA8`);
`AV_PIX_FMT` count is **3** in the dump (not 26); "swscale not found" is true only for the **1.5 MB dump
window** (not proof of absence in the full image); and the "73.5 MB sidecar = data/data.gsl
mis-attribution" theory is **speculative** — `data.gsl` is **not** the only ~73 MB file
(`map_wilds01.xvm`=85.5 MB, `map_desert03.xvm`=83 MB exist; no file is exactly 73.5 MB).

**Port impact:** orthogonal to widescreen/projection/HUD/font. **Not a pixel-parity dependency.**

---

## 6. Corrections to Prior Docs (falsified / corrected)

From the doc-audit (4 falsified) and the adversarial re-verification passes. **Where a correction
exists, it supersedes the original claim.**

**Falsified:**
1. `EPHINEA_RE_CATALOG.md`: *"ephinea.dll is packed and NOT statically analyzable; the only source of
   truth is the live memory delta."* — **FALSIFIED.** `_ephinea_decompiled.c` is a 166,098-line Ghidra
   decompile of the **unpacked** dll (16,657 `_DAT_53.../FUN_52d...` refs); the newer docs decompile it
   directly.
2. `EPHINEA_RE_CATALOG.md`: *"Title/splash detour `0x6F4CC4 → 0x52F60150` (full title fn replaced)."* —
   **FALSIFIED as a hook-installer claim:** `0x6f4cc4` appears in **no** install call in the decompile
   and is in neither installer family; the only `0x6f` site in the hooks file is `0x6febe1`. (The
   **E9 detour byte-patch itself IS present** in the .text delta and decodes exactly to `0x52F60150` —
   so the *patch* is real even though the *installer-call* framing is wrong.)
3. `EPHINEA_RE_CATALOG.md`: *"Widescreen replication COMPLETE — action item is HUDScale config, not
   code."* — **FALSIFIED.** The same doc-set proves Ephinea adds a runtime per-draw hook layer
   (full font-render replacement at `0x7ab554`, deanchor `FUN_52db2240`, ~350 installs) the
   static-constant port does not contain.
4. *"ffmpeg av*/sw* DLLs ship next to the exe."* — **FALSIFIED** (§5). (Context aside; not actually
   asserted by the 6 audited docs.)

**Corrected / partial (supersedes the original):**
5. `EPHINEA_RE_CATALOG.md`: char-glyph detour `0x7AB554 → 0x52F75AE0` is real but installed via the
   **second** installer `FUN_52dc3290(...,7)` — **len=7, absent from the 120-line hooks file**; the
   "5-byte hook in the 120-list" framing is wrong.
6. `EPHINEA_92VA_PATCH_LIST.md`: L3/L4 formulas — the recipe omitted the **`/2`** (`/_DAT_53175f20`);
   real L3 = `(W-640)/2 + x`, L4 = `(H-480)/2 + x`; L5 raw. Also **L3=19 / L4=20**, not 16/16 (six VAs
   omitted, §4).
7. `EPHINEA_DETOUR_RE.md`: *"371 detours via 5-byte hooks; the 120-site layer is the install surface."*
   — **partial.** There are **two** installer families; the 120-line file is **one** of them; true
   surface ≈ 350; the doc conflates byte-diff count with install-call count.
8. `EPHINEA_CASCADE_RPM.md`: breakpoint *values* (1.30/1.45/.../5.25) and the aspectMult table are
   **unverifiable-static** (undumped .rdata, RPM-only). The 7-level control-flow ladder **is** verified;
   the `_DAT_53175e8c=1.3333` equivalence holds **only under the live constants**.
9. `WIDESCREEN_PORT_STATUS.md`: *"FUN_52da9280 92/92 byte-identical, port is Ephinea-1:1 complete."* —
   **partial / overstated.** VA lists are present but "complete/byte-identical" is wrong: the port lacks
   the Stage-C bulk loops and the runtime per-draw worker layer; L3/L4 were undercounted; equivalence
   rests on RPM-only constants.
10. **CASCADE corrections (adversarial):** `FUN_52dac110`'s per-device detour wiring
    (claimed tramp `0x52DAC0F0` → engine `~0x408C88`) is **UNVERIFIED** — that address appears nowhere
    in the decompile or hooks file; only the handler **body** is confirmed (`renderH != 480` guard,
    stamps renderW into device vertex offsets). And `reruns_on_device_reset` is **config-reload-proven
    but D3D-Reset-UNCONFIRMED** (§2).
11. **GAP rank-1 corrections (adversarial):** the WMul/WAdd **double-apply** is a real defect with
    **zero observable effect** (those VAs are 100% overwritten by the absolute data table) — the
    dominant bug is the **frozen 16:9 data-table clobber**. The title-art collision at `0x006F49FD/A57`
    is **scale-dependent** (guarded by `value == 430.0`), not flat last-wins. The "L3/L4 written as
    multiplies via inline.A/B/M" sub-claim is **NOT supported** — those inline VAs are genuine
    multiplies and the port codes them correctly.
12. **DATA_ANCHORS correction (adversarial):** the "138 true-differ" sub-figure is **not reproducible**
    from the CSV (independent count = 166); the reliable headline is **`eph_only = 430` exactly** plus a
    16:9 1-LSB rounding band. The 408 partial-dword rows **cannot** be decoded to an exact factor from
    the CSV alone (unchanged bytes omitted) — any doc asserting a specific factor for a 1/2/3-byte row
    is **unverified in Phase 1**.

---

*Per-handler detail and decompile line citations live in `C:/Users/u03a9/Repositories/psoharness/_re/`:
`cascade.md`, `cascade_verify_adversarial.md`, `data_anchors.md`, `data_anchors_VERIFY.md`,
`code_patches.md`, `handlers.md`, `detours_1..6.md`, `ffmpeg.md`, `ffmpeg_adversarial_verify.md`,
`doc_audit.md`, `gap.md`, `adversarial_verify_rank1_multilayer_collision.md`.*
