## Appendix A — Cascade & per-draw detour deep-dives


### FUN_52da9930

**Role:** Stage A of the Ephinea widescreen runtime cascade = the master CONFIG/REGISTRY LOADER and cascade entry point. It is NOT itself a render-size or anchor calculator. It (1) opens the HKLM Ephinea-options key, reads ~55 DWORD config knobs via RegQueryValueEx, clamps each to range, writes them to ephinea.dll .data globals; (2) computes live displayW (_DAT_5318d210)/displayH (_DAT_5318dcdc) from registry resx/resy values, falling back to a 26-entry float resolution table at 0x53146240 when those are missing/equal/inverted; (3) applies the SSAA/supersample multiplier (_DAT_5387b938 = 2 or 4) to displayW/H, snapshots integer dims to _DAT_5388eca8/_DAT_53a9893c, and redirects four PsoBB.exe engine read sites (0x0082D140, 0x0082D18A, 0x008388F9, 0x00838905) to those snapshots; (4) opens the HKCU key for the windowed toggle (_DAT_5388fe08) and the co-op player struct at 0x53a90268 (count clamped <=10); (5) RegCloseKey both; (6) TAIL-CALLS Stage B (FUN_52dabbd0) at 0x52DAB9BB right after the final RegCloseKey. Returns a bitmask of registry-read error flags (local_14). Re-reads displayW/H fresh each call, so a config-reload re-invocation re-derives the whole widescreen pipeline.

**Formula:**
```
displayW = resx + roundfix(resx)   [reg val 0x5313f7bc, stored float _DAT_5318d210]
displayH = resy + roundfix(resy)   [reg val 0x5313f7d8, stored float _DAT_5318dcdc]
  roundfix(n) = *(double*)(&DAT_531761e0 + (n>>31)*-8)  // MSVC signed int->double correction (+0 if n>=0, +2^32 if n<0)

FALLBACK when (displayW==sentinel_0x53175df0 || displayH==sentinel || displayW<displayH):
  idx = clamp(reg[0x5313f7f8], <=0x19) -> _DAT_5388f988
  displayW = *(float*)(0x53146240 + idx*8)   // entry .W
  displayH = *(float*)(0x53146244 + idx*8)   // entry .H   (stride 8 = {float,float}, 26 entries)

SNAPSHOT + ENGINE REDIRECT:
  _DAT_5388eca8 = (int)displayW ; _DAT_53a9893c = (int)displayH
  *0x0082D140 = *0x008388F9 = &_DAT_5388eca8   // engine displayW read sites
  *0x0082D18A = *0x00838905 = &_DAT_53a9893c   // engine displayH read sites

SSAA APPLY (if _DAT_5387b938 != 0, factor 2 or 4):
  displayW *= _DAT_5387b938 ; displayH *= _DAT_5387b938
  _DAT_5318dccc = (int)displayW ; _DAT_5318decc = (int)displayH

SSAA factor select: aaIdx = clamp(reg[0x5313f324], 2..0x13) with {3->2, 6->5}; if aaIdx>4 a switch maps to _DAT_5387b938 (2|4) and/or _DAT_53884bd8 (MSAA 1..4) and resets _DAT_53a9895c per case.

TAIL-CALL Stage B: FUN_52dabbd0() consumes displayW/displayH -> gameRenderW=_DAT_5318deec, gameRenderH=_DAT_5318d124=480*S.

Return = local_14 bitmask (bit0=key open fail, bits1/2/3=specific value-read fails).
```

**Constants:** RESOLVED vs UNRESOLVED status of every constant Stage A touches:

VERIFIED FROM DECOMPILE (literal in code): HKLM/HKCU root 0x80000001, sam 0xf003f=KEY_READ; cbData 4 (DWORD reads), 0xa0 (two REG_SZ reads at val 0x5313fab8/0x5313fae0 -> FUN_52dd3f10 into 0x5382eb84/0x5382eb74), 0x24 (player struct). Clamp bounds all literal, e.g. SSAA idx 2..0x13 (3->2,6->5); HUD scale 0..4 (val 0x5313f26c); display mode 0..7; brightness 0x19..300 default 100 (val 0x5313f8e0); FOV/gamma 0..1000 (val 0x5313f8fc); player count <=10.

REFERENCED-ONLY / UNRESOLVABLE from the available static dump (Phase-1 warning CONFIRMED): every _DAT_5317xxxx/_DAT_5318xxxx .rdata scalar. I attempted resolution in ephinea_unpacked_base0x78480000.bin using file_va = liveVA-0x52d80000+0x78480000. THE LINEAR MAP IS INVALID FOR .data/.rdata: at file_va 0x78546240 (the supposed 0x53146240 res table) radare2 finds x86 CODE (movups/movss bytes 0f14e2/0f11...), not float pairs; the 0x531761e0 fixup region likewise decodes as code (8b4dec e8...). The unpacked bin is a proper PE (MZ, base 0x78480000, sections sect_0 .text 0x78481000, .banana packed 0x7b2c3000) whose section RVA layout != the live runtime layout the 0x52d8xxxx decompile addresses were captured at. The 1.5MB _ephinea_dump.bin covers only ~code, not 0x53146xxx/0x53175xxx .rdata. So NO resolution-table, breakpoint-ladder, or aspectMult values are confirmable from the artifacts.

TRUSTED (RPM-only, PATCHES_OVERVIEW.md:347 + stage_b_render_size.c, LOW confidence): Stage-B constants Stage A's output feeds: 0x53176128=640, 0x53176120=480, 0x53175e8c=4/3, 0x53175f20=2, 0x53175e64=1.0, 0x53175fc0=100; aspect ladder {1.30,1.45,1.60,1.70,1.85,2.00,2.35} -- all annotated 'exact RPM-only' (NOT verified from binary). The fixup &DAT_531761e0+(n>>0x1f)*-8 is the standard MSVC signed int->double correction (+0.0 for n>=0, +2^32 for n<0); structurally certain, byte-values unconfirmed.

Resolution fallback table 0x53146240: 26 entries x 8 bytes {float W, float H}, idx 0..0x19. Values UNRESOLVED.

**PsoBB VAs it reads/writes (21):** PsoBB 0x0082D140 = ptr store &_DAT_5388eca8 (engine displayW read-site redirect), PsoBB 0x0082D18A = ptr store &_DAT_53a9893c (engine displayH read-site redirect), PsoBB 0x008388F9 = ptr store &_DAT_5388eca8 (2nd displayW read-site), PsoBB 0x00838905 = ptr store &_DAT_53a9893c (2nd displayH read-site), ephinea _DAT_5318d210 = displayW (float), ephinea _DAT_5318dcdc = displayH (float), ephinea _DAT_5388eca8 = int displayW snapshot, ephinea _DAT_53a9893c = int displayH snapshot, ephinea _DAT_5318dccc / _DAT_5318decc = int SSAA-scaled W/H, ephinea _DAT_5388f988 = resolution-table index (clamp <=0x19), ephinea _DAT_53a9895c = SSAA/AA quality (clamp 2..0x13, 3->2,6->5), ephinea _DAT_5387b938 = SSAA factor 2 or 4 (multiplies displayW/H), ephinea _DAT_53884bd8 = MSAA 1..4, ephinea _DAT_53870578 = render-path enable + _DAT_5382fe84/88/8c/90/94 flag group, ephinea _DAT_5388fe08 = windowed toggle (HKCU val 0x53140018 ^1), ephinea _DAT_53a9027c = co-op player count (<=10); _DAT_53a90268 = player struct base, ephinea _DAT_5388f9c0 = HUD scale 0..4; _DAT_5318e940 = display mode 0..7, ephinea _DAT_5382fe9c = HUD-scale percent 100..0x1c2 (consumed by Stage B), ephinea _DAT_53857a5c = widescreen-enable (consumed by Stage B); _DAT_5318d1e4 = brightness 0x19..300, ephinea READ .rdata (referenced-only, unconfirmable): res table 0x53146240/0x53146244, int tables 0x53143f60 & 0x53146308, neg-int fixup 0x531761e0, default 0x53175e64, 0x531760b4, 0x53175ff8, sentinel 0x53175df0, threshold 0x53176138, ~40 more non-display knobs (vsync 0x5318eed4, frame limit 0x5318f240, tex filter 0x5318dcfc, mipmap 0x53835c5c, fullscreen 0x5318eefc, gamma 0x5318d34c, audio/input/network) = non-visual, classified ui_misc/audio/coop_net/input


### FUN_52dabbd0 — Stage B (render-size compute + 7-level aspect-breakpoint ladder + title rune-emblem master-scale)

**Role:** widescreen_rendersize — Stage B of Ephinea's 3-stage runtime widescreen cascade (A=FUN_52da9930 config/res load → B=FUN_52dabbd0 THIS → C=FUN_52da7ff0 bulk anchor apply). Stage A tail-calls Stage B at 0x52DAB9BB right after RegCloseKey, having freshly loaded live displayW=_DAT_5318d210 / displayH=_DAT_5318dcdc from the registry (or res-table) with SSAA applied. Stage B's job: (1) aspect = displayW/displayH; (2) walk a 7-level nested-<= breakpoint ladder to select an aspect-multiplier bucket (writes scratch _DAT_5318d21c/_5318da7c/_5318f038=aspectMult + scale S=local_8); (3) derive S (=1.0 if widescreen OFF _DAT_53857a5c==0; =HUDScale/100 if ON and _DAT_5382fe9c!=0; else the bucket-selected scale); (4) write the engine render-size globals gameRenderW=_DAT_5318deec=(640*S*aspectMult)/(4/3) and gameRenderH=_DAT_5318d124=480*S; (5) write legacy scale _DAT_0097A910=dVar3/100; (6) call func_0x524b7c70 twice (x87 helper) → _DAT_5318efa4 & _DAT_5318ee38; (7) the load-bearing-for-us tail: multiply the TITLE-SCREEN RUNE/SEAL EMBLEM levers by S; (8) store master scale _DAT_5387b918=S. Output consumed by FUN_52dac110 (per-device VB anchor patch, guarded renderH!=480) and Stage C bulk loops (key off renderW-640 / renderH-480).

**Formula:**
```
aspect = displayW / displayH
S (master scale) = widescreenOFF ? 1.0
                 : (HUDScale!=0 ? HUDScale/100 : bucketScale)
aspectMult = ladder_select(aspect) over 7 ascending breakpoints; the sub-widescreen (<bp0) leaf returns 4/3 (identity)
gameRenderW = _DAT_5318deec = (640 * S * aspectMult) / (4/3)   [equivalently 480 * S * aspectMult]
gameRenderH = _DAT_5318d124 = 480 * S
_DAT_0097A910 = dVar3 / 100, where
   dVar3 = widescreenOFF ? (displayH/480)*K1*(_5318d1e4 + signfix(_5318d1e4))
                         : (displayH/(S*_53175fd0))*K1*(_5318d1e4 + signfix(_5318d1e4))   [K1=_53175ef8]
_DAT_5318efa4 = f(_531760bc * S);  _DAT_5318ee38 = f((_531760bc * gameRenderW)/640)   [f = func_0x524b7c70, x87 helper]
{0x0096E114, 0x0096E168..17C(6), 0x006F49FD, 0x006F4A57} *= S    (title rune/seal emblem)
_DAT_5387b918 = S
PARITY NOTE (PATCHES_OVERVIEW.md:96-98): the (640*S*aspectMult)/(4/3) form == the port's 480*S*aspectMult == 640*S*(aspect/(4/3)) ONLY under the live constants 640/480/(4/3); equality rests on RPM-only literals.
```

**Constants:** UNRESOLVED FROM STATIC ARTIFACTS (Phase-1 caveat confirmed). The _DAT_5317xxxx / _DAT_5318xxxx float/double pool that Stage B reads lives in ephinea.dll's runtime-unpacked .data/.rdata at 0x5317xxxx, which is NOT physically present in the unpacked dump. I verified this with radare2 on unpacked/ephinea_unpacked_base0x78480000.bin (PE32, baddr 0x78480000): mapping live 0x5317xxxx → file 0x7887xxxx lands inside sect_0 which is the CODE section (bytes at 0x78875e8c decode to x86 'd0 83 c4 0c 8b 46 40 85 c0...', not floats). Value-searched the whole 52MB file for the IEEE-754 doubles 640.0 (0000000000008440), 480.0 (..7e40), 4/3 (555555555555f53f), 100.0 (..5940) → ZERO hits. So the packer leaves these data pages uninitialized on disk; they are populated only at runtime. func_0x524b7c70 is likewise unmapped (0x77BB7C70 = 0xFF padding) → its identity (x87 floor/ceil/sqrt-class CRT helper, returns float10) is inferred, NOT byte-confirmed. The EPHINEA_CASCADE_RPM.md doc that holds the exact breakpoint ladder values (per PATCHES_OVERVIEW.md:416, cited as '1.30/1.45/.../5.25' and the aspectMult table) is NOT in the allowed artifact tree. Per RULES (no live process), I cannot RPM-confirm. STRUCTURALLY-VERIFIED literal map (from PATCHES_OVERVIEW.md:347-348, structure-verified / literals RPM-only): _DAT_53176128=640.0, _DAT_53176120=480.0, _DAT_53175e8c=4/3, _DAT_53175f20=2.0, _DAT_53175e64=1.0, _DAT_53175fc0=100.0. The 7 breakpoints are ascending and the <ee8 leaf's aspectMult=_53175e8c=4/3 (identity). All other bucket consts (_53175ea0, _53175ea4, _53176000, _53176044, _5317607c, ...) are referenced-only / unconfirmable. CONFIDENCE: structure HIGH (decompile + 2 ports + stock source agree); literal values LOW (RPM-only, marked unconfirmable).

**PsoBB VAs it reads/writes (14):** READS: displayW=_DAT_5318d210 (registry/res W from Stage A), READS: displayH=_DAT_5318dcdc (registry/res H from Stage A), READS: widescreen-enable flag _DAT_53857a5c (0=off), READS: HUDScale percent _DAT_5382fe9c (0=use bucket scale), READS (breakpoints, doubles): _DAT_53175ee8 < _53175f00 < _53175f10 < _53175f18 < _53175f30 < _53175f40 < _53175f50 (7 ascending aspect breakpoints), READS (bucket consts): _DAT_53175e64(=1.0), _53175e78, _53175e8c(=4/3), _53175ea0, _53175ea4, _53175eb8, _53175f20(=2), _53175f58, _53175fa8, _53175fc8, _53175fd8, _53175ff0, _53176000, _53176044, _53176048, _53176064, _53176070, _5317607c, READS (base): _DAT_53176128(=640), _53176120(=480), _53175fc0(=100), _53175fd0, _53175ef8, _531760bc, _5318d1e4, &DAT_531761e0 (sign-fixup lookup table base, +0/-8 by sign bit), WRITES (scratch/bucket): _DAT_5318d21c, _DAT_5318da7c, _DAT_5318f038 (=aspectMult; also re-read for renderW), WRITES (RENDER SIZE — primary): _DAT_5318deec = gameRenderW; _DAT_5318d124 = gameRenderH, WRITES (legacy scale): _DAT_0097A910 = dVar3/100, WRITES (x87 helper results): _DAT_5318efa4, _DAT_5318ee38 (via func_0x524b7c70), WRITES (master scale): _DAT_5387b918 = S, WRITES (title rune/seal emblem *=S): _DAT_0096e114 (orb size), _DAT_0096e168/16c/170/174/178/17c (6-float orb-offset table), _DAT_006f49fd (seal-outer imm 430.0), _DAT_006f4a57 (seal-inner imm 178.0), DOWNSTREAM CONSUMERS (not written here): FUN_52dac110 reads _5318deec/_5318d124 (guard _5318d124!=_53176120) to patch D3D device-context VB regs; Stage C FUN_52da7ff0 keys bulk loops off (renderW-640)/(renderH-480)


### FUN_52da7ff0

**Role:** Stage C of the Ephinea widescreen runtime cascade (live VA 0x52DA7FF0 in ephinea.dll, live base 0x52D80000). The REAL bulk anchor apply: it takes the render size already computed by Stage B (gameRenderW = _DAT_5318DEEC, gameRenderH = _DAT_5318D124) plus the master scale S = _DAT_5387B918, and STAMPS them into ~700+ engine UI-anchor floats in PsoBB.exe .data via six bulk loops (122/90/35/28/235/12 iters) and ~80 individual scalar geometry writes, THEN tail-calls the 92-VA function FUN_52da9280 at 0x52DA9062, and KEEPS writing ~25 more scalars + a 6-element column array AFTER it returns. It also sets the master window-resize scale 0x0098A320/0x0098A324, re-creates 12 sprite-table objects, runs a per-player split-screen UI rescale (when _DAT_53A9027C>2), does a localization/region pass that selects the string-table pointer written to 0x00841386 (=0x8EC39C default / 0x8F83A8 localized -- the SAME region/connect pointer the harness connect-fix note tracks), and re-arms the per-draw detour layer via FUN_52DC32E0(1). Re-runs on every config/resolution re-apply through the Stage A->B->C chain.

**Formula:**
```
Inputs (from Stage B): renderW = _DAT_5318DEEC = (640*S*aspectMult)/(4/3); renderH = _DAT_5318D124 = 480*S; S = _DAT_5387B918; displayW/H = _DAT_5318D210/_DAT_5318DCDC.
BULK LOOPS:
 L1 (122): *ptr = renderW
 L2 (90):  *ptr = renderH
 L3 (35):  *ptr += (renderW - 640) / 2          // center-X half-delta
 L4 (28):  *ptr += (renderW - 640)               // right-X full-delta (raw)
 L5 (235): *ptr += (renderH - 480)               // bottom-Y full-delta (raw)
 L6 (12):  *ptr += (renderH/2 - _DAT_531760E4)    // center-Y delta
 W-prop (2):  *ptr = round((*ptr/640) * renderW)
 H-prop (20): *ptr = round((*ptr/480) * renderH)
 ushort (12): *ptr = (ushort)((float)*ptr * _DAT_53175E04 * renderH)
SCALAR CLASSES:
 CENTER-X (Trinity MOD_X_C): newX = renderW/2 - K           (e.g. 0x00761CCC = renderW/2 - 210)  [note: decompile shows these via the 009d0040/00761ccc block as renderW/2 - constant]
 RIGHT-X  (Trinity MOD_X_R): newX = renderW - K             (e.g. 0x008F87D4 = renderW - 577_delta; 0x0096F2B8 = renderW - 400_delta)
 WELCOME block: newX = ((g_5318EE38 - Z)/Z + 1) * K   and   newX = ((g_5318EFA4 - Z)/Z + 1) * K   (Z = _DAT_531760BC sentinel)
 CONGRATS X: newX = renderW/2 - (K - oldX) * ((g_5318EFA4 - Z)/Z + 1)
 MASTERS: 0x008FB114 = renderW/2 ; 0x008FB110 = renderH/2 ; 0x008FB10C = oldVal / S
 FINAL right-edge: = renderW + _DAT_53175E44 ; bottom-edge = renderH + _DAT_53175E44 ; screen-safe = displayW/H + _DAT_531760B4
 COLUMN ARRAY (6): 0x009F4564+i*0x30 = i*_DAT_531760A4 + _DAT_53176074
 HUD CLAMP: 0x00A11464 = min(_DAT_53176140, _DAT_53176144 / S)
PER-PLAYER (numPlayers _DAT_53A9027C, p = numPlayers): if p>1: 0x00927318 = _DAT_53176150. if p>2: factor f=_DAT_53175E28; for i in 0..p-3: f = (i/_DAT_53176040 + 1)*f; f = (p-2)*f + 1; scale 5 entries @[0x5318E8D4] *= f; 0x0097F1BC += (p-2); 0x0090235C -= (p-2); 0x00902358 += (p-2).
```

**Constants:** RESOLVED structurally / by formula + Trinity literal comments (design-space stock value each anchor holds): _DAT_53176128=640.0 (STOCK_WIDTH; subtrahend in L3/L4), _DAT_53176120=480.0 (STOCK_HEIGHT; L5), _DAT_53175F20=2.0 (the /2 divisor for center anchors, L3 + congrats), _DAT_53175E64=1.0 (ONE in (x-Z)/Z+1 normalization), _DAT_5318DEEC=renderW (runtime), _DAT_5318D124=renderH (runtime), _DAT_5387B918=S (runtime). Trinity comment literals give stock values: Welcome W=577, Patch bars W=400, Now-loading X=368..624, F12 slide X=650, F9 chat W=624, congrats name X=256/384/128/514, congrats Y=39/319, seal=320/240, Press-Enter X=192, Start menu X=210, Phantasy/Star wordmark 64/320/61/317/576.

NOT RESOLVABLE FROM STATIC ARTIFACTS (confidence=low): the literal float values of the _DAT_5317xxxx .rdata constants (_DAT_531760E4 [L6 center-Y offset], _DAT_53175E44 [HUD-edge pad, ~0], _DAT_531760B4 [screen-safe pad], _DAT_531760A4 + _DAT_53176074 [column-array step+base], _DAT_53176088 [final inc], _DAT_53176140/53176144 [HUD clamp pair], _DAT_53175E04 [ushort H-scale factor], _DAT_53176190 [HUD-edge const], _DAT_53176110, _DAT_53176150 [2P value], _DAT_53176040 [per-player divisor], _DAT_53175E28 [per-player seed], _DAT_531761E0 [int->double bias table]). REASON: the unpacked DLL (llama-bob-ephinea-re/unpacked/ephinea_unpacked_base0x78480000.bin) is a post-.banana-unpack memory image whose .rdata/.data are NOT at standard PE RVAs -- VA-arithmetic (liveVA-0x52D80000+0x78480000, and the PE section-paddr mapping) both yield garbage floats when probed (640/480/2 do not appear at their expected slots). r2 disasm at the computed file VA 0x784A7FF0 shows unrelated SIMD code, confirming the code layout also diverges. These are the same constants PATCHES_OVERVIEW.md flags as 'referenced-only / undumped .rdata -- structure verified, literals RPM-only'. They would need a LIVE read_memory of ephinea.dll (disallowed for this task) to pin exactly.

**PsoBB VAs it reads/writes (12):** LOOP BASES (ptr-tables, each entry points to a PsoBB float): 0x5318EEDC(6 obj), 0x5318F264(6 obj), 0x5318EE3C(12 aspectMult), 0x5318F040(122 =renderW), 0x5318E6E0(90 =renderH), 0x5318E650(35 +=(W-640)/2), 0x5318F2A0(28 +=(W-640)), 0x5318E9C8(235 +=(H-480)), 0x5318EF00(12 +=(H/2-off)), 0x5318F00C(2 W-prop round), 0x5318EF40(20 H-prop round), 0x5318E8FC(12 ushort*H), 0x5318E8D4(5 per-player), READ runtime globals (Stage A/B written): 0x5318DEEC=gameRenderW, 0x5318D124=gameRenderH, 0x5318D210=displayW, 0x5318DCDC=displayH, 0x5387B918=S/HudScale, 0x5318F038=aspectMult, 0x5318D21C/0x5318DA7C=bucket scaleX/Y, 0x5318EE38/0x5318EFA4=Stage-B intermed, 0x5318E8EC=per-player factor, 0x53A9027C=numPlayers, 0x5388ED90=languageId, SCALAR WRITES (Trinity-named): 0x006F4CF2/CFA(Phantasy start X/Y),0x006F4D02/D0A(Phantasy end X/Y),0x006F4D4E/D56/D5E/D66(Star BB start/end X/Y),0x009D0040(Press-Enter X),0x00761CCC/CDE(Start menu X),0x0076236C(Register UID menu X),0x007625F8(UID pw X),0x007627CF(UID user X),0x006F4922/4936(Seal X/Y),0x008F87D4(Welcome W),0x008F87E4(Press-any-key W),0x0096F2B8(Patch bars W),0x007584EE/0x0075851F(F12 name/time slide X),0x00978454/0x00978458(Lobby name/time-in-menu X),0x009712EC(F12 bg bottom Y),0x007165B8(F9 chat W), PATCH-STATUS bars 0x009F0ADC/AF4/B0C/B24/B3C/B5C, 0x0070D350/D2F6(title-bg center W), NOW-LOADING 0x009FF080/098/0B0/0C8/0E0/0F8/110/128/1A8, CONGRATS 0x009CA53C/544/54C/554(name X), 0x0093B8AC/A8(name Y), MASTERS 0x0098A320/0x0098A324(WindowResizeScaleX/Y), 0x008FB114(renderW/2),0x008FB110(renderH/2),0x008FB10C(/=S), 0x00A11464(HUD clamp), PER-PLAYER 0x00927318,0x0097F1BC,0x0090235C,0x00902358, FINAL block 0x0077F2D1/2ED/30D/32C/362/386/3A7/3B2/3C7/3D2, 0x0091FF30/2C/00/FC, 0x00973138/134/130/12C, 0x009F4564/594/5C4/5F4/624/654, 0x009F4654, 0x0096FFDC, SPRITE-ATLAS table 0x009A3840..009A38D8 (24 entries), COLOR 0x009B8DB4/B8/BC/C0/E8/EC (0xFF181818/0xFA181818), NON-WS branch 0x00841386 = 0x8EC39C/0x8F83A8 (region/string-table ptr)


### FUN_52da9280

**Role:** widescreen_anchor -- the load-bearing "92-VA" (really 97 arithmetic writes across 93 distinct VAs) arithmetic-apply LEAF of Ephinea's widescreen cascade. Called exactly once from Stage C (FUN_52da7ff0) at call-site 0x52DA9062, AFTER Stage C's six bulk loops (122/90/35/28/235/12 iters) and ~80 scalar writes. It takes the engine's 4:3 (640x480 design-space) UI-anchor .data block 0x008F9Exx..0x008FA1Dx, plus 12 imm32 operands baked into PsoBB.exe .text (0x0040CAxx / 0x0040CBxx / 0x0040D0xx / 0x0040D1DD), plus one dll-internal scratch 0x5318E980, and rescales each anchor from 4:3 design-space to the live render size (gameRenderW x gameRenderH) so the front-end / char-select / dressing-room / congrats 2D layout fills the widescreen canvas. It is one of the 8 functions that actually drive Ephinea widescreen, but it is NOT the whole apply: Stage C writes ~700+ more globals around this call.

**Formula:**
```
Let W=gameRenderW(_DAT_5318deec), H=gameRenderH(_DAT_5318d124), DW=displayW(_DAT_5318d210), DH=displayH(_DAT_5318dcdc). Constants 640=_DAT_53176128, 480=_DAT_53176120, 2=_DAT_53175f20.

L1 (23):  *v = (W/640) * v
L2 (17):  *v = (H/480) * v
L3 (19):  *v = ((W-640)/2) + v
L4 (20):  *v = ((H-480)/2) + v
L5 (7):   *v = (H-480) + v          (NO /2)

INLINE (11, in body order, lines 132278-132292):
  0x008FA1D8 = (DW/W) * 0x008FA1D8
  0x008FA1D0 = (DH/H) * 0x008FA1D0
  0x008FA1D4 = (DW/W) * 0x008FA1D4
  0x008FA1CC = (DH/H) * 0x008FA1CC
  0x008FA1C4 = (DW/W) * 0x008FA1C4
  0x008FA1C0 = (DW/W) * 0x008FA1C0          (also an L1 member)
  0x5318E980 = (DW/640) * 0x5318E980        (DW/_DAT_53176128; dll-internal scratch)
  0x0040D039 = (DH/H) * 0x0040D039          (.text imm32)
  0x0040D032 = (DH/H) * (0x0040D032 - 480) + DH        (special: -480 then scale then +DH)
  0x0040D1DD = (DW/W) * ( (W-640)/2 + 0x0040D1DD )      (special: L3-add then DW/W scale)
  0x008FA1AC = (DH/H) * 0x008FA1AC          (also an L4 member)

NON-ARITH (2):
  *(u32*)0x0040D04E = &DAT_5318e980          (pointer-store / redirect, NOT arithmetic)
  FUN_52dc3460(0x0040D305, 0xB)              (installs an 11-byte engine .text code patch)

ORDER NOTE: 0x5318E980 is first scaled by (DW/640) in the inline block, THEN its ADDRESS is stored to 0x0040D04E -- the redirect must come AFTER the scale, exactly as the body does.
```

**Constants:** Divisor / scale constants are .rdata/.data globals referenced by the code but living OUTSIDE the 4.9MB code dump (referenced-only). Could NOT resolve their literal float bits from static artifacts: the radare2 MCP server is registered but its tool schemas are not surfaceable via ToolSearch this session (queries 'radare2'/'+r2'/'+radare2' returned no deferred-tool match), so I could not open the unpacked dll (ephinea_unpacked_base0x78480000.bin; file_addr = liveVA - 0x52d80000 + 0x78480000, e.g. 0x53175f20 -> 0x78875f20) to dump them. Values below are INFERRED (confidence=HIGH): the cascade is self-consistent only under them and the port + PATCHES_OVERVIEW.md sec4 #5 independently assert the same:
  _DAT_53176128 = 640.0     (STOCK_WIDTH)
  _DAT_53176120 = 480.0     (STOCK_HEIGHT)
  _DAT_53175f20 = 2.0       (HALF, L3/L4 letterbox divisor)
  _DAT_53175e8c = 1.33333 (4/3)   (Stage B aspect divisor; not read by this fn)
  _DAT_53175e64 = 1.0       (master scale; sibling detours, not this fn)
  _DAT_53175fc0 = 100.0     (HUDScale/100 in Stage B; not this fn)
Live render-size INPUTS are NOT constants -- they vary with resolution/HudScale (gameRenderW/H, displayW/H from Stages A/B). At true 16:9 / HudScale 1.0: displayW=gameRenderW=853.333, displayH=gameRenderH=480, so L1 factor=1.33333, L3 add=+106.667, L4/L5 add=0, every inline DW/W and DH/H ratio=1.0 (inline is IDENTITY at native HudScale; it only diverges when SSAA or non-1.0 HudScale make displayW != gameRenderW). Sample baked results from _ephinea_delta.csv quoted in PATCHES_OVERVIEW: 0x008F9EC0 100->133.333 (L1); 0x008F9F18 31->137.667 (L3, bits 0000f841->acaa0943); 0x008FA124 410->546.667 (L1).

**PsoBB VAs it reads/writes (102):** L1 W-mul (23): 0x008FA154, 0x008FA15C, 0x008FA144, 0x008FA14C, 0x008FA134, 0x008FA13C, 0x008FA124, 0x008FA12C, 0x008FA0F8, 0x008FA098, 0x008FA068, 0x008FA0C8, 0x008FA1A4, 0x008F9EC0, 0x008FA1C0(dual L1+inline), 0x0040CA07(.text), 0x0040CA40(.text), 0x0040CA66(.text), 0x0040CA8C(.text), 0x0040CAE2(.text), 0x0040CB1E(.text), 0x0040CB44(.text), 0x0040CB6A(.text), L2 H-mul (17): 0x008FA148, 0x008FA150, 0x008FA128, 0x008FA130, 0x008FA0F4, 0x008FA0C4, 0x008FA0D4, 0x008FA094, 0x008FA0A4, 0x008FA064, 0x008FA074, 0x008FA044, 0x008FA04C, 0x008FA024, 0x008FA02C, 0x008FA104, 0x008F9EFC, L3 W-add (19): 0x008FA108, 0x008FA110, 0x008FA118, 0x008FA0D8, 0x008FA0E0, 0x008FA0E8, 0x008FA0A8, 0x008FA0B0, 0x008FA0B8, 0x008FA078, 0x008FA080, 0x008FA088, 0x008FA050, 0x008FA058, 0x008FA030, 0x008FA038, 0x008FA1D8(DOC-OMIT; dual L3+inline), 0x008F9F18(DOC-OMIT), 0x008F9F20(DOC-OMIT), L4 H-add (20): 0x008FA10C, 0x008FA114, 0x008FA11C, 0x008FA0DC, 0x008FA0E4, 0x008FA0EC, 0x008FA0AC, 0x008FA0B4, 0x008FA0BC, 0x008FA07C, 0x008FA084, 0x008FA08C, 0x008FA054, 0x008FA05C, 0x008FA034, 0x008FA03C, 0x008FA1D0(DOC-OMIT), 0x008FA1AC(DOC-OMIT; dual L4+inline), 0x008F9F1C(DOC-OMIT), 0x008F9F24(DOC-OMIT), L5 H-add-RAW (7): 0x008FA138, 0x008FA140, 0x0040CB07(.text), 0x0040CB2C(.text), 0x0040CB52(.text), 0x0040CB78(.text), 0x008F9F48, inline (11): 0x008FA1D4, 0x008FA1CC, 0x008FA1C4, 0x5318E980(dll scratch DW/640), 0x0040D039(.text DH/H), 0x0040D032(.text special), 0x0040D1DD(.text special), non-arith: 0x0040D04E(u32 ptr-store=&DAT_5318e980), 0x0040D305(FUN_52dc3460 11-byte code patch), READS: 0x5318deec(gameRenderW), 0x5318d124(gameRenderH), 0x5318d210(displayW), 0x5318dcdc(displayH), 0x53176128(=640), 0x53176120(=480), 0x53175f20(=2)


### FUN_52da6e50

**Role:** widescreen_anchor / aspect_vertex -- the core per-draw 2D-vertex aspect-correct + recenter detour. One of only 4 VISUALLY load-bearing detour handlers (the other ~350 runtime detours are non-visual: localization/coop/input/camera/audio). Installed by FUN_52da6dd0 via FUN_52dc3240(FUN_52da6e50, 0x67c4ed, 5): a 5-byte detour of the engine `call 0x82b558` at site 0x0067C4ED. Runs an aspect-fit/recenter PRE-PASS over a freshly-built packed (x,y) f32 vertex array, then tail-calls the original submit helper 0x0082b558. Site 0x0067C4ED is inside a fullscreen 2D-overlay render (identified as the boss kill-screen / hexagon-layer fullscreen quad). 0x82b558 = RenderUIPrimitive_SetupAndDraw_0082b558 (low-level UI triangle-fan submit).

**Formula:**
```
bVar4 = (gameRenderW / gameRenderH) < (4/3).
IF bVar4 (taller-than-4:3, vertical-letterbox branch): k = gameRenderW / 640; off = gameRenderH/2 - (480*k)/2.
ELSE (wider-than-4:3, horizontal/pillar branch -- the 16:9 case): k = gameRenderH / 480; off = gameRenderW/2 - (640*k)/2.
Per vertex i in [0,vertCount): x_i = x_i * k; y_i = y_i * k; IF bVar4 then y_i = y_i + off ELSE x_i = x_i + off.
Then ((void(*)(int*,u4,u4,u4,int))0x82b558)(param_1, param_2, param_3, param_4, param_1[1]).
Note: 640/2 and 480/2 are written in the decompile as _DAT_53176128/_DAT_53175f20 and _DAT_53176120/_DAT_53175f20, i.e. divide by _DAT_53175f20=2.0 (the same HALF divisor Stage C uses for its L3/L4 half-delta anchors). The center offset is the classic half-margin: off = (longDim - shortStockDim*k)/2.
```

**Constants:** CONFIRMED via cross-reference (Stage B FUN_52dabbd0 usage + clean-port header ephinea_widescreen.h + PATCHES_OVERVIEW L90-97,113-117): _DAT_53176128=640.0f (STOCK_WIDTH); _DAT_53176120=480.0f (STOCK_HEIGHT); _DAT_53175e8c=1.33333f (4/3 ASPECT); _DAT_53175f20=2.0f (HALF divisor). gameRenderW/H are runtime values (=853.333*S / 480*S at 16:9), NOT static. UNRESOLVED-STATICALLY (as PATCHES_OVERVIEW warns): _DAT_5318deec and _DAT_5318d124 are .data slots populated at runtime by Stage B; I attempted to read them from the unpacked dll (file base 0x78480000; file offset = liveVA-0x52d80000) and got garbage (e.g. 5318deec bytes 6a10ff36, 5318d124 bytes 00008d42) -> these slots are uninitialized/zero in the static image, confirming they are referenced-only / runtime-written (cannot byte-verify statically). The .rdata consts 53175e8c/53176128/53176120/53175f20 ALSO read as garbage in the raw unpacked image at their computed offsets (the unpacked dump's .rdata/.data are not laid out at file-offset==liveVA-0x52d80000 for this region, OR the dump is code-only) -- so the 640/480/4-3/2.0 values are established by USAGE/CROSS-REF + Trinity/port, NOT by a direct byte read. Confidence on 640/480/4-3/2.0 = HIGH (three independent corroborations); confidence on exact runtime gameRenderW/H = these are formulas not constants.

**PsoBB VAs it reads/writes (10):** READS _DAT_5318deec = gameRenderW (LIVE design render width; Stage B writes (640*S*aspectMult)/(4/3)); DLL-internal .data, runtime-populated, READS _DAT_5318d124 = gameRenderH (LIVE design render height; Stage B writes 480*S); DLL-internal .data, runtime-populated, READS _DAT_53175e8c = ASPECT_4_3 = 4/3 = 1.33333 (reference aspect, the < comparand); DLL .rdata const, READS _DAT_53176128 = STOCK_WIDTH = 640.0 (width divisor in taller-branch k, and stock-W in wider-branch off); DLL .rdata const, READS _DAT_53176120 = STOCK_HEIGHT = 480.0 (height divisor in wider-branch k, and stock-H in taller-branch off); DLL .rdata const, READS _DAT_53175f20 = HALF = 2.0 (the /2 center divisor); DLL .rdata const, READS+WRITES param_1[0] -> vertPtr: the packed f32 (x,y) vertex array, edited in place (x*=k,y*=k,+=off). This is a PsoBB engine STACK buffer at [esp+0x44..] built per-call by the 0x67C42B loop, NOT a static .data VA, READS param_1[1] = iVar2 (passed as 5th arg to 0x82b558); param_1[3] = iVar3 = vertCount (loop bound), CALLS engine 0x0082b558 = RenderUIPrimitive_SetupAndDraw_0082b558 (original submit, post-transform), INSTALL touches PsoBB .text site 0x0067C4ED (rewrites the 5-byte `call 0x0082b558`)


### FUN_52dc4f60

**Role:** widescreen_anchor / hud_quad (TEXT sub-path). Per-draw DETOUR wrapper that is the front door of Ephinea's reimplemented on-screen STRING/GLYPH rasterizer. Installed by the cascade installer FUN_52dc4bd0 via FUN_52dc3240(FUN_52dc4f60, 0x78A300, 5) = a 5-byte E9 detour at PsoBB site 0x0078A300 (the engine text-draw entry; 0x28 before UpdateJapaneseCharacterDisplayOption_0078A328; the stock text path is the RenderText3D_007AC12C family). It reads the LIVE 2D design-anchor scale (0x00ACC0E8/EC; the global 2D affine SCALE_X/Y), decides whether the glyph batch needs an extra design-anchor multiply, builds an optional scale-pin pointer, sets a 'param6 != sentinel' boolean, and tail-forwards to the real per-glyph layout/raster worker FUN_52dc5b30. One of only 4 VISUAL per-draw detours (with FUN_52da6e50 aspect-vertex, FUN_52d8cb90 ui-edge, FUN_52db2240 deanchor-pin).

**Formula:**
```
PER-CALL PIN (FUN_52dc4f60):
  a = *0x00ACC0E8 ; b = *0x00ACC0EC                          // live affine SCALE_X / SCALE_Y
  sx = sy = M (=_DAT_53175e64 ~ 1.0) ; pin = NONE
  if (M < a && M < b):  sx = M*a ; sy = M*b ; pin = (sx,sy)  // GUARD 1 (widescreen/HudScale up)
  if (T1(=_DAT_53175ec8) < D(=_DAT_5318e34c)): sx*=D ; sy*=D ; pin = (sx,sy)   // GUARD 2 (density/SSAA)
  shadowFlag = (param6 != S(=_DAT_53175df8))
  FUN_52dc5b30(..., shadowFlag, ..., pin)

PER-GLYPH QUAD EXPANSION inside FUN_52dc5b30 (only when pin != NULL && pin.x>T2(=_DAT_53175df0) && pin.y>T2):
  w = x2-x1 ; h = y2-y1
  x2 = x2 + pin.x * w
  y1 = y1 - pin.y * h / K(=_DAT_53175f20)        // K ~ 2.0 -> symmetric vertical growth
  y2 = y2 + pin.y * h / K
(glyph base size/advance come from the font-cell scale _DAT_5318f330/33c = fontHeight/_DAT_5317605c and advance _DAT_5318f324, set by FUN_52dc5570). Net: text grows by ~(1+pin) about its anchor when the global affine is wide; identity (pin=NULL) when affine==1.0.
```

**Constants:** UNRESOLVABLE FROM THE ARTIFACT (confidence=low, stated honestly): every _DAT_5317xxxx / _DAT_5318xxxx literal in this function lives in ephinea.dll's own .rdata/.data, NOT in PsoBB. I attempted to resolve them against the unpacked dll (C:/Users/u03a9/Repositories/llama-bob-ephinea-re/unpacked/ephinea_unpacked_base0x78480000.bin, a real PE rebased to image-base 0x78480000). Mapping liveVA(0x52d80000-based) -> dump RVA (= liveVA-0x52d80000) lands these symbols at RVA 0x3F5E64 / 0x40E34C, which falls INSIDE the dump's .text section (raw bytes decode as code: 8b4d/894d/8b45...), i.e. the decompiler's data base for these symbols is NOT 'liveVA-0x52d80000' relative to THIS dump's 0x78480000 rebase. This exactly matches the Phase-1 warning that _DAT_5317xxxx/5318xxxx 'live in .data/.rdata possibly OUTSIDE the code dump'. radare2 MCP did not surface as a callable tool in this session, so I could not cross-confirm via aflx/px. INFERRED values from code role (confidence=low): _DAT_53175e64 = 1.0f (master/identity scale; both guards compare 'scale < anchor' and multiply by it as a unit); _DAT_53175ec8 and _DAT_53175df0 = small thresholds near 1.0 (activation gates); _DAT_53175f20 = 2.0 (Y pin is split half above/half below the anchor: y1-=p*h/_; y2+=p*h/_); _DAT_5317605c = the reference font-cell height (the divisor making fontHeight/ref == 1.0 at design size; given the 14..72 clamp, ref is most likely ~14.0 or the design cell). param6 sentinel _DAT_53175df8 INFERRED ~0.0f. CONFIRMED non-dll constants: 0x00ACC0E8/EC are the verified PsoBB 2D affine; the install is byte-exact 5-length E9 at 0x0078A300; the DAT_531761e0 reference is the {0.0, 4294967296.0} int->double table (compiler idiom, not a layout const).

**PsoBB VAs it reads/writes (14):** READS 0x00ACC0E8 (2D affine SCALE_X / designAnchorX) — via FUN_52dd2610; CONFIRMED PsoBB VA, READS 0x00ACC0EC (2D affine SCALE_Y / designAnchorY) — via FUN_52dd2610; CONFIRMED PsoBB VA, READS _DAT_53175e64 (master scale, ==1.0f identity) — ephinea.dll .rdata, UNRESOLVED from artifact, READS _DAT_53175ec8 (GUARD-2 threshold for the density/SSAA factor) — ephinea.dll .rdata, UNRESOLVED, READS _DAT_5318e34c (runtime extra-scale/density factor; written by installer FUN_52dc4bd0 L145993 = (float)local_20 from screenW/refW ratio) — ephinea.dll .data, READS _DAT_53175df8 (param6 sentinel for the mono-advance/shadow boolean) — ephinea.dll .rdata, UNRESOLVED, INSTALL TARGET 0x0078A300 (PsoBB text-draw entry; 5-byte E9 detour; site label = just before UpdateJapaneseCharacterDisplayOption_0078A328), WRITES: NONE (FUN_52dc4f60 writes no .data; it only builds a stack-local scaleX/scaleY pin and passes its address), --- downstream FUN_52dc5b30 / FUN_52dc5570 globals (the actual data this path consumes) ---, READS 0x00AAB904/0x00AAB908/0x00AAB90C/0x00AAB910 (text CLIP-RECT L/T/R/B) + 0x00AAB914 (clip enable) + DAT_00AAB91C (glyph color/flags) — PsoBB live globals, READS/uses _DAT_5318f330 (glyph cell scale X) and _DAT_5318f33c (glyph cell scale Y) = fontHeight/_DAT_5317605c — ephinea.dll .data, set by FUN_52dc5570, READS _DAT_5318f324 (glyph advance = (fontHeight+5)*0x10), _DAT_5318f338 (line height), _DAT_5318f340 (half-cell), _DAT_5318e348 (font px height, clamp 14..72) — ephinea.dll .data, READS _DAT_53175df0 (pin activation threshold, pin[i] must exceed it), _DAT_53175f20 (Y half-divisor for the pin expansion), _DAT_5317605c (font reference-cell divisor) — ephinea.dll .rdata, UNRESOLVED, palette 0x0097DA10 (color-escape table) + 0x00A98478 (current-color object) — PsoBB; emitter primitives called by absolute VA: 0x82BB60, 0x7ABA94, 0x7A9044, 0x82DBAC


### FUN_52d8cb90

**Role:** widescreen_anchor — per-draw UI edge re-anchor; specifically the IN-GAME ListWindow bottom-anchor. One of the 8 visual widescreen drivers. Installed by FUN_52d8caf0 as a 5-byte E9 detour at PsoBB.exe site 0x0073FF92 (in _ephinea_hooks.txt: "0x73ff92 handler=FUN_52d8cb90 len=5"). That site is the `call 0x00737F80` inside ListWindowObject_Create_0073ff34, in the SINGLE-PLAYER arm (maybe_num_split_screen_players==1). Original 0x00737F80 = ListWindowObject_InitBase(undefined4* xy, id), which stores xy[0]->[0x009F3130](baseX), xy[1]->[0x009F3134](baseY), id->[0x009F3128]. In that arm the {X,Y} arg is &item_bank_and_quest_list_dimensions (0x00972DEC, design {385.0, 184.0}) = the construction-time base position of the in-game item-bank / bank / shop / quest-counter LIST WINDOW. The detour recomputes Y to track the TALLER widescreen viewport bottom edge (a MOD_Y_B "480-HUD_H" pattern), substitutes a live left-edge X, applies a live-screen-bounds clamp, then forwards a substituted scratch vec2 {x,y'} to the original InitBase. NB: 0x00737F80 / 0x009F3130 / the gate flag 0x00A95EDC are PsoBB.exe VAs; the 0x5317xxxx/0x5318xxxx/0x5382xxxx symbols are ephinea.dll runtime data.

**Formula:**
```
Widescreen ON: Y' = (gameRenderH - 480.0) + Yoffset, gameRenderH = 480.0*S, Yoffset(_DAT_5317610c)=334.0  =>  Y' = design_h - 146 (S = HUDScale/widescreen factor; design_h = 480/720@1.5/960@2.0). X' = *_DAT_5318d10c (live viewport-left). Bounds clamp: if (*_DAT_5318d108 - PAD <= *_DAT_5318d114 + *_DAT_5318d10c) then Y' = (_DAT_5318d108[1] - _DAT_5318d114[1]) - PAD, PAD=_DAT_53176060. Forward {X',Y'} (scratch 0x5382fe60/64) to ListWindowObject_InitBase(0x00737F80). Widescreen OFF: pass original {385,184} unchanged. Sibling X-clamp (FUN_52d8cca0): X = max(srcX, *_DAT_5318d114 + *_DAT_5318d10c + _DAT_5318d11c).
```

**Constants:** _DAT_53176120 = 480.0f (STOCK_HEIGHT) — CONFIRMED: header ephinea_widescreen.h labels it 480.0; corroborated by the Stage-B write `_DAT_5318d124 = _DAT_53176120 * S` (gameRenderH = 480*S). _DAT_5318d124 = gameRenderH = 480.0*S (runtime, not a literal). _DAT_5317610c (Yoffset) = 334.0f — RESOLVED via live read of a running Ephinea (memory listwindow-yanchor-ephinea-verified-offby150, account Zoe); it is entry 0 of an Ephinea .rdata table [334,353,378,384,400,480,576,640]; so widescreen-on Y' = design_h - 480 + 334 = design_h - 146 (Ephinea re-bases at 4:3 too: design_h=480 -> Y=334, NOT stock 184). _DAT_53176060 (clamp PAD) = UNRESOLVED literal — referenced-only, lives in unpacked-dll .data not present as init data. _DAT_5318d11c (sibling X-inset) = UNRESOLVED literal. The three pointer symbols _DAT_5318d108/10c/114 are NOT floats — they are runtime-resolved POINTERS to the live D3D screen-rect (per memory note: _DAT_5318d10c->0x00973CEC, _DAT_5318d108->~0x00972FB0, _DAT_5318d114->~0x00972F80, all PsoBB globals). STATIC-RESOLUTION FAILED: I parsed the unpacked dll PE (C:/Users/u03a9/Repositories/llama-bob-ephinea-re/unpacked/ephinea_unpacked_base0x78480000.bin, imagebase 0x78480000, 10 sections, a .banana packed section present). The data VAs (live 0x5317xxxx/0x5318xxxx -> dll 0x7887xxxx/0x7888xxxx) all map into the FIRST .text section and read as code/garbage, NOT initialized data — confirming Phase-1's warning that these _DAT_5317/5318xxxx live in runtime-populated .data/.bss outside the static dump. radare2 MCP did not surface as a callable tool this session. The only literals I can assert with confidence: 480.0 (subtrahend) and 334.0 (Yoffset, live-verified). 53176060 and 5318d11c remain unconfirmable from the artifacts.

**PsoBB VAs it reads/writes (17):** 0x0073FF92 = detour install site (E9 to FUN_52d8cb90); the ==1-arm `call 0x00737F80`, 0x00737F80 = ListWindowObject_InitBase (the original, called through both branches), 0x009F3130 / 0x009F3134 / 0x009F3128 = InitBase output globals (baseX / baseY / id) written downstream by 0x00737F80, 0x00972DEC = item_bank_and_quest_list_dimensions = the {385.0,184.0} design vec2 passed in the ==1 arm (the xy* source), 0x00A95EDC = _DAT_00a95edc = ephinea widescreen-ACTIVE dynamic flag (PsoBB VA; ==0 -> pass-through), _DAT_5318d124 = gameRenderH (= 480.0 * S; the only widescreen-scaled input; written by Stage-B @ _ephinea_decompiled.c:133478), _DAT_53176120 = 480.0f STOCK_HEIGHT (the subtrahend; const), _DAT_5317610c = Yoffset added after the delta (live-verified = 334.0; entry 0 of an ephinea .rdata table [334,353,378,384,400,480,576,640]), _DAT_5318d108 = pointer to live screen-rect struct (deref [0]=top/origin-Y test value, [1]=bottom in clamp); live VA ~0x00972FB0 per memory note, _DAT_5318d114 = pointer to a second live rect (deref [0] used in bounds test, [1] in clamp); live VA ~0x00972F80 per memory note, _DAT_5318d10c = pointer to live X-base / left viewport edge (scratch.x = *_DAT_5318d10c); maps to PsoBB *(float*)0x00973CEC per memory note, _DAT_53176060 = PAD/inset constant subtracted in the bounds test and clamp (UNRESOLVED literal), _DAT_5318d11c = additive inset used by the SIBLING X-clamp FUN_52d8cca0 (UNRESOLVED literal), _DAT_5318e458 = vtable/fn-ptr indirect-called by sibling FUN_52d8cca0 to fetch the source xy ((*_DAT_5318e458)(param)), DAT_5382fe60 / DAT_5382fe64 = ephinea scratch vec2 {x,y'} (the substituted arg forwarded to InitBase), DAT_5382fe58 / DAT_5382fe5c = sibling scratch vec2 for FUN_52d8cca0, (related, not this fn) FUN_52dac110 @0x52dac110: 0x53146308 = 0x14-entry list-object offset-table base; _DAT_5318deec = gameRenderW; _DAT_5317612c = 640.0 stock-width subtrahend


### FUN_52db2240

**Role:** deanchor

**Formula:**
```
No arithmetic. Pure save/pin/restore bracket:
  saveX = [0x00ACC0E8]; saveY = [0x00ACC0EC]
  [0x00ACC0E8] = 1.0f ; [0x00ACC0EC] = 1.0f        (1.0f sourced from _DAT_53175e64)
  if (arg2 != 0) call 0x0082B158(arg1,arg2,arg3,arg4)
  [0x00ACC0E8] = saveX ; [0x00ACC0EC] = saveY
The "pin" value is a literal 1.0f regardless of HudScale/widescreen factor — the wrapped draws render in native 2D design space; the surrounding wide affine is only restored AFTER the submit. 0x0082B158 (-> emitter 0x00836D04 / RenderUIQuad family) is the function that actually multiplies the affine into the 2D verts.
```

**Constants:** _DAT_53175e64 = 1.0f (the pin value). CONFIDENCE = HIGH but resolved by TRIANGULATION, not by direct dword read. Three independent sources agree it is 1.0f: (1) PATCHES_OVERVIEW.md line 347 lists "0x53175e64=1.0"; (2) the clean port deanchor_pin.c line 23-24 annotates `*(float*)0xACC0E8 = _DAT_53175e64; /* = 1.0f */`; (3) usage semantics across the dump — _DAT_53175e64 is used as a divisor (x/_DAT_53175e64), additive identity (x±_DAT_53175e64), and comparator (_DAT_53175e64 <= val) in aspect/normalize math, all consistent with 1.0f.
RADARE2/RAW-DUMP RESOLUTION = UNRESOLVED/UNCONFIRMABLE from the unpacked dll: the supplied formula file_addr = liveVA - 0x52d80000 + 0x78480000 maps 0x53175e64 -> file VA 0x78875e64 (RVA 0x3F5E64), but reading there (and via proper PE RVA->raw mapping) yields garbage floats — the `.banana`-packed unpacked snapshot (10 sections, names stripped, image_base 0x78480000) did NOT preserve the .rdata/.data constant pool at the linearly-translated offset (the data section has VSize 0x115d000 but RawSz only 0x4200; section 0 raw covers the float region but the constant pool was relocated by the packer, so the simple rebase does not land on it). The canonical bit patterns DO exist in the file (1.0f/0x0000803f appears 1169x, 640.0f 6x, 480.0f 10x, 1.3333f 1x) confirming the pools are present, but pinning the EXACT offset of 0x53175e64 needs the relocation map, not recoverable from these static artifacts alone. So: value 1.0f = HIGH confidence by triangulation; direct-byte radare2 confirmation = LOW/unavailable (stated per Phase-1 caveat that _DAT_5317xxxx lives in .rdata possibly outside/relocated in the dump).
Related constants seen in sibling fns (same .rdata pool, for context, same unresolved-by-rawdump caveat): _DAT_53176128=640.0, _DAT_53176120=480.0, _DAT_53175e8c=4/3, _DAT_53175f20=2.0, _DAT_53175ec8=1.0(double).

**PsoBB VAs it reads/writes (9):** 0x00ACC0E8 (engine global SCALE_X — 2D design-anchor affine X; saved, pinned to 1.0f, restored), 0x00ACC0EC (engine global SCALE_Y — 2D design-anchor affine Y; saved, pinned to 1.0f, restored), 0x0082B158 (engine 2D vertex-submit helper; called indirectly with the 4 forwarded args; the affine is multiplied into verts inside it / its emitter 0x00836D04), 0x004A9C0C (hook site 1: stock `call 0x0082B158`, redirected to FUN_52db2240), 0x004A9D28 (hook site 2: stock `call 0x0082B158`, redirected), 0x005474D4 (hook site 3: stock `call 0x0082B158`, redirected), _DAT_53175e64 (ephinea.dll .rdata float const = 1.0f — the pin value), FUN_52dc3240 (the 0xE8 CALL-rel32 detour installer used to wire the 3 sites), FUN_52db05f0 (the patch-table installer that contains the 3 FUN_52dc3240(FUN_52db2240,...) calls)
