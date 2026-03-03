# SSE Engine Fixes for Wine

> **Note:** This README documents both usage and active reverse-engineering findings. Some sections reflect ongoing investigation — conclusions are empirically verified but further discoveries may refine them. Sections marked with version numbers are pinned to confirmed behavior at that version.

A **Wine/Proton/CrossOver-compatible** drop-in replacement for [SSE Engine Fixes](https://github.com/aers/EngineFixesSkyrim64) by aers.

Provides the same 38 bug fixes and 14 patches as the original, rewritten to work under Wine, Proton, and CrossOver on macOS and Linux — including large mod lists of 1000+ plugins that the original cannot handle in Wine environments. You do not need the original SSE Engine Fixes installed.

---

## Table of Contents

- [What This Does](#what-this-does)
- [Key Differences from Original](#key-differences-from-original)
- [Windows Compatibility](#windows-compatibility)
- [Included Fixes and Patches](#included-fixes-and-patches)
- [Installation](#installation)
- [Mod Manager Integration](#mod-manager-integration)
- [Configuration](#configuration)
- [Reverse Engineering Reference](#reverse-engineering-reference)
  - [Steam DRM: On-Disk Analysis Is Impossible](#steam-drm-on-disk-analysis-is-impossible)
  - [Loading Pipeline Architecture](#loading-pipeline-architecture)
  - [The 600-File Wine Compilation Limit](#the-600-file-wine-compilation-limit)
  - [AE 13753: Form Loading Orchestrator Disassembly](#ae-13753-form-loading-orchestrator-disassembly)
  - [Key Struct Offsets and RELOCATION_IDs](#key-struct-offsets-and-relocation_ids)
  - [Address Library: versionlib Binary Format](#address-library-versionlib-binary-format)
  - [Address Library Dialog Suppression](#address-library-dialog-suppression)
  - [BSResource MAX_PATH Overflow](#bsresource-max_path-overflow)
  - [Static vs Dynamic CRT: The Measurement Trap](#static-vs-dynamic-crt-the-measurement-trap)
  - [RELOCATION_ID SE→AE Is Not a Fixed Offset](#relocation_id-seae-is-not-a-fixed-offset)
  - [Runtime Code Scanning Technique](#runtime-code-scanning-technique)
  - [Wine-Specific Behaviors](#wine-specific-behaviors)
- [Who Else This Helps](#who-else-this-helps)
- [Diagnostic Version History](#diagnostic-version-history)
- [Credits](#credits)

---

## What This Does

SSE Engine Fixes for Wine is an SKSE plugin (`.dll`) that installs during normal SKSE plugin load and provides:

- All upstream bug fixes from [SSE Engine Fixes](https://github.com/aers/EngineFixesSkyrim64) — crash fixes, rendering corrections, gameplay fixes
- A **Wine-compatible memory allocator** (HeapAlloc/HeapFree) replacing Intel TBB, which crashes under Wine
- A fix for the **600-file compilation limit** — a Wine-specific bug that silently skips all form loading when plugin count exceeds ~519 entries in plugins.txt
- Wine-specific crash fixes not present in the original
- Suppression of the SKSE Address Library warning dialog that blocks the loading screen behind fullscreen CrossOver windows

Supported: **Skyrim SE 1.5.97** and **Skyrim SE 1.6.1170 (AE)**.

---

## Key Differences from Original

SSE Engine Fixes uses a two-phase loading mechanism:
1. **Phase 1 (Preload):** A DLL disguised as `d3dx9_42.dll` installs most hooks before SKSE
2. **Phase 2 (SKSE Load):** The SKSE plugin registers message handlers

The preload phase crashes under Wine/CrossOver/Proton when large mod lists are active (~600+ compiled plugin files). This fork moves all hooks to the normal SKSE plugin load phase.

| Change | Reason |
|--------|---------|
| **No d3dx9_42.dll preloader** | Crashes during Wine initialization with large mod lists |
| **No TBB dependency** | `tbb::concurrent_hash_map` replaced with `std::shared_mutex` + sharded `std::unordered_map` (256 buckets) |
| **Wine-compatible memory allocator** | TBB scalable_malloc crashes under Wine; replaced with HeapAlloc/HeapFree via SafetyHook inline hooks |
| **ScrapHeap expansion** | Per-thread ScrapHeap expanded from 64MB to 512MB (configurable) for large mod lists |
| **600-file compilation fix** | Manually drives form loading when the engine's own pipeline is silently skipped |
| **Address Library dialog suppressor** | Auto-dismisses the SKSE Address Library warning MessageBox that renders invisibly behind CrossOver fullscreen windows |

---

## Windows Compatibility

**Windows users should use the original [SSE Engine Fixes](https://github.com/aers/EngineFixesSkyrim64), not this fork.**

This fork is built for Wine/CrossOver/Proton compatibility. Several of its changes are either unnecessary or actively suboptimal on native Windows:

| Feature | This Fork | Original | Windows Impact |
|---------|-----------|----------|----------------|
| Memory allocator | `HeapAlloc/HeapFree` (Windows API) | Intel TBB `scalable_malloc` | **Slower.** TBB uses per-thread caches tuned for high-frequency concurrent allocation. HeapAlloc is the OS baseline — correct but not optimized for sustained concurrent pressure during form loading. |
| Form caching | `std::shared_mutex` + 256-bucket sharded `unordered_map` | TBB `concurrent_hash_map` | **Comparable.** Different lock strategy but competitive at moderate thread counts. |
| 600-file fix | Only triggers when `addFormCount == 0` at `kDataLoaded` | N/A | **No impact.** The condition only occurs on Wine; on native Windows `addFormCount` is always non-zero. No overhead on Windows. |
| Address Library dialog | Suppressed unconditionally | N/A | **Harmful.** On Windows you want this dialog — it tells you when Address Library is outdated. This fork silences a warning you need to see. |
| d3dx9_42.dll preloader | Absent | Present | **Minor regression.** The preloader installs hooks earlier in the DLL load phase, giving a small timing advantage for very early engine patches. |
| ScrapHeap expansion | 512MB | 512MB (same default) | **Same.** |

**Performance summary:** On native Windows, the original SSE Engine Fixes is faster due to TBB's allocator, and it preserves diagnostic dialogs that indicate real problems. This fork does not crash on Windows, but there is no benefit to using it there.

**Exception:** If you are running Windows with Wine/WSL2 GPU passthrough, or any other Wine-based environment, use this fork instead.

---

## Included Fixes and Patches

**Bug fixes (38):**
Animation Load Signed Crash, Archery Downward Aiming, Bethesda.net Crash, BGSKeyword Form Load Crash, BSLighting Ambient Specular, BSLighting Force Alpha Test, BSLighting Parallax Bug, BSLighting Shadow Map, BSTempEffect NiRTTI, Calendar Skipping, Cell Init, Climate Load, Conjuration Enchant Absorbs, Create Armor Node Null Pointer Crash, Double Perk Apply, Equip Shout Event Spam, ESL CELL Load Bug, FaceGen MorphData Head Null Pointer Crash, GetKeywordItemCount, GHeap Leak Detection Crash, Global Time, Initialize Hit Data Null Pointer Crash, Lip Sync, Memory Access Errors, MO5S Typo, Music Overlap, NiController No Target, Null Process Crash, Null Actor Base Crash (Wine-specific), Perk Fragment IsRunning, Precomputed Paths, Removed Spell Book, Save Screenshots, Saved Havok Data Load Init, ShadowSceneNode Null Pointer Crash, Texture Load Crash, Torch Landscape, Tree Reflections, Vertical Look Sensitivity, Weapon Block Scaling

**Patches (14):**
Disable Chargen Precache, Disable Snow Flag, Editor ID Cache (Wine-safe), Enable Achievements with Mods, Form Caching (sharded, Wine-safe), INI Setting Collection, MaxStdIO, Regular Quicksaves, Safe Exit, Save Added Sound Categories, Save Game Max Size, Scrolling Doesn't Switch POV, Sleep/Wait Time Modifier, Tree LOD Reference Caching (Wine-safe), Waterflow Animation

---

## Installation

1. Install [SKSE](https://skse.silverlock.org/) for your Skyrim version
2. Install [Address Library for SKSE](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
3. Copy `SSEEngineFixesForWine.dll` → `Data/SKSE/Plugins/`
4. Copy `SSEEngineFixesForWine.toml` → `Data/SKSE/Plugins/`
5. **Remove** original SSE Engine Fixes if installed (`EngineFixes.dll` and `d3dx9_42.dll`)

**Wine/CrossOver launch note:** Wine's `wine` command does not set the working directory to the game folder. SKSE loads Address Library via a relative path (`Data\SKSE\Plugins\versionlib-*.bin`) and will fail silently without the correct CWD. Use a `launch_skse.bat` wrapper in the game directory:

```batch
@echo off
cd /d "%~dp0"
skse64_loader.exe
```

---

## Mod Manager Integration

### Corkscrew (macOS and Linux)

[Corkscrew](https://github.com/corkscrewmodding/corkscrew) has built-in support for this fork and manages it automatically. **If you are using Corkscrew, you do not need to install or update this mod manually.**

Before every game launch, Corkscrew automatically:

1. **Disables the original Engine Fixes preloader** — if `d3dx9_42.dll` is present in the game root (the DLL that hooks during Wine DLL load and crashes ~63 seconds into launch), Corkscrew renames it to `d3dx9_42.dll.disabled`. The rename is reversible; the file is not deleted.

2. **Disables the original EngineFixes.dll** — the original SKSE plugin in `Data/SKSE/Plugins/EngineFixes.dll` checks whether its preloader is present and force-closes the game if it isn't. Corkscrew renames it to `EngineFixes.dll.disabled` to prevent this.

3. **Patches EngineFixes.toml** — if the original Engine Fixes mod is installed and its TOML config still contains `= true` entries, Corkscrew sets all hooks to `false` and forces `bDisableTBB = true` in the deployed config and all staging copies. This ensures a redeploy cannot re-enable a crashing hook.

4. **Deploys this fork if absent** — if `0_SSEEngineFixesForWine.dll` is not found in `Data/SKSE/Plugins/`, Corkscrew downloads the latest release from GitHub and deploys it.

5. **Auto-updates the DLL** — Corkscrew stores the deployed release tag in `SSEEngineFixesForWine.version`. At launch, it compares this against the latest GitHub release. If a newer version is available, it downloads and deploys it automatically.

6. **Preserves your config** — `SSEEngineFixesForWine.toml` is only written on first deploy. Updates never overwrite it.

The DLL is deployed as `0_SSEEngineFixesForWine.dll` (with the `0_` prefix) so SKSE loads it before all letter-named plugins, ensuring the editor ID cache and memory allocator patches are active before other plugins' `kDataLoaded` handlers run.

### Vortex / Mod Organizer 2

No built-in support. Install manually following the [Installation](#installation) steps above. When a new release is available, update by replacing the DLL and TOML files in `Data/SKSE/Plugins/`. There is no auto-update mechanism.

---

## Configuration

Edit `Data/SKSE/Plugins/SSEEngineFixesForWine.toml`. All fixes and patches are individually toggleable. Create `SSEEngineFixesForWineCustom.toml` in the same directory to override settings without modifying the base config — the custom file takes priority.

---

## Reverse Engineering Reference

The following sections document what was learned during development. They are intended to be useful to anyone writing SKSE plugins, Wine-compatibility layers, or mod management tools for Skyrim SE/AE.

---

### Steam DRM: On-Disk Analysis Is Impossible

`SkyrimSE.exe`'s `.text` section is **encrypted on disk** by Steam DRM. The bytes at any known function offset are ciphertext until Steam decrypts them at process launch. Consequences:

- Python/IDA scripts scanning the on-disk binary will find zero valid CALL/JMP sites
- PE section headers are readable but code content is not meaningful
- Disassembly tools that read the file rather than attach to a running process produce garbage
- **You must scan from within a running SKSE plugin** — the `.text` section is decrypted in memory by the time any DLL's `DllMain` runs

The v1.22.11 runtime scanner handles this by reading in-memory PE headers (`IMAGE_DOS_HEADER` → `IMAGE_NT_HEADERS64` → `IMAGE_SECTION_HEADER`) to locate the decrypted `.text` section at runtime, then scanning it directly.

---

### Loading Pipeline Architecture

Skyrim's form loading pipeline, as determined by hooking 10+ functions across v1.22.0–21:

```
1. CATALOG SCAN
   TESFile objects constructed with open file handles
   SeekNextForm called per file (reads TES4 headers)
   CloseTES called 2× per file
   Note: OpenTES is NOT called here — files open from construction

2. CompileFiles  ← SKIPPED UNDER WINE WHEN PLUGIN COUNT ≥ 600
   AddCompileIndex called per file
   Populates compiledFileCollection.files (regular) and .smallFiles (ESL)
   Sets TDH->loadingFiles = true
   Sets TESFile->compileIndex (uint8 at +0x478; 0xFF = uncompiled)
   Sets TESFile->smallFileCompileIndex (uint16 at +0x47A) for ESL files

3. FORM LOADING LOOP
   For each compiled file:
     OpenTES → SeekNextForm → ReadData → AddFormToDataHandler → CloseTES

4. BATCH CLOSE
   All files closed again (1× each; total 3× CloseTES per file across full pipeline)

5. POST-CATALOG READS
   ~600 additional SeekNextForm + ~7000 SeekNextSubrecord + ~6400 ReadData calls
   (without OpenTES — accessed via a different, not-yet-identified mechanism)

6. kDataLoaded fired
```

**Critical:** The `kActive` flag (`TESFile::RecordFlag::kActive`, bit 3) is **set by CompileFiles**, not before it. If CompileFiles is skipped, all files have `kActive = 0`. Never use `kActive` to determine which plugins should be compiled — parse `plugins.txt` directly instead.

---

### The 600-File Wine Compilation Limit

Under Wine/CrossOver/Proton, the CompileFiles step (step 2 above) is silently skipped when the total compiled file count reaches **600 or more**. The game proceeds past catalog scan with zero compiled files, zero forms loaded, and an empty world.

**This does not happen on native Windows.**

#### Exact Threshold

Determined via binary search across 12 test runs:

| Enabled Entries in plugins.txt | Compiled Files (approx.) | Result |
|---|---|---|
| 519 | 599 (129 reg + 470 ESL) | **WORKS** — all forms loaded normally |
| 520 | 600+ | **FAILS** — 0 compiled files, 0 forms loaded |

The threshold is total files (regular + ESL combined), not per-type. ~80 Creation Club and base game files are added automatically by the engine regardless of plugins.txt, so the effective threshold is ~519 user-enabled entries. Swapping which specific plugins are enabled at the same count produces the same result — it is purely count-based.

**Diagnostic signature of the bug:** SeekNextForm count is non-zero (catalog scan ran), but AddCompileIndex, OpenTES, and AddFormToDataHandler all stay at 0.

#### The Fix (v1.22.11–21)

**Phase 1 — Compilation:** A CloseTES hook fires on the main thread during the catalog scan. After detecting zero AddCompileIndex calls despite many CloseTES calls, `ManuallyCompileFiles()` is triggered:

1. Parses `plugins.txt` via Win32 API to determine enabled status
   - Line with `*` prefix → enabled, compile it
   - Line without `*` prefix → disabled, skip
   - Files not in plugins.txt (base game, CC) → always compile
2. Assigns sequential `compileIndex` (0–0xFD) to regular plugins
3. Assigns `compileIndex = 0xFE` + sequential `smallFileCompileIndex` (0–0xFFF) to ESL/light plugins
4. Populates `compiledFileCollection.files` and `.smallFiles`
5. Sets `TDH->loadingFiles = true`

**Phase 2 — Form Loading:** Despite correct compilation, the form loading loop (step 3) never starts on its own — the engine's code path already branched past it. At `kDataLoaded`, if zero `AddFormToDataHandler` calls were observed, `ForceLoadAllForms()` invokes AE 13753 directly to load forms using the engine's own code. See [AE 13753 disassembly](#ae-13753-form-loading-orchestrator-disassembly) for details.

**Result with 1720 plugins:** 284,294 AddFormToDataHandler calls, 3,636 OpenTES calls, 1,064,567 forms in TDH hash table, game reaches main menu in ~110 seconds.

---

### AE 13753: Form Loading Orchestrator Disassembly

AE 13753 (offset `0x1B96E0`, ~976 bytes) is the form loading orchestrator that drives the full form loading loop. Its correct signature and behavior were determined by dumping 512 bytes of machine code from a running SKSE plugin and performing manual x86-64 disassembly.

#### Signature

```cpp
void TESDataHandler__LoadFiles(TESDataHandler* rcx, bool dl);
// +0x21: movzx r12d, dl   — stores 2nd param (false=initial load, true=save/reload)
// +0x25: mov rsi, rcx     — stores TDH*
```

Calling this as `void(TDH*)` (one parameter) produces zero forms — the `bool` selects between initial load and save-game paths.

#### Validation Loop (+0xE0)

Before entering the form loading loop, the function iterates `TDH->files` linked list and calls two functions per file:

```
module+0x1C8E40  — SomeFunc(file): if returns false → SKIP this file, continue
module+0x1C6E90  — AnotherFunc(file): if returns false → ABORT entire function
+0x106: JE +9  — jump to abort path (sets TLS status = 0, returns false)
```

Under Wine, `AnotherFunc` returns false for at least one file, triggering the abort. The form loading loop at `+0x11C` is never reached.

**Fix (v1.22.20):** Temporarily NOP the abort jump (`74 09` → `90 90`), converting "abort all loading" into "skip this file, continue to next." Restore original bytes after the call.

#### Compile State Conflict

ManuallyCompileFiles pre-sets `file->compileIndex` on each TESFile. When AE 13753's own internal compilation pass then calls `AddCompileIndex` per form, the pre-set `compileIndex` causes the internal file iterator to lock — the same form is returned on every iteration, producing an infinite loop (1.5 billion AddCompileIndex calls on a single form with no SeekNextForm in between).

**Fix (v1.22.21):** Reset all `file->compileIndex = 0xFF` and clear `compiledFileCollection` before calling AE 13753, giving it a clean slate. It compiles from scratch with no state conflict, and then drives form loading via its own native loop.

#### TLS Setup

```
+0x28: mov r8d, [rip+...]   — load TLS index
+0x38: mov r9, [rax+r8*8]   — get TLS slot
+0x4F: mov dword [rax], 100 — set loading token = 100
+0x5B: add r13, r9           — r13 = TLS_slot + 0x5D8 (status flag)
+0x5E: mov byte [r13], 1     — set TLS status = 1 (loading)
+0xB7: cmp [rsi+0xDA2], dil  — check TDH->saveLoadGame flag
```

---

### Key Struct Offsets and RELOCATION_IDs

#### TESDataHandler Layout (AE 1.6.1170)

| Offset | Member | Type | Notes |
|--------|--------|------|-------|
| `+0xD58` | `activeFile` | `TESFile*` | Currently-loading file |
| `+0xD60` | `files` | `BSSimpleList<TESFile*>` | All registered files |
| `+0xD70` | `compiledFileCollection` | `TESFileCollection` | — |
| `+0xD70` | `.files` | `BSTArray<TESFile*>` | Regular plugins (compileIndex 0–0xFD) |
| `+0xD88` | `.smallFiles` | `BSTArray<TESFile*>` | ESL/light plugins |
| `+0xDA2` | `saveLoadGame` | `bool` | Selects save-reload vs initial-load path in AE 13753 |
| `+0xDA6` | `hasDesiredFiles` | `bool` | — |
| `+0xDA8` | `loadingFiles` | `bool` | Set by CompileFiles |

#### TESFile Compile Index Fields

| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| `+0x478` | `compileIndex` | `uint8_t` | `0xFF` = uncompiled; `0–0xFD` = regular index; `0xFE` = ESL |
| `+0x47A` | `smallFileCompileIndex` | `uint16_t` | `0–0xFFF` within ESL pool |

#### Loading Pipeline RELOCATION_IDs

| Function | SE ID | AE ID | AE Offset |
|----------|-------|-------|-----------|
| `TESDataHandler::GetSingleton` | 514141 | 400269 | — |
| `TESDataHandler::AddFormToDataHandler` | 13597 | 13693 | `0x1B14C0` |
| `TESDataHandler::ClearData` | 13646 | 13754 | `0x1B9AB0` |
| `TESDataHandler::LoadScripts` | 13657 | 13766 | `0x1BCBC0` |
| `TESFile::OpenTES` | 13855 | 13931 | `0x1C4DA0` |
| `TESFile::CloseTES` | 13857 | 13933 | `0x1C5580` |
| `TESFile::SeekNextForm` | 13894 | 13979 | `0x1C7D30` |
| `TESFile::SeekNextSubrecord` | 13903 | 13990 | `0x1C8590` |
| `TESFile::ReadData` | 13904 | 13991 | `0x1C8640` |
| `TESForm::GetFormByNumericId` | 14461 | 14617 | `0x1E01A0` |
| `AddCompileIndex` | 14509 | 14667 | `0x1E1640` |
| `TESForm::InitializeFormDataStructures` | 14511 | 14669 | `0x1E17F0` |
| `LoadFiles orchestrator (AE 13753)` | — | 13753 | `0x1B96E0` |

---

### Address Library: versionlib Binary Format

The Address Library files (`versionlib-X-X-X-X.bin`) use a delta-encoded binary format. **Format 1** is SE (1.5.97); **Format 2** is AE (1.6.x). The AE 1.6.1170 database contains 428,461 entries.

```
HEADER:
  i32  format        (1 = SE, 2 = AE)
  i32  version[4]    (major, minor, patch, sub)
  i32  nameLen
  char name[nameLen] ("SkyrimSE.exe")
  i32  pointerSize   (8 for 64-bit)
  i32  addressCount

ENTRIES (delta-encoded {id, offset} pairs):
  For each entry:
    byte  typeInfo   (lo nibble = ID encoding, hi nibble = offset encoding)

  Encoding values (lo or hi nibble):
    0 = full uint64
    1 = prev + 1
    2 = prev + uint8
    3 = prev - uint8
    4 = prev + uint16
    5 = prev - uint16
    6 = uint16 (absolute)
    7 = uint32 (absolute)
    If hi bit 3 is set → divide or multiply by pointerSize (8)
```

#### versionlib Exe Hash

The versionlib header stores an 8-byte hash of `SkyrimSE.exe` used to verify the loaded executable matches the database. The hash format is:

```
High DWORD = SizeOfHeaders >> 2
Low DWORD  = derived from exe content (likely involves PE checksum and/or file hash)
```

If `SkyrimSE.exe` has its PE checksum zeroed (common in stripped or modified builds), the stored hash will not match, and SKSE marks all `UsesAddressLibrary()` plugins as disabled and shows a MessageBoxA warning dialog. The function offsets in the database remain valid — only the verification hash fails.

---

### Address Library Dialog Suppression

When Address Library cannot verify the running exe against a versionlib bin, SKSE's plugin loader shows a `MessageBoxA` / `MessageBoxW` with:
- Caption: contains `"SKSE"`
- Body: contains `"Address Library"`

Under CrossOver fullscreen on macOS (and likely other Wine fullscreen configurations), this dialog renders behind the game window. It is invisible and receives no input, permanently blocking the loading screen.

**Fix (this mod):** Hook `MessageBoxA` and `MessageBoxW` in `user32.dll` via `safetyhook::create_inline`. Intercept any call where caption contains `"SKSE"` and body contains `"Address Library"`. Return `IDNO` immediately (equivalent to clicking "No — don't open NexusMods browser"). All other message boxes pass through unmodified.

This hook is installed unconditionally at `SKSEPlugin_Load` time, before any Address Library validation occurs.

```cpp
// user32.dll hooks — suppress SKSE Address Library warning dialog
auto user32 = REX::W32::GetModuleHandleW(L"user32.dll");
auto mbA = GetProcAddress(user32, "MessageBoxA");
g_hk_MessageBoxA = safetyhook::create_inline(mbA, Hooked_MessageBoxA);

int WINAPI Hooked_MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {
    if (lpCaption && strstr(lpCaption, "SKSE") && lpText && strstr(lpText, "Address Library"))
        return IDNO;  // auto-dismiss
    return g_hk_MessageBoxA.call<int>(hWnd, lpText, lpCaption, uType);
}
```

---

### BSResource MAX_PATH Overflow

`BSResource::LooseFileLocation` builds absolute file paths by concatenating the game's working directory with a relative path in a fixed `MAX_PATH`-sized stack buffer. When the resulting path exceeds 259 characters (MAX_PATH minus null terminator), a CRT stack buffer overflow occurs:

- The CRT security cookie at `[rsp + stack_frame_size]` is overwritten by path string data
- The CRT detects the corruption and raises a non-continuable exception via `kernelbase.dll!RaiseException`
- Call stack: `kernelbase → ucrtbase (EINVAL/ERANGE) → SkyrimSE.exe fn69981 → skee64.dll`

**Trigger:** skee64 (RaceMenu / SKSE Character Editor) iterates all loaded plugins at `kDataLoaded` and for each plugin constructs:

```
{GameDir}\data\Meshes\actors\character\FaceGenMorphs\{ESP_NAME}\replacements.ini
```

With the default CrossOver game path `C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\` (70 chars), the fixed portions consume 130 chars, leaving **129 chars** for the ESP name before overflow.

**Formula:** `len(game_dir) + len("data\Meshes\actors\character\FaceGenMorphs\") + len(ESP_name) + len("\replacements.ini") + 1 (null) ≤ 260`

Any ESP with a filename ≥ 130 characters will crash skee64 during FaceGenMorphs scanning. This is silent until the crash — there is no warning. The game appears to load normally until this point.

**Fix:** Rename the offending ESP(s) to fewer than 130 characters and update `Plugins.txt` and `loadorder.txt`. No other ESP needs to reference the renamed file as a master (patch files never serve as masters to other patches).

**Detection:** Search for ESPs with filenames ≥ 130 characters:
```bash
ls "$DATA_DIR" | awk '{if (length($0) >= 130) print length($0), $0}'
```

---

### Static vs Dynamic CRT: The Measurement Trap

SKSE plugins built with static CRT linkage (`/MT`, `CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded"`) get their **own private copy** of the CRT, separate from the game's `ucrtbase.dll`. This creates a common measurement error:

- `_setmaxstdio(8192)` from a static-linked DLL raises **that DLL's** stream limit, not the game's
- `fopen()` / `_wfopen_s()` from a static-linked DLL uses **that DLL's** fd table (default max: 512)
- The game's `ucrtbase.dll` has its own `MSVCRT_max_streams` variable, unaffected

**To measure the game's CRT from a static-linked DLL**, resolve functions via `GetProcAddress`:

```cpp
auto ucrt = GetModuleHandleW(L"API-MS-WIN-CRT-STDIO-L1-1-0.DLL");
auto dyn_wfopen_s = reinterpret_cast<wfopen_s_t>(GetProcAddress(ucrt, "_wfopen_s"));
```

**Three-layer file descriptor stress test:**
1. **Static CRT** — `_wfopen_s` directly → tests your DLL's CRT limit
2. **Dynamic ucrtbase** — `_wfopen_s` via `GetProcAddress` → tests game's CRT limit
3. **Win32 `CreateFile`** — bypasses CRT entirely → tests kernel handle limit

If all three disagree, you are measuring the wrong layer. Under Wine, the game's dynamic CRT handles 2000+ open files after `_setmaxstdio(8192)`. The apparent 512-file limit observed in v1.22.6–7 was the static DLL's own CRT, not the game's.

---

### RELOCATION_ID SE→AE Is Not a Fixed Offset

The offset between SE IDs and AE IDs for the same function is **not constant**, even within the same class:

| SE ID | AE ID | Function | Delta |
|-------|-------|----------|-------|
| 13597 | 13693 | `TESDataHandler::AddFormToDataHandler` | +96 |
| 13618 | 13716 | `TESDataHandler::GetExtCellData` | +98 |
| 13855 | 13931 | `TESFile::OpenTES` | +76 |
| 13894 | 13979 | `TESFile::SeekNextForm` | +85 |
| 14461 | 14617 | `TESForm::GetFormByNumericId` | +156 |
| 14509 | 14667 | `AddCompileIndex` | +158 |

Guessing AE IDs from SE IDs by adding a fixed delta will hook the wrong function. Always verify against:
- The Address Library databases directly (versionlib-1-6-1170-0.bin)
- CommonLibSSE-NG source (`REL::ID` definitions)
- CrashLoggerSSE output from a crash at the target function

---

### Runtime Code Scanning Technique

To find which function calls a known function at runtime (when you have the target address but not the caller):

```cpp
// 1. Locate decrypted .text section from in-memory PE headers
auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase + dos->e_lfanew);
auto* sec = IMAGE_FIRST_SECTION(nt);
uintptr_t textStart = 0;
size_t    textSize  = 0;
for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    if (strncmp((char*)sec[i].Name, ".text", 5) == 0) {
        textStart = moduleBase + sec[i].VirtualAddress;
        textSize  = sec[i].SizeOfRawData;
        break;
    }
}

// 2. Scan for E8 (CALL rel32) and E9 (JMP rel32) to the target
auto* bytes = reinterpret_cast<const uint8_t*>(textStart);
for (size_t i = 0; i + 5 <= textSize; ++i) {
    if (bytes[i] == 0xE8 || bytes[i] == 0xE9) {
        int32_t rel32;
        memcpy(&rel32, &bytes[i + 1], 4);
        auto target = textStart + i + 5 + static_cast<int64_t>(rel32);
        if (target == knownFunctionAddr) {
            // Found a caller; use REL::IDDatabase::Offset2ID to identify it
            auto callSiteOffset = textStart + i - moduleBase;
            // binary-search Offset2ID table for the containing function
        }
    }
}
```

**Caveat:** This technique finds only direct `CALL/JMP rel32` sites. Indirect calls via vtable, function pointer, or register (`call rax`) are invisible to this scan.

**Critical Wine warning:** Loading the `REL::IDDatabase::Offset2ID` reverse-lookup table (428,461 entries, ~7MB allocation) during SKSE plugin load causes Wine to hang at startup indefinitely — TESDataHandler is never created, the game thread stalls at ~200% CPU, and no further loading occurs. Remove any `Offset2ID` access from your production plugin before shipping to Wine users.

---

### Wine-Specific Behaviors

Behaviors confirmed under Wine/CrossOver/Proton that differ from native Windows:

| Behavior | Details |
|----------|---------|
| **CWD not set to game dir** | `wine path/to/skse64_loader.exe` does not cd to the executable's directory. Address Library's relative path fails. Fix: launch via .bat with `cd /d "%~dp0"`. |
| **CompileFiles silently skipped at 600+ files** | See [The 600-File Wine Compilation Limit](#the-600-file-wine-compilation-limit). |
| **TBB crashes** | Intel TBB's TLS and memory pool initialization is incompatible with Wine. Anything using `scalable_malloc` or `concurrent_hash_map` will crash. Replace with HeapAlloc + `std::shared_mutex`. |
| **`Offset2ID` 7MB allocation hangs startup** | Allocating a large sorted vector during DLL load causes Wine's allocator to stall the entire process. Keep early-init allocations small. |
| **MessageBox behind fullscreen window** | Win32 `MessageBoxA/W` calls from within a fullscreen DirectX window render behind it under CrossOver/Proton. The dialog exists and waits, but is invisible and unclickable. |
| **`_setmaxstdio` behaves correctly** | Wine's `ucrtbase` implements `_setmaxstdio` and the game CRT handles 2000+ open files after calling it. No Wine-specific limit beyond this has been observed. |
| **`kActive` flag requires CompileFiles** | `TESFile::RecordFlag::kActive` is set by CompileFiles, not on file construction. Don't use it to determine plugin enabled status — parse `plugins.txt` instead. |

---

## Who Else This Helps

The work in this project has broad applicability beyond just SSE Engine Fixes. The following are concrete examples of projects that could directly benefit from these findings.

### SKSE Plugin Authors Targeting Wine/Proton/Linux

Any SKSE plugin that:
- **Uses Intel TBB** (concurrent data structures, scalable allocator) — replace with `std::shared_mutex` + `std::unordered_map` or HeapAlloc-based pools
- **Calls MessageBoxA/W** during startup or loading — these render invisible under CrossOver fullscreen and block the game
- **Makes large allocations during DLL load** (`DllMain` or early `SKSEPlugin_Load`) — keep early-init allocations small; defer 7MB+ allocations until after `kDataLoaded`
- **Uses relative file paths** — Wine doesn't guarantee CWD is the game directory; resolve paths from `GetModuleFileName` instead

### Mod Managers (Vortex, Mod Organizer 2, Corkscrew)

Mod managers that deploy plugins on Linux/macOS via Wine need to:
- Track the **total compiled file count** (not just plugin count), because the 600-file limit counts ESLs and base game files in addition to user plugins
- Warn users when enabling plugins would push the compiled file count ≥ 600 and this fork is not installed
- Rename or flag ESPs with filenames ≥ 130 characters when the Steam game directory is at the standard CrossOver/Proton path (which gives exactly 130 chars of headroom)
- Detect whether `d3dx9_42.dll` is the SSE Engine Fixes preloader and alert the user to replace it

The compiled file count formula for the standard CrossOver/Proton path:
```
safe_esp_name_length = 259 - len(game_dir) - len("data\Meshes\actors\character\FaceGenMorphs\") - len("\replacements.ini")
# For C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\ (70 chars):
# safe_esp_name_length = 259 - 70 - 42 - 17 = 130 chars max
```

### Wabbajack and Automated Modlist Installers

Large curated modlists (Gate to Sovngarde, Licentia, Nolvus, etc.) often have 1000–2000 plugins. Automated installers targeting macOS/Linux need to:
- **Verify this fork is present** and the original Engine Fixes is absent before considering installation complete
- **Check for long-named ESPs** in the modlist and either rename them or warn the user
- **Compute compiled file count** (user plugins + ~80 CC/base files) and verify it doesn't exceed 599 when this fork is absent

### RaceMenu / skee64 Development

The BSResource MAX_PATH overflow documented above is a bug in how skee64 enumerates `FaceGenMorphs` data. Since skee64 iterates **all loaded plugins** (not just ones that have FaceGenMorphs directories), it constructs a path for every single plugin. With 1700+ plugins, any plugin whose name causes the full path to exceed MAX_PATH will crash the game.

A fix in skee64 itself would be to check path length before calling `BSResource::LooseFileLocation::Open`, or to use heap-allocated path buffers instead of fixed `MAX_PATH` stack arrays.

### ENB and Community Shaders Under Wine

Both ENB and Community Shaders hook into Skyrim's DirectX pipeline. On Wine:
- **ENB** is known to have issues with fullscreen exclusive mode under CrossOver; the same MessageBox visibility problem applies to any dialog ENB might show
- **Community Shaders** hooks shader compilation — the static/dynamic CRT distinction matters if Community Shaders needs to measure or intercept the game's file I/O

### Other Bethesda Games on Proton (Fallout 4, Starfield)

The loading pipeline architecture is similar across Bethesda's Creation Engine games. Specific findings likely to transfer:
- **CompileFiles is a bottleneck** — Fallout 4 and Starfield may have analogous pipeline stages that break differently under Proton
- **TBB replacement need** — any Creation Engine game that uses TBB internally (Fallout 4 does) will have the same crash pattern under Wine
- **BSResource MAX_PATH** — FaceGenMorphs-style fixed-buffer path lookups likely exist in Fallout 4's `BSResource::LooseFileLocation` too

### Lutris / Bottles / Heroic Games Launcher Prefix Scripts

Wine prefix setup tools that target Skyrim modded installs can use this fork's findings to:
- Automate deployment of this DLL and removal of original Engine Fixes
- Set registry key `HKCU\Software\Wine\Mac Driver\CaptureDisplaysForFullscreen = Y` (and related X11 Driver keys) to fix cursor capture and fullscreen behavior that would otherwise cause the MessageBox-behind-fullscreen issue
- Create a SKSE launch wrapper `.bat` to set the CWD correctly

### Address Library Fork Maintainers

The versionlib hash format documented in [Address Library: versionlib Binary Format](#address-library-versionlib-binary-format) explains why builds of `SkyrimSE.exe` with zeroed PE checksums (modified/stripped builds) fail Address Library verification even when the actual code is byte-identical to a known build. A fork of Address Library that performs a fallback code-signature verification (rather than pure hash check) would allow these builds to verify correctly without needing a new versionlib entry.

---

## Diagnostic Version History

| Version | Change |
|---------|--------|
| v1.22.0 | Instrumented 10 loading pipeline functions + 200ms monitor thread |
| v1.22.1 | Added OpenTES/CloseTES hooks |
| v1.22.2 | Added AddCompileIndex + SeekNextForm hooks |
| v1.22.3 | Added CloseTES hook + plugins.txt verification |
| v1.22.4 | Hooked `_setmaxstdio` — confirmed game never resets the limit |
| v1.22.5 | Added `kActive`/`kMaster`/`kSmallFile` flag counting |
| v1.22.6–7 | FD stress tests (appeared to show 512 CRT limit — was measurement error, see [Static vs Dynamic CRT](#static-vs-dynamic-crt-the-measurement-trap)) |
| v1.22.8 | Fixed FD stress test methodology — debunked file handle limit theory |
| v1.22.9 | CompileFiles pipeline hooks (crashed — wrong RELOCATION_IDs) |
| v1.22.10 | Removed crashed hooks, added DumpFilesListState diagnostics |
| v1.22.11 | Runtime code scanner + `ManuallyCompileFiles()` — first working fix for 600-file limit |
| v1.22.12 | Monitor-based compilation detection (replaced broken inline hook on AE 11596) |
| v1.22.13 | Disabled runtime scanner (suspected early-termination side effect) |
| v1.22.14 | Fixed `ManuallyCompileFiles` — parse `plugins.txt` for enabled status (`kActive = 0` when skipped) |
| v1.22.15 | Main-thread trigger via CloseTES hook; confirmed: `loadingFiles=true` alone does not trigger form loading |
| v1.22.16 | Hooked AE 11596 (CompileFiles candidate) — confirmed NEVER called during initial load |
| v1.22.17 | Multi-target `.text` scanner: scan for callers of OpenTES, AddFormToDataHandler, AddCompileIndex |
| v1.22.18 | Removed `.text` scanner (caused total startup hang via 7MB Offset2ID allocation under Wine). Tested AE 13753 (0 forms, wrong signature) and AE 13698 (78 engine defaults only). |
| v1.22.19 | **Breakthrough:** Hex dump of AE 13753; manual disassembly revealed correct 2-param signature, validation loop, and abort jump at `+0x106` |
| v1.22.20 | NOP abort jump (`74 09` → `90 90`), call AE 13753 with `(TDH*, false)` — forms load but infinite loop on Denizens of Morthal (1.5B AddCompileIndex calls) |
| v1.22.21 | **Full solution:** Reset `compileIndex = 0xFF` on all files before AE 13753 call to eliminate compile-state conflict. 284,294 forms loaded, game reaches main menu with 1720 plugins. |
| v1.22.22 | `ForceLoadAllForms` at `kDataLoaded` as safety fallback for cases where manual compilation ran but form loading still didn't trigger |
| v1.22.23 | Added `suppress_address_library_dialog` — hooks `MessageBoxA/W` in `user32.dll` to auto-dismiss SKSE Address Library warning dialog that renders invisibly behind CrossOver fullscreen |
| v1.22.24 | Suppress SKSE Address Library warning dialog for AE builds. Confirmed with Gate to Sovngarde (1720+ plugins, 3710 total compiled files, 1,069,819 forms): main menu in 117s. |

---

## Credits

All credit for the original reverse engineering and bug fixes goes to **[aers](https://github.com/aers)** and contributors of [SSE Engine Fixes](https://github.com/aers/EngineFixesSkyrim64).

- Original mod: [SSE Engine Fixes on NexusMods](https://www.nexusmods.com/skyrimspecialedition/mods/17230)
- Original source: [EngineFixesSkyrim64 on GitHub](https://github.com/aers/EngineFixesSkyrim64)

This fork is maintained by [Corkscrew](https://github.com/corkscrewmodding) for the macOS and Linux Skyrim modding community.

## License

MIT License (same as upstream SSE Engine Fixes)
