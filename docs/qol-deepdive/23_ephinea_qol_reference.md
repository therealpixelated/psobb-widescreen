# 23 — Ephinea as the QoL Reference (`ephinea.dll` RE)

> **Why this section exists.** Ephinea is the most polished PSOBB QoL client in the wild — widescreen,
> high-res HUD, virtual-fullscreen, **a working FFmpeg-decoded intro FMV**, skip-logos, AA/AF, the
> d3d8→d3d9/dgVoodoo/dxvk backend chain, scale toggles. Rather than re-derive every technique, **RE
> how Ephinea applies its QoL and mirror the good parts.** The user's directive: *"dig into the
> ephinea.dll … so we can find out how they apply their QoL"* and *"they do the ffmpeg thing, for
> example."* This is a **primary reference source for P1**, alongside anzz1 (§03/§20).

> **Legal/scope note:** this is clean-room *reference* RE to learn technique for our own additive
> QoL plugins — not redistribution of Ephinea's binaries or assets. We ship our own code/assets.

---

## 23.1 The Ephinea client at a glance (what's on disk)

Install: `C:/Users/u03a9/EphineaPSO/`. The QoL is split across a few binaries:

| Binary | Size | Role | Decompiled? |
|---|---|---|---|
| `PsoBB.exe` | ~6.98 MB | the game (same Sega/BB lineage as psobb.io) | native |
| **`ephinea.dll`** | **12.6 MB** | **the QoL/widescreen/FMV patch DLL** — loads into PsoBB.exe; **PACKED** (`.banana` section), unpacks at runtime; widescreen handlers at `FUN_52d8xxxx` | **only via live dump** (see 23.3) |
| `patchclient.dll` | — | the **updater** (file patching); exported `PatchDLL_*` funcs called by the launcher | native |
| `online.exe` / `option.exe` | 0.64 MB / — | WPF launcher + options dialog (sets flags, launches the game) | **yes → `C:/tmp/ephinea_decompile/`** |
| `dinput8.dll` | — | the ASI/loader shim + input gate (`eph_input_gate.log`) | native |
| `dgVoodoo_d3d9.dll`, `dxvk_d3d9.dll`, `dxdg/*` | — | the D3D backend chain (§06) | n/a |
| `data/opening_j.sfd` | — | **the classic-intro FMV asset** (CRI Sofdec/SFD video) | asset |
| `data/data.gsl` | 71.78 MB | PSO asset pack (NOT a codec — debunks an old "73 MB avcodec" claim) | asset |

The decompiled **launcher** at `C:/tmp/ephinea_decompile/` (`online/online/MainWindow.cs` 153 KB,
`EphineaPatch.cs`, `Nutella.cs`, plus the `option/` C++/CLI options app) is the **control surface** —
it shows every QoL knob and how the launcher hands off to the native code, but the *implementation*
lives in the packed `ephinea.dll`.

---

## 23.2 The QoL knob surface (from the launcher decompile — evidence)

`MainWindow.cs` string table + handlers expose the full Ephinea QoL menu. These are the *features to
study*; each is implemented in `ephinea.dll` (or the engine via a flag it reads).

| Knob (launcher string) | Backing flag / mechanism | Our analogue |
|---|---|---|
| Windowed / **Virtual Fullscreen** | window restyle | our `Windowed=2` borderless (§07) |
| **HIGH RES HUD** | HUD scale path in `ephinea.dll` | our HudScale (P-LAST) — **study this for the scale model** |
| **CLASSIC INTRO FMV** | reg `HKCU\Software\SonicTeam\PSOBB\Ephinea\CLASSIC_INTRO` (DWORD); `Nutella.ClassicIntro`; `Classic_Intro_Click` toggle | our char-create/intro video (P3, §22 §4.11.1) — **study the FMV path** |
| **SW VIDEO DECODE** | `LS_SwVideoDecode` — software-decode toggle for the FMV | confirms a software (ffmpeg/libav) decoder, not hardware/Bink |
| NO DAMAGE SCALE / **NO MINIMAP SCALE** | engine scale opt-outs | per-element scale gating (relevant to P1 scale-notes) |
| SKIP LOGOS / FAST STARTUP | boot-path skips | our boot splash (P3, §13/§9.5) |
| Direct3D API / AA / Anisotropic | backend + sampler state | §06 backend chain |
| IME / DISABLE SKIN SFX / LOW PERFORMANCE | misc | — |

> **Hand-off pattern:** the launcher writes registry flags under
> `HKCU\Software\SonicTeam\PSOBB\Ephinea`, then launches `PsoBB.exe`; `ephinea.dll` (loaded via the
> `dinput8.dll` shim) reads those flags at startup and applies the corresponding patch. This is the
> same shape as our ASI reading `pso_widescreen.ini` — confirm the exact reg keys by RE.
> The launcher↔updater boundary is `EphineaPatch.cs` → `patchclient.dll` (`PatchDLL_PatchLoop`,
> `PatchDLL_ConnectServer`, `PatchDLL_SetCurrentPath`, …) — that's the file updater, **not** the QoL.

---

## 23.3 RE'ing the packed `ephinea.dll` (live dump — the method)

`ephinea.dll` is **packed**: on disk its symbols/code are encrypted in a `.banana` section and only
materialize after the runtime unpacker decrypts them into the image. **Static analysis of the on-disk
file is near-useless** (string scan finds only packer noise). You must dump from a **live** process.

**Prior artifacts already exist (start here, don't redo):**

| File (in `psoharness/`) | What it is |
|---|---|
| `_ephinea_dump.bin` (1.5 MB) | the unpacked region (where ffmpeg + handlers live) |
| `_ephinea_code.bin` (512 KB) | the code subset of that region |
| `_ephinea_decompiled.c`, `_ephinea_decompile.txt` | prior decompile output |
| `_ephinea_hooks.txt` | the detour/hook map (`FUN_52d8xxxx`) |
| `_ephinea_delta.csv`, `_ephinea_only_gap.json` | IO-vs-Ephinea diffs (widescreen) |
| `_ephinea_u03a9_engine_blob.json` | engine-state blob |
| `_re/ffmpeg.md`, `_re/ffmpeg_adversarial_verify.md` | the verified FFmpeg findings (23.4) |

**Live-dump runbook (do when prior artifacts are stale):**
1. `mcp__psoharness__launch_ephinea {}` — starts Ephinea via its own auto-login (separate window).
   This loads + **unpacks** `ephinea.dll` in memory.
2. `mcp__psoharness__modules {pid:<eph>}` (or `read_memory`) → get `ephinea.dll` **base** (historically
   `~0x52D80000`; handlers appear as `FUN_52d8xxxx`) and size.
3. Dump the unpacked image: `read_memory` the module range in chunks → reassemble; or attach r2 to the
   live pid. Focus regions: handlers `0x52D8xxxx`; the embedded-ffmpeg blob (unpacked file offsets
   ~`0xEBDA8`–`0x17AD0C`).
4. Analyze with r2/ghidra against the **dumped** image (load base = the live base). Diff vs psobb.io
   for the widescreen handlers (`_ephinea_delta.csv` is the prior diff).

> Gotchas: the unpacker may re-pack or checksum — dump *after* the title screen is up. The
> `dinput8.dll` shim + `eph_input_gate.log` indicate an input gate that may interfere with our
> harness injection; treat Ephinea as **read-only reference** (don't inject our agent into it).

---

## 23.4 The FFmpeg FMV path — *they do the ffmpeg thing* (verified)

From `_re/ffmpeg.md` (static scan, 2026-05-28) + the launcher knobs:

- **FFmpeg IS used, statically linked + packed inside `ephinea.dll`.** No separate `av*.dll`/`sw*.dll`
  anywhere in the tree (the old "73.5 MB avcodec sidecar" claim is debunked — that was `data.gsl`).
- The codec strings only appear in the **unpacked** dumps: `_ephinea_dump.bin` (~95 ffmpeg hits),
  `_ephinea_code.bin` (~37). The ffmpeg blob sits at unpacked offsets `0xEBDA8`–`0x17AD0C`, a
  **different** region from the `0x52D8xxxx` widescreen handlers → **FMV and widescreen are
  orthogonal subsystems** in ephinea.dll.
- Decoder coverage seen in the dump: video incl. **Smacker/Bink**; audio incl. **CRI ADX/HCA**
  (SEGA/CRI formats), Smacker/Bink audio, AAC/AC3/MP3/Vorbis/Opus/PCM/ADPCM/RealAudio — i.e. a broad
  libav build, not a single-codec shim.
- **The asset:** the classic intro plays **`data/opening_j.sfd`** — a **CRI Sofdec (.sfd)** movie (the
  original PSO intro). The **CLASSIC INTRO FMV** toggle gates it; **SW VIDEO DECODE** picks the
  software path. So Ephinea's "FMV" = *decode `opening_j.sfd` via the embedded ffmpeg and blit it as a
  fullscreen video layer at boot.*

### What this means for OUR video features (P3, §22 §4.11.1)
- **Reconciles the §13/§9.3 "BB has no native video to JMP" finding:** correct — Blue Burst *stripped*
  the native Sofdec intro; **Ephinea re-added it** as an FFmpeg-backed decoder in `ephinea.dll`. So
  the right architecture for us is **additive** (mirror Ephinea), not "detour a native decoder."
- **Reference implementation to study:** dump ephinea.dll's ffmpeg region + find the call site that
  (a) opens `opening_j.sfd`, (b) decodes frames, (c) blits them as a fullscreen quad through the
  present/device path, (d) honors a skip key. That is *exactly* the shape of our
  `pso_charcreate_video-asi` (task #18) and `pso_bootsplash` — only the trigger differs (we hook the
  char-create scene start, §22 §4.11.1, instead of boot).
- **Format choice:** Ephinea uses `.sfd` (Sofdec) because it's the original asset; we are free to use
  `.mov`/`.mp4` (our own asset) since we ship our own embedded/forwarded ffmpeg. The *technique*
  (embedded libav → decode → fullscreen blit → skip gate) is what we copy.

---

## 23.5 The widescreen handlers (`FUN_52d8xxxx`) — reference for P1/P2

`ephinea.dll`'s widescreen lives in ~74 detour handlers at `0x52D8xxxx` (`_ephinea_hooks.txt`). Unlike
anzz1 (a pure static `.data`/`.text` bake, §03), Ephinea uses **runtime detours** — closer to our ASI
model. The prior IO-vs-Ephinea diff (`_ephinea_delta.csv`, `_ephinea_only_gap.json`) is the starting
map. **For P1**, cross-reference each on-screen draw path (§22 §4) against the Ephinea handler that
touches the same engine site — Ephinea's handler shows the *intended* corrected coordinate/scale,
which is the answer key for our re-anchor/scale-note decisions.

> Caution (already learned, see `ws-diff-report-is-frontend-read-artifact`): a naive IO-vs-Ephinea
> diff taken with our build at the front-end is a **front-end-read artifact** (we're native by design;
> Ephinea scales its front-end). Always diff in the **same game state** (both in a lobby, §22 RB-4).

---

## 23.6 Workstream: "RE Ephinea QoL" (how to use this section)

1. **Refresh the dump** (23.3) if `_ephinea_*` artifacts predate the current Ephinea version.
2. **FMV (P3):** locate the `opening_j.sfd` open + decode + blit + skip-gate call chain in the ffmpeg
   region; document it in §22 §4.11.1; mirror the architecture in `pso_charcreate_video-asi`.
3. **Widescreen/anchoring (P1/P2):** map each `FUN_52d8xxxx` handler to the engine site + the §22
   draw path it corrects; use as the answer key for our re-anchor math.
4. **Scale model (P-LAST):** study the **HIGH RES HUD** / **NO MINIMAP SCALE** / **NO DAMAGE SCALE**
   handlers for how Ephinea gates scale per-element — directly informs our §22 §6 scale-note matrix.
5. **Backend (§06):** confirm the d3d8→d3d9 + dgVoodoo/dxvk selection logic (launcher `Direct3D API`
   knob → which DLL).

### Verification
- Every claim about `ephinea.dll` internals must come from a **live-process dump** (23.3), not the
  packed on-disk file. Cite the dump file + offset/VA.
- Cross-check launcher behaviour against the decompile at `C:/tmp/ephinea_decompile/`.
- Treat Ephinea as **read-only** — never inject our agent into it; observe + dump only.

### Open questions
1. Exact registry keys `ephinea.dll` reads (CLASSIC_INTRO confirmed; HIGH_RES_HUD / scale flags TODO).
2. The `opening_j.sfd` decode→blit call chain VA in the unpacked dump (the FMV answer key).
3. Current `ephinea.dll` load base + whether it's ASLR-randomized (historically `0x52D80000`).
4. Whether the input gate (`dinput8.dll` shim / `eph_input_gate.log`) blocks harness observation.
