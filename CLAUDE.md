# SSE Engine Fixes for Wine

Wine/CrossOver/Proton-compatible fork of SSE Engine Fixes for Skyrim SE/AE.
SKSE plugin (C++17, CommonLibSSE, mimalloc). License: MIT.

## Build

Windows-only (MSVC). CI builds on push to main; tag `v*` triggers release (SE + AE zips).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SKYRIMAE=ON \
  -DVCPKG_TARGET_TRIPLET=x64-windows-static \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## Project Structure

| Path | Purpose |
|------|---------|
| `src/patches/form_caching.h` | Form loading, VEH handlers, sentinel page, code cave patches, watchdog |
| `src/patches/patches.cpp` | Patch registration / Install() dispatch |
| `src/main.cpp` | SKSE entry, message handler (kDataLoaded, kNewGame) |
| `SSEEngineFixesForWine.toml` | Default config (keys must match `src/settings.h`) |
| `.claude/rules/` | VEH safety, Wine gotchas, atomics, RE workflow |

## Key Architectural Decisions

- No d3dx9_42.dll preloader (crashes Wine) — all hooks at SKSE plugin load
- Memory allocator: mimalloc (`mi_zalloc`/`mi_free`), not TBB
- Form lookups: 256-shard `std::shared_mutex` + `std::unordered_map` cache, authoritative after kDataLoaded
- BSTHashMap::SetAt serialized with global spinlock (Wine threading races)
- VEH logging via `VehLog()` (pre-opened HANDLE + WriteFile) — see `.claude/rules/veh-safety.md`

## Cross-Repo Integration

Auto-deployed by **Corkscrew** (mod manager) — `skse.rs` downloads latest GitHub release:
- DLL: `0_SSEEngineFixesForWine.dll` (must keep `0_` prefix)
- Config: `SSEEngineFixesForWine.toml` / `SSEEngineFixesForWineCustom.toml`

Changes to DLL name, TOML schema, or release artifact names break Corkscrew integration.

## Testing

```bash
# Launch via Corkscrew (auto-deploys latest DLL):
corkscrew --launch skyrimse Steam --skse

# Direct launch (bypasses auto-deploy — tests current DLL in bottle):
'/Users/cashconway/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition/skse64_loader.exe'
```

**Log locations** (both symlink to native `~/Documents`):
- SKSE log: `~/Documents/My Games/Skyrim Special Edition/SKSE/SSEEngineFixesForWine.log`
- Crash log: `~/Documents/My Games/Skyrim Special Edition/SKSE/SSEEngineFixesForWine_crash.log`

## Memory

Shared auto-memory with Corkscrew at `~/.claude/projects/-Users-cashconway-Corkscrew/memory/`:
- `sseef-wine.md` — RE findings, crash history, key offsets, code cave registry

Local project memory at `~/.claude/projects/-Users-cashconway-SSEEngineFixesForWine/memory/`:
- `MEMORY.md` — current status, root cause analysis, lessons learned

After significant changes, update version numbers and crash history in both.
