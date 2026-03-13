#pragma once

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "tree_lod_reference_caching.h"

// Wine-compatible form caching — replaces tbb::concurrent_hash_map with
// std::shared_mutex + std::unordered_map (256 shards by master index)

// notes on form caching
// 14688 - SetAt - inserts form to map, replaces if it already exists
// 14710 - RemoveAt - removes form from map
// g_FormMap xrefs
// 13689 - TESDataHandler::dtor - clears entire form map, called by TES::dtor, this only happens on game shutdown
// 13754 - TESDataHandler::ClearData - clears entire form map, called on game shutdown but also on Main::PerformGameReset, hook to clear our cache
// 13785 - HotLoadPlugin command handler - no one should be using this, so don't worry about it
// 14593 - TESForm::ctor - calls RemoveAt and SetAt, so handled by those hooks
// 14594 - TESForm::dtor_1 - deallocs the form map if there are zero forms
// 14617 - TESForm::GetFormByNumericId - form lookup, hooked
// 14627 - TESForm::RemoveFromDataStructures - calls RemoveAt, handled by that hook
// 14666 - TESForm::SetFormId - changes formid of form, if form is NOT temporary, removes old id from map and adds new one, calls SetAt/RemoveAt
// 441564 - TESForm::ReleaseFormDataStructures - deletes form map, inlined in TESForm dtors
// 14669 - TESForm::InitializeFormDataStructures - creates new empty form map, hook to clear our cache
// 14703 - TESForm::dtor_2 - deallocs the form map if there are zero forms
// 22839 - ConsoleFunc::Help - inlined form reader
// 22869 - ConsoleFunc::TestCode - inlined form reader
// 35865 - LoadGameCleanup - inlined form reader

namespace Patches::FormCaching
{
    namespace detail
    {
        // ================================================================
        // v1.22.0: Loading pipeline instrumentation counters
        // These track calls to critical loading functions to determine
        // whether forms are ever created during the 51-second load.
        // ================================================================
        inline std::atomic<std::uint64_t> g_clearDataCalls{ 0 };
        inline std::atomic<std::uint64_t> g_initFormDataCalls{ 0 };
        inline std::atomic<std::uint64_t> g_addFormCalls{ 0 };
        inline std::atomic<std::uint64_t> g_addFormNullCalls{ 0 };
        inline std::atomic<std::uint64_t> g_openTESCalls{ 0 };
        inline std::atomic<std::uint64_t> g_openTESSuccesses{ 0 };
        inline std::atomic<std::uint64_t> g_addCompileIndexCalls{ 0 };
        inline std::atomic<std::uint64_t> g_seekNextFormCalls{ 0 };
        inline std::atomic<std::uint64_t> g_closeTESCalls{ 0 };
        inline std::atomic<std::uint64_t> g_seekNextSubrecordCalls{ 0 };
        inline std::atomic<std::uint64_t> g_readDataCalls{ 0 };
        inline std::atomic<std::uint64_t> g_readDataBytes{ 0 };
        inline std::atomic<bool> g_seekNextFormReturnedFalse{ false };

        // v1.22.23: ForceLoadAllForms VEH support
        // g_forceLoadInProgress: true while AE 13753 is executing — suppresses
        // CloseTES hook's ManuallyCompileFiles call (prevents compiledFileCollection
        // iterator invalidation while AE 13753 iterates it).
        // g_vehSkipCount: number of null dereferences skipped by ForceLoadVEH.
        inline std::atomic<bool> g_forceLoadInProgress{ false };
        inline std::atomic<std::uint64_t> g_vehSkipCount{ 0 };

        // v1.22.27: Form-reference fixup VEH — persistent after ForceLoadAllForms.
        // Catches access violations where a register holds a raw form ID
        // (< 0x10000000) instead of a resolved pointer, resolves it via
        // TESForm_GetFormByNumericId, and continues execution.
        inline std::atomic<bool> g_formFixupActive{ false };
        inline std::atomic<std::uint64_t> g_formFixupCount{ 0 };
        // v1.22.27: Count of forms that faulted during InitItem (SEH-caught)
        inline std::atomic<std::uint64_t> g_initFaultCount{ 0 };
        // v1.22.36: Sentinel form page — a fake TESForm that looks "deleted"
        // to the engine. VEH redirects null form pointers here. The engine
        // reads kDeleted from formFlags and skips processing entirely.
        //
        // Layout (PAGE_READONLY, 64KB):
        //   [0x0000]: vtable ptr → g_stubVtable (array of "ret 0" stubs)
        //   [0x0010]: formFlags = 0x20 (kDeleted)
        //   [0x0014]: formID = 0
        //   [0x001A]: formType = 0
        //   [0x04B8]: stub function ptr (for direct function pointer fields)
        //   All other bytes: 0
        //
        // A companion "stub function" page (PAGE_EXECUTE_READ) holds
        // `xor eax, eax; ret` — all virtual/function pointer calls return 0.
        // A "stub vtable" page (PAGE_READONLY) holds 512 pointers to the stub.
        inline void* g_zeroPage = nullptr;
        inline void* g_stubFuncPage = nullptr;
        inline void* g_stubVtable = nullptr;
        inline DWORD64 g_zeroPageBase = 0;
        inline DWORD64 g_zeroPageEnd = 0;
        inline std::atomic<std::uint64_t> g_zeroPageUseCount{ 0 };
        inline std::atomic<std::uint64_t> g_zeroPageWriteSkips{ 0 };
        inline std::atomic<std::uint64_t> g_catchAllCount{ 0 };
        // v1.22.52: Track whether zero page is currently PAGE_READWRITE.
        // Only set during ForceLoadAllForms reload; cleared when it restores PAGE_READONLY.
        // NEVER set during gameplay — sentinel must stay PAGE_READONLY to prevent
        // vtable corruption from engine writes.
        inline std::atomic<bool> g_zpWritable{false};
        // v1.22.46: Main thread handle for watchdog RIP sampling
        inline HANDLE g_mainThreadHandle = nullptr;
        inline DWORD g_mainThreadId = 0;
        // v1.22.95: Main thread stack bounds for direct stack scanning
        // (fallback when GetThreadContext fails under Wine).
        inline DWORD64 g_mainThreadStackBase = 0;   // high address (TEB+0x08)
        inline DWORD64 g_mainThreadStackLimit = 0;   // low address (TEB+0x10)
        inline DWORD64 g_mainThreadLastRsp = 0;       // from last successful GetThreadContext
        inline std::atomic<std::uint64_t> g_sentinelUseCount{ 0 };

        // v1.22.51: Persistent VEH handles — stored for potential cleanup.
        inline PVOID g_vehFormFixup = nullptr;
        inline PVOID g_vehCrashLogger = nullptr;

        // v1.22.57: Low null guard — map readable memory at address 0.
        // If Wine allows this, null-form dereferences ([null+offset]) read
        // zeros silently with zero VEH overhead. Eliminates ALL null-form
        // floods regardless of code site.
        inline void* g_lowNullGuard = nullptr;
        inline DWORD64 g_lowNullGuardEnd = 0;

        // v1.22.54: Code cave fault recovery.
        // When a garbage form pointer (not null, not valid heap) causes a fault
        // inside a code cave, the VEH redirects execution to the cave's null path.
        // This handles the rare case where form pointers are corrupted data
        // (e.g., XML strings 0x3C003C00 or non-canonical 0x9E50...) that pass
        // the inline null check (cmp reg, 0x10000) but aren't valid memory.
        struct CavePatchInfo {
            std::uintptr_t caveStart;
            std::uintptr_t caveEnd;
            std::uintptr_t nullReturnAddr;  // game address to resume at (null/skip path)
            const char* name;               // for logging
            std::uint8_t* patchSite;        // original code address (for watchdog verification)
            int patchSize;                   // number of bytes overwritten
        };
        inline CavePatchInfo g_cavePatches[32] = {};
        inline std::atomic<int> g_numCavePatches{ 0 };
        inline std::atomic<std::uint64_t> g_caveFaultCount{ 0 };
        inline std::atomic<std::uint64_t> g_execRecoverCount{ 0 };

        inline void RegisterCavePatch(std::uint8_t* caveStart, int caveSize,
                                      std::uintptr_t nullReturnAddr, const char* name,
                                      std::uint8_t* patchSite = nullptr, int patchSize = 0)
        {
            constexpr int kMaxCavePatches = static_cast<int>(sizeof(g_cavePatches) / sizeof(g_cavePatches[0]));
            // Atomically claim a slot via CAS — safe even if called concurrently
            int idx = g_numCavePatches.load(std::memory_order_relaxed);
            while (idx < kMaxCavePatches) {
                if (g_numCavePatches.compare_exchange_weak(idx, idx + 1, std::memory_order_acq_rel))
                    break;
            }
            if (idx >= kMaxCavePatches) {
                logger::error("RegisterCavePatch: OVERFLOW — {} not registered (idx={}, max={})",
                    name, idx, kMaxCavePatches);
                return;
            }
            g_cavePatches[idx].caveStart = reinterpret_cast<std::uintptr_t>(caveStart);
            g_cavePatches[idx].caveEnd = reinterpret_cast<std::uintptr_t>(caveStart) + caveSize;
            g_cavePatches[idx].nullReturnAddr = nullReturnAddr;
            g_cavePatches[idx].name = name;
            g_cavePatches[idx].patchSite = patchSite;
            g_cavePatches[idx].patchSize = patchSize;
        }

        // v1.22.76: INT3 probing for freeze detection.
        // When the watchdog detects a freeze (counters stable for 30+ seconds),
        // it writes INT3 (0xCC) at candidate loop entry points. When the VEH
        // catches EXCEPTION_BREAKPOINT, it logs the RIP and registers, restores
        // the original byte, and continues. This reveals which loop is stuck.
        struct ProbeInfo {
            std::uintptr_t offset;      // offset from image base
            std::uint8_t origByte;
            std::atomic<bool> active{false};
            std::atomic<bool> fired{false};
        };
        // Candidate loop entry points identified via backward-jump analysis:
        inline ProbeInfo g_probes[] = {
            { 0x179218 },  // refcount release loop (back-edge at +0x17923E)
            { 0x179420 },  // linked list loop 1 in form type processing (back-edge at +0x1794B7)
            { 0x1794D3 },  // linked list loop 2 in form type processing (back-edge at +0x1795C3)
            { 0x179548 },  // hash chain inner loop (back-edge at +0x179556)
            { 0x1AF612 },  // hash chain in form ref lookup (back-edge at +0x1AF61E)
        };
        constexpr int g_numProbes = sizeof(g_probes) / sizeof(g_probes[0]);
        inline std::atomic<bool> g_probesInstalled{false};

        // v1.22.86: Authoritative cache flag.  After kDataLoaded fires,
        // our cache contains all forms from ESM loading + SetAt hooks.
        // From this point, cache misses return nullptr instead of falling
        // through to the native BSTHashMap::find, which has corrupted
        // bucket chains from grow/rehash races under Wine.
        // Manual "New Game" does NOT call ClearData, so ESM forms persist
        // and the cache remains valid across sessions.
        inline std::atomic<bool> g_cacheAuthoritative{false};

        // v1.22.91: BSTScatterTable sentinel pointer — the address of the
        // game's BSTScatterTableSentinel bytes {0xDE,0xAD,0xBE,0xEF}.
        // Captured from the global form map (in GetFormByNumericId hook)
        // or from grow() hooks as fallback. Written into g_zeroPage at
        // +0x10/+0x28 so that when VEH CASE 1 redirects a null chain
        // follow to g_zeroPage, the find loop reads sentinel from
        // [g_zeroPage+0x10] → sentinel comparison matches → loop exits.
        inline std::atomic<void*> g_bstSentinel{nullptr};
        inline std::atomic<std::uint64_t> g_growCallCount{ 0 };

        // v1.22.85: Lazy cache invalidation for New Game (DISABLED in v1.22.86).
        // Kept for reference.  ESM forms persist across New Game — clearing
        // the cache forces 900+ lookups to fall through to the corrupted
        // native map, causing the freeze.
        inline std::atomic<bool> g_needsCacheClear{false};

        // v1.22.97: Guard flag to prevent VEH/watchdog from tearing g_zeroPage
        // concurrently. Best-effort: VEH cannot take a mutex.
        inline std::atomic<bool> g_sentinelRepairInProgress{false};

        // v1.22.77: Sentinel repair helper — zeroes first 0x500 bytes and
        // restores vtable, kDeleted flag, and stub function pointer.
        // Called from VEH redirect paths to prevent corrupted sentinel data
        // from cascading into subsequent engine reads. VEH-safe (no heap allocs).
        inline void RepairSentinel()
        {
            if (!g_zeroPage) return;
            // Best-effort guard: skip if another thread is already repairing
            if (g_sentinelRepairInProgress.exchange(true, std::memory_order_acquire))
                return;
            auto* page = reinterpret_cast<std::uint8_t*>(g_zeroPage);
            memset(page, 0, 0x500);
            if (g_stubVtable) {
                auto vtAddr = reinterpret_cast<DWORD64>(g_stubVtable);
                memcpy(page + 0x00, &vtAddr, 8);
            }
            // v1.22.91: If BST sentinel is known, write it at +0x10 instead of
            // formFlags=0x20. This breaks the VEH chain-follow infinite loop:
            // VEH redirects null → zeroPage, find loop reads [zeroPage+0x10],
            // gets sentinel → cmp matches → loop exits cleanly.
            auto* sentinel = g_bstSentinel.load(std::memory_order_acquire);
            if (sentinel) {
                auto sentAddr = reinterpret_cast<DWORD64>(sentinel);
                memcpy(page + 0x10, &sentAddr, 8);
                // Also at +0x28 and +0x30 for 48-byte entries
                memcpy(page + 0x28, &sentAddr, 8);
                memcpy(page + 0x30, &sentAddr, 8);
            } else {
                auto kDeleted = static_cast<std::uint32_t>(0x20);
                memcpy(page + 0x10, &kDeleted, 4);
            }
            if (g_stubFuncPage) {
                auto stubAddr = reinterpret_cast<DWORD64>(g_stubFuncPage);
                memcpy(page + 0x4B8, &stubAddr, 8);
            }
            g_sentinelRepairInProgress.store(false, std::memory_order_release);
        }

        // v1.22.91: Write BST sentinel pointer into the ZERO PAGE (g_zeroPage)
        // at chain-follow offsets (+0x10 for 24-byte entries, +0x28 for 48-byte).
        // VEH CASE 1 redirects null pointer faults to g_zeroPage. When a find
        // loop follows a stolen null chain pointer, VEH sets the register to
        // g_zeroPage. The find loop then reads [g_zeroPage + 0x10] as the
        // "next" entry pointer. If this contains sentinel → loop exits cleanly.
        // If it contains formFlags=0x20 → infinite cycle (the v1.22.90 bug).
        // Also patches g_lowNullGuard (address ~0) if available.
        inline void PatchNullPageWithSentinel()
        {
            auto* sentinel = g_bstSentinel.load(std::memory_order_acquire);
            if (!sentinel) return;

            auto sentAddr = reinterpret_cast<DWORD64>(sentinel);

            // Patch the VEH redirect target (g_zeroPage) — this is the critical one
            if (g_zeroPage) {
                auto* page = reinterpret_cast<std::uint8_t*>(g_zeroPage);
                // g_zeroPage is PAGE_READWRITE, no VirtualProtect needed
                memcpy(page + 0x10, &sentAddr, 8);
                memcpy(page + 0x28, &sentAddr, 8);
                memcpy(page + 0x30, &sentAddr, 8);

                logger::info("v1.22.91: Patched g_zeroPage (0x{:X}) with BST sentinel 0x{:X} at +0x10, +0x28, +0x30",
                    reinterpret_cast<std::uintptr_t>(g_zeroPage),
                    reinterpret_cast<std::uintptr_t>(sentinel));
            }

            // Also patch the low null guard (address ~0) if mapped
            if (g_lowNullGuard) {
                auto* page = reinterpret_cast<std::uint8_t*>(g_lowNullGuard);
                DWORD oldProt = 0;
                if (VirtualProtect(page, 0x10000, PAGE_READWRITE, &oldProt)) {
                    memcpy(page + 0x10, &sentAddr, 8);
                    memcpy(page + 0x28, &sentAddr, 8);
                    memcpy(page + 0x30, &sentAddr, 8);
                    VirtualProtect(page, 0x10000, PAGE_READONLY, &oldProt);
                    logger::info("v1.22.91: Also patched g_lowNullGuard (0x{:X}) with sentinel",
                        reinterpret_cast<std::uintptr_t>(g_lowNullGuard));
                }
            }
        }

        // v1.22.77: Cached DLL module range for fault recovery.
        inline DWORD64 g_dllBase = 0;
        inline DWORD64 g_dllEnd = 0;

        // Verify all code cave patches still have JMP opcode at their sites.
        // Wine may revert file-backed code pages; re-apply if needed.
        inline void VerifyAndRepairCavePatches()
        {
            int numCaves = g_numCavePatches.load(std::memory_order_acquire);
            for (int i = 0; i < numCaves; ++i) {
                auto& cp = g_cavePatches[i];
                if (!cp.patchSite || cp.patchSize == 0) continue;
                if (cp.patchSite[0] != 0xE9) {
                    // Patch was reverted! Re-apply.
                    logger::warn("{}: patch REVERTED at 0x{:X} (byte={:02X}), re-applying",
                        cp.name, reinterpret_cast<std::uintptr_t>(cp.patchSite), cp.patchSite[0]);
                    DWORD oldProt;
                    VirtualProtect(cp.patchSite, cp.patchSize, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = static_cast<std::intptr_t>(cp.caveStart);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(cp.patchSite + 5);
                    auto rel32 = static_cast<std::int32_t>(jmpTarget - jmpFrom);
                    cp.patchSite[0] = 0xE9;
                    std::memcpy(&cp.patchSite[1], &rel32, 4);
                    for (int b = 5; b < cp.patchSize; ++b)
                        cp.patchSite[b] = 0x90;
                    FlushInstructionCache(GetCurrentProcess(), cp.patchSite, cp.patchSize);
                    logger::info("{}: re-applied (verify={:02X})", cp.name, cp.patchSite[0]);
                }
            }
        }

        // v1.22.97: Namespace-scope RIP dedup arrays for VEH byte dumps.
        // Promoted from function-local statics to avoid thread-safety issues.
        inline DWORD64 g_zeroDumpedRips[32] = {};
        inline std::atomic<int> g_zeroDumpCount{0};
        inline DWORD64 g_catchAllDumpedRips[32] = {};
        inline std::atomic<int> g_catchAllDumpCount{0};

        // v1.22.51: Dynamic log paths — resolved once at init from SKSE log dir.
        // Fixed-size char arrays are safe to read from VEH handlers (no allocation).
        inline char g_crashLogPath[MAX_PATH] = {};

        // v1.22.97: Pre-opened file handle for VEH logging — avoids CRT
        // heap allocations (fopen_s/fprintf) inside exception handlers.
        // WriteFile + stack-local buffer is VEH-safe.
        inline HANDLE g_vehLogHandle = INVALID_HANDLE_VALUE;

        inline void VehLogInit(const char* path)
        {
            g_vehLogHandle = CreateFileA(path, FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }

        inline void VehLog(const char* fmt, ...)
        {
            if (g_vehLogHandle == INVALID_HANDLE_VALUE) return;
            char buf[512];
            va_list ap;
            va_start(ap, fmt);
            int len = vsnprintf(buf, sizeof(buf), fmt, ap);
            va_end(ap);
            if (len > 0) {
                DWORD written;
                WriteFile(g_vehLogHandle, buf, static_cast<DWORD>(len), &written, nullptr);
            }
        }

        inline void InitPaths()
        {
            auto logDir = SKSE::log::log_directory();
            if (logDir) {
                auto crashPath = *logDir / "SSEEngineFixesForWine_crash.log";
                strncpy_s(g_crashLogPath, crashPath.string().c_str(), _TRUNCATE);
            }
            logger::info("Crash log path: {}", g_crashLogPath);
            // Open persistent handle for VEH-safe logging
            VehLogInit(g_crashLogPath);
        }

        // v1.22.51: RAII guard for temporary VEH handlers.
        // Ensures RemoveVectoredExceptionHandler runs on all exit paths,
        // including exceptions thrown by the guarded code.
        struct VehGuard {
            PVOID handle;
            VehGuard(ULONG first, PVECTORED_EXCEPTION_HANDLER handler)
                : handle(AddVectoredExceptionHandler(first, handler)) {}
            ~VehGuard() {
                if (handle)
                    RemoveVectoredExceptionHandler(handle);
            }
            VehGuard(const VehGuard&) = delete;
            VehGuard& operator=(const VehGuard&) = delete;
        };

        // v1.22.97: VEH-safe memory readability check — replaces IsBadReadPtr
        // which can recurse/deadlock inside VEH handlers. VirtualQuery does
        // not install an exception handler, making it safe for VEH context.
        inline bool IsReadableMemory(const void* ptr, size_t len)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
            if (mbi.State != MEM_COMMIT) return false;
            constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE |
                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
            if (!(mbi.Protect & readable)) return false;
            // Check that the entire range fits within this region
            auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            auto queryEnd = reinterpret_cast<std::uintptr_t>(ptr) + len;
            return queryEnd <= regionEnd;
        }

        // v1.22.10: Enhanced ClearData diagnostic — dumps files list state
        // when ClearData fires, to see what the engine knew about before compile
        inline void DumpFilesListState(RE::TESDataHandler* a_self, const char* context)
        {
            if (!a_self) return;

            std::size_t fileCount = 0;
            std::size_t masterCount = 0;
            std::size_t smallFileCount = 0;
            std::size_t activeCount = 0;
            std::size_t compiledCount = 0;

            for (auto& file : a_self->files) {
                if (!file) continue;
                ++fileCount;
                if (file->recordFlags.all(RE::TESFile::RecordFlag::kMaster))
                    ++masterCount;
                if (file->recordFlags.all(RE::TESFile::RecordFlag::kSmallFile))
                    ++smallFileCount;
                if (file->recordFlags.all(RE::TESFile::RecordFlag::kActive))
                    ++activeCount;
                if (file->compileIndex != 0xFF)
                    ++compiledCount;

                // Log first 20 files and last 5
                if (fileCount <= 20) {
                    logger::info("  [{}] {} '{}' flags=0x{:08X} compIdx=0x{:02X} smallIdx=0x{:04X}",
                        context, fileCount, file->fileName,
                        file->recordFlags.underlying(),
                        file->compileIndex, file->smallFileCompileIndex);
                }
            }

            // Log last 5 if there are more than 25
            if (fileCount > 25) {
                std::size_t idx = 0;
                for (auto& file : a_self->files) {
                    if (!file) continue;
                    ++idx;
                    if (idx > fileCount - 5) {
                        logger::info("  [{}] {} '{}' flags=0x{:08X} compIdx=0x{:02X} smallIdx=0x{:04X}",
                            context, idx, file->fileName,
                            file->recordFlags.underlying(),
                            file->compileIndex, file->smallFileCompileIndex);
                    }
                }
            }

            auto& cc = a_self->compiledFileCollection;
            logger::info("  [{}] SUMMARY: {} files, {} master, {} small, {} active, {} compiled, "
                         "compiledCollection={} reg + {} ESL, loadingFiles={}",
                context, fileCount, masterCount, smallFileCount, activeCount, compiledCount,
                cc.files.size(), cc.smallFiles.size(), a_self->loadingFiles);
        }

        // ================================================================
        // v1.22.82: BSTHashMap::SetAt serialization
        //
        // Root cause of the New Game freeze: BSTHashMap::SetAt (the insert
        // function) has ZERO synchronization around chain pointer writes.
        // Under Wine's more aggressive thread scheduling, concurrent inserts
        // to the same bucket create circular linked lists, causing infinite
        // traversal loops in the find functions.
        //
        // Fix: wrap both SetAt template variants with a global spinlock.
        // The grow/rehash function (+0x198390) is called FROM SetAt, so
        // it's automatically protected while the lock is held.
        //
        // SetAt variant A: +0x1945D0 (414 bytes)
        // SetAt variant B: +0x1947C0 (template duplicate)
        // ================================================================
        // Sharded form cache — must be declared before SetAt wrappers
        // so the cache-update code can reference it.
        struct ShardedCache
        {
            mutable std::shared_mutex mutex;
            std::unordered_map<std::uint32_t, RE::TESForm*> map;
        };

        inline ShardedCache g_formCache[256];

        inline std::atomic_flag g_hashMapSetAtLock = ATOMIC_FLAG_INIT;
        inline std::atomic<std::uint64_t> g_setAtCallCount{ 0 };
        inline std::atomic<std::uint64_t> g_setAtContentionCount{ 0 };
        inline std::atomic<std::uint64_t> g_setAtCacheUpdates{ 0 };

    } // namespace detail
} // namespace Patches::FormCaching
