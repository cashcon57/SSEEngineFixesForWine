# SSE Engine Fixes for Wine

A **Wine/Proton/CrossOver-compatible** fork of [SSE Engine Fixes](https://github.com/aers/EngineFixesSkyrim64) by aers.

This mod provides the same bug fixes and performance patches as SSE Engine Fixes, rewritten to work under Wine, Proton, and CrossOver (macOS and Linux). It is a **drop-in replacement** — you do not need the original SSE Engine Fixes installed.

## What's Different

SSE Engine Fixes uses a two-phase loading mechanism:
1. **Phase 1 (Preload):** A DLL disguised as `d3dx9_42.dll` loads before SKSE and installs most hooks
2. **Phase 2 (SKSE Load):** The SKSE plugin registers message handlers

This preload phase is **incompatible with Wine/CrossOver/Proton** — it causes crashes during game initialization, especially with large mod lists.

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

**Phase 2 — Form Loading (IN PROGRESS, v1.22.18):** Despite successful compilation, form loading never starts on its own. The engine's code path already branched past form loading before ManuallyCompileFiles ran. We must invoke form loading ourselves at kDataLoaded. Key findings:

- **AE 11596 is NOT the initial loading function** (v1.22.16): Hook installed but never entered. The initial data load uses a different code path.
- **loadingFiles is a status flag, not a gate** (v1.22.15): Setting it to true on the main thread does not cause the form loading loop to start.
- **v1.22.17 .text scanner identified two candidates** but had to be removed — its Offset2ID allocation (428K entries) caused a total startup hang (zero activity, TDH never created, 202% CPU for 47 min):
  - **AE 13698** at offset 0x1B1D30: 54 AddFormToDataHandler call sites
  - **AE 13753** at offset 0x1B96E0: 2 OpenTES call sites (loading orchestration)
  - AE 13785 at offset 0x1BE460: 3 OpenTES calls (HotLoadPlugin — irrelevant)
- **v1.22.18 tested both candidates via ForceLoadAllForms():**
  - **AE 13753** (orchestration): Called as `void(TESDataHandler*)` — returned in 7ms producing 0 forms, 0 OpenTES calls. Likely wrong signature or checks an internal state flag that causes early return.
  - **AE 13698** (form loading): Called as `void(TESDataHandler*)` — produced 78 AddFormToDataHandler calls but 0 OpenTES calls. All 78 forms were low-formID engine defaults (0x1F5, 0x1A, 0x1B, etc.). **This function creates built-in default forms, not file-loaded forms.**
- **Neither AE function loads forms from plugin files.** The actual per-file form loading loop must be implemented manually.

**Next step (v1.22.19):** Implement a custom form loading loop using CommonLib primitives:
1. Iterate `compiledFileCollection.files` and `.smallFiles`
2. For each file: `file->OpenTES()` → iterate records with `file->SeekNextForm(true)` → `file->GetFormType()` → `IFormFactory::GetFormFactoryByType(ft)->Create()` → `form->Load(file)` → `TESDataHandler::AddFormToDataHandler(form)`
3. Handle FormID compilation via `TESForm::AddCompileIndex(&id, file)`
4. Skip TES4/GRUP header records

All required CommonLib wrappers verified available: `TESFile::OpenTES` (AE 13931), `SeekNextForm` (AE 13979), `GetFormType` (AE 13982), `ReadData` (AE 13991), `CloseTES` (AE 13933), `IFormFactory::GetFormFactoryByType` (global array at AE 400508), `TESForm::AddCompileIndex` (AE 14667).

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

**Current status (v1.22.18):** ManuallyCompileFiles successfully populates compiledFileCollection (211 reg + 1589 ESL = 1800 files) and sets loadingFiles=true on the main thread during the catalog scan. However, the form loading loop (step 3) never executes because the engine's code path already branched past it. The v1.22.17 .text scanner identified AE 13698 and AE 13753 as candidates, but testing in v1.22.18 revealed that AE 13698 only creates 78 built-in engine forms (not file-loaded) and AE 13753 returns without doing anything. **The actual form loading must be implemented as a custom loop using CommonLib's TESFile I/O + IFormFactory primitives** — this is the v1.22.19 task.

### TESDataHandler Key Offsets (AE 1.6.1170)

| Offset | Member | Type |
|--------|--------|------|
| 0xD58 | activeFile | TESFile* |
| 0xD60 | files | BSSimpleList\<TESFile*\> |
| 0xD70 | compiledFileCollection | TESFileCollection |
| 0xD70 | .files | BSTArray\<TESFile*\> (regular plugins) |
| 0xD88 | .smallFiles | BSTArray\<TESFile*\> (ESL/light plugins) |
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

### AE 13698 and AE 13753: Not the File-Loading Functions (v1.22.18)

The v1.22.17 .text scanner identified two promising functions by counting call sites to known loading primitives:

- **AE 13698** (offset 0x1B1D30): 54 `AddFormToDataHandler` call sites — appeared to be the big form-type switch that loads all ~54 form types from files.
- **AE 13753** (offset 0x1B96E0): 2 `OpenTES` call sites — appeared to be the orchestrator that opens files and calls the form reader.

**Testing in v1.22.18 proved both wrong:**

| Function | Signature Used | Result |
|----------|---------------|--------|
| AE 13753 | `void(TESDataHandler*)` | 0 forms, 0 OpenTES, returned in 7ms |
| AE 13698 | `void(TESDataHandler*)` | 78 AddForm calls, 0 OpenTES — all low-formID engine defaults |

AE 13698's 78 forms had formIDs like 0x1F5, 0x1A, 0x1B — these are hardcoded engine defaults created without reading any files. The 54 AddFormToDataHandler call sites correspond to 54 different default form type initializations, not per-file form reading. AE 13753 likely checks an internal state flag or requires additional parameters.

**Lesson:** A high count of `AddFormToDataHandler` call sites does NOT mean the function reads forms from files — it may just create built-in default instances of many different form types.

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

## Credits

All credit for the original reverse engineering and bug fixes goes to **[aers](https://github.com/aers)** and contributors of [SSE Engine Fixes](https://github.com/aers/EngineFixesSkyrim64).

- Original mod: [SSE Engine Fixes on NexusMods](https://www.nexusmods.com/skyrimspecialedition/mods/17230)
- Original source: [EngineFixesSkyrim64 on GitHub](https://github.com/aers/EngineFixesSkyrim64)

This fork is maintained by [Corkscrew](https://github.com/corkscrewmodding) for the macOS and Linux Skyrim modding community.

## License

MIT License (same as upstream SSE Engine Fixes)
