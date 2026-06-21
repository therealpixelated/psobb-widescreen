# In-game List-Window Bottom-Anchor — Ephinea `FUN_52d8cb90` RE & Port Verification

**Date:** 2026-06-17
**Subject:** Verify whether our ported in-game list-window bottom-anchor (`pso_widescreen.c`, `lw_yanchor_compute` ~L3438) exactly matches Ephinea's `FUN_52d8cb90`.
**Method:** Live read of a running Ephinea client (account *Zoe*) via the psoharness harness + the decompiled sources (`_ephinea_decompiled.c`, `original-psobb-client-source`). Cross-checked against a live psobb.io `[pixelated]` build.

---

## TL;DR

| Item | Verdict |
|---|---|
| **Y anchor** | Port was **OFF BY +150px** (window 150px too high). Constants are **C1=480.0, C2=334.0** → Ephinea `Y = design_h − 146`. Port computed `design_h − 296`. **Fixed** (base 184 → 334). |
| **X substitution** | Port's live-X source `0x00973CEC` is, in our build, a **jump-table of code pointers — not a coordinate**. The shipped guard does **not** reject the resulting denormal, so the X path risks an **X≈0 (far-left) regression**. Live in-game value **unverified** (no reachable server). |

The Y fix is correct and shipped. **The X fix is unsafe as written** and needs the guard hardened (worst case → stock 385) and the address re-validated in an in-game 16:9 session.

---

## 1. Ephinea `FUN_52d8cb90` — the function

Installed by `FUN_52d8caf0` as a **detour** over the stock list-window construction site:

```c
FUN_52dc3240(FUN_52d8cb90, 0x73ff92, 5);   // patch `call 0x737f80` @ PsoBB.exe 0x0073FF92
```

`0x0073FF92` is the `[0xA48A3C]==1` (single-player) arm of `ListWindowObject_Create_0073ff34`. The stock arm pushes `0x972DEC` (`item_bank_and_quest_list_dimensions` = design `{X=385.0, Y=184.0}`) then the id, and calls `ListWindowObject_InitBase_00737f80(xy*, id)`, which stores `xy[0]→[0x9F3130]`, `xy[1]→[0x9F3134]`, `id→[0x9F3128]`. That `{X,Y}` is the construction-time design-space base position of the in-game **item-bank / quest-counter / shop** list window (then scaled to screen by the global 2D affine `0x00ACC0E8/EC` — there is no render-time screen-edge recombination).

Decompiled body (Ghidra; the `_` symbols overlap-warn — actual operands confirmed by live capstone, see §2):

```c
void FUN_52d8cb90(p1, p2) {
  float local_c;
  if (_DAT_00a95edc == 0) {                 // Ephinea widescreen-active flag == 0 → stock path
    (*(code*)0x737f80)(p1, p2);
  } else {                                   // widescreen branch
    local_c = (design_h[0x5318d124] - C1[0x53176128]) + C2[0x53176114];
    if (*_DAT_5318d108 - _DAT_53176060 <= *_DAT_5318d114 + *_DAT_5318d10c)   // live-bounds clamp
        local_c = (_DAT_5318d108[1] - _DAT_5318d114[1]) - _DAT_53176060;
    _DAT_5382fe60 = *_DAT_5318d10c;          // scratch.X = live-X deref (NO scaling)
    _DAT_5382fe64 = local_c;                 // scratch.Y
    (*(code*)0x737f80)(&DAT_5382fe60, p2);   // call InitBase with the substituted {X,Y}
  }
}
```

---

## 2. Locating it live (reusable technique)

Ephinea's `ephinea.dll` is packed (`.banana`) and ASLR'd; its **PE image base is NOT the engine** (the engine is reached via detours over `PsoBB.exe`). Steps used:

1. `PsoBB.exe` is at `0x00400000` in the Ephinea process (confirmed via PowerShell `Get-Process … .Modules`). The detour site `0x0073FF92` is therefore directly readable.
2. Read 5 bytes at `0x0073FF92`: `E8 59 DF 70 78` = `CALL rel32`, target `= 0x73FF97 + 0x7870DF59 = 0x78E4DEF0` → **live `FUN_52d8cb90`**.
3. Read the function bytes at `0x78E4DEF0` and parse the absolute operands directly (ground truth — no need to solve the unpacked-region base):

```
78e4df1e: f3 0f 10 05 24 f1 24 79   movss xmm0,[0x7924F124]   ; design_h   (_DAT_5318d124)
78e4df26: f3 0f 5c 05 28 81 23 79   subss xmm0,[0x79238128]   ; − C1
78e4df2e: f3 0f 58 05 14 81 23 79   addss xmm0,[0x79238114]   ; + C2
...
78e4df4d: 8b 0d 0c f1 24 79         mov   ecx,[0x7924F10C]     ; X-pointer (_DAT_5318d10c)
78e4df94: f3 0f 10 02               movss xmm0,[edx]          ; *_DAT_5318d10c
78e4df98: f3 0f 11 05 60 1e 8f 79   movss [0x798F1E60],xmm0   ; scratch.X (verbatim, no scale)
```

**Relocation deltas** (Ghidra decompile-VA → live): **data = `0x260C2000`** (matches the long-noted "dump→decompile delta"), **.text = `0x260C1360`** (differs by 0xCA0 — *parse the live operands, don't assume a uniform delta*).

---

## 3. The constants (live-read from the running Ephinea)

`read_typed f32` at `0x79238114`, count 8 → an Ephinea `.rdata` table:

```
[0x79238114] 334.0   ← C2 (added)
[0x79238118] 353.0
[0x7923811C] 378.0
[0x79238120] 384.0
[0x79238124] 400.0
[0x79238128] 480.0   ← C1 (subtracted)
[0x7923812C] 576.0
[0x79238130] 640.0
```

- **C1 = 480.0** (operand `0x53176128`, subtracted)
- **C2 = 334.0** (operand `0x53176114`, added)
- `design_h` (`0x5318d124`) = `0x43F00000` = **480.0** at the front-end
- `_DAT_53176060` = **15.0**

⇒ **Ephinea `Y = design_h − 480 + 334 = design_h − 146`.**

---

## 4. Y verdict — port was off by +150

| | Formula | @ design_h = 480 |
|---|---|---|
| **Ephinea** | `design_h − C1 + C2` = `design_h − 146` | 334 |
| **Port (before fix)** | `184 + (design_h − 480)` = `design_h − 296` | 184 |

**`C1 − C2 = 146`, NOT 296.** The exactness criterion (`C1 − C2 == 296`) fails. The port used the **stock construction base** `orig_xy[1] = 184` (from `0x972DEC {385,184}`), but Ephinea uses its **own .rdata base C2 = 334**. The window was placed **150px too high** at all hud-scales. Note Ephinea also re-bases at 4:3 (design_h=480 → Y=334, *not* the stock 184) — it does **not** no-op at 4:3.

**Fix (shipped):** `newy = dh − 480.0f + 334.0f;` (= `dh − 146`). ✅ Correct.

---

## 5. X verdict — UNSAFE as shipped ⚠️

Ephinea replaces the fixed design-X 385 with a **verbatim live deref** `scratch.X = *(float*)0x00973CEC` (`_DAT_5318d10c` resolves to the PsoBB.exe global `0x00973CEC`). Structurally this is a deliberate widescreen substitution → **materially intended to shift the X in widescreen** (else the substitution would be pointless). The port dropped it and kept 385; the shipped fix instead derefs `0x00973CEC` with a guard.

### The bug

`0x00973CEC` in **our build (and Ephinea) at the front-end** is **entry +0x2C of a 24-slot table at `0x00973CC0` of CODE addresses** — *not* a coordinate. Live dump (byte-identical between our psobb.io and Ephinea):

```
00973CC0: 00751041 00751041 007510c8 00750f5b
00973CD0: 00750fde 00750eea 00750e95 00750e8d
00973CE0: 00750dbb 00750e2b 00750cf5 00750d4b  ← [0x00973CEC] = 0x00750d4b
00973CF0: 00750c45 00750cc2 00750bd6 00750b68
00973D00: 0075...
```

Disassembling the entries (`0x00750d4b`, `0x00750cf5`, …) shows **mid-function switch/jump-table labels** (`mov eax,[ebp-0x14]; mov edx,[eax+0x34]; …`), not function prologues — i.e. `0x00973CC0` is a dispatch/jump table. (It is runtime-written: `0x00750C8B` during boot → `0x00750d4b` settled.)

Therefore `*(float*)0x00973CEC` = a `0x0075xxxx` code address reinterpreted as float ≈ **1.08e-38** (a tiny positive denormal). The shipped guard:

```c
uint32_t lxbits = *(uint32_t*)0x00973CEC;     // = 0x00750d4b
float lx; memcpy(&lx, &lxbits, 4);            // ≈ 1.08e-38
if (lx > -3000.0f && lx < 5000.0f)            // 1.08e-38 passes BOTH → TRUE  (BUG)
    g_lwy_scratch[0] = lxbits;                // scratch.X ≈ 0.0 → window snaps FAR-LEFT
```

**The guard does not reject code pointers.** Its comment assumes "code-pointer reads as ~1e32" — wrong; `0x0075xxxx` reads as ~1e-38, which passes `< 5000.0f`. So if a bank/shop/quest window constructs in-game while `0x00973CEC` holds a code pointer, **scratch.X collapses to ≈0 (far-left) — a regression vs the stock 385**, not a no-op.

### Unresolved

`0x00973CEC` is runtime-written, so it is *conceivable* it is repurposed to a real coordinate float once the in-game list-window subsystem runs. **This could not be confirmed:** no PSO server was reachable (both psobb.io clients stall at the Login menu, nothing listening on common server ports), and Ephinea's title reads DirectInput so the agent-less foreign build can't be driven in-game. The single missing datum is **`*(float*)0x00973CEC` in a 16:9 in-game bank/shop window**.

### Recommendations

1. **Harden the guard now** (safe, one-line) so the worst case is stock-385, never X→0:
   ```c
   if (lx >= 10.0f && lx < 5000.0f)   // reject the ~1e-38 denormal / code-ptr; optionally && isnormal(lx)
   ```
2. **Re-validate the address.** Read `0x00973CEC` in a live 16:9 in-game bank/shop window:
   - If it is a plausible screen-X → the live-X path is correct; keep it (with the hardened guard).
   - If it stays a code pointer → the port **mis-mapped Ephinea's X global** to our build; find our build's true viewport-left global, or **drop the live-X and keep stock 385**.

---

## 6. Address / evidence reference

| Symbol | Decompile VA | Live (this Ephinea session) | Value / meaning |
|---|---|---|---|
| `FUN_52d8cb90` | `0x52d8cb90` | `0x78E4DEF0` | the handler |
| detour site | — | PsoBB `0x0073FF92` | `E8`→handler (Ephinea) / `E8`→ASI shim (ours) |
| stock InitBase | — | PsoBB `0x00737F80` | stores `{X→9F3130, Y→9F3134, id→9F3128}` |
| stock dims | — | PsoBB `0x00972DEC` | `{385.0, 184.0}` |
| C1 | `0x53176128` | `0x79238128` | **480.0** (subtracted) |
| C2 | `0x53176114` | `0x79238114` | **334.0** (added) |
| design_h | `0x5318d124` | `0x7924F124` | 480.0 (front-end) |
| X-ptr `_DAT_5318d10c` | `0x5318d10c` | `0x7924F10C` → **`0x00973CEC`** | jump-table slot (NOT a coord, front-end) |
| clamp ptr `_DAT_5318d108` | `0x5318d108` | → `0x00972FB0` | live-bounds |
| clamp ptr `_DAT_5318d114` | `0x5318d114` | → `0x00972F80` | live-bounds |
| `_DAT_53176060` | `0x53176060` | `0x79238068` | 15.0 |
| scratch out | `0x5382fe60/64` | `0x798F1E60/64` | substituted `{X,Y}` |

Cross-refs: psoharness memory `listwindow-yanchor-ephinea-verified-offby150`; `ephinea-dll-qol-reference-source`; `ingame-scaling-already-matches-ephinea`.
