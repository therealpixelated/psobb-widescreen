# Widescreen Port Status — VERIFIED FROM SCRATCH

Date: 2026-05-28 · Phase 1 (purely static; no live game process). Verify-from-scratch.
Subject: `C:/Users/u03a9/Repositories/pixelateds-psobb-mods/psobb-patches/pso_widescreen-asi/pso_widescreen.c` (5251 lines) + headers `ephinea_data_table.h` (337), `ephinea_diff_tables.h` (WMul/WAdd extras), `ephinea_verbatim_table.h` (legacy/unused), `ephinea_predata_safe.h` (5).

> **This document supersedes `WIDESCREEN_PORT_STATUS.md`.** That doc's headline claim — "FUN_52da9280 92/92 and FUN_52dabbd0 10/10 are byte-identical; the port is Ephinea-1:1 complete" — is **NOT TRUE**. The doc audit graded it `partial`: the VA lists are present, but "byte-identical / complete" is overstated. The port is structurally divergent from Ephinea (one-shot vs re-derived), stacks contradictory apply layers, mis-types several anchors, and lacks Ephinea's entire bulk-anchor and runtime per-draw layers. Treat the prior "complete" framing as debunked.

---

## 1. What the port actually does today (verified against source)

### Entry / install
- `Hook_Direct3DCreate8` (line 1556) installs IDirect3D8 `vtable[15]=CreateDevice` (1567).
- `Hook_CreateDevice` (1483): rewrites `pp->BackBufferWidth/Height/Windowed` (1499-1514); then **ONE-SHOT** (`InterlockedExchange(&g_dim_patches_done,1)==0`, 1529) calls `apply_ephinea_widescreen_coords()` (1542); always `patch_device_vtable(*out)` hooking device slots **15 Present / 37 SetTransform / 40 SetViewport / 72 DrawPrimitiveUP / 73 DrawIndexedPrimitiveUP** (1395-1474).
- **Slot 14 (Reset) is NOT hooked** (explicit comment line 183: "IDirect3DDevice8::Reset, which we don't intercept").

### Apply pipeline
`apply_ephinea_widescreen_coords()` (3797) → per-kind detours (3806) → W/H storage redirect (3812) → res table (3851) → `apply_ephinea_exact_92va(w,h,hud_scale)` (3884) → SEGA splash / title-art / deanchor / cursor-warp.

`apply_ephinea_exact_92va()` (3328, one-shot `g_ephinea_92va_done`, 3330) writes IN THIS ORDER:
1. **L1 mul x23 / L2 mul x17 / L3 add x16 / L4 add x16 / L5 add x7** (3367-3376). Factors: `L1mul=gameRenderW/640`, `L2mul=gameRenderH/480`, `L3add=L4add=L5add=delta/2` (3352-3356).
2. **Inline A–M** (3381-3458): 0x008FA1D8/1D0/1D4/1CC/1C4/1C0/1AC written as **multiplies**; plus pointer redirect 0x40D04E, code patches 0x40D032/0x40D305(NOP)/0x40D1DD.
3. **WMulExtra (96 entries) `*= L1mul`; WAddExtra (57 entries) `+= L3add`** (3466-3469).
4. **`kEphineaDataTable` 337 ABSOLUTE byte writes** (3491-3505).
5. `kEphineaPredataSafe` 5 absolute writes (3511-3524).
6. 0x0097A910 mul + abd0 extras `*= S` (3538-3556).

`gameRenderW = 640*S*(aspect/(4/3))`, `S = hud_scale` (3344-3346). Anchors are mutated **in place** (`*=` / `+=`, lines 3273/3290) → **NOT idempotent**.

### One-shot vs cascade — the structural gap
| | Ephinea (verified) | Our port |
|---|---|---|
| Render size | `FUN_52dabbd0` re-derives renderW/H from live registry/res-table on **every** config load (`FUN_52da9930` tail-calls it @0x52DAB9BB) | Computed once at CreateDevice |
| Bulk anchor apply | `FUN_52da7ff0`: loops **122/90/35/28/235/12 + ~80 scalars** (≈700 globals) THEN calls `FUN_52da9280` | Port implements only the `FUN_52da9280` "92-VA" subset + tables; **the ~700-global bulk layer is absent** |
| Per-device stamp | `FUN_52dac110` re-stamps renderW into device vertex offsets on device (re)create | Absent |
| Re-apply on Reset | Re-derived on config/resolution/device events | **Never** — slot 14 Reset not hooked, one-shot guards block re-run |
| Apply idempotency | Recomputed from live W/H/S each run | `*=`/`+=` in place → not idempotent |

`has_reset_reapply = NO`. The port writes the anchors exactly **once**, at the first successful CreateDevice.

---

## 2. Which anchors match / mismatch (from the data-anchor analysis)

The data-anchor pass examined 736 `.data`/unsectioned rows. Of those, only ~51 are genuine 4-byte f32 scalar anchors and ~17 are u32; the rest are 140 reloc-only pointers, 67 detour-pointer installs, 408 partial-dword fragments, and 53 tables/structs.

### Parity verdict on float layout anchors (load-bearing)
- **ZERO decodable float anchors genuinely diverge between Ephinea and PSOBB.IO.** Every `differ`-class 4-byte float decodes to the SAME value in both clients; the raw-byte "difference" the delta tool flags is a **1-LSB mantissa rounding artifact** (e.g. `433faaac` vs `433faaa8`, both = 191.667). The IO port already reproduces all *decodable* float layout anchors that it touches.
- The widescreen virtual width **853.333** (= 640×1.3333) sits at 0x009B8D08 / 0x009B8D50 in BOTH clients (eph == io), confirming the three transform classes derive from one design width: **W-mul ×1.3333**, **W-add +106.667** (half-letterbox `(853.33-640)/2`), **W-add +213.333** (full delta `853.33-640`).

### Where the port is MISSING patches (the real gap)
- **430 `eph_only` rows** (VAs Ephinea writes, IO never touches) — this is the **load-bearing actionable number** (verified EXACTLY; prefer it over the looser 568 total).
  - ≈16 `eph_only` f32 widescreen layout constants (W-mul/W-add) IO is missing and should port — e.g. 0x008F9EC0 (133.333, ×1.3333), 0x008FA124 (546.667, ×1.3333), 0x008FA0D8/E0/E8 (+106.667), 0x008FA528/ABF8/AC00/AC08 (+213.333), 0x008FADC8 (+106.667), 0x0096F2B8 (+213.333).
  - **67 detour-pointer installs** writing dll handlers (0x52F…) into engine vtables/data that IO never installs (e.g. glyph 0x52F75…, title/splash 0x52F60150-family). These are the runtime per-draw layer the port lacks.

### Counting corrections (adversarial verdict applied — prefer these)
- The "138 true-differ" sub-figure in the data-anchor summary is **NOT reproducible** from the CSV; an independent recount found **166** differ-class non-reloc rows whose raw bytes differ. The clean statement is **"430 missing `eph_only` patches + a 1-LSB 16:9 rounding band of differ rows the port already effectively matches"** — not a hard 138. `568` mixes in rows the port already matches and should NOT be the headline.
- Exactly one non-pointer u32 has a TRUE eph≠io divergence: **0x009F0B3B** (eph `0x35555642` vs io `0x35555442`, one exponent byte). Both are 16:9-family but differently quantized — flagged for targeted decompile lookup, not yet explained.
- The 408 partial-dword rows (dominant `000020`→eph `565555`/io `545555` ×69) **cannot** be decoded to a clean factor from the CSV alone (the run omits the unchanged dword bytes). Any prior doc asserting a specific factor for a 1/2/3-byte row is UNVERIFIED in Phase 1 and needs live bytes.

---

## 3. Ranked slop hypotheses (with confidence + adversarial verdict)

> The adversarial pass independently re-derived the source facts for the top two hypotheses and did **NOT refute** either; it tightened the corrections noted below. Where it corrected a claim, the correction is stated and preferred.

### #1 — Multi-layer apply collision (in-table double-apply + 16:9-locked data-table clobber)
**Confidence: HIGH. Adversarial verdict: NOT REFUTED (high).** Every quantitative sub-claim reproduced from source.
- `kEphineaWMulExtra` = **96 entries / 70 unique** → 26 VAs get `*= L1mul` TWICE (`L1mul²` ≈ 1.78 vs intended 1.333 at 16:9). `kEphineaWAddExtra` = **57 / 31** → 26 VAs get `+= L3add` twice (+213 instead of +106). Helpers are read-modify-write in place (3273/3290); loops 3466-3469 iterate every (non-unique) entry.
- `kEphineaDataTable` (337) is a **frozen 1920×1080 / S=1 absolute snapshot** (dominant `0x44555556` = 853.333; also `0x44870000` = 1080.0, `0x40100000` = 2.25), applied **LAST** (3491) with **no aspect/scale guard**.
- Overlap is total on the W-axis: **70/70 WMul, 31/31 WAdd, ALL 16 L3, 12/23 L1** VAs are in the data table → clobbered. **L2 (0/17), L4 (0/16), L5 (0/7) survive.** So at any non-16:9 aspect or hud_scale≠1.0 the data table freezes ~half the anchors at 16:9 while the other half scale per-aspect → visible slop. At exactly 16:9@S=1 the layers coincide, masking the bugs.

  **Adversarial corrections (prefer these):**
  - The WMul/WAdd **double-apply has ZERO observable effect** — 100% of those unique VAs are subsequently overwritten by the absolute data table. The duplication is a latent defect, not a visible slop source; the rank-1 evidence over-weights it. **The dominant defect is the frozen-16:9 data-table clobber**, not the duplication.
  - Title-art collision at 0x006F49FD/A57 is real but **scale-dependent, not flat last-wins**: title-art is guarded by `if value == 0x43d70000 (430.0)`. At S=1.0 the flare `*=S` leaves 430.0 → title-art wins; at S≠1.0 the value becomes 430·S, the guard fails, title-art skips, and the flare value survives.

### #2 — One-shot at CreateDevice; no Reset / no config-reload re-apply
**Confidence: HIGH that the hook is absent; MEDIUM that it is the dominant *visible* slop (only bites on device reset / mid-session resolution change).** Adversarial pass corroborated the Ephinea re-derive mechanism.
- Only device slots 15/37/40/72/73 hooked; slot 14 Reset untouched. Both applies one-shot-guarded. In-place `*=`/`+=` → re-running would double, so the guard is load-bearing — but it also blocks the **needed** re-derive on resolution change.
- Verified Ephinea: `FUN_52dabbd0` re-derives renderW/H from live registry/res-table every `FUN_52da9930` call; `FUN_52dac110` re-stamps device vertex anchors on device (re)create.

  **Adversarial correction (Phase-1 honesty):** The `FUN_52dac110` per-device detour (asserted at engine ~0x408C88 / tramp 0x52DAC0F0) and `reruns_on_device_reset=true` are **proven only for the config/registry re-apply path** (the tail-call). `FUN_52da9930` and `FUN_52da7ff0` have **zero callers / data refs in the entire 4.9 MB decompile**, and there is **no visible IDirect3DDevice9::Reset hook**. The D3D-Reset rerun is **plausible but statically UNCONFIRMED** — needs Phase-2 RPM of the call stack. Do not state the Reset-hook rerun as established fact.

### #3 — Missing & mis-typed sites (L3/L4 undercount; W-add/H-add written as multiplies)
**Confidence: HIGH that the typing is wrong.** Adversarial pass corroborated against the verified anchor list.
- L3 (W-add) should be **19** VAs; `kEphineaL3` has **16** — MISSING 0x008FA1D8, 0x008F9F18, 0x008F9F20.
- L4 (H-add) should be **20** VAs; `kEphineaL4` has **16** — MISSING 0x008FA1D0, 0x008FA1AC, 0x008F9F1C, 0x008F9F24.
- The missing ones are written WRONG-TYPE as **multiplies**: 0x008FA1D8 via inline.A (`*=WoverGameW`), 0x008FA1D0 via inline.B, 0x008FA1AC via inline.M — all verified additive. 0x008F9F18/20 are in WAddExtra (doubled, #1) AND the data table; 0x008F9F1C/24 only reachable via the 16:9-locked data table.

  **Adversarial caveat:** The separate sub-claim that *the documented* inline VAs (per EPHINEA_92VA_PATCH_LIST.md lines 50-61) are "mis-typed as multiplies" is **NOT supported** — those are correctly documented and coded as multiplies. The genuine typing error is specifically the **L3/L4 anchors the port omitted from the L-lists and routed through inline-mul / the data table.**

### #4 — Sub-pixel / texel-center (+0.5 half-texel) alignment on 2D/HUD & DrawPrimitiveUP RHW
**Confidence: MEDIUM-LOW.** `scale_rhw_apply` (1234) does pure float center-divide with no +0.5 / no pixel snap, but is **GATED OFF unless `hud_compress != 1.0`** (1239) — that is the optional HUDCompress feature, not the widescreen mechanism, so by default it does nothing. Anchors are stored raw f32 (no floor/round), matching Ephinea; data-anchor deltas are sub-ULP (1-LSB), so op-order float slop is **not** the issue. Likely only matters if HUDCompress is enabled or as a faint 1px shimmer.

### #5 — Font atlas: 592 UV constants written but the 256px atlas texture may still be bound
**Confidence: LOW-MEDIUM.** Font constants ARE swapped to Ephinea's 592px layout via the data table (0x0097DB88=592.0, 0x0097DB80=0.054054=32/592, 0x0097DB68/78/7C/84=32.0). BUT there is **no CreateTexture redirect / atlas swap / glyph-texture replacement** in the port. If PSOBB.IO ships a 256/512px atlas while we tell the engine cell-step over a 592px atlas, glyph UVs sample the wrong region → garbled text. Unverifiable in Phase 1 (asset-dependent).

---

## 4. Single most-likely root cause to fix FIRST

**The frozen 16:9 absolute `kEphineaDataTable` (337 entries) applied LAST clobbers the per-aspect arithmetic cascade.** At exactly 16:9 @ S=1.0 the data table equals the cascade, masking every bug; at ANY other aspect or hud_scale≠1.0 it freezes the W-axis anchors (all 70 WMul + 31 WAdd + 16 L3 + 12 L1 VAs) at 16:9 while the H-axis (L2/L4/L5) scales per-aspect — a guaranteed half-frozen layout = the visible slop the acceptance test will reject. (The WMul/WAdd duplication is a real but latent defect that is fully masked by this same clobber, so it is secondary.)

**Fix it by replacing all three apply layers with ONE ordered arithmetic pass keyed on live W/H/S, applying each VA exactly once with its correct factor_class, computed from absolute stock baselines (so it is idempotent and re-runnable).** Stop applying the absolute 16:9 data table for the cascade-owned VAs.

---

## 5. Ordered TODO — implementation phases

### Phase 2 — Live confirmation (RPM; do FIRST, before any code change)
1. Launch BOTH PSOBB.IO (patched) and live Ephinea at the **same non-16:9 mode** (2560×1080 21:9, then 1920×1200 16:10), hud_scale=1.0.
2. Read the W-axis clobber witnesses 0x0096F5E0 / 0x0097A170 / 0x0097BB0C (WMul) and 0x00972000 / 0x00972620 (WAdd) plus H-axis survivors 0x008FA148 (L2) / 0x008FA10C (L4) in both processes. **Expect:** Ephinea tracks aspect on all; ours frozen at the 16:9 data-table constant on the W-axis. Read immediately after attach to confirm clobber order. Rerun at hud_scale=1.5 — data-table VAs stay put while Ephinea moves. (Confirms #1.)
3. Hook/break on `IDirect3DDevice8::Reset` (or watch D3DERR_DEVICELOST→Reset); alt-tab in exclusive fullscreen or change launcher resolution. Read 0x008FA148 / 0x008FA108 and engine W/H 0x00ACBE7C/80 before/after Reset in both clients. **Expect:** Ephinea re-applies; ours reverts/stays stale. (Confirms #2.) Also try to capture the Ephinea call stack to settle whether the rerun is a real D3D Reset hook vs config-reload only.
4. Read 0x008FA1D8/1D0/1AC and 0x008F9F18/1C/20/24 at a non-16:9 mode in both clients. **Expect:** Ephinea = stock ± delta/2 (additive), NOT stock × ratio. (Confirms #3.)
5. Hook SetTexture; log bound font/glyph atlas width/height when the glyph shader runs in both clients; compare 0x0097DB60..8C. (Resolves #5.)
6. Confirm `scale_rhw_apply` early-returns at default hud_compress=1.0 (read `g_draw_calls_accepted == 0`). (De-risks #4.)

### Phase 3 — Core rewrite (root cause #1)
7. Collapse the three apply layers into ONE ordered, idempotent cascade keyed on live `W/H/S`, applying each VA exactly once with its correct factor_class, from absolute stock baselines (no `*=`/`+=`).
8. STOP applying the absolute 337-entry 16:9 `kEphineaDataTable` for cascade-owned VAs (keep only the genuinely-static, aspect-independent entries — e.g. font block, breakpoint tables — after auditing each).
9. De-duplicate `WMulExtra` (70 unique) and `WAddExtra` (31 unique) to sets (defensive; the clobber currently masks it).
10. Resolve one-writer-per-VA collisions: title-art/flare at 0x006F49FD/A57 (respect the `==430.0` guard semantics), and inline-mul vs data-table at 0x008FA1C4/1D4.

### Phase 4 — Fix mis-typed / missing anchors (root cause #3)
11. Move 0x008FA1D8→L3, 0x008FA1D0/1AC→L4; ensure 0x008F9F18/20→L3 and 0x008F9F1C/24→L4 — each applied exactly once as additive; remove their inline-mul and data-table entries. (Target L3=19, L4=20.)
12. Port the ≈16 missing `eph_only` f32 layout anchors (W-mul/W-add) IO never touches (e.g. 0x008F9EC0, 0x008FA124, 0x008FA0D8/E0/E8, 0x008FA528/ABF8/AC00/AC08, 0x008FADC8, 0x0096F2B8).
13. Investigate 0x009F0B3B (the lone true eph≠io u32, `…5642` vs `…5442`) via targeted decompile lookup; decide whether to match it.

### Phase 5 — Reset / re-derive (root cause #2)
14. Hook device vtable **slot 14 (Reset)**: snapshot pre-Reset, on success re-run the now-idempotent Phase-3 cascade computed from absolute baselines, driving W/H/S from `pp` at Reset exactly like CreateDevice. Drop the one-shot guards once the cascade is idempotent.
15. (If Phase-2 step 3 confirms it) mirror Ephinea's config-reload re-derive so resolution changes recompute renderW/H.

### Phase 6 — Runtime per-draw layer (gap closure toward true parity)
16. Evaluate the 67 `eph_only` detour-pointer installs (dll handler region 0x52F…) — the runtime per-draw correction layer Ephinea adds and the port lacks. Decide which to replicate (glyph render, title/splash, 2D-anchor block 0x748944-0x748ABE, HUD quad family 0x52F67xxx) versus which the static-anchor approach already covers.
17. Resolve the font atlas question from step 5 (ship/load 592px atlas, or gate the 0x0097DB6x-8x writes on detected atlas width).

### Phase 7 — Pixel-parity acceptance
18. Side-by-side screenshot diff vs live Ephinea at the acceptance config AND at off-16:9 modes and hud_scale≠1.0 (title/splash, char-select glyph grid, HUD, quest text). Target "crisp, pixel-perfect, no slop." Iterate.

### Explicitly OUT of scope (do not chase)
- **FFmpeg** — statically linked + packed inside ephinea.dll (.banana), an isolated video/cutscene subsystem, orthogonal to widescreen. No av*.dll sidecar exists. Not a parity dependency.
- **Projection-aspect immediates** 0x0082EF4C / 0x0082EC74 and perspective builder 0x00892565 — byte-identical 16:9 in BOTH clients; do NOT revert to 4:3, do NOT patch.
- **"HUDScale denom" reloc VAs** 0x004011D2 / 0x006F4922 — reloc noise (+2 operand shift), NOT scale rewrites.
- **140 reloc-only pointers** (delta −0xAD90000) — image-base relocations, not layout; exclude from any "patches we don't match" metric.
