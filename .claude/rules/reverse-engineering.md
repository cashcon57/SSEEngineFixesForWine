---
description: Reverse engineering workflow and conventions for adding/modifying code cave patches
globs: ["src/patches/form_caching.h"]
---

# Reverse Engineering Conventions

## RE Toolchain

| Tool | Location | Purpose |
|------|----------|---------|
| Unpacked exe | `/Users/cashconway/skyrim-re/SkyrimSE.unpacked.exe` | Original bytes (SteamStub-stripped). Use for offset verification. |
| Memory dump | `~/Documents/My Games/Skyrim Special Edition/SKSE/SkyrimSE_text_dump.bin` | Runtime .text section (has our patches applied). Use for live analysis. |
| Ghidra project | `/Users/cashconway/skyrim-re/ghidra-project/` | Full disassembly/decompilation of unpacked exe. |
| Ghidra scripts | `/Users/cashconway/skyrim-re/ghidra_scripts/` | `DecompileCrashSites.java`, `DumpImportXrefs.java` |
| Python RE venv | `/Users/cashconway/.skyrim-re-venv/` | capstone + pefile. Activate: `source /Users/cashconway/.skyrim-re-venv/bin/activate` |
| Python scripts | `/Users/cashconway/skyrim-re/*.py` | `verify_bytes.py`, `disasm_crash_sites.py`, `analyze_hashmap_pe.py`, etc. |

## Adding a New Code Cave Patch

1. **Identify the crash/fault site** from crash log (`SSEEngineFixesForWine_crash.log`) — note the RIP offset
2. **Disassemble in Ghidra** or with `disasm_crash_sites.py` — understand the instruction sequence
3. **Verify bytes** against unpacked exe using `verify_bytes.py` or inline `memcmp` — never patch without verifying
4. **Write the cave**: hand-assemble x86-64 into a `cave[]` byte array with comments for each instruction
5. **Register**: call `RegisterCavePatch()` so VEH fault recovery and watchdog verification work
6. **Use `VirtualProtect` + `FlushInstructionCache`**: never restore original protection (Wine reverts file-backed pages)

## Code Cave Conventions

- All offsets are relative to image base (e.g., `base + 0x2D0C33`)
- Byte verification array before patching: `static const uint8_t verify[] = { ... };`
- Use `memcmp(site, verify, N)` — if bytes don't match, log error and skip (another patch may have written first)
- JMP encoding: `0xE9` + rel32 for near, `0xFF 0x25 0x00000000` + abs64 for far
- NOP padding: fill remaining overwritten bytes with `0x90`
- Cave validation: `test reg, reg` / `jz .skip` for null, `shr r10, 32` / `cmp r10d, 0x7F` / `ja .skip` for canonical
- Skip path should jump to a safe resume point (past the faulting instruction sequence)

## x86-64 Assembly Reference (common cave patterns)

```
48 85 F6          test rsi, rsi           # null check
74 XX             je .skip                # short jump
49 89 F2          mov r10, rsi            # copy for validation
49 C1 EA 20       shr r10, 32            # check high dword
41 FF CA          dec r10d               # canonical: high dword should be 1 (0x00000001)
41 83 FA 7F       cmp r10d, 0x7F         # if > 0x7F, non-canonical
77 XX             ja .skip               # skip on invalid
FF 25 00000000    jmp [rip+0]            # abs indirect jump (followed by 8-byte target)
31 C0             xor eax, eax           # zero return (safe default)
```

## Key Offsets (AE 1.6.1170)

- +0x1945D0: BSTHashMap::SetAt variant A
- +0x1947C0: BSTHashMap::SetAt variant B
- +0x198390: BSTHashMap::grow/rehash
- +0x2D0C33: struct field load (PatchR6 — mov+sub with RSI validation)
- +0x1AF612: hash chain in form ref lookup (known freeze site)
- BSTScatterTableSentinel: 0x141FD5F3C
- .text section: 0x1000–0x174F000

## SkyrimSE.exe Notes

- .text is encrypted on disk (SteamStub DRM) — always use unpacked exe for static analysis
- Runtime image base varies per launch (ASLR) — use `REL::Module::get().base()` for offsets
- AE build uses Address Library for version-independent hooking via CommonLibSSE
