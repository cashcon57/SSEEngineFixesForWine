---
description: Atomic variable conventions and thread safety patterns
globs: ["src/patches/form_caching.h", "src/main.cpp"]
---

# Atomic Conventions

## Memory Ordering
- `release` on stores, `acquire` on loads for cross-thread flags
- `relaxed` for counters and diagnostics
- `acq_rel` for compare-exchange operations

## Patterns
- First-writer-wins: use `compare_exchange_strong(nullptr, value)` not plain store
- Slot claiming: use `fetch_add` then bounds-check, not load+check+store
- Best-effort guards (e.g., `g_sentinelRepairInProgress`): use `exchange(true)` at entry, `store(false)` at exit

## What Must Be Atomic
- Any `void*` or `bool` read from VEH handlers (can fire on any thread)
- Any global written by grow/SetAt hooks (called from game worker threads)
- Dedup arrays: use atomic count + non-atomic array (count is the synchronization point)
