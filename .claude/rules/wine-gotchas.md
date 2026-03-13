---
description: Wine/CrossOver-specific behavioral differences that affect code correctness
globs: ["src/**"]
---

# Wine Behavioral Differences

These are non-obvious Wine behaviors that WILL cause bugs if ignored:

- **VirtualProtect can revert file-backed code pages**: After patching game .text bytes, NEVER restore the original page protection. Leave pages as PAGE_EXECUTE_READWRITE.
- **SuspendThread returns err=5, GetThreadContext returns err=31**: Wine blocks these after 2-3 calls. Cannot reliably sample main thread RIP. Use direct stack scanning as fallback.
- **Low-address VirtualAlloc fails**: `g_lowNullGuard` (mapping memory at address 0) does not work under Wine. Null dereferences must be caught by VEH.
- **SafetyHook grow hooks may not fire**: `g_growCallCount` can be 0 even after loading. Capture BST sentinel from form map in `GetFormByNumericId` as fallback.
- **Thread scheduling is more aggressive**: Concurrent `BSTHashMap::SetAt` calls create circular chains. All SetAt variants are serialized with a spinlock (with Sleep(0) backoff after 64 spins).
