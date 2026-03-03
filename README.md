# SSE Engine Fixes for Wine

NOTE: This readme is being periodically updated by Claude as I have it iterate, check logs, and iterate over and over again. It is essentially reverse-engineering parts of Skyrim SE/AE's loading process by Brute-Force method. This document is constantly changing and it's current state is not going to be the same or even anywhere close to what it currently is at. Claude may make claims that turn out to be untrue after it discovers more things. Don't take this document as the 100% truth, yet. 

A **Wine/Proton/CrossOver-compatible** fork of [SSE Engine Fixes](https://github.com/aers/EngineFixesSkyrim64) by aers.

This mod provides the same bug fixes and performance patches as SSE Engine Fixes, rewritten to work under Wine, Proton, and CrossOver (macOS and Linux). It is a **drop-in replacement** — you do not need the original SSE Engine Fixes installed.

## What's Different

SSE Engine Fixes uses a two-phase loading mechanism:
1. **Phase 1 (Preload):** A DLL disguised as `d3dx9_42.dll` loads before SKSE and installs most hooks
2. **Phase 2 (SKSE Load):** The SKSE plugin registers message handlers

This preload phase is **incompatible with Wine/CrossOver/Proton** (after a certain number of plugins are loaded, ~600) — it causes crashes during game initialization, preventing the use of large mod lists.

**SSE Engine Fixes for Wine** moves all hooks to the normal SKSE plugin load phase and makes three key changes:

| Change | Why |
|--------|-----|
| **No d3dx9_42.dll preloader** | Wine crashes when hooks are installed during the preload phase |
| **No TBB dependency** | `tbb::concurrent_hash_map` replaced with `std::shared_mutex` + `std::unordered_map` (256 shards) |
| **Wine-compatible memory allocator** | TBB malloc replacement crashes under Wine; replaced with HeapAlloc/HeapFree (v1.21.0+) |
| **ScrapHeap expansion** | Per-thread ScrapHeap expanded from 64MB to 512MB for large mod lists |

All **38 bug fixes** and **14 patches** from SSE Engine Fixes are preserved, plus Wine-specific additions.

## Included Fixes

- Archery Downward Aiming, Animation Load Signed Crash, Bethesda.net Crash
- BGSKeyword Form Load Crash, BSLighting Ambient Specular, BSLighting Force Alpha Test
- BSLighting Parallax Bug, BSLighting Shadow Map, BSTempEffect NiRTTI
- Calendar Skipping, Cell Init, Climate Load, Conjuration Enchant Absorbs
- Create Armor Node Null Pointer Crash, Double Perk Apply, Equip Shout Event Spam
- ESL CELL Load Bug, FaceGen MorphData Head Null Pointer Crash
- GetKeywordItemCount, GHeap Leak Detection Crash, Global Time
- Initialize Hit Data Null Pointer Crash, Lip Sync, Memory Access Errors
- MO5S Typo, Music Overlap, NiController No Target, Null Process Crash
- Perk Fragment IsRunning, Precomputed Paths, Removed Spell Book
- Save Screenshots, Saved Havok Data Load Init, ShadowSceneNode Null Pointer Crash
- Texture Load Crash, Torch Landscape, Tree Reflections
- Vertical Look Sensitivity, Weapon Block Scaling

## Wine-Specific Additions

- **Wine null actor base crash fix** — prevents crashes from null parent cells and actor bases unique to Wine
- **Wine-compatible memory allocator** — replaces TBB with HeapAlloc/HeapFree via SafetyHook inline hooks on MemoryManager::Allocate/Deallocate/Reallocate. Uses a dedicated growable heap (HeapCreate) with 32-byte allocation headers for tracking.
- **ScrapHeap expansion** — hooks GetThreadScrapHeap to expand per-thread reserve from 64MB to 512MB before first use
- **Editor ID cache** — diagnostic + safe repopulation of editor IDs for Wine compatibility
- **600-file compilation fix (v1.22.11–18)** — monitor-based detection and manual compilation fallback for the engine's file compilation limit under Wine (see below)

## Included Patches

- Form Caching (with Wine-safe sharded cache)
- Tree LOD Reference Caching (with Wine-safe cache)
- INI Setting Collection, MaxStdIO, Enable Achievements with Mods
- Save Game Max Size, Regular Quicksaves, Safe Exit
- Save Added Sound Categories, Scrolling Doesn't Switch POV
- Sleep/Wait Time Modifier, Disable Chargen Precache, Disable Snow Flag
- Waterflow Animation

## Installation

1. Install [SKSE](https://skse.silverlock.org/) and [Address Library for SKSE](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
2. Copy `SSEEngineFixesForWine.dll` to `Data/SKSE/Plugins/`
3. Copy `SSEEngineFixesForWine.toml` to `Data/SKSE/Plugins/`
4. **Remove** the original SSE Engine Fixes if installed (`EngineFixes.dll` and `d3dx9_42.dll`)

## Configuration

Edit `Data/SKSE/Plugins/SSEEngineFixesForWine.toml` to toggle individual fixes and patches. Create `SSEEngineFixesForWineCustom.toml` in the same directory to override settings without modifying the main config.

## Supported Versions

- Skyrim SE 1.5.97 (SE build)
- Skyrim SE 1.6.1170 (AE build)

## Building

Requires:
- Visual Studio 2022 with C++23 support
- vcpkg
- [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG)

```
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

For AE build:
```
cmake -B build -S . -DBUILD_SKYRIMAE=ON -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

## Wine 600-File Compilation Limit (v1.22.11)

### The Problem

Under Wine/CrossOver/Proton, Skyrim SE's loading pipeline fails silently when the total compiled file count reaches **600 or more**. The engine's compile phase is skipped entirely — `AddCompileIndex` is never called, `loadingFiles` is never set to `true`, and zero forms are loaded. The game appears to load but arrives at the main menu with an empty world.

This does NOT happen on native Windows. It is specific to Wine's runtime environment.

### Exact Threshold

Through binary search across 12 test runs:

| Enabled Entries | Compiled Files | Result |
|-----------------|---------------|--------|
| 519 | 599 (129 reg + 470 ESL) | **WORKS** — all forms loaded |
| 520 | 600+ | **FAILS** — 0 compiled, 0 forms |

The threshold is **exactly 599→600 compiled files**. The engine auto-adds ~80 Creation Club files, so 519 entries in `plugins.txt` → 599 compiled files. The count is **total** (regular + ESL), not per-type.

This is count-based, not file-specific — swapping which plugins are enabled at the same count produces the same result.

### The Fix (v1.22.11–v1.22.17, IN PROGRESS)

**Phase 1 — Compilation (SOLVED):** Manual compilation successfully populates the engine's data structures:

1. **Loading Monitor Thread** (editor_id_cache.h): A background thread polls every 200ms, tracking all loading pipeline counters.

2. **Main-Thread Compilation Trigger** (v1.22.15): The CloseTES hook fires on the main thread during the catalog scan. After 2000+ CloseTES calls with 0 AddCompileIndex calls, it triggers `ManuallyCompileFiles()` — which is idempotent and re-runs every 1000 CloseTES calls to pick up newly-cataloged files.

3. **Manual Compilation** (v1.22.14+): `ManuallyCompileFiles()` parses `plugins.txt` via Win32 API to determine which files are enabled (the `kActive` flag can't be used because it's set BY the compilation process, which was skipped). Then:
   - Files enabled in `plugins.txt` (with `*` prefix) → compiled
   - Files not listed in `plugins.txt` (base game/CC always-on files) → compiled
   - Files listed but disabled (without `*` prefix) → skipped
   - Assigns sequential `compileIndex` (0-0xFD) to regular plugins
   - Assigns `compileIndex = 0xFE` + sequential `smallFileCompileIndex` (0-0xFFF) to ESL/light plugins
   - Populates `compiledFileCollection.files` and `compiledFileCollection.smallFiles`
   - Sets `loadingFiles = true`

**Result:** compiledFileCollection is correctly populated (211 reg + 1589 ESL = 1800 files), loadingFiles=true on the main thread during the catalog scan.

**Phase 2 — Form Loading (BREAKTHROUGH, v1.22.19–20):** Despite successful compilation, form loading never starts on its own. The engine's code path already branched past form loading before ManuallyCompileFiles ran. We must invoke form loading ourselves at kDataLoaded.

**v1.22.18 — Candidate testing:**
- **AE 13753** (orchestration): Called as `void(TESDataHandler*)` — 0 forms, 0 OpenTES. Wrong signature (see v1.22.19).
- **AE 13698** (form loading): 78 AddFormToDataHandler calls, 0 OpenTES — all engine defaults, not file-loaded forms.

**v1.22.19 — BREAKTHROUGH via hex dump analysis:**
Dumped 512 bytes of AE 13753 machine code at runtime and performed full x86-64 disassembly, revealing:

1. **Wrong function signature (v1.22.18 bug):** AE 13753 is `void(TESDataHandler*, bool)` — at offset +0x21, `movzx r12d, dl` reads the second parameter. v1.22.18 called it with only one parameter.
2. **Validation loop at +0xE0:** Iterates ALL files in TDH->files linked list, calling two functions per file:
   - **SomeFunc** (module+0x1C8E40): if returns false → skip file (continue to next)
   - **AnotherFunc** (module+0x1C6E90): if returns false → **ABORT ENTIRE FUNCTION** (return false)
3. **Abort instruction at +0x106:** `74 09` (JE +9) jumps to abort path that sets TLS flag to 0 and returns
4. **Form loading loop at +0x11C:** Only executes if validation completes without abort — iterates files, calls OpenTES, loads forms using the engine's native code

Also confirmed via diagnostic form iteration: all 8 test files opened successfully (mode 0), 1057 records across 8 files, all form types had valid IFormFactory entries. **The files are readable — the validation is the only blocker.**

**v1.22.20 — Validation bypass fix:**
Temporarily NOPs the abort jump at +0x106 (`74 09` → `90 90`), converting "abort all loading" into "skip this file, continue to next." Then calls AE 13753 with the correct 2-parameter signature `(TDH*, false)`. The engine's own form loading loop at +0x11C then executes, loading forms using native code. Original bytes are restored after the call.

### Important: SKSE Working Directory Fix

Wine's `wine` command does NOT set the working directory to the game folder. SKSE uses a relative path to load the Address Library (`Data\SKSE\Plugins\versionlib-*.bin`), so it fails silently without the correct CWD.

**Fix**: Create a `launch_skse.bat` in the game directory:
```batch
@echo off
cd /d "%~dp0"
skse64_loader.exe
```

Then launch this `.bat` instead of `skse64_loader.exe` directly.

### Bisection Methodology

To determine the exact threshold, a systematic binary search was performed by modifying `plugins.txt` (located at `%LOCALAPPDATA%\Skyrim Special Edition\Plugins.txt`). The Python script disabled entries by removing the `*` prefix:

```python
# Disable entries beyond TARGET count
for line in lines:
    if line.strip().startswith('*'):
        enabled_count += 1
        if enabled_count <= TARGET:
            result.append(line)
        else:
            result.append(line.replace('*', '', 1))
```

The test sequence: 1720 → 1100 → 810 → 665 → 592 → 555 → 537 → 528 → 523 → 521 → 520 → 519. All runs ≥520 entries showed 0 compiled files; 519 showed 599 compiled files and full form loading.

## Reverse Engineering Notes

Lessons learned during the v1.22.x investigation that may be useful for other SKSE plugin and Wine RE projects.

### Static CRT vs Dynamic CRT: The Measurement Trap

SKSE plugins built with static CRT linkage (`/MT` via `CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded"`) get their **own copy** of the CRT, separate from the game's dynamic ucrtbase.dll. This means:

- `_setmaxstdio(8192)` called from your DLL raises **your DLL's** stream limit, not the game's
- `_wfopen_s()` / `fopen()` from your DLL uses **your DLL's** CRT fd table (default maxstdio=512)
- The game's CRT (ucrtbase.dll) has its own separate `MSVCRT_max_streams` variable

**To test the game's CRT from a static-linked DLL**, you must resolve functions via `GetProcAddress`:
```cpp
auto ucrt = GetModuleHandleW(L"API-MS-WIN-CRT-STDIO-L1-1-0.DLL");
auto dyn_wfopen_s = reinterpret_cast<wfopen_s_t>(GetProcAddress(ucrt, "_wfopen_s"));
```

This distinction cost us several days — v1.22.6–v1.22.7 appeared to show a hard 512 CRT file handle limit under Wine, which was actually our own DLL's static CRT default. The game's dynamic CRT handles 2000+ files fine.

**Three-layer FD stress test methodology:**
1. **Static CRT** — `_wfopen_s` directly (tests your DLL's CRT)
2. **Dynamic ucrtbase** — `_wfopen_s` via `GetProcAddress` on `API-MS-WIN-CRT-STDIO-L1-1-0.DLL` (tests the game's CRT)
3. **Win32 CreateFile** — bypasses CRT entirely (tests kernel-level handle limits)

If all three disagree, you're measuring the wrong layer.

### RELOCATION_ID Mapping: SE→AE Is Not a Fixed Offset

The `RELOCATION_ID(SE_ID, AE_ID)` macro maps function IDs between Skyrim SE 1.5.97 and AE 1.6.1170. The offset between SE and AE IDs is **not constant**, even for functions in the same class:

| SE ID | AE ID | Function | Offset |
|-------|-------|----------|--------|
| 13597 | 13693 | TESDataHandler::AddFormToDataHandler | +96 |
| 13618 | 13716 | TESDataHandler::GetExtCellData | +98 |
| 13855 | 13931 | TESFile::OpenTES | +76 |
| 13894 | 13979 | TESFile::SeekNextForm | +85 |
| 14461 | 14617 | TESForm::GetFormByNumericId | +156 |
| 14509 | 14667 | AddCompileIndex | +158 |

Guessing AE IDs by adding a fixed offset to SE IDs will hook the **wrong function**. Always verify against the Address Library databases or CommonLibSSE-NG source.

### Steam DRM: Static Binary Analysis Is Impossible

SkyrimSE.exe's `.text` section is **encrypted on disk** (Steam DRM). The bytes at known function offsets are garbage until Steam decrypts them at launch. This means:
- Python scripts scanning the on-disk binary will find 0 CALL/JMP sites
- PE section headers are readable, but code bytes are not meaningful
- Must scan from **within a running SKSE plugin** (the .text section is decrypted by the time DLLs load)

The v1.22.11 runtime scanner handles this by parsing the in-memory PE headers and scanning the decrypted .text section.

### Runtime Code Scanning Technique

To find which function calls a known function (when you know the target but not the caller):

```cpp
// Get .text section from in-memory PE headers
auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase + dos->e_lfanew);
auto* sec = IMAGE_FIRST_SECTION(nt);
// Find .text, get textStart and textSize

// Scan for CALL (E8) and JMP (E9) rel32 instructions
auto* bytes = reinterpret_cast<const uint8_t*>(textStart);
for (size_t i = 0; i + 5 <= textSize; ++i) {
    if (bytes[i] == 0xE8 || bytes[i] == 0xE9) {
        int32_t rel32;
        memcpy(&rel32, &bytes[i + 1], 4);
        auto target = textStart + i + 5 + rel32;
        if (target == knownFunctionAddr) {
            // Found a caller! Use Offset2ID to identify it
        }
    }
}
```

Then use `REL::IDDatabase::Offset2ID` (binary search sorted by offset) to find the containing function's Address Library ID.

### Address Library (versionlib) Binary Format

The Address Library files (`versionlib-X-X-X-X.bin`) use a delta-encoded binary format:
- **Header**: format(i32), version(4×i32), nameLen(i32), name(bytes), pointerSize(i32), addressCount(i32)
- **Entries**: delta-encoded `{id, offset}` pairs, one type byte per entry
- **Type byte**: lo nibble = ID encoding, hi nibble = offset encoding
  - 0=full u64, 1=prev+1, 2=prev+u8, 3=prev-u8, 4=prev+u16, 5=prev-u16, 6=u16, 7=u32
  - Hi bit 3 = divide/multiply by pointer_size

Format 1 is SE (1.5.97), Format 2 is AE (1.6.x). The v1.6.1170 database contains 428,461 entries.

### Wine `_setmaxstdio` Behavior

Wine's ucrtbase implements `_setmaxstdio` with a two-level limit system:
- **fd-level**: `MSVCRT_MAX_FILES = 8192` (hardcoded in Wine source, not changeable)
- **FILE* stream-level**: `MSVCRT_max_streams` (defaults to 512, raised by `_setmaxstdio`)

`_setmaxstdio(8192)` returns success and does raise `MSVCRT_max_streams`, but the interaction between these two levels can be confusing when debugging. Under our testing, Wine's CRT can actually open 2000+ files after `_setmaxstdio(8192)` — the 512 limit only applies when `_setmaxstdio` hasn't been called.

### Loading Pipeline Architecture

Skyrim's form loading pipeline runs in this order (based on v1.22.0–17 instrumentation):

1. **Catalog scan** — TESFile objects constructed with open file handles; `SeekNextForm` iterates TES4 headers, `CloseTES` closes each file (2× per file). Note: `OpenTES` is NOT called during the catalog scan — files are already open from construction.
2. **CompileFiles** — assigns compile indices via `AddCompileIndex`, populates `compiledFileCollection`, sets `loadingFiles = true`
3. **Form loading loop** — for each compiled file: `OpenTES` (re-opens) → `SeekNextForm` → `ReadData` → `AddFormToDataHandler` → `CloseTES`
4. **Batch close** — all files closed again (1× per file, total 3× CloseTES per file)
5. **Post-catalog reads** — ~600 additional SeekNextForm + ~7000 SeekNextSubrecord + ~6400 ReadData calls (without OpenTES — files accessed via a different mechanism)
6. **kDataLoaded** — signals loading complete

If step 2 is skipped (the 600-file bug), step 3 never runs — `OpenTES` stays at 0, zero forms are created. ClearData is NOT called during initial loading (it's a reset function for game reloads). InitializeFormDataStructures is also not called.

**Critical finding (v1.22.16):** AE 11596 (identified by the v1.22.11 scanner as containing AddCompileIndex call sites) is NEVER called during initial loading. The initial loading pipeline uses a **different code path** — possibly inlined code or indirect calls that the E8/E9 scanner doesn't detect.

**Current status (v1.22.21): FULLY SOLVED.** ManuallyCompileFiles populates compiledFileCollection (211 reg + 1589 ESL = 1800 files). v1.22.20 NOPs the abort jump and calls AE 13753. v1.22.21 fixes an infinite-loop regression by resetting compile state before the AE 13753 call. **Result: 284,294 forms loaded, 1,064,567 total in TDH hash table, game reaches main menu in ~110s with 1720 enabled plugins.** See v1.22.20–21 details below.

### TESDataHandler Key Offsets (AE 1.6.1170)

| Offset | Member | Type |
|--------|--------|------|
| 0xD58 | activeFile | TESFile* |
| 0xD60 | files | BSSimpleList\<TESFile*\> |
| 0xD70 | compiledFileCollection | TESFileCollection |
| 0xD70 | .files | BSTArray\<TESFile*\> (regular plugins) |
| 0xD88 | .smallFiles | BSTArray\<TESFile*\> (ESL/light plugins) |
| 0xDA0 | (unknown flag) | bool |
| 0xDA2 | saveLoadGame | bool |
| 0xDA6 | hasDesiredFiles | bool |
| 0xDA8 | loadingFiles | bool |

TESFile compile index fields: `compileIndex` (uint8_t at +0x478, 0xFF = uncompiled), `smallFileCompileIndex` (uint16_t at +0x47A).

### Key RELOCATION_IDs for Loading Pipeline

| Function | SE ID | AE ID | AE Offset |
|----------|-------|-------|-----------|
| TESDataHandler::GetSingleton | 514141 | 400269 | — |
| TESDataHandler::AddFormToDataHandler | 13597 | 13693 | 0x1B14C0 |
| TESDataHandler::ClearData | 13646 | 13754 | 0x1B9AB0 |
| TESDataHandler::LoadScripts | 13657 | 13766 | 0x1BCBC0 |
| TESFile::OpenTES | 13855 | 13931 | 0x1C4DA0 |
| TESFile::CloseTES | 13857 | 13933 | 0x1C5580 |
| TESFile::SeekNextForm | 13894 | 13979 | 0x1C7D30 |
| TESFile::SeekNextSubrecord | 13903 | 13990 | 0x1C8590 |
| TESFile::ReadData | 13904 | 13991 | 0x1C8640 |
| TESForm::GetFormByNumericId | 14461 | 14617 | 0x1E01A0 |
| AddCompileIndex | 14509 | 14667 | 0x1E1640 |
| TESForm::InitializeFormDataStructures | 14511 | 14669 | 0x1E17F0 |

### AE 13698: Default Form Factory (v1.22.18)

AE 13698 (offset 0x1B1D30) has 54 `AddFormToDataHandler` call sites, but these are hardcoded engine defaults (formIDs like 0x1F5, 0x1A, 0x1B) — built-in form type initializations created without reading any files.

**Lesson:** A high count of `AddFormToDataHandler` call sites does NOT mean the function reads forms from files — it may just create built-in default instances of many different form types.

### AE 13753: Form Loading Orchestrator — Full Disassembly (v1.22.19)

AE 13753 (offset 0x1B96E0, ~976 bytes) IS the form loading orchestrator. v1.22.18 failed because it was called with the **wrong signature** (`void(TDH*)` instead of `void(TDH*, bool)`).

**Key disassembly findings from runtime hex dump:**

```
Signature: void(TESDataHandler* rcx, bool dl)
  +0x21: movzx r12d, dl     — stores 2nd param (bool: false=initial, true=save)
  +0x25: mov rsi, rcx        — stores TDH*

TLS setup:
  +0x28: mov r8d, [rip+...]  — load TLS index
  +0x38: mov r9, [rax+r8*8]  — get TLS slot
  +0x4F: mov dword [rax], 100 — set loading token to 100
  +0x5B: add r13, r9          — r13 = TLS_slot + 0x5D8 (status flag)
  +0x5E: mov byte [r13], 1    — set TLS status = 1 (loading)

saveLoadGame check:
  +0xB7: cmp [rsi+0xDA2], dil — check saveLoadGame flag
  +0xBE: je +0x20 → 0xE0      — if false (initial load), go to validation

VALIDATION LOOP (+0xE0–0x10F):
  Iterates TDH->files linked list (r14 = node, [r14] = TESFile*, [r14+8] = next)
  For each file:
    +0xF3: call module+0x1C8E40  — SomeFunc(file): if false → SKIP file
    +0xFF: call module+0x1C6E90  — AnotherFunc(file): if false → ABORT ALL
    +0x106: JE +0x09 → abort path (THIS IS THE BUG)

  Abort path (+0x111):
    mov byte [r13], 0     — clear TLS status flag
    xor al, al            — return false
    jmp to epilogue

FORM LOADING LOOP (+0x11C–0x1B6):
  Only reached if validation completes without abort.
  Iterates same file list via rbx:
    +0x14E: call module+0x1C8E40  — SomeFunc again (per-file filter)
    +0x15A: call module+0x1C8D10  — ValidateFunc2
    +0x177: call module+0x1C9EE0  — OpenFileFunc (sets activeFile)
    +0x180: mov [rsi+0xD58], rdi  — TDH->activeFile = file
  Loop continues until all files processed.
```

**The fix (v1.22.20–21):** Two-step solution:

1. **NOP the abort jump at +0x106** (`74 09` → `90 90`): Converts "if file fails validation, abort everything" into "if file fails validation, skip to next file." The form loading loop at +0x11C can now execute.

2. **Reset compile state before calling AE 13753 (v1.22.21):** ManuallyCompileFiles pre-sets `file->compileIndex` on each TESFile and populates `compiledFileCollection`. When AE 13753's own compilation pass then calls `TESForm::AddCompileIndex` per form, the pre-set `compileIndex` causes the function's internal file iterator to lock — the same form is returned on every iteration, producing an infinite loop (observed: `Denizens of Morthal - AI Overhaul Patch.esp` at formID 0x00044381, 1.5 billion calls with no `SeekNextForm` between them). Fix: wipe `file->compileIndex` and clear `compiledFileCollection` before calling AE 13753 so it compiles from scratch with no state conflict.

**Result:** 284,294 `AddFormToDataHandler` calls, 3,636 `OpenTES` calls, 1,064,567 forms in TDH hash table, 376,879 editor IDs. Game reaches main menu with full world state in ~110 seconds with 1720 enabled plugins (1800 compiled files: 211 regular + 1589 ESL).

### Wine-Specific Observations

- **`kActive` flag is set BY compilation**: `TESFile::RecordFlag::kActive` (bit 3) is set during the `CompileFiles` process, not before. When `CompileFiles` is skipped (the 600-file bug), ALL files have `kActive = 0`. You CANNOT use `kActive` to determine which files should be compiled — parse `plugins.txt` instead.
- **SeekNextForm fires during catalog scan** regardless of whether the full load succeeds. A high SeekNextForm count with zero AddCompileIndex/OpenTES/AddFormToDataHandler calls means the catalog scan ran but the compile phase was skipped.
- **Wine CWD**: `wine --bottle "path/to/skse64_loader.exe"` does NOT set CWD to the game directory. SKSE's relative Address Library path fails. Use a .bat wrapper with `cd /d "%~dp0"`.
- **Offset2ID allocation kills Wine startup** (v1.22.17→18): Loading the `REL::IDDatabase::Offset2ID` reverse-lookup table (428,461 entries, ~7MB) during SKSE plugin load causes a total startup hang under Wine — TESDataHandler is never created, zero activity, 202% CPU indefinitely. This happens reliably. The scanner was removed in v1.22.18 and startup immediately recovered. Suspect Wine's memory manager or allocation interlock is disrupted by the large sorted-vector allocation during early initialization.

## Diagnostic Version History

| Version | Change |
|---------|--------|
| v1.22.0 | Instrumented 10 loading pipeline functions + 200ms monitor thread |
| v1.22.1 | Added OpenTES/CloseTES hooks |
| v1.22.2 | Added AddCompileIndex + SeekNextForm hooks |
| v1.22.3 | Added CloseTES hook + plugins.txt verification |
| v1.22.4 | Hooked `_setmaxstdio` — confirmed game never resets the limit |
| v1.22.5 | Added kActive/kMaster/kSmallFile flag counting |
| v1.22.6–7 | FD stress tests (appeared to show 512 CRT limit — was measurement error) |
| v1.22.8 | Fixed FD stress test (static vs dynamic CRT) — debunked file handle theory |
| v1.22.9 | CompileFiles pipeline hooks (crashed — wrong RELOCATION_IDs) |
| v1.22.10 | Removed crashed hooks, added DumpFilesListState diagnostics |
| v1.22.11 | Runtime code scanner + manual compilation fallback (fixes 600-file limit) |
| v1.22.12 | Monitor-based compilation detection (replaced broken inline hook that crashed on AE 11596) |
| v1.22.13 | Disabled runtime scanner (not needed, was suspected of causing early termination) |
| v1.22.14 | Fixed ManuallyCompileFiles — parse plugins.txt for enabled status (kActive is 0 when compilation is skipped) |
| v1.22.15 | Main-thread compilation trigger via CloseTES hook (idempotent ManuallyCompileFiles, cached plugins.txt). Confirmed: loadingFiles=true on main thread still doesn't trigger form loading |
| v1.22.16 | Hook AE 11596 (CompileFiles candidate) — confirmed NEVER called during initial loading. The engine uses a different code path. |
| v1.22.17 | Multi-target .text scanner: scan for callers of OpenTES + AddFormToDataHandler + AddCompileIndex to identify the actual form loading function |
| v1.22.18 | Removed .text scanner (caused zero-activity startup hang via Offset2ID 428K allocation). Removed FD stress test from monitor thread (killed thread at tick 10). Added ForceLoadAllForms() at kDataLoaded — tested AE 13753 (0 forms, wrong sig) and AE 13698 (78 engine defaults only, not file-loaded). Confirmed: neither function loads forms from plugin files. |
| v1.22.19 | **BREAKTHROUGH:** Hex dump of AE 13753 (512 bytes) + diagnostic form iteration. Disassembly revealed: (1) correct signature is `void(TDH*, bool)` not `void(TDH*)`, (2) validation loop at +0xE0 aborts ALL loading if any file fails check at module+0x1C6E90, (3) abort instruction at +0x106 (`JE 74 09`), (4) form loading loop at +0x11C never runs because validation aborts. Diagnostic iteration confirmed all files readable with valid IFormFactory entries. |
| v1.22.20 | Validation bypass: temporarily NOP abort jump at +0x106 (`74 09` → `90 90`), call AE 13753 with correct 2-param signature `(TDH*, false)`. Restores original bytes after call. AddForm counts go up, OpenTES fires — but infinite loop on `Denizens of Morthal - AI Overhaul Patch.esp` (1.5B AddCompileIndex calls, 0 SeekNextForm). |
| v1.22.21 | **FULL SOLUTION:** Root cause of infinite loop: ManuallyCompileFiles pre-sets `file->compileIndex`, conflicting with AE 13753's own compilation pass → `TESForm::AddCompileIndex` locks on the same form. Fix: reset all `file->compileIndex = 0xFF` and clear `compiledFileCollection` before calling AE 13753, giving it a clean slate. **Result: 284,294 forms loaded (AddFormToDataHandler), 3,636 OpenTES calls, 1,064,567 total forms in TDH hash table, game reaches main menu with 1720 enabled plugins.** |

## Credits

All credit for the original reverse engineering and bug fixes goes to **[aers](https://github.com/aers)** and contributors of [SSE Engine Fixes](https://github.com/aers/EngineFixesSkyrim64).

- Original mod: [SSE Engine Fixes on NexusMods](https://www.nexusmods.com/skyrimspecialedition/mods/17230)
- Original source: [EngineFixesSkyrim64 on GitHub](https://github.com/aers/EngineFixesSkyrim64)

This fork is maintained by [Corkscrew](https://github.com/corkscrewmodding) for the macOS and Linux Skyrim modding community.

## License

MIT License (same as upstream SSE Engine Fixes)
