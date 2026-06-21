# The `.banana` packer — how `ephinea.dll` protects itself

`ephinea.dll` is a 32-bit (PE32) DLL run through a **custom executable packer**.
Its original code, imports and exports are encrypted inside the file; a small
hand-written bootstrap at the entry point rebuilds everything in memory at load
time. This is the static analysis of that bootstrap, verified against the exact
`ephinea.dll` shipped in this repo (`SHA256SUMS.txt`).

> If you just want to *work on the unpacked code*, skip this — `unpacked/`
> already contains a runtime dump taken after the bootstrap finished. This file
> is for anyone who wants to defeat the packer themselves.

## PE-level fingerprint (on-disk, packed)

| field | value |
|---|---|
| Machine | `0x14C` (i386), PE32 |
| ImageBase (preferred) | `0x10000000` |
| SizeOfImage | `0x031E6000` (~52 MB) |
| AddressOfEntryPoint | `0x031E22DC` (VA `0x131E22DC` at preferred base) |
| Sections | 10 — **8 with blank/stripped names** + `.rsrc` + `.banana` |
| Section flags | payload sections all `0xE0000040` = **RWX + CODE** (self-modifying) |
| Section entropy | payload sections ≈ **8.0 bits/byte** (encrypted / compressed) |
| `.banana` | vaddr `0x02E43000`, vsize `0x003A3000` — **holds the entry point** |
| IMPORT dir | rva `0x02E4D370`, size `0x4BC` — plaintext, inside `.banana` |
| EXPORT dir | rva `0x02E46020`, size `0x7350` — inside `.banana` |

Eight blank-named, max-entropy, RWX sections with the entry point sitting in the
*last* one is the classic packed-binary signature. A full plaintext import table
is preserved (so the loader is satisfied): 19 system DLLs, all `OriginalFirstThunk = 0`
(kernel32, user32, advapi32, oleaut32, gdi32, shell32, version, PSAPI, WINMM,
Secur32, bcrypt, d3d9, IPHLPAPI, ole32, SHLWAPI, gdiplus, WS2_32, IMM32, SETUPAPI).

## Stage 0 — the entry stub (plaintext, runs at `DllMain`)

```asm
131e22dc eb 08                  jmp   0x131e22e6        ; hop over 8 marker/junk bytes
131e22e6 60                     pushal
131e22e7 e8 00000000            call  0x131e22ec        ; get-EIP trick
131e22ec 5d                     pop   ebp
131e22ed 81 ed 10000000         sub   ebp, 0x10
131e22f3 81 ed dc221e03         sub   ebp, 0x31e22dc    ; ebp = real ImageBase
131e22f9 8a 84 24 28000000      mov   al, [esp+0x28]    ; fdwReason from the pushal frame
131e2300 80 f8 01               cmp   al, 1             ; == DLL_PROCESS_ATTACH ?
131e2303 0f84 07000000          je    0x131e2310
131e2309 61                     popal                   ; not attach:
131e230a 33 c0                  xor   eax, eax
131e230c 40                     inc   eax               ; eax = TRUE
131e230d c2 0c00                ret   0xc               ; return from DllMain, no unpack
131e2310 e9 04000000            jmp   0x131e2319        ; + junk island (anti-disasm)
131e2319 b8 dc221e03            mov   eax, 0x31e22dc     ; EP RVA
131e231e 03 c5                  add   eax, ebp           ; -> EP VA
131e2320 81 c0 63000000         add   eax, 0x63          ; -> EP+0x63
131e2326 b9 c1050000            mov   ecx, 0x5c1         ; length = 1473
131e232b ba c760f77b            mov   edx, 0x7bf760c7    ; key (only DL = 0xC7 used)
131e2330 30 10                  xor   [eax], dl          ; <== LAYER 1: single-byte XOR 0xC7
131e2332 40                     inc   eax
131e2333 49                     dec   ecx
131e2334 0f85 f6ffffff          jne   0x131e2330
131e233a e9 04000000            jmp   0x131e2343         ; falls into the just-decrypted stage 2
```

Tricks on display:
- **Position independence** via `call $+5 / pop / sub / sub` — `ebp` ends up
  holding the real load base regardless of ASLR.
- **DllMain-reason gate**: only unpacks on `DLL_PROCESS_ATTACH`; every other
  reason returns `TRUE` immediately.
- **Layer-1 cipher**: single-byte `XOR 0xC7` over `0x5C1` bytes at `EP+0x63`. The
  long `c7 c7 c7 …` runs in the raw bytes are zero-padding XORed with the key.
- **Anti-disassembly junk islands**: short `jmp +N` over 4–8 garbage bytes, so a
  linear sweep desyncs and emits nonsense (`popfd`, `out dx,al`, `iretd`).
  `tools/pe_probe.py` reproduces exactly that desync so you can see it.

## Stage 1/2 — revealed by XOR-0xC7 (entry at `EP+0x67` = `0x131E2343`)

Run `tools/pe_stage2.py` to decrypt and disassemble it. Highlights:
- Walks the **PE section table** (`[ImageBase+0x3C]` = e_lfanew, `+0xF8` = end of
  the 16-entry data directory, `0x28` = `sizeof IMAGE_SECTION_HEADER`) and reads
  each section's VirtualAddress.
- **dword-XOR section decryptor** at `0x131E2387`: `(key, ptr, len)` →
  `shr len,2; xor [ptr],key; ptr+=4; …` — a 32-bit key (e.g. `0x565C66D8`),
  stronger than the 1-byte bootstrap.
- **CALL/JMP rel32 fixup scanner** at `0x131E23DC` / `0x131E245C`: scans for
  opcode `E8`/`E9` → `sub [p+1], runningCtr`; `FF 25` → `sub [p+2], runningCtr2`.
  Re-normalizes relative call/jump displacements and indirect (`jmp dword[mem]`)
  thunks after the code is decrypted/moved — control-flow + IAT reconstruction.

The full chain decrypts the remaining sections, rebuilds the IAT from the
preserved descriptors, fixes relocations, and jumps to the original entry point.

## Reproduce

```
pip install capstone
python tools/pe_probe.py      # PE layout, sections, entropy, entry-stub disasm
python tools/pe_stage2.py     # peel XOR-0xC7 layer 1 -> stage-2 disasm
python tools/pe_imports.py    # bootstrap import table + entropy samples
```
