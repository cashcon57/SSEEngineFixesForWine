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
- **Editor ID cache** — diagnostic + safe repopulation of editor IDs for Wine compatibility (investigating form loading with large mod lists)

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

## Known Issues / Active Investigation

**Form loading with large mod lists under Wine (v1.22.x):**

With the Gate to Sovngarde modlist (1720 enabled plugins, 3368 total files, 485 BSAs), the game's record-loading pipeline is completely skipped — `AddCompileIndex` is never called, `loadingFiles` is never set to `true`, and `AddFormToDataHandler` registers 0 forms. The game goes from catalog scan (SeekNextForm) straight to `kDataLoaded` with only ~166 engine-internal forms.

**Root Cause: Under Investigation (v1.22.9)**

Through extensive binary search and diagnostic instrumentation (v1.22.0–v1.22.9), the root cause has been narrowed but not yet identified:

| Finding | Detail |
|---------|--------|
| **Threshold** | Exactly **599 compiled files** work; **600+ fails** |
| **Count-based** | Swapping which plugins are enabled doesn't matter — only total compiled count |
| **NOT file handles** | 3-way FD stress test (static CRT, dynamic ucrtbase, Win32 CreateFile) all show **2000+ available** after `_setmaxstdio(8192)` |
| **NOT Unix FDs** | The game process has 4000+ Unix FDs available (via lsof); wineserver has 6000+ |
| **NOT `_setmaxstdio` reset** | Hooked `_setmaxstdio` — game never calls it again after our patch; stays at 8192 |

**CRT file handle theory debunked (v1.22.8):**
- v1.22.6–v1.22.7 appeared to show a CRT limit of 508/512 files, but this was a **measurement error**
- Our SKSE DLL uses **static CRT** (`/MT` link), which has its own `MSVCRT_max_streams` separate from the game's dynamic ucrtbase.dll
- The stress test was calling `_wfopen_s` in our own static CRT (maxstdio=512), not the game's dynamic CRT (maxstdio=8192)
- After fixing the test to: (1) raise our own static CRT's maxstdio, and (2) test the game's ucrtbase via GetProcAddress — all three layers show 2000+
- **Conclusion: file handles are NOT the bottleneck**

**Current investigation (v1.22.9):**
- Hooking CompileFiles pipeline functions (AE IDs 13707, 13716, 13721) to trace which loading phases execute
- These unnamed TESDataHandler functions control compile index assignment and the `loadingFiles` flag
- Goal: determine whether CompileFiles is never called, exits early, or the compile index loop iterates 0 times

**Diagnostic versions:**
- v1.22.0–v1.22.2: Instrumented all 10 loading pipeline functions (AddFormToDataHandler, OpenTES, AddCompileIndex, etc.)
- v1.22.3: Added loading monitor thread with 200ms polling of TESDataHandler state
- v1.22.4: Hooked `_setmaxstdio` — confirmed game never resets the limit
- v1.22.5: Added kActive/kMaster/kSmallFile flag counting
- v1.22.6–v1.22.7: FD stress tests (CRT + Win32) — initially appeared to show 512 CRT limit
- v1.22.8: Fixed FD stress test (static vs dynamic CRT) — **debunked file handle theory**
- v1.22.9: CompileFiles pipeline hooks to trace compile index assignment

**Eliminated causes:**
- **CRT file handles** (v1.22.8 — static CRT measurement error; actual limit is 2000+)
- Memory allocator (v1.21.0 HeapAlloc replacement works fine, 0 failures)
- Skyrim.ini settings (AE ignores them)
- FormScatterTable::SetAt hooks (disabled, not the cause)
- Specific plugin files (count-based, not file-specific)
- plugins.txt entry count (only compiled file count matters)
- kActive flag (never set, even in working cases — normal behavior)

## Credits

All credit for the original reverse engineering and bug fixes goes to **[aers](https://github.com/aers)** and contributors of [SSE Engine Fixes](https://github.com/aers/EngineFixesSkyrim64).

- Original mod: [SSE Engine Fixes on NexusMods](https://www.nexusmods.com/skyrimspecialedition/mods/17230)
- Original source: [EngineFixesSkyrim64 on GitHub](https://github.com/aers/EngineFixesSkyrim64)

This fork is maintained by [Corkscrew](https://github.com/corkscrewmodding) for the macOS and Linux Skyrim modding community.

## License

MIT License (same as upstream SSE Engine Fixes)
