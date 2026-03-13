---
description: Rules for code running inside Vectored Exception Handlers (VEH)
globs: ["src/patches/form_caching.h"]
---

# VEH-Safe Code Rules

Code inside VEH handlers (`CrashLoggerVEH`, `FormReferenceFixupVEH`, `ForceLoadVEH`) must follow these constraints:

## No Heap Allocations
- Use `VehLog()` for logging (pre-opened HANDLE + WriteFile), never `fopen_s`/`fprintf`/`fclose`
- Use stack-local `char[]` buffers, never `std::string` or `std::vector`
- Use `char[MAX_PATH]` globals for paths (e.g., `g_crashLogPath`)

## No Unsafe APIs
- Use `IsReadableMemory()` (VirtualQuery-based), never `IsBadReadPtr` (can recurse/deadlock in VEH)
- Never call `malloc`/`new`/`free`/`delete` — CRT heap may be corrupted when VEH fires

## Memory Access
- Always check `IsReadableMemory()` before dereferencing any pointer from game state
- Use `VirtualQuery` for memory checks, not SEH inside VEH

## Atomics in VEH
- VEH handlers can run on any thread — use atomics for all shared state
- Use `std::memory_order_relaxed` for counters, `acquire`/`release` for flags
- For dedup arrays (e.g., `g_zeroDumpedRips`), claim slots with `fetch_add` not `++`
