# SSE Engine Fixes for Wine

Wine/CrossOver/Proton-compatible fork of SSE Engine Fixes for Skyrim SE/AE.
SKSE plugin (C++17, CommonLibSSE). License: MIT.

## Build

Windows-only (MSVC required). CI builds via GitHub Actions on push to main or tag.

```bash
# Local build (Windows only)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SKYRIMAE=ON \
  -DVCPKG_TARGET_TRIPLET=x64-windows-static \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Tag with `v*` to trigger a release build (creates GitHub Release with SE + AE zips).

## Project Structure

| Path | Purpose |
|------|---------|
| `src/patches/form_caching.h` | **ALL** form loading, crash fixes, VEH handlers, sentinel page |
| `src/patches/patches.cpp` | Patch registration and Install() entry point |
| `src/main.cpp` | SKSE plugin entry, message handler (kDataLoaded, kNewGame, etc.) |
| `SSEEngineFixesForWine.toml` | Default config shipped with DLL |
| `.github/workflows/build.yml` | CI: builds SE + AE, creates release on tag |

## Key Differences from Original Engine Fixes

- No d3dx9_42.dll preloader (crashes Wine)
- No TBB dependency (`std::shared_mutex` + 256-shard map instead)
- No memory manager overrides (Wine-incompatible)
- All hooks moved to SKSE plugin load phase

## Coding Standards

- Write code to a senior-engineer standard
- All code must be VEH-safe where applicable (no heap allocations in exception handlers)
- Use `char[MAX_PATH]` globals for paths accessed from VEH context
- Memory ordering: `release` on stores, `acquire` on loads for cross-thread flags
- RAII for VEH handlers (VehGuard pattern)

## Cross-Repo Integration

This plugin is auto-deployed by **Corkscrew** (mod manager):
- Corkscrew's `skse.rs` downloads latest release from GitHub and deploys DLL + TOML
- DLL naming convention: `0_SSEEngineFixesForWine.dll` (must keep `0_` prefix)
- TOML naming: `SSEEngineFixesForWine.toml` (deployed alongside DLL)
- Custom overrides: `SSEEngineFixesForWineCustom.toml` (user edits, preserved across updates)

Changes to DLL name, TOML schema, or GitHub release artifact names will break Corkscrew integration.

## Memory

Shared auto-memory with Corkscrew project at:
`~/.claude/projects/-Users-cashconway-Corkscrew/memory/`

Key files:
- `sseef-wine.md` — RE findings, architecture, crash history, key offsets
- `engine-fixes-wine.md` — Corkscrew integration details
- `audit-findings.md` — Code quality audit status

After significant changes, update these memory files (especially version numbers and crash history).

## Deployment & Testing

```bash
# Deploy: copy AE DLL from CI artifacts to game's SKSE plugins dir
# Test via Corkscrew CLI:
/Users/cashconway/Corkscrew/src-tauri/target/aarch64-apple-darwin/release/corkscrew \
  --launch skyrimse Steam --skse

# Log locations:
# SKSE log: ~/Documents/My Games/Skyrim Special Edition/SKSE/SSEEngineFixesForWine.log
# Crash log: <bottle>/drive_c/users/crossover/My Documents/My Games/Skyrim Special Edition/SKSE/SSEEngineFixesForWine_crash.log
```
