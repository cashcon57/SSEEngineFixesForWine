#pragma once

#include <algorithm>
#include <atomic>
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
        // Legacy sentinel (kept for FormReferenceFixupVEH compatibility)
        alignas(64) inline std::uint8_t g_sentinelForm[0x400] = {};
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
            int idx = g_numCavePatches.load(std::memory_order_relaxed);
            if (idx < kMaxCavePatches) {
                g_cavePatches[idx].caveStart = reinterpret_cast<std::uintptr_t>(caveStart);
                g_cavePatches[idx].caveEnd = reinterpret_cast<std::uintptr_t>(caveStart) + caveSize;
                g_cavePatches[idx].nullReturnAddr = nullReturnAddr;
                g_cavePatches[idx].name = name;
                g_cavePatches[idx].patchSite = patchSite;
                g_cavePatches[idx].patchSize = patchSize;
                g_numCavePatches.store(idx + 1, std::memory_order_release);
            } else {
                logger::error("RegisterCavePatch: OVERFLOW — {} not registered (idx={}, max={})",
                    name, idx, kMaxCavePatches);
            }
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

        // v1.22.85: Lazy cache invalidation for New Game.
        // Manual "New Game" from the UI does NOT call ClearData or
        // InitializeFormDataStructures, so g_formCache retains stale
        // pointers to forms that the engine may have freed/repurposed.
        // The VEH handler sets this flag on the first post-loading event
        // (same trigger as probe installation).  GetFormByNumericId checks
        // the flag and clears all 256 shards before the next lookup.
        inline std::atomic<bool> g_needsCacheClear{false};

        // v1.22.77: Sentinel repair helper — zeroes first 0x500 bytes and
        // restores vtable, kDeleted flag, and stub function pointer.
        // Called from VEH redirect paths to prevent corrupted sentinel data
        // from cascading into subsequent engine reads. VEH-safe (no heap allocs).
        inline void RepairSentinel()
        {
            if (!g_zeroPage) return;
            auto* page = reinterpret_cast<std::uint8_t*>(g_zeroPage);
            memset(page, 0, 0x500);
            if (g_stubVtable) {
                auto vtAddr = reinterpret_cast<DWORD64>(g_stubVtable);
                memcpy(page + 0x00, &vtAddr, 8);
            }
            auto kDeleted = static_cast<std::uint32_t>(0x20);
            memcpy(page + 0x10, &kDeleted, 4);
            if (g_stubFuncPage) {
                auto stubAddr = reinterpret_cast<DWORD64>(g_stubFuncPage);
                memcpy(page + 0x4B8, &stubAddr, 8);
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

        // v1.22.51: Dynamic log paths — resolved once at init from SKSE log dir.
        // Fixed-size char arrays are safe to read from VEH handlers (no allocation).
        inline char g_crashLogPath[MAX_PATH] = {};

        inline void InitPaths()
        {
            auto logDir = SKSE::log::log_directory();
            if (logDir) {
                auto crashPath = *logDir / "SSEEngineFixesForWine_crash.log";
                strncpy_s(g_crashLogPath, crashPath.string().c_str(), _TRUNCATE);
            }
            logger::info("Crash log path: {}", g_crashLogPath);
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

        // v1.22.84: Captured form map inner pointer for cache-update-on-SetAt.
        // The global form map pointer is at RVA +0x20FBB88 (from GetFormByNumericId).
        // SetAt receives this+8 (inner struct past vtable/header). Set lazily on
        // first GetFormByNumericId call that sees a non-null form map.
        inline std::atomic<void*> g_formMapInner{ nullptr };

        inline SafetyHookInline g_hk_SetAtA{};
        inline SafetyHookInline g_hk_SetAtB{};

        // SetAt signature: __fastcall(this, key_ptr, value_ptr_or_functor)
        // We use a generic 4-arg signature to forward all register args.
        // Return value is in RAX (varies by template: bool, iterator pair, etc.)
        inline std::uint64_t __fastcall BSTHashMap_SetAt_A(
            void* a_this, void* a_key, void* a_value, void* a_extra)
        {
            g_setAtCallCount.fetch_add(1, std::memory_order_relaxed);
            if (g_hashMapSetAtLock.test_and_set(std::memory_order_acquire)) {
                // Contended — spin with pause
                g_setAtContentionCount.fetch_add(1, std::memory_order_relaxed);
                do { _mm_pause(); }
                while (g_hashMapSetAtLock.test_and_set(std::memory_order_acquire));
            }
            auto result = g_hk_SetAtA.call<std::uint64_t>(a_this, a_key, a_value, a_extra);
            g_hashMapSetAtLock.clear(std::memory_order_release);

            // v1.22.84: If this SetAt targets the global form map, update our
            // sharded cache so GetFormByNumericId never falls through to the
            // native unbounded find loop for newly-inserted forms.
            void* expectedInner = g_formMapInner.load(std::memory_order_acquire);
            if (expectedInner != nullptr && a_this == expectedInner) {
                auto formId = *reinterpret_cast<std::uint32_t*>(a_key);
                auto* formPtr = *reinterpret_cast<RE::TESForm**>(a_value);
                if (formPtr != nullptr) {
                    const std::uint8_t  masterId = (formId & 0xFF000000) >> 24;
                    const std::uint32_t baseId   = (formId & 0x00FFFFFF);
                    std::unique_lock lock(g_formCache[masterId].mutex);
                    g_formCache[masterId].map.insert_or_assign(baseId, formPtr);
                    g_setAtCacheUpdates.fetch_add(1, std::memory_order_relaxed);
                }
            }

            return result;
        }

        inline std::uint64_t __fastcall BSTHashMap_SetAt_B(
            void* a_this, void* a_key, void* a_value, void* a_extra)
        {
            g_setAtCallCount.fetch_add(1, std::memory_order_relaxed);
            if (g_hashMapSetAtLock.test_and_set(std::memory_order_acquire)) {
                g_setAtContentionCount.fetch_add(1, std::memory_order_relaxed);
                do { _mm_pause(); }
                while (g_hashMapSetAtLock.test_and_set(std::memory_order_acquire));
            }
            auto result = g_hk_SetAtB.call<std::uint64_t>(a_this, a_key, a_value, a_extra);
            g_hashMapSetAtLock.clear(std::memory_order_release);

            // v1.22.84: Same form-map cache update as SetAt_A above.
            void* expectedInner = g_formMapInner.load(std::memory_order_acquire);
            if (expectedInner != nullptr && a_this == expectedInner) {
                auto formId = *reinterpret_cast<std::uint32_t*>(a_key);
                auto* formPtr = *reinterpret_cast<RE::TESForm**>(a_value);
                if (formPtr != nullptr) {
                    const std::uint8_t  masterId = (formId & 0xFF000000) >> 24;
                    const std::uint32_t baseId   = (formId & 0x00FFFFFF);
                    std::unique_lock lock(g_formCache[masterId].mutex);
                    g_formCache[masterId].map.insert_or_assign(baseId, formPtr);
                    g_setAtCacheUpdates.fetch_add(1, std::memory_order_relaxed);
                }
            }

            return result;
        }

        inline SafetyHookInline g_hk_GetFormByNumericId{};

        inline RE::TESForm* TESForm_GetFormByNumericId(RE::FormID a_formId)
        {
            // v1.22.85: Lazy cache invalidation — clear all shards once when
            // flagged by the VEH handler (first post-loading event = New Game).
            // This runs in normal code context so heap operations are safe.
            if (g_needsCacheClear.exchange(false, std::memory_order_acq_rel)) {
                for (auto& s : g_formCache) {
                    std::unique_lock lock(s.mutex);
                    s.map.clear();
                }
                // Also reset g_formMapInner so it gets recaptured fresh
                g_formMapInner.store(nullptr, std::memory_order_release);

                FILE* cf = nullptr;
                fopen_s(&cf, g_crashLogPath, "a");
                if (cf) {
                    fprintf(cf, "CACHE-CLEAR: invalidated all 256 shards (New Game detected)\n");
                    fflush(cf);
                    fclose(cf);
                }
            }

            const std::uint8_t  masterId = (a_formId & 0xFF000000) >> 24;
            const std::uint32_t baseId = (a_formId & 0x00FFFFFF);

            auto& shard = g_formCache[masterId];

            // lookup form in our cache first (shared/read lock)
            {
                std::shared_lock lock(shard.mutex);
                auto it = shard.map.find(baseId);
                if (it != shard.map.end()) {
                    return it->second;
                }
            }

            // v1.22.84: Lazily capture the form map inner pointer so SetAt
            // cache-update can identify calls targeting the global form map.
            // The global form map pointer lives at RVA +0x20FBB88.  SetAt
            // receives (formMap + 8) — the inner struct past the vtable header.
            if (g_formMapInner.load(std::memory_order_acquire) == nullptr) {
                auto fmBase = REL::Module::get().base();
                auto* formMapGlobal = reinterpret_cast<void**>(fmBase + 0x20FBB88);
                void* formMap = *formMapGlobal;
                if (formMap) {
                    void* inner = reinterpret_cast<std::uint8_t*>(formMap) + 8;
                    void* expected = nullptr;
                    if (g_formMapInner.compare_exchange_strong(expected, inner,
                            std::memory_order_release, std::memory_order_relaxed)) {
                        logger::info("v1.22.84: Captured g_formMapInner = 0x{:X} "
                            "(formMap=0x{:X})",
                            reinterpret_cast<std::uintptr_t>(inner),
                            reinterpret_cast<std::uintptr_t>(formMap));
                    }
                }
            }

            // Call the GAME's native GetFormByNumericId via trampoline.
            // This uses the game's actual BSTHashMap code with the correct struct
            // layout, bypassing CommonLibSSE-NG's broken BSTHashMap template.
            RE::TESForm* formPointer = g_hk_GetFormByNumericId.call<RE::TESForm*>(a_formId);

            if (formPointer != nullptr) {
                std::unique_lock lock(shard.mutex);
                shard.map.emplace(baseId, formPointer);
            }

            return formPointer;
        }

        // Call the game's native GetFormByNumericId directly (via trampoline).
        // Used by editor_id_cache.h for brute-force form enumeration.
        inline RE::TESForm* GameLookupFormByID(RE::FormID a_formId)
        {
            return g_hk_GetFormByNumericId.call<RE::TESForm*>(a_formId);
        }

#ifdef SKYRIM_AE
        inline SafetyHookInline g_hk_RemoveAt{};

        inline std::uint64_t FormMap_RemoveAt(RE::BSTHashMap<RE::FormID, RE::TESForm*>* a_self, RE::FormID* a_formIdPtr, void* a_prevValueFunctor)
        {
            const auto result = g_hk_RemoveAt.call<std::uint64_t>(a_self, a_formIdPtr, a_prevValueFunctor);

            const std::uint8_t  masterId = (*a_formIdPtr & 0xFF000000) >> 24;
            const std::uint32_t baseId = (*a_formIdPtr & 0x00FFFFFF);

            if (result == 1) {
                {
                    std::unique_lock lock(g_formCache[masterId].mutex);
                    g_formCache[masterId].map.erase(baseId);
                }
                TreeLodReferenceCaching::detail::RemoveCachedForm(baseId);
            }

            return result;
        }

        static inline REL::Relocation<bool(std::uintptr_t a_self, RE::FormID* a_formIdPtr, RE::TESForm** a_valueFunctor, void* a_unk)> orig_FormScatterTable_SetAt;

        // the functor stores the TESForm to set as the first field
        inline bool FormScatterTable_SetAt(std::uintptr_t a_self, RE::FormID* a_formIdPtr, RE::TESForm** a_valueFunctor, void* a_unk)
        {
            const std::uint8_t  masterId = (*a_formIdPtr & 0xFF000000) >> 24;
            const std::uint32_t baseId = (*a_formIdPtr & 0x00FFFFFF);

            RE::TESForm* formPointer = *a_valueFunctor;

            if (formPointer != nullptr) {
                std::unique_lock lock(g_formCache[masterId].mutex);
                g_formCache[masterId].map.insert_or_assign(baseId, formPointer);

                TreeLodReferenceCaching::detail::RemoveCachedForm(baseId);
            }

            return orig_FormScatterTable_SetAt(a_self, a_formIdPtr, a_valueFunctor, a_unk);
        }
#else
        inline SafetyHookInline g_hk_RemoveAt{};

        inline std::uint32_t FormMap_RemoveAt(std::uintptr_t a_self, std::uintptr_t a_arg2, std::uint32_t a_crc, RE::FormID* a_formIdPtr, void* a_prevValueFunctor)
        {
            const auto result = g_hk_RemoveAt.call<std::uint32_t>(a_self, a_arg2, a_crc, a_formIdPtr, a_prevValueFunctor);

            const std::uint8_t  masterId = (*a_formIdPtr & 0xFF000000) >> 24;
            const std::uint32_t baseId = (*a_formIdPtr & 0x00FFFFFF);

            if (result == 1) {
                {
                    std::unique_lock lock(g_formCache[masterId].mutex);
                    g_formCache[masterId].map.erase(baseId);
                }
                TreeLodReferenceCaching::detail::RemoveCachedForm(baseId);
            }

            return result;
        }

        static inline REL::Relocation<bool(std::uintptr_t a_self, std::uintptr_t a_arg2, std::uint32_t a_crc, RE::FormID* a_formIdPtr, RE::TESForm** a_valueFunctor, void* a_unk)> orig_FormScatterTable_SetAt;

        // the functor stores the TESForm to set as the first field
        inline bool FormScatterTable_SetAt(std::uintptr_t a_self, std::uintptr_t a_arg2, std::uint32_t a_crc, RE::FormID* a_formIdPtr, RE::TESForm** a_valueFunctor, void* a_unk)
        {
            const std::uint8_t  masterId = (*a_formIdPtr & 0xFF000000) >> 24;
            const std::uint32_t baseId = (*a_formIdPtr & 0x00FFFFFF);

            RE::TESForm* formPointer = *a_valueFunctor;

            if (formPointer != nullptr) {
                std::unique_lock lock(g_formCache[masterId].mutex);
                g_formCache[masterId].map.insert_or_assign(baseId, formPointer);

                TreeLodReferenceCaching::detail::RemoveCachedForm(baseId);
            }

            return orig_FormScatterTable_SetAt(a_self, a_arg2, a_crc, a_formIdPtr, a_valueFunctor, a_unk);
        }
#endif

        inline SafetyHookInline g_hk_ClearData;

        inline void ManuallyCompileFiles();  // forward decl for ClearData hook

        // the game does not lock the form table on these clears so we won't either
        // maybe fix later if it causes issues
        inline void TESDataHandler_ClearData(RE::TESDataHandler* a_self)
        {
            auto count = g_clearDataCalls.fetch_add(1, std::memory_order_relaxed) + 1;
            logger::info(">>> ClearData called (call #{}) — dumping state BEFORE clear"sv, count);

            // v1.22.10: Dump files list state before ClearData wipes it
            DumpFilesListState(a_self, "PRE-ClearData");

            for (auto& shard : g_formCache) {
                std::unique_lock lock(shard.mutex);
                shard.map.clear();
            }

            TreeLodReferenceCaching::detail::ClearCache();

            // v1.22.50: Do NOT set PAGE_READWRITE during ClearData.
            // ClearData calls virtual functions on forms (destructors, cleanup).
            // With PAGE_READWRITE, the vtable on the sentinel page gets
            // corrupted by engine writes, and virtual calls enter infinite loops.
            // The write-skip flood (~7K/s) under PAGE_READONLY is preferable
            // to a hang. NOTE: manual "New Game" from UI does NOT call
            // ClearData — only resetGame=true (auto_newgame) triggers this path.
            g_hk_ClearData.call(a_self);

            // Dump state after ClearData too
            DumpFilesListState(a_self, "POST-ClearData");

            // v1.22.42: After ClearData, recompile all plugins.
            // During New Game (resetGame=true), ClearData clears compile
            // indices. Under Wine, the engine's CompileFiles is skipped
            // (the original 600-file bug), so without recompilation the
            // engine can't load forms and hangs on the loading screen.
            // Also reset AddForm counter so ForceLoadAllForms triggers
            // again if kDataLoaded fires during the reload.
            logger::info(">>> Post-ClearData: recompiling plugins for Wine reload");
            g_addFormCalls.store(0, std::memory_order_relaxed);
            ManuallyCompileFiles();
            logger::info(">>> Post-ClearData: recompilation done");
        }

        inline SafetyHookInline g_hk_InitializeFormDataStructures;

        inline void TESForm_InitializeFormDataStructures()
        {
            auto count = g_initFormDataCalls.fetch_add(1, std::memory_order_relaxed) + 1;
            logger::info(">>> InitializeFormDataStructures called (call #{})"sv, count);

            // v1.22.10: Check TDH state at init time
            auto* tdh = RE::TESDataHandler::GetSingleton();
            if (tdh) {
                DumpFilesListState(tdh, "PRE-InitFormData");
            }

            for (auto& shard : g_formCache) {
                std::unique_lock lock(shard.mutex);
                shard.map.clear();
            }

            TreeLodReferenceCaching::detail::ClearCache();

            g_hk_InitializeFormDataStructures.call();
        }

        // ================================================================
        // v1.22.0: AddFormToDataHandler hook — counts form registrations
        // This is the CRITICAL function: if this is never called, the
        // engine never creates forms from plugin files.
        // ================================================================
        inline SafetyHookInline g_hk_AddFormToDataHandler;

        inline bool TESDataHandler_AddFormToDataHandler(RE::TESDataHandler* a_self, RE::TESForm* a_form)
        {
            if (!a_form) {
                g_addFormNullCalls.fetch_add(1, std::memory_order_relaxed);
            } else {
                auto count = g_addFormCalls.fetch_add(1, std::memory_order_relaxed);
                // Log the first 20 forms added, then every 10000th
                if (count < 20 || (count > 0 && count % 10000 == 0)) {
                    auto formId = a_form->GetFormID();
                    auto formType = static_cast<std::uint8_t>(a_form->GetFormType());
                    logger::info("  AddForm #{}: formID=0x{:08X} type=0x{:02X}",
                        count + 1, formId, formType);
                }
            }
            return g_hk_AddFormToDataHandler.call<bool>(a_self, a_form);
        }

        // ================================================================
        // v1.22.1: TESFile::OpenTES hook — counts file opens
        // If files are never opened for reading, the loading loop didn't run.
        // ================================================================
        inline SafetyHookInline g_hk_OpenTES;

        inline bool TESFile_OpenTES(RE::TESFile* a_self, std::uint32_t a_mode, bool a_lock)
        {
            auto count = g_openTESCalls.fetch_add(1, std::memory_order_relaxed);
            bool result = g_hk_OpenTES.call<bool>(a_self, a_mode, a_lock);
            if (result) {
                auto successes = g_openTESSuccesses.fetch_add(1, std::memory_order_relaxed);
                if (successes < 10 || (successes > 0 && successes % 500 == 0)) {
                    logger::info("  OpenTES #{}: '{}' mode={} lock={} -> SUCCESS",
                        count + 1, a_self->fileName, a_mode, a_lock);
                }
            } else {
                logger::warn("  OpenTES #{}: '{}' mode={} lock={} -> FAILED",
                    count + 1, a_self->fileName, a_mode, a_lock);
            }
            return result;
        }

        // ================================================================
        // v1.22.2: Hook TESForm::AddCompileIndex — counts compile index assignments
        // If this is never called, compile indices are never assigned.
        // ================================================================
        inline SafetyHookInline g_hk_AddCompileIndex;

        inline void TESForm_AddCompileIndex(RE::FormID* a_id, RE::TESFile* a_file)
        {
            auto count = g_addCompileIndexCalls.fetch_add(1, std::memory_order_relaxed);
            if (count < 10 || (count > 0 && count % 50000 == 0)) {
                logger::info("  AddCompileIndex #{}: file='{}' formID=0x{:08X}",
                    count + 1, a_file ? a_file->fileName : "null",
                    a_id ? *a_id : 0);
            }
            g_hk_AddCompileIndex.call(a_id, a_file);
        }

        // ================================================================
        // v1.22.2: Hook TESFile::SeekNextForm — counts record iteration
        // This is called repeatedly during record reading. If it's never
        // called, the game never attempts to read any records.
        // ================================================================
        inline SafetyHookInline g_hk_SeekNextForm;

        inline bool TESFile_SeekNextForm(RE::TESFile* a_self, bool a_skipIgnored)
        {
            auto count = g_seekNextFormCalls.fetch_add(1, std::memory_order_relaxed);
            if (count < 5 || (count > 0 && count % 100000 == 0)) {
                logger::info("  SeekNextForm #{}: file='{}'",
                    count + 1, a_self ? a_self->fileName : "null");
            }
            return g_hk_SeekNextForm.call<bool>(a_self, a_skipIgnored);
        }

        // ================================================================
        // v1.22.31: x86-64 instruction length decoder.
        // Used by ForceLoadVEH and FormReferenceFixupVEH to advance RIP
        // past faulting instructions.
        // Handles REX prefix, legacy prefixes, ModRM, SIB, disp8/disp32,
        // and immediate operands (imm8/imm16/imm32).
        // ================================================================
        inline std::size_t x86_instr_len(const std::uint8_t* ip)
        {
            const auto* start = ip;

            // Consume REX prefix (40–4F)
            bool hasRex = false;
            if ((*ip & 0xF0) == 0x40) { hasRex = true; ++ip; }

            // Consume legacy prefixes (operand-size, address-size, REP, LOCK, segment)
            bool has66 = false;
            while (*ip == 0x66 || *ip == 0xF0 || *ip == 0xF2 || *ip == 0xF3 ||
                   *ip == 0x2E || *ip == 0x3E || *ip == 0x26 ||
                   *ip == 0x36 || *ip == 0x64 || *ip == 0x65) {
                if (*ip == 0x66) has66 = true;
                ++ip;
            }

            const auto op = *ip++;
            bool isTwoByteOp = false;

            // Two-byte escape (0F xx); three-byte escape (0F 38 xx / 0F 3A xx)
            if (op == 0x0F) {
                isTwoByteOp = true;
                const auto op2 = *ip++;
                if (op2 == 0x38 || op2 == 0x3A) ++ip; // third opcode byte
            }

            // All crashing memory-access instructions have a ModRM byte.
            const auto modrm = *ip++;
            const auto mod   = (modrm >> 6) & 0x3;
            const auto reg   = (modrm >> 3) & 0x7;
            const auto rm    = modrm & 0x7;

            // Displacement
            if (mod != 3) {
                if (rm == 4) ++ip;            // SIB byte
                if      (mod == 0 && rm == 5) ip += 4; // RIP-relative disp32
                else if (mod == 1)            ip += 1; // disp8
                else if (mod == 2)            ip += 4; // disp32
            }

            // Immediate operands — opcodes that include an imm after ModRM+disp
            if (!isTwoByteOp) {
                // F6 /0 = TEST r/m8, imm8
                if (op == 0xF6 && reg == 0) ip += 1;
                // F7 /0 = TEST r/m16/32, imm16/32
                else if (op == 0xF7 && reg == 0) ip += (has66 ? 2 : 4);
                // 80, 82 = op r/m8, imm8
                else if (op == 0x80 || op == 0x82) ip += 1;
                // 81 = op r/m16/32, imm16/32
                else if (op == 0x81) ip += (has66 ? 2 : 4);
                // 83 = op r/m16/32, imm8
                else if (op == 0x83) ip += 1;
                // C6 /0 = MOV r/m8, imm8
                else if (op == 0xC6 && reg == 0) ip += 1;
                // C7 /0 = MOV r/m16/32, imm16/32
                else if (op == 0xC7 && reg == 0) ip += (has66 ? 2 : 4);
                // 69 = IMUL r, r/m, imm16/32
                else if (op == 0x69) ip += (has66 ? 2 : 4);
                // 6B = IMUL r, r/m, imm8
                else if (op == 0x6B) ip += 1;
            }

            return static_cast<std::size_t>(ip - start);
        }

        // ================================================================
        // v1.22.23: Vectored Exception Handler — null-dereference guard
        // for AE 13753 (ForceLoadAllForms).
        //
        // Large modlists (GTS 1720+ plugins) contain ESL-range actor forms
        // whose race/template pointer is null. AE 13753's loading loop
        // crashes at `mov ecx, [rax+0x38]` (AE 37791+0x8D, RAX=0) when
        // it hits one of these forms.
        //
        // This VEH catches the null AV, decodes the instruction length,
        // advances RIP past the failing instruction, and continues.
        // The destination register is left zero, which is the same as if
        // the load had returned 0 — null checks downstream handle it.
        // ================================================================
        inline LONG CALLBACK ForceLoadVEH(PEXCEPTION_POINTERS pep)
        {
            // Only active during ForceLoadAllForms
            if (!g_forceLoadInProgress.load(std::memory_order_acquire))
                return EXCEPTION_CONTINUE_SEARCH;

            // Only handle access violations
            if (pep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
                return EXCEPTION_CONTINUE_SEARCH;

            // Only handle null dereferences (target address < 0x10000)
            // to avoid masking legitimate faults in unrelated code
            ULONG_PTR targetAddr = 0;
            if (pep->ExceptionRecord->NumberParameters >= 2)
                targetAddr = pep->ExceptionRecord->ExceptionInformation[1];
            if (targetAddr >= 0x10000)
                return EXCEPTION_CONTINUE_SEARCH;

            // Advance RIP past the failing instruction
            const auto* rip = reinterpret_cast<const std::uint8_t*>(pep->ContextRecord->Rip);
            pep->ContextRecord->Rip += x86_instr_len(rip);

            g_vehSkipCount.fetch_add(1, std::memory_order_relaxed);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // ================================================================
        // v1.22.27: Form-Reference Fixup VEH
        //
        // After ForceLoadAllForms, some forms still contain raw form IDs
        // (32-bit values < 0x10000000) stored in 64-bit pointer fields.
        // These were set during TESForm::Load() when the referenced form
        // hadn't been loaded yet. On Windows, a post-load resolution
        // pass fixes these, but our ForceLoadAllForms path skips it.
        //
        // This VEH catches access violations where the target address
        // looks like a form ID (0x100 <= addr < 0x10000000), finds which
        // register holds that value, resolves it via our sharded cache,
        // and patches the register to the resolved pointer.
        //
        // Crash pattern: `test dword ptr [rax+0x28], 0x3FF`
        //   RAX = 0x5000823 → form ID 0x05000823
        //   target = RAX + 0x28 = 0x500084B
        //
        // This VEH stays installed permanently after ForceLoadAllForms.
        // ================================================================
        inline LONG CALLBACK FormReferenceFixupVEH(PEXCEPTION_POINTERS pep)
        {
            if (!g_formFixupActive.load(std::memory_order_acquire))
                return EXCEPTION_CONTINUE_SEARCH;

            if (pep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
                return EXCEPTION_CONTINUE_SEARCH;

            // v1.22.29: Only handle faults within SkyrimSE.exe's code —
            // do not interfere with crash logger, Wine internals, or other DLLs.
            {
                static auto imgBase = REL::Module::get().base();
                static auto imgEnd = [&]() {
                    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imgBase);
                    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(imgBase + dos->e_lfanew);
                    return imgBase + nt->OptionalHeader.SizeOfImage;
                }();
                auto rip = pep->ContextRecord->Rip;
                if (rip < imgBase || rip >= imgEnd)
                    return EXCEPTION_CONTINUE_SEARCH;
            }

            // Get the faulting address
            ULONG_PTR targetAddr = 0;
            if (pep->ExceptionRecord->NumberParameters >= 2)
                targetAddr = pep->ExceptionRecord->ExceptionInformation[1];

            // v1.22.36: Handle null-pointer form dereferences.
            // After kDeleted flagging + code cave, the engine still hits
            // null form pointers (RAX=0) in the AI evaluation chain.
            // Pattern: `cmp byte ptr [rax+0x1A], 0x2B` where rax=0.
            // The target address is < 0x100 (null page + small offset).
            //
            // Instead of patching each crash site, skip the faulting
            // instruction. The destination register stays 0 / unchanged,
            // and downstream null checks handle the rest.
            if (targetAddr < 0x100) {
                static std::atomic<int> s_nullSkipCount{ 0 };
                auto count = s_nullSkipCount.fetch_add(1, std::memory_order_relaxed);

                const auto* rip = reinterpret_cast<const std::uint8_t*>(pep->ContextRecord->Rip);
                auto instrLen = x86_instr_len(rip);
                pep->ContextRecord->Rip += instrLen;

                if (count < 50) {
                    logger::warn("FormFixupVEH: null-ptr skip #{}: RIP=0x{:X} (+{}B) target=0x{:X} RAX=0x{:X}",
                        count + 1, pep->ContextRecord->Rip - instrLen, instrLen,
                        targetAddr, pep->ContextRecord->Rax);
                } else if (count == 50) {
                    logger::warn("FormFixupVEH: suppressing null-ptr skip messages (50 logged)");
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // Form IDs are 32-bit values: master index (0x00-0xFE) in top byte.
            // Valid range: [0x100, 0xFF000000). Under Wine/64-bit, heap addresses
            // are >> 0x100000000 (e.g., 0x248...), so the 32-bit range is safe.
            // The faulting address may be offset from the base register
            // (e.g., [rax+0x28]), so we also check all registers below.
            // Reject real pointers (>= 0xFF000000).
            if (targetAddr >= 0xFF000000)
                return EXCEPTION_CONTINUE_SEARCH;

            // Scan all general-purpose registers for a value that looks like a form ID.
            // The register holding the form ID will be the base of the memory access.
            // x86-64 register order in CONTEXT: Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8-R15
            DWORD64* regs[] = {
                &pep->ContextRecord->Rax,
                &pep->ContextRecord->Rcx,
                &pep->ContextRecord->Rdx,
                &pep->ContextRecord->Rbx,
                // Skip Rsp — never holds a form ID
                &pep->ContextRecord->Rbp,
                &pep->ContextRecord->Rsi,
                &pep->ContextRecord->Rdi,
                &pep->ContextRecord->R8,
                &pep->ContextRecord->R9,
                &pep->ContextRecord->R10,
                &pep->ContextRecord->R11,
                &pep->ContextRecord->R12,
                &pep->ContextRecord->R13,
                &pep->ContextRecord->R14,
                &pep->ContextRecord->R15,
            };
            const char* regNames[] = {
                "RAX", "RCX", "RDX", "RBX", "RBP", "RSI", "RDI",
                "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
            };

            for (int i = 0; i < 15; ++i) {
                DWORD64 val = *regs[i];

                // Must look like a form ID: [0x100, 0xFF000000)
                if (val < 0x100 || val >= 0xFF000000)
                    continue;

                // The target address should be at or near this register value
                // (within a reasonable struct offset, say +/- 0x1000)
                auto diff = static_cast<std::int64_t>(targetAddr) - static_cast<std::int64_t>(val);
                if (diff < -0x1000 || diff > 0x1000)
                    continue;

                // This register holds what looks like a form ID. Resolve it.
                auto formId = static_cast<RE::FormID>(val);
                auto* resolved = TESForm_GetFormByNumericId(formId);

                if (resolved) {
                    auto count = g_formFixupCount.fetch_add(1, std::memory_order_relaxed);
                    if (count < 20) {
                        logger::warn("FormFixupVEH: {}=0x{:X} (formID 0x{:08X}) → 0x{:X} at RIP=0x{:X}",
                            regNames[i], val, formId,
                            reinterpret_cast<std::uintptr_t>(resolved),
                            pep->ContextRecord->Rip);
                    } else if (count == 20) {
                        logger::warn("FormFixupVEH: suppressing further log messages (20 logged, more occurring)");
                    }
                    *regs[i] = reinterpret_cast<DWORD64>(resolved);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }

                // v1.22.36: Form not in cache — let the crash happen cleanly.
                // Previous approaches (sentinel, instruction skip, zero+ZF)
                // all caused cascading failures downstream. A clean crash
                // at the original site gives better diagnostic information.
                auto sentCount = g_sentinelUseCount.fetch_add(1, std::memory_order_relaxed);
                if (sentCount < 50) {
                    logger::warn("FormFixupVEH: {}=0x{:X} (formID 0x{:08X}) NOT FOUND at RIP=0x{:X} — passing through",
                        regNames[i], val, formId, pep->ContextRecord->Rip);
                } else if (sentCount == 50) {
                    logger::warn("FormFixupVEH: suppressing pass-through log messages (50 logged)");
                }
                return EXCEPTION_CONTINUE_SEARCH;
            }

            // No register held a form ID — this is a different kind of crash
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // ================================================================
        // v1.22.36: First-chance crash logger VEH.
        //
        // Catches access violations and writes diagnostic info
        // (crash address, exception code, registers, module offset)
        // directly to a file. Uses a known game directory path.
        //
        // Only active after g_crashLoggerActive is set (post form loading).
        // Logs once per crash site, then returns EXCEPTION_CONTINUE_SEARCH.
        // ================================================================
        inline std::atomic<bool> g_crashLoggerActive{ false };
        inline std::atomic<int> g_crashLogCount{ 0 };
        inline std::atomic<int> g_nullSkipCount{ 0 };
        inline std::atomic<int> g_formIdSkipCount{ 0 };

        // Classify an x86 instruction at `ip` for VEH skip handling.
        // Returns: 'C' = CMP/TEST, 'F' = CALL indirect, 'O' = other
        inline char classifyInstruction(const std::uint8_t* ip)
        {
            // Skip prefixes (REX 40-4F, LOCK F0, 66, F2, F3, segment)
            while ((*ip >= 0x40 && *ip <= 0x4F) || *ip == 0xF0 ||
                   *ip == 0x66 || *ip == 0xF2 || *ip == 0xF3 ||
                   *ip == 0x2E || *ip == 0x3E || *ip == 0x26 ||
                   *ip == 0x36 || *ip == 0x64 || *ip == 0x65) {
                ++ip;
            }

            auto op = *ip;

            // CMP/TEST group: 38-3D, 80-83 /7(CMP), F6-F7 /0(TEST), 84-85
            if (op >= 0x38 && op <= 0x3D) return 'C';
            if (op == 0x84 || op == 0x85) return 'C';
            if (op == 0x80 || op == 0x82 || op == 0x83) {
                auto reg = (ip[1] >> 3) & 0x7;
                if (reg == 7) return 'C'; // /7 = CMP
            }
            if (op == 0x81) {
                auto reg = (ip[1] >> 3) & 0x7;
                if (reg == 7) return 'C'; // /7 = CMP
            }
            if (op == 0xF6 || op == 0xF7) {
                auto reg = (ip[1] >> 3) & 0x7;
                if (reg == 0) return 'C'; // /0 = TEST
            }
            // Two-byte: 0F BA /4(BT), 0F A3(BT), etc — also comparisons
            if (op == 0x0F) {
                auto op2 = ip[1];
                if (op2 == 0xBA || op2 == 0xA3 || op2 == 0xAB ||
                    op2 == 0xB3 || op2 == 0xBB) return 'C';
            }

            // CALL indirect: FF /2
            if (op == 0xFF) {
                auto reg = (ip[1] >> 3) & 0x7;
                if (reg == 2) return 'F'; // /2 = CALL
            }

            return 'O';
        }

        inline LONG CALLBACK CrashLoggerVEH(PEXCEPTION_POINTERS pep)
        {
            if (!g_crashLoggerActive.load(std::memory_order_acquire))
                return EXCEPTION_CONTINUE_SEARCH;

            auto code = pep->ExceptionRecord->ExceptionCode;

            // v1.22.76: INT3 probe handler — catch breakpoints from freeze detection.
            if (code == EXCEPTION_BREAKPOINT) {
                auto rip = pep->ContextRecord->Rip;
                static auto sImgBase2 = REL::Module::get().base();
                for (int i = 0; i < g_numProbes; ++i) {
                    if (g_probes[i].active.load(std::memory_order_acquire) &&
                        rip == sImgBase2 + g_probes[i].offset) {
                        // Log the probe hit with full register context
                        FILE* f = nullptr;
                        fopen_s(&f, g_crashLogPath, "a");
                        if (f) {
                            fprintf(f, "PROBE-HIT: +0x%llX",
                                (unsigned long long)g_probes[i].offset);
                            fprintf(f, " RAX=0x%llX RCX=0x%llX RDX=0x%llX RDI=0x%llX RSI=0x%llX R14=0x%llX RBP=0x%llX R8=0x%llX R9=0x%llX R15=0x%llX",
                                (unsigned long long)pep->ContextRecord->Rax,
                                (unsigned long long)pep->ContextRecord->Rcx,
                                (unsigned long long)pep->ContextRecord->Rdx,
                                (unsigned long long)pep->ContextRecord->Rdi,
                                (unsigned long long)pep->ContextRecord->Rsi,
                                (unsigned long long)pep->ContextRecord->R14,
                                (unsigned long long)pep->ContextRecord->Rbp,
                                (unsigned long long)pep->ContextRecord->R8,
                                (unsigned long long)pep->ContextRecord->R9,
                                (unsigned long long)pep->ContextRecord->R15);
                            // Log return address from stack
                            auto* rspPtr = reinterpret_cast<DWORD64*>(pep->ContextRecord->Rsp);
                            if (!IsBadReadPtr(rspPtr, 64)) {
                                fprintf(f, " STK=[");
                                for (int s = 0; s < 8; ++s) {
                                    DWORD64 val = rspPtr[s];
                                    if (val >= (DWORD64)sImgBase2 &&
                                        val < (DWORD64)sImgBase2 + 0x4000000) {
                                        fprintf(f, "+0x%llX ", (unsigned long long)(val - sImgBase2));
                                    }
                                }
                                fprintf(f, "]");
                            }
                            fprintf(f, "\n");
                            fflush(f);
                            fclose(f);
                        }
                        // Restore original byte and mark as fired
                        auto* site = reinterpret_cast<std::uint8_t*>(sImgBase2 + g_probes[i].offset);
                        *site = g_probes[i].origByte;
                        FlushInstructionCache(GetCurrentProcess(), site, 1);
                        g_probes[i].active.store(false, std::memory_order_release);
                        g_probes[i].fired.store(true, std::memory_order_release);
                        // Re-execute the original instruction (RIP stays at breakpoint addr)
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
                // Not our probe — pass to other handlers
                return EXCEPTION_CONTINUE_SEARCH;
            }

            // Log non-AV exceptions (stack overflow, illegal instruction, etc.)
            if (code != EXCEPTION_ACCESS_VIOLATION) {
                static std::atomic<int> s_nonAvCount{ 0 };
                auto cnt = s_nonAvCount.fetch_add(1, std::memory_order_relaxed);
                if (cnt < 5) {
                    FILE* f = nullptr;
                    fopen_s(&f, g_crashLogPath, "a");
                    if (f) {
                        auto imgBase = REL::Module::get().base();
                        auto rip2 = pep->ContextRecord->Rip;
                        fprintf(f, "NON-AV EXCEPTION #%d: code=0x%08lX RIP=0x%llX (+0x%llX)\n",
                            cnt + 1, code, rip2, rip2 - imgBase);
                        fflush(f);
                        fclose(f);
                    }
                }
                return EXCEPTION_CONTINUE_SEARCH;
            }

            ULONG_PTR targetAddr = 0;
            ULONG_PTR accessType = 0; // 0=read, 1=write, 8=DEP
            if (pep->ExceptionRecord->NumberParameters >= 2) {
                accessType = pep->ExceptionRecord->ExceptionInformation[0];
                targetAddr = pep->ExceptionRecord->ExceptionInformation[1];
            }

            // v1.22.79: Install INT3 probes on the first post-loading AV.
            // kNewGame fires AFTER the engine freeze, so we can't wait for it.
            // Instead, install probes at the very first access violation that
            // happens in the game image after the counters were all zero (i.e.,
            // after the main menu was stable). This catches the freeze on its
            // first iteration through any probed loop.
            if (!g_probesInstalled.load(std::memory_order_acquire) &&
                g_formFixupActive.load(std::memory_order_acquire)) {
                auto imgBase2 = REL::Module::get().base();
                int installed = 0;
                for (int i = 0; i < g_numProbes; ++i) {
                    auto* site = reinterpret_cast<std::uint8_t*>(imgBase2 + g_probes[i].offset);
                    DWORD oldProt;
                    if (VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
                        g_probes[i].origByte = *site;
                        *site = 0xCC;
                        FlushInstructionCache(GetCurrentProcess(), site, 1);
                        g_probes[i].active.store(true, std::memory_order_release);
                        installed++;
                    }
                }
                g_probesInstalled.store(true, std::memory_order_release);

                // v1.22.85: Signal cache invalidation — the next
                // GetFormByNumericId call will clear all 256 shards before
                // performing the lookup.  Safe: just an atomic store.
                g_needsCacheClear.store(true, std::memory_order_release);

                FILE* pf = nullptr;
                fopen_s(&pf, g_crashLogPath, "a");
                if (pf) {
                    fprintf(pf, "PROBES: installed %d/%d INT3 probes on first VEH event\n"
                                "CACHE-CLEAR: flagged for lazy invalidation\n",
                        installed, g_numProbes);
                    fflush(pf);
                    fclose(pf);
                }
            }

            // ─────────────────────────────────────────────────────────────
            // CASE W: Write to zero page — skip the instruction.
            // v1.22.43: Sentinel page is PAGE_READONLY to prevent vtable
            // corruption. All write attempts are caught here and silently
            // skipped, preserving the sentinel's vtable/flags/stubs.
            // ─────────────────────────────────────────────────────────────
            if (accessType == 1 && g_zeroPage &&
                targetAddr >= g_zeroPageBase &&
                targetAddr < g_zeroPageEnd) {
                auto rip2 = pep->ContextRecord->Rip;
                const auto* ripBytes2 = reinterpret_cast<const std::uint8_t*>(rip2);
                if (!IsBadReadPtr(ripBytes2, 16)) {
                    auto instrLen = x86_instr_len(ripBytes2);
                    pep->ContextRecord->Rip += instrLen;

                    auto count = g_zeroPageWriteSkips.fetch_add(1, std::memory_order_relaxed);
                    if (count < 200) {
                        const char* logPath = g_crashLogPath;
                        FILE* f2 = nullptr;
                        fopen_s(&f2, logPath, "a");
                        if (f2) {
                            fprintf(f2, "ZERO-WRITE-SKIP #%d: RIP=+0x%llX target=0x%llX +%dB\n",
                                count + 1, rip2 - REL::Module::get().base(),
                                (unsigned long long)targetAddr, instrLen);
                            fflush(f2);
                            fclose(f2);
                        }
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            // Compute SkyrimSE.exe image bounds (cached)
            static auto sImgBase = REL::Module::get().base();
            static auto sImgEnd = [&]() {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(sImgBase);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(sImgBase + dos->e_lfanew);
                return sImgBase + nt->OptionalHeader.SizeOfImage;
            }();
            auto rip = pep->ContextRecord->Rip;

            // ─────────────────────────────────────────────────────────────
            // CASE 0: Null code execution (RIP < 0x10000)
            // Happens when CALL jumped to a null function pointer (e.g.,
            // vtable read from zero page returned 0). The CALL already
            // pushed the return address onto the stack.
            // Pop the return address, set RAX=0 (null return), continue.
            // ─────────────────────────────────────────────────────────────
            if (rip < 0x10000) {
                auto* rspPtr = reinterpret_cast<DWORD64*>(pep->ContextRecord->Rsp);
                if (!IsBadReadPtr(rspPtr, 8)) {
                    pep->ContextRecord->Rip = *rspPtr;
                    pep->ContextRecord->Rsp += 8;
                    pep->ContextRecord->Rax = 0;

                    auto count = g_nullSkipCount.fetch_add(1, std::memory_order_relaxed);
                    if (count < 50) {
                        const char* logPath = g_crashLogPath;
                        FILE* f2 = nullptr;
                        fopen_s(&f2, logPath, "a");
                        if (f2) {
                            fprintf(f2, "NULL-CALL-RET #%d: RIP=0x%llX returning to 0x%llX\n",
                                count + 1, rip, *rspPtr);
                            fflush(f2);
                            fclose(f2);
                        }
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            // Faults outside SkyrimSE.exe image
            if (rip < sImgBase || rip >= sImgEnd) {
                // v1.22.43: If RIP is an invalid address (high kernel-space
                // or very low), the engine jumped here via a corrupted vtable
                // in the zero page. The CALL already pushed a return address.
                // Pop it and return 0, same as CASE 0 for null calls.
                bool isInvalidCodeAddr = (rip > 0x00007FFFFFFFFFFFULL) || (rip < 0x10000);
                if (isInvalidCodeAddr) {
                    auto* rspPtr = reinterpret_cast<DWORD64*>(pep->ContextRecord->Rsp);
                    if (!IsBadReadPtr(rspPtr, 8)) {
                        DWORD64 retAddr = *rspPtr;
                        pep->ContextRecord->Rip = retAddr;
                        pep->ContextRecord->Rsp += 8;
                        pep->ContextRecord->Rax = 0;

                        // v1.22.43: No vtable repair needed — page is PAGE_READONLY

                        static std::atomic<int> s_extFixCount{ 0 };
                        auto cnt2 = s_extFixCount.fetch_add(1, std::memory_order_relaxed);
                        if (cnt2 < 50) {
                            const char* logPath = g_crashLogPath;
                            FILE* f = nullptr;
                            fopen_s(&f, logPath, "a");
                            if (f) {
                                fprintf(f, "EXT-CALL-RET #%d: RIP=0x%llX → returning to 0x%llX RAX=0\n",
                                    cnt2 + 1, rip, retAddr);
                                fflush(f);
                                fclose(f);
                            }
                        }
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }

                // v1.22.54: Check if fault is inside one of our code caves.
                // This happens when garbage form pointers (e.g., 0x3C003C00,
                // 0x9E50...) pass the inline null check but aren't valid memory.
                // Redirect to the cave's null/skip path.
                {
                    int numCaves = g_numCavePatches.load(std::memory_order_acquire);
                    for (int i = 0; i < numCaves; ++i) {
                        if (rip >= g_cavePatches[i].caveStart && rip < g_cavePatches[i].caveEnd) {
                            pep->ContextRecord->Rip = g_cavePatches[i].nullReturnAddr;
                            pep->ContextRecord->Rax = 0; // safe: null paths return 0 or ignore RAX

                            // v1.22.69→v1.22.77: Repair sentinel on every cave-fault.
                            RepairSentinel();

                            auto count = g_caveFaultCount.fetch_add(1, std::memory_order_relaxed);
                            if (count < 200) {
                                const char* logPath = g_crashLogPath;
                                FILE* f = nullptr;
                                fopen_s(&f, logPath, "a");
                                if (f) {
                                    fprintf(f, "CAVE-FAULT #%llu: RIP=0x%llX (%s) target=0x%llX → redirect 0x%llX\n",
                                        (unsigned long long)(count + 1), (unsigned long long)rip,
                                        g_cavePatches[i].name,
                                        (unsigned long long)targetAddr,
                                        (unsigned long long)g_cavePatches[i].nullReturnAddr);
                                    fflush(f);
                                    fclose(f);
                                }
                            }
                            return EXCEPTION_CONTINUE_EXECUTION;
                        }
                    }
                }

                // v1.22.55: Execute-fault recovery — corrupted function pointer
                // or vtable entry jumped to non-executable memory (heap, freed
                // page, etc.). When RIP == targetAddr, the CPU faulted trying
                // to fetch instruction bytes at that address. If the stack top
                // holds a valid return address in the game image, this was a
                // CALL through a bad pointer — pop the return addr and continue.
                if (rip == targetAddr && accessType != 1) {
                    auto* rspPtr2 = reinterpret_cast<DWORD64*>(pep->ContextRecord->Rsp);
                    if (!IsBadReadPtr(rspPtr2, 8)) {
                        DWORD64 retAddr = *rspPtr2;

                        // v1.22.64: Also accept return addresses inside our code caves.
                        // PatchA's cave calls [rax+0x4B8] — if the function pointer is
                        // garbage, the CPU faults here with retAddr pointing back into
                        // the cave, not the game image. Redirect to the cave's null path.
                        bool inGameImage = (retAddr >= sImgBase && retAddr < sImgEnd);
                        bool inCave = false;
                        int caveIdx = -1;
                        if (!inGameImage) {
                            int numCaves = g_numCavePatches.load(std::memory_order_acquire);
                            for (int i = 0; i < numCaves; ++i) {
                                if (retAddr >= g_cavePatches[i].caveStart && retAddr < g_cavePatches[i].caveEnd) {
                                    inCave = true;
                                    caveIdx = i;
                                    break;
                                }
                            }
                        }

                        if (inGameImage || inCave) {
                            if (inCave) {
                                // Return to cave's null/skip path instead of resuming mid-cave
                                pep->ContextRecord->Rip = g_cavePatches[caveIdx].nullReturnAddr;
                            } else {
                                pep->ContextRecord->Rip = retAddr;
                            }
                            pep->ContextRecord->Rsp += 8;
                            pep->ContextRecord->Rax = 0;

                            auto cnt3 = static_cast<int>(g_execRecoverCount.fetch_add(1, std::memory_order_relaxed));
                            if (cnt3 < 50) {
                                const char* logPath = g_crashLogPath;
                                FILE* f = nullptr;
                                fopen_s(&f, logPath, "a");
                                if (f) {
                                    fprintf(f, "EXT-EXEC-RECOVER #%d: RIP=0x%llX (bad call target) → returning to 0x%llX (%s%s) RAX=0\n",
                                        cnt3 + 1, rip,
                                        (unsigned long long)pep->ContextRecord->Rip,
                                        inCave ? "cave:" : "+0x",
                                        inCave ? g_cavePatches[caveIdx].name : "");
                                    if (!inCave) {
                                        fprintf(f, "  (offset +0x%llX)\n", retAddr - sImgBase);
                                    }
                                    fflush(f);
                                    fclose(f);
                                }
                            }
                            return EXCEPTION_CONTINUE_EXECUTION;
                        }
                    }
                }

                // v1.22.77: Faults in our own DLL — game jumped here via
                // corrupted sentinel vtable/function pointer, or our hook
                // received garbage data. Recover by popping return address
                // (like a failed CALL) and returning 0 to caller.
                if (g_dllBase && rip >= g_dllBase && rip < g_dllEnd) {
                    auto* rspPtr = reinterpret_cast<DWORD64*>(pep->ContextRecord->Rsp);
                    if (!IsBadReadPtr(rspPtr, 8)) {
                        DWORD64 retAddr = *rspPtr;
                        pep->ContextRecord->Rip = retAddr;
                        pep->ContextRecord->Rsp += 8;
                        pep->ContextRecord->Rax = 0;
                        RepairSentinel();

                        static std::atomic<int> s_dllFaultCount{ 0 };
                        auto cnt = s_dllFaultCount.fetch_add(1, std::memory_order_relaxed);
                        if (cnt < 50) {
                            const char* logPath = g_crashLogPath;
                            FILE* f = nullptr;
                            fopen_s(&f, logPath, "a");
                            if (f) {
                                fprintf(f, "DLL-FAULT-RECOVER #%d: RIP=0x%llX target=0x%llX RAX=0x%llX → returning to 0x%llX\n",
                                    cnt + 1, rip,
                                    (unsigned long long)targetAddr,
                                    (unsigned long long)pep->ContextRecord->Rax,
                                    (unsigned long long)retAddr);
                                fflush(f);
                                fclose(f);
                            }
                        }
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }

                // Faults in other DLLs (Wine, etc.) — can't handle
                static std::atomic<int> s_extCount{ 0 };
                auto cnt = s_extCount.fetch_add(1, std::memory_order_relaxed);
                if (cnt < 10) {
                    // Identify which module contains the faulting RIP
                    char modName[MAX_PATH] = "<unknown>";
                    HMODULE hMod = nullptr;
                    if (GetModuleHandleExA(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(rip), &hMod)) {
                        GetModuleFileNameA(hMod, modName, MAX_PATH);
                    }

                    const char* logPath = g_crashLogPath;
                    FILE* f = nullptr;
                    fopen_s(&f, logPath, "a");
                    if (f) {
                        fprintf(f, "EXT-CRASH #%d: RIP=0x%llX (module: %s) target=0x%llX RAX=0x%llX\n",
                            cnt + 1, rip,
                            modName,
                            (unsigned long long)targetAddr,
                            (unsigned long long)pep->ContextRecord->Rax);
                        fflush(f);
                        fclose(f);
                    }
                }
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const auto* ripBytes = reinterpret_cast<const std::uint8_t*>(rip);

            // ─────────────────────────────────────────────────────────────
            // CASE 1: Invalid pointer dereference — zero page substitution
            //
            // Covers TWO scenarios:
            //  a) Null pointer: targetAddr < 0x10000 (register is near 0)
            //  b) Garbage pointer: targetAddr > 0x7FFFFFFFFFFF (above
            //     user-mode VA space on Windows x64, e.g. 0xFFFFFFFF00000008)
            //
            // These arise because the engine reads zero from the zero page,
            // then combines it with other registers (OR, shift, etc.)
            // producing invalid kernel-space addresses.
            //
            // Instead of skipping instructions (which corrupts control flow),
            // replace the bad register with g_zeroPage and RETRY the
            // instruction. The engine reads zeros from every field and
            // takes "nothing here" branches naturally.
            // ─────────────────────────────────────────────────────────────
            bool isNullAccess = (targetAddr < 0x10000);
            bool isHighInvalid = (targetAddr > 0x00007FFFFFFFFFFFULL);
            if ((isNullAccess || isHighInvalid) && g_zeroPage) {
                DWORD64 zp = reinterpret_cast<DWORD64>(g_zeroPage);
                DWORD64* gprs[] = {
                    &pep->ContextRecord->Rax,
                    &pep->ContextRecord->Rcx,
                    &pep->ContextRecord->Rdx,
                    &pep->ContextRecord->Rbx,
                    // Skip RSP
                    &pep->ContextRecord->Rbp,
                    &pep->ContextRecord->Rsi,
                    &pep->ContextRecord->Rdi,
                    &pep->ContextRecord->R8,
                    &pep->ContextRecord->R9,
                    &pep->ContextRecord->R10,
                    &pep->ContextRecord->R11,
                    &pep->ContextRecord->R12,
                    &pep->ContextRecord->R13,
                    &pep->ContextRecord->R14,
                    &pep->ContextRecord->R15,
                };

                bool patched = false;
                if (isNullAccess) {
                    // Null pointer: find register near zero
                    for (int i = 0; i < 15; ++i) {
                        DWORD64 val = *gprs[i];
                        if (val < 0x100 && targetAddr >= val &&
                            targetAddr - val < 0x10000) {
                            *gprs[i] = zp + val;
                            patched = true;
                            break;
                        }
                    }
                } else {
                    // High-invalid: find register with invalid high bits
                    // that when offset-adjusted matches targetAddr
                    for (int i = 0; i < 15; ++i) {
                        DWORD64 val = *gprs[i];
                        if (val <= 0x00007FFFFFFFFFFFULL)
                            continue; // Valid user-mode address, skip
                        auto diff = static_cast<std::int64_t>(targetAddr) -
                                    static_cast<std::int64_t>(val);
                        if (diff >= -0x10000 && diff <= 0x10000) {
                            *gprs[i] = zp; // Replace with zero page base
                            patched = true;
                            break;
                        }
                    }
                }

                if (!patched) {
                    // Fallback: set RAX to zero page (most common target)
                    pep->ContextRecord->Rax = zp;
                    patched = true;
                }

                // v1.22.77: Repair sentinel on every redirect. Sentinel is
                // PAGE_READWRITE so engine freely corrupts it with ASCII data;
                // zeroing here ensures the retried instruction sees clean fields.
                RepairSentinel();

                auto count = g_zeroPageUseCount.fetch_add(1, std::memory_order_relaxed);

                if (count < 600) {
                    const char* logPath = g_crashLogPath;
                    FILE* f2 = nullptr;
                    fopen_s(&f2, logPath, "a");
                    if (f2) {
                        fprintf(f2, "ZERO-PAGE #%d: RIP=+0x%llX target=0x%llX %s → zeroPage=0x%llX\n",
                            count + 1, rip - sImgBase,
                            (unsigned long long)targetAddr,
                            isNullAccess ? "null" : "high-invalid",
                            (unsigned long long)zp);
                        // Byte dump: first time we see each RIP, dump 32 bytes
                        static DWORD64 s_dumpedRips[32] = {};
                        static int s_dumpCount = 0;
                        bool alreadyDumped = false;
                        for (int d = 0; d < s_dumpCount; ++d)
                            if (s_dumpedRips[d] == rip) { alreadyDumped = true; break; }
                        if (!alreadyDumped && s_dumpCount < 32) {
                            s_dumpedRips[s_dumpCount++] = rip;
                            fprintf(f2, "  BYTES[+0x%llX]:", rip - sImgBase);
                            if (!IsBadReadPtr(ripBytes, 32))
                                for (int b = 0; b < 32; ++b) fprintf(f2, " %02X", ripBytes[b]);
                            fprintf(f2, "\n");
                        }

                        // v1.22.85: Enhanced diagnostics for high-invalid
                        // targets — dump full register state + memory around
                        // corrupted pointer to identify corruption source.
                        if (isHighInvalid && count < 50) {
                            fprintf(f2, "  REGS: RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n",
                                (unsigned long long)pep->ContextRecord->Rax,
                                (unsigned long long)pep->ContextRecord->Rbx,
                                (unsigned long long)pep->ContextRecord->Rcx,
                                (unsigned long long)pep->ContextRecord->Rdx);
                            fprintf(f2, "        RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\n",
                                (unsigned long long)pep->ContextRecord->Rsi,
                                (unsigned long long)pep->ContextRecord->Rdi,
                                (unsigned long long)pep->ContextRecord->Rbp,
                                (unsigned long long)pep->ContextRecord->Rsp);
                            fprintf(f2, "        R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX\n",
                                (unsigned long long)pep->ContextRecord->R8,
                                (unsigned long long)pep->ContextRecord->R9,
                                (unsigned long long)pep->ContextRecord->R10,
                                (unsigned long long)pep->ContextRecord->R11);
                            fprintf(f2, "        R12=%016llX R13=%016llX R14=%016llX R15=%016llX\n",
                                (unsigned long long)pep->ContextRecord->R12,
                                (unsigned long long)pep->ContextRecord->R13,
                                (unsigned long long)pep->ContextRecord->R14,
                                (unsigned long long)pep->ContextRecord->R15);

                            // Dump stack (return addresses)
                            auto* stk = reinterpret_cast<DWORD64*>(pep->ContextRecord->Rsp);
                            if (!IsBadReadPtr(stk, 64)) {
                                fprintf(f2, "  STACK:");
                                for (int s = 0; s < 8; ++s)
                                    fprintf(f2, " %016llX", (unsigned long long)stk[s]);
                                fprintf(f2, "\n");
                            }

                            // For each register that looks like a valid
                            // pointer, dump 64 bytes at that address so we
                            // can see the data structure it points to.
                            const char* regNames[] = {
                                "RAX","RBX","RCX","RDX","RSI","RDI",
                                "R8","R9","R10","R11","R12","R13","R14","R15"
                            };
                            DWORD64 regVals[] = {
                                pep->ContextRecord->Rax, pep->ContextRecord->Rbx,
                                pep->ContextRecord->Rcx, pep->ContextRecord->Rdx,
                                pep->ContextRecord->Rsi, pep->ContextRecord->Rdi,
                                pep->ContextRecord->R8,  pep->ContextRecord->R9,
                                pep->ContextRecord->R10, pep->ContextRecord->R11,
                                pep->ContextRecord->R12, pep->ContextRecord->R13,
                                pep->ContextRecord->R14, pep->ContextRecord->R15
                            };
                            for (int r = 0; r < 14; ++r) {
                                DWORD64 rv = regVals[r];
                                // Valid user-mode heap pointer?
                                if (rv > 0x10000 && rv <= 0x00007FFFFFFFFFFFULL) {
                                    auto* mem = reinterpret_cast<std::uint8_t*>(rv);
                                    if (!IsBadReadPtr(mem, 64)) {
                                        fprintf(f2, "  MEM[%s=%016llX]:", regNames[r],
                                            (unsigned long long)rv);
                                        for (int b = 0; b < 64; ++b) {
                                            if (b % 16 == 0 && b > 0)
                                                fprintf(f2, "\n                              ");
                                            fprintf(f2, " %02X", mem[b]);
                                        }
                                        // Also print as ASCII for string detection
                                        fprintf(f2, "\n  ASCII: \"");
                                        for (int b = 0; b < 64; ++b) {
                                            char c = (char)mem[b];
                                            fprintf(f2, "%c", (c >= 32 && c < 127) ? c : '.');
                                        }
                                        fprintf(f2, "\"\n");
                                    }
                                }
                            }
                        }

                        fflush(f2);
                        fclose(f2);
                    }
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // ─────────────────────────────────────────────────────────────
            // CASE 2: Form-ID-as-pointer dereference (targetAddr < 0xFF000000)
            // Wine leaves raw form IDs (32-bit values) in 64-bit pointer fields.
            // Scan registers for a value that looks like a form ID and is close
            // to the target address. Try to resolve via GetFormByNumericId.
            // If resolved: patch register, retry instruction.
            // If NOT resolved: skip instruction (form doesn't exist).
            // ─────────────────────────────────────────────────────────────
            if (targetAddr < 0xFF000000ULL) {
                DWORD64* regs[] = {
                    &pep->ContextRecord->Rax,
                    &pep->ContextRecord->Rcx,
                    &pep->ContextRecord->Rdx,
                    &pep->ContextRecord->Rbx,
                    &pep->ContextRecord->Rbp,
                    &pep->ContextRecord->Rsi,
                    &pep->ContextRecord->Rdi,
                    &pep->ContextRecord->R8,
                    &pep->ContextRecord->R9,
                    &pep->ContextRecord->R10,
                    &pep->ContextRecord->R11,
                    &pep->ContextRecord->R12,
                    &pep->ContextRecord->R13,
                    &pep->ContextRecord->R14,
                    &pep->ContextRecord->R15,
                };

                for (int i = 0; i < 15; ++i) {
                    DWORD64 val = *regs[i];
                    if (val < 0x100 || val >= 0xFF000000ULL)
                        continue;
                    // Skip values that point into our zero page — not form IDs!
                    if (g_zeroPage && val >= g_zeroPageBase && val < g_zeroPageEnd)
                        continue;
                    auto diff = static_cast<std::int64_t>(targetAddr) - static_cast<std::int64_t>(val);
                    if (diff < -0x1000 || diff > 0x1000)
                        continue;

                    // Found a register that looks like a form ID
                    auto formId = static_cast<RE::FormID>(val);
                    auto* resolved = TESForm_GetFormByNumericId(formId);

                    if (resolved) {
                        // Resolved! Replace register with real pointer, retry instruction
                        *regs[i] = reinterpret_cast<DWORD64>(resolved);
                        auto count = g_formIdSkipCount.fetch_add(1, std::memory_order_relaxed);
                        if (count < 20) {
                            const char* logPath = g_crashLogPath;
                            FILE* f2 = nullptr;
                            fopen_s(&f2, logPath, "a");
                            if (f2) {
                                fprintf(f2, "FORM-RESOLVE #%d: RIP=+0x%llX formID=0x%08X → 0x%llX\n",
                                    count + 1, rip - sImgBase, formId,
                                    reinterpret_cast<unsigned long long>(resolved));
                                fflush(f2);
                                fclose(f2);
                            }
                        }
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }

                    // Form not found in hash table — replace register with
                    // zero page pointer so the engine reads zeros from all
                    // fields and takes natural "nothing" branches.
                    // This avoids instruction skipping which corrupts control flow.
                    if (g_zeroPage) {
                        DWORD64 zp = reinterpret_cast<DWORD64>(g_zeroPage);
                        auto offset = static_cast<std::int64_t>(targetAddr) - static_cast<std::int64_t>(val);
                        *regs[i] = zp; // Point to zero page

                        // v1.22.77: Repair sentinel before retry
                        RepairSentinel();

                        auto count = g_formIdSkipCount.fetch_add(1, std::memory_order_relaxed);
                        if (count < 50) {
                            const char* logPath = g_crashLogPath;
                            FILE* f2 = nullptr;
                            fopen_s(&f2, logPath, "a");
                            if (f2) {
                                fprintf(f2, "FORM-ZEROPAGE #%d: RIP=+0x%llX formID=0x%08X NOT FOUND → zeroPage=0x%llX\n",
                                    count + 1, rip - sImgBase, formId, (unsigned long long)zp);
                                fflush(f2);
                                fclose(f2);
                            }
                        }
                        return EXCEPTION_CONTINUE_EXECUTION; // Retry with zero page
                    }

                    // No zero page available — fall through to crash log
                    break;
                }
            }

            // ─────────────────────────────────────────────────────────────
            // CASE 3: Catch-all — redirect register to zero page + RETRY.
            // v1.22.44: Instead of skipping the instruction (which breaks
            // control flow and causes freezes), find the register that
            // holds the unmapped address, redirect it to the zero page,
            // and retry. The instruction reads zeros from the sentinel
            // and continues naturally.
            // ─────────────────────────────────────────────────────────────
            if (g_zeroPage) {
                DWORD64 zp = reinterpret_cast<DWORD64>(g_zeroPage);
                DWORD64* gprs[] = {
                    &pep->ContextRecord->Rax,
                    &pep->ContextRecord->Rcx,
                    &pep->ContextRecord->Rdx,
                    &pep->ContextRecord->Rbx,
                    &pep->ContextRecord->Rbp,
                    &pep->ContextRecord->Rsi,
                    &pep->ContextRecord->Rdi,
                    &pep->ContextRecord->R8,
                    &pep->ContextRecord->R9,
                    &pep->ContextRecord->R10,
                    &pep->ContextRecord->R11,
                    &pep->ContextRecord->R12,
                    &pep->ContextRecord->R13,
                    &pep->ContextRecord->R14,
                    &pep->ContextRecord->R15,
                };

                // v1.22.47: Two-phase redirect strategy.
                // Phase 1: Find register within 1MB of target (single-register addressing).
                // Phase 2: Find largest pointer-like register (multi-register addressing
                //          like [r14 + rax*8] where base ptr is far from target).
                // Never fall back to skip+zero — always redirect + retry.
                bool patched = false;

                // Phase 1: Register close to target (handles [reg+offset] patterns)
                for (int i = 0; i < 15; ++i) {
                    DWORD64 val = *gprs[i];
                    if (val == 0 || val == zp) continue;
                    auto diff = static_cast<std::int64_t>(targetAddr) -
                                static_cast<std::int64_t>(val);
                    if (diff >= 0 && diff < 0x100000) { // Within 1MB
                        *gprs[i] = zp;
                        patched = true;
                        break;
                    }
                }

                // Phase 2: Redirect largest pointer-like register
                // (handles [base + index*scale] where base is unmapped)
                if (!patched) {
                    int bestIdx = -1;
                    DWORD64 bestVal = 0;
                    for (int i = 0; i < 15; ++i) {
                        DWORD64 val = *gprs[i];
                        // Must look like a pointer: > 64KB, not zero page,
                        // not in SkyrimSE.exe image, and <= target address
                        if (val > 0x10000 && val != zp &&
                            val <= targetAddr && val > bestVal) {
                            bestVal = val;
                            bestIdx = i;
                        }
                    }
                    if (bestIdx >= 0) {
                        *gprs[bestIdx] = zp;
                        patched = true;
                    }
                }

                // v1.22.77: Repair sentinel before retry
                RepairSentinel();

                auto count = g_catchAllCount.fetch_add(1, std::memory_order_relaxed);
                if (count < 200) {
                    const char* logPath = g_crashLogPath;
                    FILE* f2 = nullptr;
                    fopen_s(&f2, logPath, "a");
                    if (f2) {
                        fprintf(f2, "CATCH-ALL #%d: RIP=+0x%llX target=0x%llX patched=%s RAX=0x%llX R14=0x%llX\n",
                            count + 1, rip - sImgBase,
                            (unsigned long long)targetAddr,
                            patched ? "redirect" : "FAILED",
                            (unsigned long long)pep->ContextRecord->Rax,
                            (unsigned long long)pep->ContextRecord->R14);
                        // Byte dump: first time per unique RIP
                        static DWORD64 s_dumpedRips[32] = {};
                        static int s_dumpCount = 0;
                        bool alreadyDumped = false;
                        for (int d = 0; d < s_dumpCount; ++d)
                            if (s_dumpedRips[d] == rip) { alreadyDumped = true; break; }
                        if (!alreadyDumped && s_dumpCount < 32) {
                            s_dumpedRips[s_dumpCount++] = rip;
                            fprintf(f2, "  BYTES[+0x%llX]:", rip - sImgBase);
                            if (!IsBadReadPtr(ripBytes, 32))
                                for (int b = 0; b < 32; ++b) fprintf(f2, " %02X", ripBytes[b]);
                            fprintf(f2, "\n");
                        }

                        // v1.22.85: Full register + memory dump for first 20 catch-alls
                        if (count < 20) {
                            fprintf(f2, "  REGS: RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n",
                                (unsigned long long)pep->ContextRecord->Rax,
                                (unsigned long long)pep->ContextRecord->Rbx,
                                (unsigned long long)pep->ContextRecord->Rcx,
                                (unsigned long long)pep->ContextRecord->Rdx);
                            fprintf(f2, "        RSI=%016llX RDI=%016llX R8 =%016llX R14=%016llX\n",
                                (unsigned long long)pep->ContextRecord->Rsi,
                                (unsigned long long)pep->ContextRecord->Rdi,
                                (unsigned long long)pep->ContextRecord->R8,
                                (unsigned long long)pep->ContextRecord->R14);
                            // Dump memory at RSI (common source struct) if valid
                            auto rsiVal = pep->ContextRecord->Rsi;
                            if (rsiVal > 0x10000 && rsiVal <= 0x00007FFFFFFFFFFFULL) {
                                auto* mem = reinterpret_cast<std::uint8_t*>(rsiVal);
                                if (!IsBadReadPtr(mem, 128)) {
                                    fprintf(f2, "  MEM[RSI=%016llX]:", (unsigned long long)rsiVal);
                                    for (int b = 0; b < 128; ++b) {
                                        if (b % 16 == 0 && b > 0)
                                            fprintf(f2, "\n                              ");
                                        fprintf(f2, " %02X", mem[b]);
                                    }
                                    fprintf(f2, "\n  ASCII: \"");
                                    for (int b = 0; b < 128; ++b) {
                                        char c = (char)mem[b];
                                        fprintf(f2, "%c", (c >= 32 && c < 127) ? c : '.');
                                    }
                                    fprintf(f2, "\"\n");
                                }
                            }
                        }

                        fflush(f2);
                        fclose(f2);
                    }
                }
                // Safety valve: after 10000 catch-all redirects, stop handling
                if (count < 10000)
                    return EXCEPTION_CONTINUE_EXECUTION;
            }

            // Only log first 20 non-handled crashes
            if (g_crashLogCount.fetch_add(1, std::memory_order_relaxed) >= 20)
                return EXCEPTION_CONTINUE_SEARCH;

            // Write crash details to SKSE log directory
            {
                const char* logPath = g_crashLogPath;
                FILE* f = nullptr;
                fopen_s(&f, logPath, "a");
                if (f) {
                    // rip, targetAddr, sImgBase already computed above
                    auto offset = rip - sImgBase;
                    bool inModule = true; // We already verified RIP is in SkyrimSE.exe

                    fprintf(f, "\n=== SSEEngineFixesForWine CRASH LOG (v1.22.64) ===\n");
                    fprintf(f, "Exception: 0x%08lX at RIP=0x%llX (SkyrimSE.exe+0x%llX)\n",
                        code, rip, offset);
                    fprintf(f, "Target address: 0x%llX\n", (unsigned long long)targetAddr);
                    fprintf(f, "Registers:\n");
                    fprintf(f, "  RAX=0x%016llX  RBX=0x%016llX  RCX=0x%016llX  RDX=0x%016llX\n",
                        pep->ContextRecord->Rax, pep->ContextRecord->Rbx,
                        pep->ContextRecord->Rcx, pep->ContextRecord->Rdx);
                    fprintf(f, "  RSI=0x%016llX  RDI=0x%016llX  RBP=0x%016llX  RSP=0x%016llX\n",
                        pep->ContextRecord->Rsi, pep->ContextRecord->Rdi,
                        pep->ContextRecord->Rbp, pep->ContextRecord->Rsp);
                    fprintf(f, "  R8 =0x%016llX  R9 =0x%016llX  R10=0x%016llX  R11=0x%016llX\n",
                        pep->ContextRecord->R8, pep->ContextRecord->R9,
                        pep->ContextRecord->R10, pep->ContextRecord->R11);
                    fprintf(f, "  R12=0x%016llX  R13=0x%016llX  R14=0x%016llX  R15=0x%016llX\n",
                        pep->ContextRecord->R12, pep->ContextRecord->R13,
                        pep->ContextRecord->R14, pep->ContextRecord->R15);

                    // Dump bytes at RIP for disassembly
                    fprintf(f, "Bytes at RIP:");
                    if (!IsBadReadPtr(ripBytes, 16)) {
                        for (int i = 0; i < 16; ++i)
                            fprintf(f, " %02X", ripBytes[i]);
                    } else {
                        fprintf(f, " <unreadable>");
                    }
                    fprintf(f, "\n");

                    // Stack dump (16 entries)
                    fprintf(f, "Stack (RSP):");
                    auto* rspPtr = reinterpret_cast<const DWORD64*>(pep->ContextRecord->Rsp);
                    if (!IsBadReadPtr(rspPtr, 128)) {
                        fprintf(f, "\n");
                        for (int i = 0; i < 16; ++i)
                            fprintf(f, "  [RSP+0x%02X] = 0x%016llX\n", i * 8, rspPtr[i]);
                    } else {
                        fprintf(f, " <unreadable>\n");
                    }

                    fprintf(f, "=== END CRASH LOG ===\n");
                    fflush(f);
                    fclose(f);
                }
            }

            // Also try to flush spdlog
            try { spdlog::default_logger()->flush(); } catch (...) {}

            return EXCEPTION_CONTINUE_SEARCH;
        }

        // ================================================================
        // v1.22.27: SEH-isolated InitItem — prevents crash on forms with
        // null internal fields (e.g., Character forms with null race ptr).
        // MSVC: __try/__except cannot coexist with C++ destructors, so
        // this must be a separate function (same pattern as SafeGetEditorID).
        // ================================================================
        inline bool SafeInitItem(RE::TESForm* form) noexcept
        {
            __try {
                form->InitItem();
                return true;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Forward declarations (defined after CloseTES hook)
        inline void ManuallyCompileFiles();
        inline std::atomic<bool> g_manualCompileDone{ false };

        // ================================================================
        // v1.22.3: Hook TESFile::CloseTES — detect if files are closed
        // after catalog scan (would indicate catalog is a separate pass)
        //
        // v1.22.15: MAIN-THREAD COMPILATION TRIGGER
        // Under Wine with 600+ files, CompileFiles is skipped entirely.
        // The form loading decision is made on the main thread AFTER the
        // catalog scan. Previously, ManuallyCompileFiles ran from a
        // background monitor thread — too late (the decision was already
        // made). Now we trigger it from this CloseTES hook which runs on
        // the main thread DURING the catalog scan. We call it repeatedly
        // (idempotent) to compile files as they're cataloged, and keep
        // re-asserting loadingFiles=true so the engine sees it when it
        // checks on the main thread after the catalog scan ends.
        // ================================================================
        inline SafetyHookInline g_hk_CloseTES;

        inline void TESFile_CloseTES(RE::TESFile* a_self, bool a_force)
        {
            auto count = g_closeTESCalls.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 5 || (count % 1000 == 0)) {
                logger::info("  CloseTES #{}: file='{}' force={}",
                    count, a_self ? a_self->fileName : "null", a_force);
            }
            g_hk_CloseTES.call(a_self, a_force);

            // --- v1.22.15: Main-thread compilation trigger ---
            // Static locals are safe here: CloseTES is called from the main thread only.
            static bool s_firstCompileDone = false;
            static std::size_t s_lastCompileAtClose = 0;

            // After first compile: keep re-asserting loadingFiles=true on the main
            // thread until form loading actually starts (OpenTES > 0). This ensures
            // the engine's main-thread check sees it before the form loading decision.
            if (s_firstCompileDone) {
                if (g_openTESCalls.load(std::memory_order_relaxed) == 0) {
                    auto* tdh = RE::TESDataHandler::GetSingleton();
                    if (tdh) {
                        tdh->loadingFiles = true;
                    }
                }

                // Re-compile every 1000 CloseTES calls to pick up newly-cataloged files.
                // SUPPRESSED during ForceLoadAllForms: AE 13753 iterates compiledFileCollection
                // and calling ManuallyCompileFiles here would clear+rebuild it (iterator invalidation).
                if (!g_forceLoadInProgress.load(std::memory_order_acquire) &&
                    count >= s_lastCompileAtClose + 1000) {
                    s_lastCompileAtClose = count;
                    ManuallyCompileFiles();
                }
                return;
            }

            // First compile: trigger when enough catalog work is done.
            // Check every 200 CloseTES calls starting from 2000 (enough files to matter).
            // Conditions: no normal compilation, no form loading yet, TDH exists with files.
            if (count >= 2000 && count % 200 == 0 &&
                g_addCompileIndexCalls.load(std::memory_order_relaxed) == 0 &&
                g_openTESCalls.load(std::memory_order_relaxed) == 0)
            {
                auto* tdh = RE::TESDataHandler::GetSingleton();
                if (tdh && !tdh->loadingFiles) {
                    // Count current files
                    std::size_t fileCount = 0;
                    for (auto& file : tdh->files) {
                        if (file) ++fileCount;
                    }

                    // Trigger when we have substantial files (>100) and have seen
                    // enough CloseTES calls to have processed them (>= fileCount)
                    if (fileCount > 100 && count >= fileCount) {
                        logger::warn(">>> MAIN-THREAD COMPILE TRIGGER: CloseTES #{}, {} files cataloged, "
                                     "0 AddCompileIndex, 0 OpenTES — triggering ManuallyCompileFiles <<<",
                            count, fileCount);

                        // Prevent monitor thread from also triggering
                        g_manualCompileDone.store(true, std::memory_order_release);

                        ManuallyCompileFiles();

                        s_firstCompileDone = true;
                        s_lastCompileAtClose = count;

                        logger::info(">>> MAIN-THREAD COMPILE DONE: loadingFiles={}, "
                                     "compiledCollection={} reg + {} ESL <<<",
                            tdh->loadingFiles,
                            tdh->compiledFileCollection.files.size(),
                            tdh->compiledFileCollection.smallFiles.size());
                    }
                }
            }
        }

        // ================================================================
        // v1.22.3: Hook TESFile::SeekNextSubrecord — counts subrecord
        // iteration. The loading pass reads subrecords within each form.
        // A high count means the game is reading form data, not just headers.
        // ================================================================
        inline SafetyHookInline g_hk_SeekNextSubrecord;

        inline bool TESFile_SeekNextSubrecord(RE::TESFile* a_self)
        {
            g_seekNextSubrecordCalls.fetch_add(1, std::memory_order_relaxed);
            return g_hk_SeekNextSubrecord.call<bool>(a_self);
        }

        // ================================================================
        // v1.22.3: Hook TESFile::ReadData — counts bytes read from files.
        // Shows total I/O volume (header-only = small, full load = huge).
        // ================================================================
        inline SafetyHookInline g_hk_ReadData;

        inline bool TESFile_ReadData(RE::TESFile* a_self, void* a_buf, std::uint32_t a_inLength)
        {
            g_readDataCalls.fetch_add(1, std::memory_order_relaxed);
            g_readDataBytes.fetch_add(a_inLength, std::memory_order_relaxed);
            return g_hk_ReadData.call<bool>(a_self, a_buf, a_inLength);
        }

        // v1.22.16: AE 11596 hook REMOVED — confirmed never called during initial loading.
        // The initial data load uses a different code path than AE 11596.

        // v1.22.17: Multi-target .text scanner REMOVED in v1.22.18 — served its
        // purpose identifying AE 13698 (form loading, 54 AddForm call sites) and
        // AE 13753 (loading orchestration, 2 OpenTES calls). Scanner caused
        // zero-activity startup (possible Offset2ID allocation interference).

        inline void ReplaceFormMapFunctions()
        {
            const REL::Relocation getForm{ RELOCATION_ID(14461, 14617) };
            g_hk_GetFormByNumericId = safetyhook::create_inline(getForm.address(), TESForm_GetFormByNumericId);

            const REL::Relocation RemoveAt{ RELOCATION_ID(14514, 14710) };
            g_hk_RemoveAt = safetyhook::create_inline(RemoveAt.address(), FormMap_RemoveAt);

            // DISABLED: SetAt callsite hooks may corrupt TESForm::ctor with wrong
            // offsets for AE 1.6.1170, preventing all form creation after SKSE loads.
            // The 166 pre-existing forms (created before SKSE) are the only ones that
            // survive, while all ESM form creation silently fails.
            // TODO: Verify correct offsets for AE 1.6.1170 before re-enabling.
            logger::info("form caching: SetAt callsite hooks DISABLED (investigating form creation failure)"sv);
#if 0
            // there is one call that is not the form table so we will callsite hook
#ifdef SKYRIM_AE
            constexpr std::array todoSetAt = {
                std::pair(14593, 0x2B0),
                std::pair(14593, 0x301),
                std::pair(14666, 0xFE)
            };
#else
            constexpr std::array todoSetAt = {
                std::pair(14438, 0x1DE),
                std::pair(14438, 0x214),
                std::pair(14508, 0x16C),
                std::pair(14508, 0x1A2)
            };
#endif

            for (auto& [id, offset] : todoSetAt) {
                REL::Relocation target{ REL::ID(id), offset };
                orig_FormScatterTable_SetAt = target.write_call<5>(FormScatterTable_SetAt);
            }
#endif

            const REL::Relocation ClearData{ RELOCATION_ID(13646, 13754) };
            g_hk_ClearData = safetyhook::create_inline(ClearData.address(), TESDataHandler_ClearData);

            const REL::Relocation InitializeFormDataStructures{ RELOCATION_ID(14511, 14669) };
            g_hk_InitializeFormDataStructures = safetyhook::create_inline(InitializeFormDataStructures.address(), TESForm_InitializeFormDataStructures);

            // v1.22.0: Hook AddFormToDataHandler to count form registrations
            const REL::Relocation AddForm{ RELOCATION_ID(13597, 13693) };
            g_hk_AddFormToDataHandler = safetyhook::create_inline(AddForm.address(), TESDataHandler_AddFormToDataHandler);
            logger::info("form caching: AddFormToDataHandler hook installed (counting form registrations)"sv);

            // v1.22.1: Hook TESFile::OpenTES to count file opens
            const REL::Relocation OpenTES{ RELOCATION_ID(13855, 13931) };
            g_hk_OpenTES = safetyhook::create_inline(OpenTES.address(), TESFile_OpenTES);
            logger::info("form caching: OpenTES hook at 0x{:X} (offset 0x{:X})"sv,
                OpenTES.address(), OpenTES.address() - REL::Module::get().base());

            // v1.22.2: Hook AddCompileIndex to count compile index assignments
            const REL::Relocation AddCompileIndex{ RELOCATION_ID(14509, 14667) };
            g_hk_AddCompileIndex = safetyhook::create_inline(AddCompileIndex.address(), TESForm_AddCompileIndex);
            logger::info("form caching: AddCompileIndex hook at 0x{:X} (offset 0x{:X})"sv,
                AddCompileIndex.address(), AddCompileIndex.address() - REL::Module::get().base());

            // v1.22.2: Hook SeekNextForm to count record iteration
            const REL::Relocation SeekNextForm{ RELOCATION_ID(13894, 13979) };
            g_hk_SeekNextForm = safetyhook::create_inline(SeekNextForm.address(), TESFile_SeekNextForm);
            logger::info("form caching: SeekNextForm hook at 0x{:X} (offset 0x{:X})"sv,
                SeekNextForm.address(), SeekNextForm.address() - REL::Module::get().base());

            // v1.22.3: Hook CloseTES to detect file close after catalog
            const REL::Relocation CloseTES{ RELOCATION_ID(13857, 13933) };
            g_hk_CloseTES = safetyhook::create_inline(CloseTES.address(), TESFile_CloseTES);
            logger::info("form caching: CloseTES hook at 0x{:X} (offset 0x{:X})"sv,
                CloseTES.address(), CloseTES.address() - REL::Module::get().base());

            // v1.22.3: Hook SeekNextSubrecord to count subrecord reads
            const REL::Relocation SeekNextSubrecord{ RELOCATION_ID(13903, 13990) };
            g_hk_SeekNextSubrecord = safetyhook::create_inline(SeekNextSubrecord.address(), TESFile_SeekNextSubrecord);
            logger::info("form caching: SeekNextSubrecord hook at 0x{:X}"sv, SeekNextSubrecord.address());

            // v1.22.3: Hook ReadData to count bytes read
            const REL::Relocation ReadData{ RELOCATION_ID(13904, 13991) };
            g_hk_ReadData = safetyhook::create_inline(ReadData.address(), TESFile_ReadData);
            logger::info("form caching: ReadData hook at 0x{:X}"sv, ReadData.address());

            // Log all hook addresses for verification
            logger::info("form caching: hook summary — GetForm=0x{:X} AddForm=0x{:X} OpenTES=0x{:X} AddCompileIdx=0x{:X} SeekNextForm=0x{:X} CloseTES=0x{:X}"sv,
                getForm.address(), AddForm.address(), OpenTES.address(),
                AddCompileIndex.address(), SeekNextForm.address(), CloseTES.address());

            // v1.22.17 scanner results (now removed — caused startup hang):
            //   AE 13698 at 0x1B1D30: 54 AddFormToDataHandler calls = FORM LOADING FUNCTION
            //   AE 13753 at 0x1B96E0: 2 OpenTES calls = LOADING ORCHESTRATION
            //   AE 13785 at 0x1BE460: 3 OpenTES calls = HotLoadPlugin (irrelevant)
            // See ForceLoadAllForms() for the actual fix.

            // v1.22.82: BSTHashMap::SetAt serialization — prevents circular bucket chains
            // under Wine's threading model. Both template variants share a single spinlock.
            {
                auto base = REL::Module::get().base();
                g_hk_SetAtA = safetyhook::create_inline(
                    reinterpret_cast<void*>(base + 0x1945D0), BSTHashMap_SetAt_A);
                g_hk_SetAtB = safetyhook::create_inline(
                    reinterpret_cast<void*>(base + 0x1947C0), BSTHashMap_SetAt_B);
            }
            logger::info("form caching: BSTHashMap::SetAt spinlock hooks installed at +0x1945D0, +0x1947C0 (A={}, B={})"sv,
                static_cast<bool>(g_hk_SetAtA), static_cast<bool>(g_hk_SetAtB));
        }

        // Cached plugins.txt data (parsed once, reused across idempotent calls)
        inline std::set<std::string> g_enabledNames;   // lowercase enabled plugin names
        inline std::set<std::string> g_disabledNames;  // lowercase disabled plugin names
        inline bool g_pluginsTxtParsed = false;
        inline bool g_pluginsTxtLoaded = false;

        inline void EnsurePluginsTxtLoaded()
        {
            if (g_pluginsTxtLoaded) return;
            g_pluginsTxtLoaded = true;

            WCHAR localAppData[MAX_PATH] = {};
            HRESULT hr = SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData);
            if (hr != S_OK) return;

            std::wstring pluginsPath = localAppData;
            pluginsPath += L"\\Skyrim Special Edition\\Plugins.txt";

            HANDLE hFile = CreateFileW(pluginsPath.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

            if (hFile == INVALID_HANDLE_VALUE) return;

            DWORD fileSize = GetFileSize(hFile, NULL);
            if (fileSize > 0 && fileSize < 10 * 1024 * 1024) {
                std::vector<char> buf(fileSize + 1, 0);
                DWORD bytesRead = 0;
                if (ReadFile(hFile, buf.data(), fileSize, &bytesRead, NULL) && bytesRead > 0) {
                    std::string content(buf.data(), bytesRead);
                    std::istringstream stream(content);
                    std::string line;
                    while (std::getline(stream, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line.empty() || line[0] == '#') continue;

                        if (line[0] == '*') {
                            std::string name = line.substr(1);
                            std::transform(name.begin(), name.end(), name.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            g_enabledNames.insert(std::move(name));
                        } else {
                            std::string name = line;
                            std::transform(name.begin(), name.end(), name.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            g_disabledNames.insert(std::move(name));
                        }
                    }
                    g_pluginsTxtParsed = true;
                }
            }
            CloseHandle(hFile);

            logger::info("ManuallyCompileFiles: plugins.txt parsed={}, {} enabled, {} disabled",
                g_pluginsTxtParsed, g_enabledNames.size(), g_disabledNames.size());
        }

        // Manual compilation: assign compile indices to all enabled files
        // and populate compiledFileCollection. This replicates what the
        // engine's CompileFiles does when it doesn't skip.
        //
        // v1.22.14: The kActive flag is set BY the compilation process, so it's
        // always 0 when compilation is skipped. Instead, we parse plugins.txt
        // to determine which files are enabled (same source the engine uses).
        //
        // v1.22.15: IDEMPOTENT — safe to call multiple times. Skips files that
        // already have a compile index assigned (compileIndex != 0xFF). This
        // allows incremental compilation during the catalog scan: each call
        // processes any newly-cataloged files since the previous call.
        inline void ManuallyCompileFiles()
        {
            auto* tdh = RE::TESDataHandler::GetSingleton();
            if (!tdh) {
                logger::error("ManuallyCompileFiles: TDH is null!"sv);
                return;
            }

            // Parse plugins.txt once (cached for subsequent calls)
            EnsurePluginsTxtLoaded();

            // --- Find current max compile indices (for idempotent re-entry) ---
            std::uint8_t nextRegIdx = 0;
            std::uint16_t nextEslIdx = 0;

            for (auto& file : tdh->files) {
                if (!file || file->compileIndex == 0xFF) continue;
                if (file->compileIndex == 0xFE) {
                    // ESL — track highest smallFileCompileIndex
                    if (file->smallFileCompileIndex >= nextEslIdx)
                        nextEslIdx = file->smallFileCompileIndex + 1;
                } else {
                    // Regular — track highest compileIndex
                    if (file->compileIndex >= nextRegIdx)
                        nextRegIdx = file->compileIndex + 1;
                }
            }

            // --- Assign compile indices to NEW files only ---
            std::size_t regCount = 0;
            std::size_t eslCount = 0;
            std::size_t skippedAlready = 0;
            std::size_t skippedDisabled = 0;

            for (auto& file : tdh->files) {
                if (!file) continue;

                // Skip files already compiled (idempotent guard)
                if (file->compileIndex != 0xFF) {
                    ++skippedAlready;
                    continue;
                }

                // Determine if this file should be compiled
                bool shouldCompile;
                if (g_pluginsTxtParsed) {
                    std::string lowerName(file->fileName);
                    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    bool isEnabled = g_enabledNames.count(lowerName) > 0;
                    bool isDisabled = g_disabledNames.count(lowerName) > 0;
                    shouldCompile = isEnabled || !isDisabled;
                } else {
                    shouldCompile = true;  // fallback: compile everything
                }

                if (!shouldCompile) {
                    ++skippedDisabled;
                    continue;
                }

                if (file->IsLight()) {
                    if (nextEslIdx <= 0xFFF) {
                        file->compileIndex = 0xFE;
                        file->smallFileCompileIndex = nextEslIdx++;
                        tdh->compiledFileCollection.smallFiles.push_back(file);
                        ++eslCount;
                    } else {
                        logger::warn("ManualCompile: ESL limit exceeded at '{}'", file->fileName);
                    }
                } else {
                    if (nextRegIdx <= 0xFD) {
                        file->compileIndex = nextRegIdx++;
                        tdh->compiledFileCollection.files.push_back(file);
                        ++regCount;
                    } else {
                        logger::warn("ManualCompile: regular plugin limit exceeded at '{}'", file->fileName);
                    }
                }
            }

            // Set loadingFiles = true so the engine proceeds with form loading
            tdh->loadingFiles = true;

            logger::info("ManuallyCompileFiles: assigned {} reg + {} ESL = {} NEW (skipped {} already-compiled, {} disabled)",
                regCount, eslCount, regCount + eslCount, skippedAlready, skippedDisabled);
            logger::info("ManuallyCompileFiles: compiledFileCollection.files={} .smallFiles={} | loadingFiles=TRUE",
                tdh->compiledFileCollection.files.size(),
                tdh->compiledFileCollection.smallFiles.size());
        }

        // ================================================================
        // v1.22.21: ForceLoadAllForms — VALIDATION BYPASS + CLEAN COMPILE STATE
        //
        // Called at kDataLoaded when form loading was skipped (0 AddForm
        // calls). Under Wine with 600+ plugins, the engine skips
        // CompileFiles and consequently all form loading.
        //
        // BREAKTHROUGH (v1.22.19 hex dump analysis):
        // AE 13753 is the engine's form loading orchestrator. It has:
        //   1. Correct signature: void(TESDataHandler*, bool)
        //      (v1.22.18 called with wrong 1-param signature!)
        //   2. Validation loop at +0xE0 iterates ALL files in TDH->files:
        //      - Calls SomeFunc (module+0x1C8E40): false → skip file
        //      - Calls AnotherFunc (module+0x1C6E90): false → ABORT ALL
        //   3. Abort instruction at +0x106: JE (74 09) → abort path
        //   4. Form loading loop at +0x11C runs ONLY if validation passes
        //
        // Fix: Temporarily NOP the abort jump (74 09 → 90 90) to convert
        // the abort into a skip-to-next-file. Then call AE 13753 with
        // the correct 2-param signature. The engine's own form loading
        // loop at +0x11C then runs, loading all forms natively.
        // ================================================================
        // ================================================================
        // v1.22.36: Binary patch at AE 29846+0x95 — targeted crash fix
        // with form ID resolution.
        //
        // The instruction `test dword ptr [rax+0x28], 0x3FF` crashes when
        // RAX holds a raw form ID (< 0x1'00000000) instead of a pointer.
        //
        // Code cave logic:
        //   1. Check if RAX high 32 bits != 0 → valid pointer → original TEST
        //   2. If form ID: call TESForm::GetFormByNumericId to resolve
        //   3. If resolved: replace RAX, execute original TEST
        //   4. If not resolved: xor eax,eax + ZF=1, skip
        //
        // v1.22.36: Now resolves form IDs instead of just zeroing RAX.
        // This prevents cascading null-pointer failures downstream.
        // ================================================================
        inline void PatchFormPointerValidation()
        {
#ifdef SKYRIM_AE
            REL::Relocation<std::uintptr_t> fn29846{ REL::ID(29846) };
            auto* patchSite = reinterpret_cast<std::uint8_t*>(fn29846.address() + 0x95);
            auto* returnSite = patchSite + 7; // +0x9C = after the 7-byte TEST

            logger::info("PatchFormPointerValidation: bytes at 29846+0x95: "
                "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                patchSite[0], patchSite[1], patchSite[2], patchSite[3],
                patchSite[4], patchSite[5], patchSite[6], patchSite[7]);

            if (patchSite[0] != 0xF7 || patchSite[1] != 0x40 || patchSite[2] != 0x28) {
                logger::error("PatchFormPointerValidation: unexpected bytes — NOT patching");
                return;
            }

            // Get the address of TESForm::GetFormByNumericId (AE 14617)
            REL::Relocation<std::uintptr_t> fnGetForm{ REL::ID(14617) };
            auto getFormAddr = fnGetForm.address();
            logger::info("PatchFormPointerValidation: GetFormByNumericId at 0x{:X}", getFormAddr);

            // Allocate code cave from SKSE trampoline (near SkyrimSE.exe for rel32)
            auto caveSize = 256;
            auto& trampoline = SKSE::GetTrampoline();
            auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(caveSize));
            auto returnAddr = reinterpret_cast<std::uint64_t>(returnSite);

            int off = 0;

            // ---- CHECK IF RAX IS A VALID POINTER ----
            // push r11
            cave[off++] = 0x41; cave[off++] = 0x53;
            // mov r11, rax
            cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xC3;
            // shr r11, 32
            cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEB; cave[off++] = 0x20;
            // test r11, r11
            cave[off++] = 0x4D; cave[off++] = 0x85; cave[off++] = 0xDB;
            // pop r11
            cave[off++] = 0x41; cave[off++] = 0x5B;
            // jz .form_id
            cave[off++] = 0x74;
            int jzOffset = off;
            cave[off++] = 0x00;

            // ---- VALID POINTER PATH ----
            // test dword ptr [rax+28h], 3FFh
            cave[off++] = 0xF7; cave[off++] = 0x40; cave[off++] = 0x28;
            cave[off++] = 0xFF; cave[off++] = 0x03; cave[off++] = 0x00; cave[off++] = 0x00;
            // jmp [rip+xx] → returnSite
            cave[off++] = 0xFF; cave[off++] = 0x25;
            cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
            std::memcpy(&cave[off], &returnAddr, 8); off += 8;

            // ---- FORM ID PATH: resolve via GetFormByNumericId ----
            int formIdPathStart = off;
            cave[jzOffset] = static_cast<std::uint8_t>(formIdPathStart - (jzOffset + 1));

            // Save volatile registers (preserve caller state for the call)
            // push rcx
            cave[off++] = 0x51;
            // push rdx
            cave[off++] = 0x52;
            // push r8
            cave[off++] = 0x41; cave[off++] = 0x50;
            // push r9
            cave[off++] = 0x41; cave[off++] = 0x51;
            // push r10
            cave[off++] = 0x41; cave[off++] = 0x52;
            // push r11
            cave[off++] = 0x41; cave[off++] = 0x53;
            // push rax (save original form ID)
            cave[off++] = 0x50;
            // sub rsp, 0x28 (shadow space 0x20 + 8 alignment: 7 pushes=56, +0x28=96, 96%16=0)
            cave[off++] = 0x48; cave[off++] = 0x83; cave[off++] = 0xEC; cave[off++] = 0x28;

            // mov ecx, eax (form ID → first arg, zero-extended)
            cave[off++] = 0x89; cave[off++] = 0xC1;

            // mov rax, <GetFormByNumericId address>
            cave[off++] = 0x48; cave[off++] = 0xB8;
            std::memcpy(&cave[off], &getFormAddr, 8); off += 8;

            // call rax
            cave[off++] = 0xFF; cave[off++] = 0xD0;

            // test rax, rax (check if form was found)
            cave[off++] = 0x48; cave[off++] = 0x85; cave[off++] = 0xC0;

            // jz .not_found
            cave[off++] = 0x74;
            int jzNotFoundOff = off;
            cave[off++] = 0x00;

            // ---- FOUND: RAX = resolved TESForm* ----
            // add rsp, 0x30 (0x28 shadow + 8 skip saved rax)
            cave[off++] = 0x48; cave[off++] = 0x83; cave[off++] = 0xC4; cave[off++] = 0x30;
            // pop r11
            cave[off++] = 0x41; cave[off++] = 0x5B;
            // pop r10
            cave[off++] = 0x41; cave[off++] = 0x5A;
            // pop r9
            cave[off++] = 0x41; cave[off++] = 0x59;
            // pop r8
            cave[off++] = 0x41; cave[off++] = 0x58;
            // pop rdx
            cave[off++] = 0x5A;
            // pop rcx
            cave[off++] = 0x59;
            // RAX holds resolved pointer — execute original TEST
            // test dword ptr [rax+28h], 3FFh
            cave[off++] = 0xF7; cave[off++] = 0x40; cave[off++] = 0x28;
            cave[off++] = 0xFF; cave[off++] = 0x03; cave[off++] = 0x00; cave[off++] = 0x00;
            // jmp [rip+xx] → returnSite
            cave[off++] = 0xFF; cave[off++] = 0x25;
            cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
            std::memcpy(&cave[off], &returnAddr, 8); off += 8;

            // ---- NOT FOUND: zero RAX as last resort ----
            int notFoundStart = off;
            cave[jzNotFoundOff] = static_cast<std::uint8_t>(notFoundStart - (jzNotFoundOff + 1));
            // add rsp, 0x30 (0x28 shadow + 8 skip saved rax)
            cave[off++] = 0x48; cave[off++] = 0x83; cave[off++] = 0xC4; cave[off++] = 0x30;
            // pop r11
            cave[off++] = 0x41; cave[off++] = 0x5B;
            // pop r10
            cave[off++] = 0x41; cave[off++] = 0x5A;
            // pop r9
            cave[off++] = 0x41; cave[off++] = 0x59;
            // pop r8
            cave[off++] = 0x41; cave[off++] = 0x58;
            // pop rdx
            cave[off++] = 0x5A;
            // pop rcx
            cave[off++] = 0x59;
            // xor eax, eax (RAX=0, ZF=1)
            cave[off++] = 0x31; cave[off++] = 0xC0;
            // jmp [rip+xx] → returnSite
            cave[off++] = 0xFF; cave[off++] = 0x25;
            cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
            std::memcpy(&cave[off], &returnAddr, 8); off += 8;

            logger::info("PatchFormPointerValidation: code cave built, {} bytes at 0x{:X}",
                off, reinterpret_cast<std::uintptr_t>(cave));

            // Patch the original site: 5-byte JMP rel32 + 2 NOPs
            DWORD oldProtect;
            VirtualProtect(patchSite, 7, PAGE_EXECUTE_READWRITE, &oldProtect);

            auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
            auto jmpFrom = reinterpret_cast<std::intptr_t>(patchSite + 5);
            auto rel32 = static_cast<std::int32_t>(jmpTarget - jmpFrom);

            auto actualDist = jmpTarget - jmpFrom;
            if (actualDist > INT32_MAX || actualDist < INT32_MIN) {
                logger::error("PatchFormPointerValidation: JMP distance too large ({} bytes) — NOT patching", actualDist);
                VirtualProtect(patchSite, 7, oldProtect, &oldProtect);
                return;
            }

            patchSite[0] = 0xE9;
            std::memcpy(&patchSite[1], &rel32, 4);
            patchSite[5] = 0x90;
            patchSite[6] = 0x90;

            VirtualProtect(patchSite, 7, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), patchSite, 7);

            logger::info("PatchFormPointerValidation: patched 29846+0x95 → code cave (rel32={})", rel32);
#endif
        }

        // v1.22.36: Binary patches at hot VEH addresses with inline null checks.
        // Eliminates VEH overhead for the most-hit crash sites by checking for
        // null/sentinel form pointers before the faulting instruction.
        inline void PatchHotSpotNullChecks()
        {
#ifdef SKYRIM_AE
            auto base = REL::Module::get().base();
            auto& trampoline = SKSE::GetTrampoline();

            // ── Patch A: +0x2ED880 — null check before call [rax+0x4B8] ──
            // Original: FF 90 B8 04 00 00 (6 bytes)
            // If RAX is null or in sentinel page, skip the call and return 0.
            // The containing function's layout:
            //   +0x2ED880: call [rax+0x4B8]     ← PatchA (6 bytes)
            //   +0x2ED886: ...5 bytes...
            //   +0x2ED88B: test byte [rax+0x40], 0x01  ← PatchC (6 bytes)
            //   +0x2ED891: xor eax,eax; add rsp,0x28; ret  ← "return 0" epilogue
            // When the call fails (null RAX or bad call target), we must skip
            // to +0x2ED891 to return 0 from the function, NOT to +0x2ED886
            // which would execute with RAX=0 and freeze in an infinite loop.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2ED880);
                auto returnAddr = reinterpret_cast<std::uint64_t>(site + 6);
                // "return 0" epilogue: xor eax,eax; add rsp,0x28; ret
                auto nullAddr = static_cast<std::uint64_t>(base + 0x2ED891);

                logger::info("PatchHotSpotA: bytes at +0x2ED880: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                if (site[0] != 0xFF || site[1] != 0x90 || site[2] != 0xB8 ||
                    site[3] != 0x04 || site[4] != 0x00 || site[5] != 0x00) {
                    logger::error("PatchHotSpotA: unexpected bytes — NOT patching");
                } else {
                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(80));
                    int off = 0;

                    // test rax, rax
                    cave[off++] = 0x48; cave[off++] = 0x85; cave[off++] = 0xC0;
                    // jz .null_form
                    cave[off++] = 0x74;
                    int jzOff = off;
                    cave[off++] = 0x00; // placeholder

                    // Check sentinel range using R11 (volatile, destroyed by call)
                    if (g_zeroPage) {
                        // mov r11, <sentinelBase>
                        cave[off++] = 0x49; cave[off++] = 0xBB;
                        std::memcpy(&cave[off], &g_zeroPageBase, 8); off += 8;
                        // cmp rax, r11
                        cave[off++] = 0x4C; cave[off++] = 0x39; cave[off++] = 0xD8;
                        // jb .do_call
                        cave[off++] = 0x72;
                        int jbCallOff = off;
                        cave[off++] = 0x00;
                        // add r11, 0x10000
                        cave[off++] = 0x49; cave[off++] = 0x81; cave[off++] = 0xC3;
                        cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x01; cave[off++] = 0x00;
                        // cmp rax, r11
                        cave[off++] = 0x4C; cave[off++] = 0x39; cave[off++] = 0xD8;
                        // jb .null_form (in sentinel range)
                        cave[off++] = 0x72;
                        int jbNullOff = off;
                        cave[off++] = 0x00;

                        // .do_call:
                        int doCallStart = off;
                        cave[jbCallOff] = static_cast<std::uint8_t>(doCallStart - (jbCallOff + 1));

                        // Original: call qword ptr [rax+0x4B8]
                        cave[off++] = 0xFF; cave[off++] = 0x90;
                        cave[off++] = 0xB8; cave[off++] = 0x04; cave[off++] = 0x00; cave[off++] = 0x00;
                        // jmp [rip+0] → returnSite
                        cave[off++] = 0xFF; cave[off++] = 0x25;
                        cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                        std::memcpy(&cave[off], &returnAddr, 8); off += 8;

                        // .null_form:
                        int nullStart = off;
                        cave[jzOff] = static_cast<std::uint8_t>(nullStart - (jzOff + 1));
                        cave[jbNullOff] = static_cast<std::uint8_t>(nullStart - (jbNullOff + 1));
                    } else {
                        // No sentinel — just null check
                        // Original: call qword ptr [rax+0x4B8]
                        cave[off++] = 0xFF; cave[off++] = 0x90;
                        cave[off++] = 0xB8; cave[off++] = 0x04; cave[off++] = 0x00; cave[off++] = 0x00;
                        // jmp [rip+0] → returnSite
                        cave[off++] = 0xFF; cave[off++] = 0x25;
                        cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                        std::memcpy(&cave[off], &returnAddr, 8); off += 8;

                        // .null_form:
                        int nullStart = off;
                        cave[jzOff] = static_cast<std::uint8_t>(nullStart - (jzOff + 1));
                    }

                    // xor eax, eax  (null path — skip entire function, return 0)
                    cave[off++] = 0x31; cave[off++] = 0xC0;
                    // jmp [rip+0] → nullAddr (+0x2ED891: xor eax,eax; add rsp,0x28; ret)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &nullAddr, 8); off += 8;

                    logger::info("PatchHotSpotA: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + NOP (1 byte)
                    DWORD oldProt;
                    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotA: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 6, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90;
                        // NOTE: Do NOT restore original page protection — Wine reverts
                        // file-backed code pages when protection is restored, undoing patches.
                        FlushInstructionCache(GetCurrentProcess(), site, 6);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(nullAddr), "PatchA", site, 6);
                        logger::info("PatchHotSpotA: +0x2ED880 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch B: +0x32D146 — null check before cmp byte [rax+0x1A] ──
            // Original: 80 78 1A 2B 4C 0F 44 D0 (8 bytes)
            // cmp byte [rax+0x1A], 0x2B; cmovz r10, rax
            // If RAX is null, skip both instructions (type won't match anyway).
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x32D146);
                auto returnAddr = reinterpret_cast<std::uint64_t>(site + 8);

                logger::info("PatchHotSpotB: bytes at +0x32D146: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3],
                    site[4], site[5], site[6], site[7]);

                if (site[0] != 0x80 || site[1] != 0x78 || site[2] != 0x1A || site[3] != 0x2B ||
                    site[4] != 0x4C || site[5] != 0x0F || site[6] != 0x44 || site[7] != 0xD0) {
                    logger::error("PatchHotSpotB: unexpected bytes — NOT patching");
                } else {
                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(48));
                    int off = 0;

                    // test rax, rax
                    cave[off++] = 0x48; cave[off++] = 0x85; cave[off++] = 0xC0;
                    // jz .skip
                    cave[off++] = 0x74;
                    int jzOff = off;
                    cave[off++] = 0x00;
                    // Original: cmp byte [rax+0x1A], 0x2B
                    cave[off++] = 0x80; cave[off++] = 0x78; cave[off++] = 0x1A; cave[off++] = 0x2B;
                    // Original: cmovz r10, rax
                    cave[off++] = 0x4C; cave[off++] = 0x0F; cave[off++] = 0x44; cave[off++] = 0xD0;
                    // .skip:
                    int skipStart = off;
                    cave[jzOff] = static_cast<std::uint8_t>(skipStart - (jzOff + 1));
                    // jmp [rip+0] → returnSite
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &returnAddr, 8); off += 8;

                    logger::info("PatchHotSpotB: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 3 NOPs
                    DWORD oldProt;
                    VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotB: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 8, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; site[6] = 0x90; site[7] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 8);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(returnAddr), "PatchB", site, 8);
                        logger::info("PatchHotSpotB: +0x32D146 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch C: +0x2ED88B — null check before test byte [rax+0x40] ──
            // Original: F6 40 40 01 74 07 (6 bytes)
            //   test byte [rax+0x40], 0x01
            //   je +7  → +0x2ED898 (mov rax, [rax+0x128]; ret)
            // If je NOT taken: xor eax,eax; add rsp,0x28; ret (return 0)
            //
            // Function returns NULL if bit 0 is set, or [rax+0x128] if not.
            // When RAX is null/near-null (~0x7D00), the VEH redirect costs
            // ~50μs per call. At 42K/sec this consumes >100% of a CPU core
            // and freezes the game after New Game.
            //
            // Null behavior: return 0 (same as "flag set" path).
            // Sentinel page reads are harmless (no VEH), no sentinel check needed.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2ED88B);

                logger::info("PatchHotSpotC: bytes at +0x2ED88B: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                if (site[0] != 0xF6 || site[1] != 0x40 || site[2] != 0x40 ||
                    site[3] != 0x01 || site[4] != 0x74 || site[5] != 0x07) {
                    logger::error("PatchHotSpotC: unexpected bytes — NOT patching");
                } else {
                    // Return sites:
                    //   "return 0"   path: +0x2ED891 (xor eax,eax; add rsp,0x28; ret)
                    //   "get member" path: +0x2ED898 (mov rax,[rax+0x128]; add rsp,0x28; ret)
                    std::uint64_t returnZeroAddr = static_cast<std::uint64_t>(base + 0x2ED891);
                    std::uint64_t getMemberAddr  = static_cast<std::uint64_t>(base + 0x2ED898);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(48));
                    int off = 0;

                    // 64-bit validation: any 32-bit value (null, sentinel 0x7FD70000,
                    // form-ID-as-pointer like 0x3C003C00) has high dword == 0.
                    // Valid heap pointers are always > 4GB on x64.
                    // mov r10, rax
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xC2;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .return_zero
                    cave[off++] = 0x74;
                    int jbRetZeroOff = off;
                    cave[off++] = 0x00;

                    // Original: test byte [rax+0x40], 0x01
                    cave[off++] = 0xF6; cave[off++] = 0x40; cave[off++] = 0x40; cave[off++] = 0x01;
                    // je .get_member (ZF=1 → bit not set → return member)
                    cave[off++] = 0x74;
                    int jeGetMemOff = off;
                    cave[off++] = 0x00;

                    // .return_zero: (bit IS set, or null → return 0)
                    int retZeroStart = off;
                    cave[jbRetZeroOff] = static_cast<std::uint8_t>(retZeroStart - (jbRetZeroOff + 1));
                    // jmp [rip+0] → returnZeroAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &returnZeroAddr, 8); off += 8;

                    // .get_member:
                    int getMemStart = off;
                    cave[jeGetMemOff] = static_cast<std::uint8_t>(getMemStart - (jeGetMemOff + 1));
                    // jmp [rip+0] → getMemberAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &getMemberAddr, 8); off += 8;

                    logger::info("PatchHotSpotC: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + NOP (1 byte)
                    DWORD oldProt;
                    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotC: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 6, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 6);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(returnZeroAddr), "PatchC", site, 6);
                        logger::info("PatchHotSpotC: +0x2ED88B patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch D: +0x664A0B — null check before test byte [rbp+0x40] ──
            // Original: F6 45 40 01 74 05 (6 bytes)
            //   test byte [rbp+0x40], 0x01
            //   je +5  → +0x664A16 (test rbx, rbx; ...)
            // If je NOT taken: mov rsi, rbp; jmp +0x30 (use form)
            //
            // Main loop hot path — iterates forms and checks flag at +0x40.
            // When RBP is null (~0x7D00), VEH at 42K/sec freezes the game.
            //
            // Null behavior: take the je path (skip form, go to +0x664A16).
            // Sentinel page reads are harmless (no VEH), no sentinel check needed.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x664A0B);

                logger::info("PatchHotSpotD: bytes at +0x664A0B: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                if (site[0] != 0xF6 || site[1] != 0x45 || site[2] != 0x40 ||
                    site[3] != 0x01 || site[4] != 0x74 || site[5] != 0x05) {
                    logger::error("PatchHotSpotD: unexpected bytes — NOT patching");
                } else {
                    // Return sites:
                    //   je NOT taken: +0x664A11 (mov rsi, rbp — use this form)
                    //   je taken / null: +0x664A16 (test rbx, rbx — skip form)
                    std::uint64_t useFormAddr  = static_cast<std::uint64_t>(base + 0x664A11);
                    std::uint64_t skipFormAddr = static_cast<std::uint64_t>(base + 0x664A16);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(48));
                    int off = 0;

                    // 64-bit validation: high dword == 0 → invalid (null, sentinel, form ID)
                    // mov r10, rbp
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xEA;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .skip_form (32-bit value → take je path)
                    cave[off++] = 0x74;
                    int jbSkipOff = off;
                    cave[off++] = 0x00;

                    // Original: test byte [rbp+0x40], 0x01
                    cave[off++] = 0xF6; cave[off++] = 0x45; cave[off++] = 0x40; cave[off++] = 0x01;
                    // je .skip_form
                    cave[off++] = 0x74;
                    int jeSkipOff = off;
                    cave[off++] = 0x00;

                    // .use_form: (flag is set → use this form)
                    // jmp [rip+0] → useFormAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &useFormAddr, 8); off += 8;

                    // .skip_form: (flag not set, or null → skip)
                    int skipStart = off;
                    cave[jbSkipOff] = static_cast<std::uint8_t>(skipStart - (jbSkipOff + 1));
                    cave[jeSkipOff] = static_cast<std::uint8_t>(skipStart - (jeSkipOff + 1));
                    // jmp [rip+0] → skipFormAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &skipFormAddr, 8); off += 8;

                    logger::info("PatchHotSpotD: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + NOP (1 byte)
                    DWORD oldProt;
                    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotD: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 6, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 6);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(skipFormAddr), "PatchD", site, 6);
                        logger::info("PatchHotSpotD: +0x664A0B patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch E: +0x1AF7EF ──────────────────────────────────────
            // Hot site in a form ref-counting function.
            // Original bytes: 39 07 75 14 F0 FF 47 04 (8 bytes)
            //   cmp [rdi], eax     ; compare form type
            //   jne +0x14          ; if not equal, goto +0x1AF807
            //   lock inc [rdi+4]   ; atomic refcount++
            // When rdi is null/near-null (zero-paged form), this faults
            // ~47K/sec causing 100% CPU freeze on New Game.
            // Fix: inline null check → skip to function epilogue.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x1AF7EF);

                logger::info("PatchHotSpotE: bytes at +0x1AF7EF: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5], site[6], site[7]);

                if (site[0] != 0x39 || site[1] != 0x07 || site[2] != 0x75 ||
                    site[3] != 0x14 || site[4] != 0xF0 || site[5] != 0xFF ||
                    site[6] != 0x47 || site[7] != 0x04) {
                    logger::error("PatchHotSpotE: unexpected bytes — NOT patching");
                } else {
                    // Return sites:
                    //   epilogue (null or equal path): +0x1AF7F7 (mov rbp, [rsp+0x40])
                    //   jne target (not-equal path):   +0x1AF807
                    std::uint64_t epilogueAddr  = static_cast<std::uint64_t>(base + 0x1AF7F7);
                    std::uint64_t notEqualAddr  = static_cast<std::uint64_t>(base + 0x1AF807);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(48));
                    int off = 0;

                    // 64-bit validation: high dword == 0 → invalid (null, sentinel, form ID)
                    // mov r10, rdi
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xFA;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .null_path (32-bit value → skip to epilogue)
                    cave[off++] = 0x74;
                    int jbNullOff = off;
                    cave[off++] = 0x00;

                    // Original: cmp [rdi], eax
                    cave[off++] = 0x39; cave[off++] = 0x07;
                    // jne .not_equal
                    cave[off++] = 0x75;
                    int jneOff = off;
                    cave[off++] = 0x00;

                    // Equal path: lock inc dword [rdi+4]
                    cave[off++] = 0xF0; cave[off++] = 0xFF; cave[off++] = 0x47; cave[off++] = 0x04;

                    // .null_path: (also equal-path falls through here)
                    int nullStart = off;
                    cave[jbNullOff] = static_cast<std::uint8_t>(nullStart - (jbNullOff + 1));
                    // jmp [rip+0] → epilogueAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &epilogueAddr, 8); off += 8;

                    // .not_equal:
                    int notEqStart = off;
                    cave[jneOff] = static_cast<std::uint8_t>(notEqStart - (jneOff + 1));
                    // jmp [rip+0] → notEqualAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &notEqualAddr, 8); off += 8;

                    logger::info("PatchHotSpotE: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 3 NOPs (overwrite all 8 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotE: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 8, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; site[6] = 0x90; site[7] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 8);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(epilogueAddr), "PatchE", site, 8);
                        logger::info("PatchHotSpotE: +0x1AF7EF patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            logger::info("PatchHotSpotNullChecks: {} code caves registered", g_numCavePatches.load());

            // ── Patch F: +0x2EB95D ──────────────────────────────────────
            // Hot site in form flag-checking code (same area as Patch C).
            // Original bytes: F6 46 40 01 48 0F 44 F3 (8 bytes)
            //   test byte [rsi+0x40], 0x01  ; check form flag
            //   cmovz rsi, rbx              ; if flag clear, rsi = rbx
            // When rsi is null/near-null (0x7D00), [rsi+0x40] = 0x7D40
            // faults ~43K/sec on New Game.
            // Fix: inline null check → skip test, force cmovz (rsi=rbx).
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2EB95D);

                logger::info("PatchHotSpotF: bytes at +0x2EB95D: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5], site[6], site[7]);

                if (site[0] != 0xF6 || site[1] != 0x46 || site[2] != 0x40 ||
                    site[3] != 0x01 || site[4] != 0x48 || site[5] != 0x0F ||
                    site[6] != 0x44 || site[7] != 0xF3) {
                    logger::error("PatchHotSpotF: unexpected bytes — NOT patching");
                } else {
                    // After the 8 patched bytes, code continues at +0x2EB965:
                    //   4C 8B FB  mov r15, rbx
                    //   48 85 F6  test rsi, rsi
                    //   75 09     jne +9 → +0x2EB971
                    // Null path: rsi=rbx (rbx is the fallback), continue at +0x2EB965
                    // Normal path: test [rsi+0x40], cmovz, continue at +0x2EB965
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x2EB965);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(56));
                    int off = 0;

                    // 64-bit validation: high dword == 0 → invalid (null, sentinel, form ID)
                    // mov r10, rsi
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xF2;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .null_path (32-bit value → force rsi=rbx)
                    cave[off++] = 0x74;
                    int jbNullOff = off;
                    cave[off++] = 0x00;

                    // Original: test byte [rsi+0x40], 0x01
                    cave[off++] = 0xF6; cave[off++] = 0x46; cave[off++] = 0x40; cave[off++] = 0x01;
                    // Original: cmovz rsi, rbx
                    cave[off++] = 0x48; cave[off++] = 0x0F; cave[off++] = 0x44; cave[off++] = 0xF3;
                    // jmp continue
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .null_path: mov rsi, rbx (force fallback)
                    int nullStart = off;
                    cave[jbNullOff] = static_cast<std::uint8_t>(nullStart - (jbNullOff + 1));
                    cave[off++] = 0x48; cave[off++] = 0x89; cave[off++] = 0xDE; // mov rsi, rbx
                    // jmp continue
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    logger::info("PatchHotSpotF: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 3 NOPs (overwrite all 8 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotF: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 8, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; site[6] = 0x90; site[7] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 8);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(continueAddr), "PatchF", site, 8);
                        logger::info("PatchHotSpotF: +0x2EB95D patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch G: +0x179337 ──────────────────────────────────────
            // Linked list virtual call site. Iterates form nodes:
            //   mov rax, [rdi]     ; load vtable from form object
            //   mov rcx, rdi       ; this = rdi
            //   call [rax+0x08]    ; virtual function call (vtable slot 1)
            // When rdi = sentinel page (PAGE_READWRITE), engine writes
            // corrupt the vtable pointer (offset +0x00) to 0xFFFFFFFF.
            // call [0xFFFFFFFF+8] faults → CATCH-ALL flood → crash.
            // Fix: validate vtable is in SkyrimSE.exe image (high dword=1)
            // before calling. Invalid vtable → exit loop (not-found path).
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x179337);

                logger::info("PatchHotSpotG: bytes at +0x179337: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4],
                    site[5], site[6], site[7], site[8]);

                // Expected: 48 8B 07 48 8B CF FF 50 08
                //   mov rax, [rdi]; mov rcx, rdi; call [rax+0x08]
                if (site[0] != 0x48 || site[1] != 0x8B || site[2] != 0x07 ||
                    site[3] != 0x48 || site[4] != 0x8B || site[5] != 0xCF ||
                    site[6] != 0xFF || site[7] != 0x50 || site[8] != 0x08) {
                    logger::error("PatchHotSpotG: unexpected bytes — NOT patching");
                } else {
                    // After the 9 patched bytes, +0x179340:
                    //   3B C5        cmp eax, ebp     ; compare result
                    //   74 0B        je +0x0B         ; found → +0x17934F
                    //   48 8B 7F 08  mov rdi,[rdi+8]  ; next node
                    //   48 85 FF     test rdi, rdi    ; null check
                    //   75 EA        jne → loop       ; +0x179337
                    //   EB 43        jmp → +0x179392  ; not-found exit
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x179340);
                    // Loop exit (not-found): +0x17934D is "jmp +0x43" → +0x179392
                    std::uint64_t exitLoopAddr = static_cast<std::uint64_t>(base + 0x17934D);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(80));
                    int off = 0;

                    // 1. 64-bit validation: high dword == 0 → invalid (null, sentinel, form ID)
                    // mov r10, rdi
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xFA;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .exit_loop
                    cave[off++] = 0x74;
                    int jbExitOff = off;
                    cave[off++] = 0x00;

                    // 2. mov rax, [rdi]  — load vtable
                    cave[off++] = 0x48; cave[off++] = 0x8B; cave[off++] = 0x07;

                    // 3. Validate vtable: high dword must be 1 (SkyrimSE.exe image = 0x14XXXXXXX)
                    //    mov r10, rax
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xC2;
                    //    shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    //    cmp r10d, 1
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x01;
                    //    jne .exit_loop
                    cave[off++] = 0x75;
                    int jneExitOff = off;
                    cave[off++] = 0x00;

                    // 4. Vtable valid — original code: mov rcx, rdi; call [rax+0x08]
                    cave[off++] = 0x48; cave[off++] = 0x8B; cave[off++] = 0xCF;  // mov rcx, rdi
                    cave[off++] = 0xFF; cave[off++] = 0x50; cave[off++] = 0x08;  // call [rax+0x08]

                    // 5. jmp [rip+0] → continueAddr (+0x179340)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .exit_loop:
                    int exitStart = off;
                    cave[jbExitOff] = static_cast<std::uint8_t>(exitStart - (jbExitOff + 1));
                    cave[jneExitOff] = static_cast<std::uint8_t>(exitStart - (jneExitOff + 1));
                    // jmp [rip+0] → exitLoopAddr (+0x17934D, which is "jmp +0x43" → not-found)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &exitLoopAddr, 8); off += 8;

                    logger::info("PatchHotSpotG: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch site: JMP rel32 (5 bytes) + 4 NOPs (overwrite all 9 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 9, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotG: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 9, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; site[6] = 0x90; site[7] = 0x90; site[8] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 9);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(exitLoopAddr), "PatchG", site, 9);
                        logger::info("PatchHotSpotG: +0x179337 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch H: +0x1790A4 ──────────────────────────────────────
            // Array indexed access in form processing code.
            // Original bytes: 41 8B 04 C6  25 00 00 F0 03 (9 bytes)
            //   mov eax, [r14 + rax*8]  ; load from array using r14 as base
            //   and eax, 0x03F00000     ; mask upper bits
            // When r14 = sentinel page (0x7FD70000, only 64KB), the index
            // rax*8 exceeds the page boundary → fault at unmapped address.
            // The CATCH-ALL handler can't fix this (wrong register gets
            // redirected), leading to state corruption → later EXT-CRASH.
            // Fix: check if r14 is a valid 64-bit heap pointer (high dword
            // must be nonzero). Sentinel/null are 32-bit → skip with eax=0.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x1790A4);

                logger::info("PatchHotSpotH: bytes at +0x1790A4: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4],
                    site[5], site[6], site[7], site[8]);

                // Expected: 41 8B 04 C6 25 00 00 F0 03
                if (site[0] != 0x41 || site[1] != 0x8B || site[2] != 0x04 ||
                    site[3] != 0xC6 || site[4] != 0x25 || site[5] != 0x00 ||
                    site[6] != 0x00 || site[7] != 0xF0 || site[8] != 0x03) {
                    logger::error("PatchHotSpotH: unexpected bytes — NOT patching");
                } else {
                    // After the 9 patched bytes, code continues at +0x1790AD:
                    //   0B C1        or eax, ecx
                    //   89 03        mov [rbx], eax
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x1790AD);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(64));
                    int off = 0;

                    // 1. Check r14 is a valid 64-bit pointer (high dword must be nonzero)
                    //    mov r10, r14
                    cave[off++] = 0x4D; cave[off++] = 0x89; cave[off++] = 0xF2;
                    //    shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    //    test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    //    jz .skip (r14 is 32-bit address → sentinel or invalid)
                    cave[off++] = 0x74;
                    int jzSkipOff = off;
                    cave[off++] = 0x00;

                    // 2. R14 valid — original: mov eax, [r14 + rax*8]
                    cave[off++] = 0x41; cave[off++] = 0x8B; cave[off++] = 0x04; cave[off++] = 0xC6;
                    // Original: and eax, 0x03F00000
                    cave[off++] = 0x25; cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0xF0; cave[off++] = 0x03;

                    // 3. jmp [rip+0] → continueAddr (+0x1790AD)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .skip: r14 is sentinel/invalid — return eax=0
                    int skipStart = off;
                    cave[jzSkipOff] = static_cast<std::uint8_t>(skipStart - (jzSkipOff + 1));
                    //    xor eax, eax
                    cave[off++] = 0x31; cave[off++] = 0xC0;
                    //    jmp [rip+0] → continueAddr (+0x1790AD)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    logger::info("PatchHotSpotH: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch site: JMP rel32 (5 bytes) + 4 NOPs (overwrite all 9 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 9, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotH: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 9, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; site[6] = 0x90; site[7] = 0x90; site[8] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 9);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(continueAddr), "PatchH", site, 9);
                        logger::info("PatchHotSpotH: +0x1790A4 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch I: +0x2D5DE0 ──────────────────────────────────────
            // Hash table chain traversal — linked list walk comparing
            // node keys. This is the #1 crash site on New Game.
            // Original bytes: 48 39 08 0F 84 FC 00 00 00 (9 bytes)
            //   cmp [rax], rcx      ; compare node value with search key
            //   je +0xFC            ; → +0x2D5EE5 (found handler)
            // When rax = sentinel (0x7FD70000, PAGE_READWRITE), engine
            // writes corrupt the sentinel data — [rax] reads garbage
            // (0xFFFFFFFF etc.) which faults. The CATCH-ALL redirects
            // to sentinel but [sentinel+8] (next ptr) is also garbage,
            // creating an infinite fault loop (200+ events → crash).
            // Fix: 64-bit validate rax before dereference. Invalid →
            // exit loop to not-found path at +0x2D5DF3.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2D5DE0);

                logger::info("PatchHotSpotI: bytes at +0x2D5DE0: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4],
                    site[5], site[6], site[7], site[8]);

                // Expected: 48 39 08 0F 84 FC 00 00 00
                if (site[0] != 0x48 || site[1] != 0x39 || site[2] != 0x08 ||
                    site[3] != 0x0F || site[4] != 0x84 || site[5] != 0xFC ||
                    site[6] != 0x00 || site[7] != 0x00 || site[8] != 0x00) {
                    logger::error("PatchHotSpotI: unexpected bytes — NOT patching");
                } else {
                    // Return sites:
                    //   continue (after patched bytes): +0x2D5DE9 (mov rax, [rax+8])
                    //   found handler:                  +0x2D5EE5 (je +0xFC target)
                    //   not-found (loop exit):          +0x2D5DF3 (natural fall-through)
                    std::uint64_t continueAddr  = static_cast<std::uint64_t>(base + 0x2D5DE9);
                    std::uint64_t foundAddr     = static_cast<std::uint64_t>(base + 0x2D5EE5);
                    std::uint64_t notFoundAddr  = static_cast<std::uint64_t>(base + 0x2D5DF3);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(64));
                    int off = 0;

                    // 1. 64-bit validation: high dword == 0 → invalid
                    // mov r10, rax
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xC2;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .not_found
                    cave[off++] = 0x74;
                    int jzNotFoundOff = off;
                    cave[off++] = 0x00;

                    // 2. Original: cmp [rax], rcx
                    cave[off++] = 0x48; cave[off++] = 0x39; cave[off++] = 0x08;
                    // je .found_handler
                    cave[off++] = 0x74;
                    int jeFoundOff = off;
                    cave[off++] = 0x00;

                    // 3. Fall-through (not equal): jmp → continueAddr (+0x2D5DE9)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .found_handler:
                    int foundStart = off;
                    cave[jeFoundOff] = static_cast<std::uint8_t>(foundStart - (jeFoundOff + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &foundAddr, 8); off += 8;

                    // .not_found: exit loop
                    int notFoundStart = off;
                    cave[jzNotFoundOff] = static_cast<std::uint8_t>(notFoundStart - (jzNotFoundOff + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &notFoundAddr, 8); off += 8;

                    logger::info("PatchHotSpotI: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 4 NOPs (overwrite all 9 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 9, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotI: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 9, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; site[6] = 0x90; site[7] = 0x90; site[8] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 9);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(notFoundAddr), "PatchI", site, 9);
                        logger::info("PatchHotSpotI: +0x2D5DE0 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch J: +0x19CDF5 ──────────────────────────────────────
            // Form type check — movzx eax, byte [rdx+0x44] then cmp al, 3.
            // RDX is corrupted sentinel data. Safe default: xor eax, eax.
            // Overwrite 6 bytes: 0F B6 42 44 3C 03 (movzx + cmp al,3).
            // Cave executes both if valid. Continue at +0x19CDFB.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x19CDF5);

                logger::info("PatchHotSpotJ: bytes at +0x19CDF5: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                // Expected: 0F B6 42 44 3C 03
                if (site[0] != 0x0F || site[1] != 0xB6 || site[2] != 0x42 ||
                    site[3] != 0x44 || site[4] != 0x3C || site[5] != 0x03) {
                    logger::error("PatchHotSpotJ: unexpected bytes — NOT patching");
                } else {
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x19CDFB);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(48));
                    int off = 0;

                    // 64-bit validation: high dword == 0 → invalid
                    // mov r10, rdx
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xD2;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .invalid
                    cave[off++] = 0x74;
                    int jzInvalidOff = off;
                    cave[off++] = 0x00;

                    // .valid: original movzx eax, byte [rdx+0x44]
                    cave[off++] = 0x0F; cave[off++] = 0xB6; cave[off++] = 0x42; cave[off++] = 0x44;
                    // original cmp al, 3
                    cave[off++] = 0x3C; cave[off++] = 0x03;
                    // jmp [rip+0] → continueAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .invalid: xor eax, eax (safe default) then continue
                    int invalidStart = off;
                    cave[jzInvalidOff] = static_cast<std::uint8_t>(invalidStart - (jzInvalidOff + 1));
                    cave[off++] = 0x31; cave[off++] = 0xC0;
                    // cmp al, 3 (set flags consistently — al=0 so CF=1, ZF=0)
                    cave[off++] = 0x3C; cave[off++] = 0x03;
                    // jmp [rip+0] → continueAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    logger::info("PatchHotSpotJ: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 1 NOP (overwrite all 6 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotJ: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 6, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 6);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(continueAddr), "PatchJ", site, 6);
                        logger::info("PatchHotSpotJ: +0x19CDF5 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch K: +0x2B54D0 ──────────────────────────────────────
            // Reading formFlags — mov eax, [rcx+0x10] then shr eax, 0xA.
            // RCX is corrupted. Safe default: xor eax, eax.
            // Overwrite 6 bytes: 8B 41 10 C1 E8 0A (mov + shr).
            // Cave executes both if valid. Continue at +0x2B54D6.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2B54D0);

                logger::info("PatchHotSpotK: bytes at +0x2B54D0: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                // Expected: 8B 41 10 C1 E8 0A
                if (site[0] != 0x8B || site[1] != 0x41 || site[2] != 0x10 ||
                    site[3] != 0xC1 || site[4] != 0xE8 || site[5] != 0x0A) {
                    logger::error("PatchHotSpotK: unexpected bytes — NOT patching");
                } else {
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x2B54D6);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(48));
                    int off = 0;

                    // 64-bit validation: high dword == 0 → invalid
                    // mov r10, rcx
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xCA;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .invalid
                    cave[off++] = 0x74;
                    int jzInvalidOff = off;
                    cave[off++] = 0x00;

                    // .valid: original mov eax, [rcx+0x10]
                    cave[off++] = 0x8B; cave[off++] = 0x41; cave[off++] = 0x10;
                    // original shr eax, 0xA
                    cave[off++] = 0xC1; cave[off++] = 0xE8; cave[off++] = 0x0A;
                    // jmp [rip+0] → continueAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .invalid: xor eax, eax (safe default, already 0 after shr)
                    int invalidStart = off;
                    cave[jzInvalidOff] = static_cast<std::uint8_t>(invalidStart - (jzInvalidOff + 1));
                    cave[off++] = 0x31; cave[off++] = 0xC0;
                    // jmp [rip+0] → continueAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    logger::info("PatchHotSpotK: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 1 NOP (overwrite all 6 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotK: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 6, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 6);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(continueAddr), "PatchK", site, 6);
                        logger::info("PatchHotSpotK: +0x2B54D0 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch L: +0x2D5DB0 ──────────────────────────────────────
            // Pointer read from structure — mov rbp, [rbx+0x20] then test rbp, rbp.
            // RBX may be corrupted (sentinel-like 0x7D000000XX passes 64-bit check
            // but [rbx+0x20] faults). VEH CAVE-FAULT then redirects with wrong flags.
            // Fix: invalid path and VEH fallback both jump to function epilogue
            // at +0x2D5EE5 (restores regs from stack, returns al=1) — safe regardless
            // of register/flag state.
            // Overwrite 7 bytes: 48 8B 6B 20 48 85 ED (mov + test).
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2D5DB0);

                logger::info("PatchHotSpotL: bytes at +0x2D5DB0: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5], site[6]);

                // Expected: 48 8B 6B 20 48 85 ED
                if (site[0] != 0x48 || site[1] != 0x8B || site[2] != 0x6B ||
                    site[3] != 0x20 || site[4] != 0x48 || site[5] != 0x85 ||
                    site[6] != 0xED) {
                    logger::error("PatchHotSpotL: unexpected bytes — NOT patching");
                } else {
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x2D5DB7);
                    // Function epilogue: restores rbx/rbp/rsi from stack, returns al=1
                    std::uint64_t epilogueAddr = static_cast<std::uint64_t>(base + 0x2D5EE5);

                    // Also need grow-table address for rbp==0 case
                    std::uint64_t growTableAddr = static_cast<std::uint64_t>(base + 0x2D5E32);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(96));
                    int off = 0;

                    // 1. Validate RBX — canonical range check.
                    //    high dword must be 1-128 (valid user-mode heap/image).
                    //    Rejects: 0 (null/sentinel), >128 (non-canonical/garbage text).
                    //    Catches ASCII text like 0x574E4D454D414C43 that old check missed.
                    // mov r10, rbx
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xDA;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // dec r10d  (0→0xFFFFFFFF, 1→0, 0x574E4D45→0x574E4D44)
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA;
                    // cmp r10d, 0x7F  (valid range [1..128] → dec'd [0..127])
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F;
                    // ja .invalid
                    cave[off++] = 0x77;
                    int jaInvalid1Off = off;
                    cave[off++] = 0x00; // patched below

                    // 2. Original: mov rbp, [rbx+0x20]
                    cave[off++] = 0x48; cave[off++] = 0x8B; cave[off++] = 0x6B; cave[off++] = 0x20;

                    // 3. Check rbp == 0 → grow table
                    // test rbp, rbp
                    cave[off++] = 0x48; cave[off++] = 0x85; cave[off++] = 0xED;
                    // jz .growTable
                    cave[off++] = 0x74;
                    int jzGrowOff = off;
                    cave[off++] = 0x00; // patched below

                    // 4. Validate RBP — same canonical range check
                    // mov r10, rbp
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xEA;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // dec r10d
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA;
                    // cmp r10d, 0x7F
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F;
                    // ja .invalid
                    cave[off++] = 0x77;
                    int jaInvalid2Off = off;
                    cave[off++] = 0x00; // patched below

                    // 5. RBP valid. Need ZF=0 for je at +0x2D5DB7 to NOT take.
                    //    After cmp r10d,0x7F where r10d<128, CF=1 but ZF=0. Good.
                    // jmp [rip+0] → continueAddr (+0x2D5DB7)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .growTable: RBP is zero — jump to grow-table path (+0x2D5E32)
                    int growStart = off;
                    cave[jzGrowOff] = static_cast<std::uint8_t>(growStart - (jzGrowOff + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &growTableAddr, 8); off += 8;

                    // .invalid: RBX or RBP failed canonical check → xor rax,rax + epilogue
                    int invalidStart = off;
                    cave[jaInvalid1Off] = static_cast<std::uint8_t>(invalidStart - (jaInvalid1Off + 1));
                    cave[jaInvalid2Off] = static_cast<std::uint8_t>(invalidStart - (jaInvalid2Off + 1));
                    // xor eax, eax (zero RAX — signal "not found" to caller)
                    cave[off++] = 0x31; cave[off++] = 0xC0;
                    // jmp [rip+0] → epilogueAddr (+0x2D5EE5)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &epilogueAddr, 8); off += 8;

                    logger::info("PatchHotSpotL: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 2 NOPs (overwrite all 7 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 7, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotL: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 7, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; site[6] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 7);
                        // VEH CAVE-FAULT fallback also goes to epilogue (safe regardless of flags)
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(epilogueAddr), "PatchL", site, 7);
                        logger::info("PatchHotSpotL: +0x2D5DB0 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch M: +0x2D0C33 ──────────────────────────────────────
            // Reading field — mov eax, [rsi+0x94]. RSI is corrupted.
            // Safe default: xor eax, eax.
            // Overwrite 6 bytes: 8B 86 94 00 00 00 (exact fit for JMP rel32 + NOP).
            // Continue at +0x2D0C39.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2D0C33);

                logger::info("PatchHotSpotM: bytes at +0x2D0C33: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                // Expected: 8B 86 94 00 00 00
                if (site[0] != 0x8B || site[1] != 0x86 || site[2] != 0x94 ||
                    site[3] != 0x00 || site[4] != 0x00 || site[5] != 0x00) {
                    logger::error("PatchHotSpotM: unexpected bytes — NOT patching");
                } else {
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x2D0C39);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(48));
                    int off = 0;

                    // 64-bit validation: high dword == 0 → invalid
                    // mov r10, rsi
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xF2;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .invalid
                    cave[off++] = 0x74;
                    int jzInvalidOff = off;
                    cave[off++] = 0x00;

                    // .valid: original mov eax, [rsi+0x94]
                    cave[off++] = 0x8B; cave[off++] = 0x86;
                    cave[off++] = 0x94; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    // jmp [rip+0] → continueAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .invalid: xor eax, eax (safe default)
                    int invalidStart = off;
                    cave[jzInvalidOff] = static_cast<std::uint8_t>(invalidStart - (jzInvalidOff + 1));
                    cave[off++] = 0x31; cave[off++] = 0xC0;
                    // jmp [rip+0] → continueAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    logger::info("PatchHotSpotM: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 1 NOP (overwrite all 6 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotM: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 6, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 6);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(continueAddr), "PatchM", site, 6);
                        logger::info("PatchHotSpotM: +0x2D0C33 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }
            // ── Patch N: +0x2D5E15 ──────────────────────────────────────
            // Hash table bucket probe: cmp qword [rcx+8], 0.
            // RCX = rbp + index*16 where rbp may be stale/corrupted (stack addr
            // or sentinel). Causes CATCH-ALL flood (200 events → CTD).
            // If RCX high dword == 0 → treat bucket as empty (skip cmp,
            // set ZF=1 so cmovne at +0x2D5E1A doesn't fire).
            // Overwrite 5 bytes: 48 83 79 08 00. Continue at +0x2D5E1A.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2D5E15);

                logger::info("PatchHotSpotN: bytes at +0x2D5E15: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4]);

                // Expected: 48 83 79 08 00
                if (site[0] != 0x48 || site[1] != 0x83 || site[2] != 0x79 ||
                    site[3] != 0x08 || site[4] != 0x00) {
                    logger::error("PatchHotSpotN: unexpected bytes — NOT patching");
                } else {
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x2D5E1A);
                    // Function epilogue — safe bail-out if bucket walk is hopeless
                    std::uint64_t epilogueAddr = static_cast<std::uint64_t>(base + 0x2D5EE5);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(64));
                    int off = 0;

                    // Canonical range check: high dword must be 1-128
                    // mov r10, rcx
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xCA;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // dec r10d
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA;
                    // cmp r10d, 0x7F
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F;
                    // ja .invalid
                    cave[off++] = 0x77;
                    int jzInvalidOff = off;
                    cave[off++] = 0x00;

                    // .valid: original cmp qword [rcx+8], 0
                    cave[off++] = 0x48; cave[off++] = 0x83; cave[off++] = 0x79;
                    cave[off++] = 0x08; cave[off++] = 0x00;
                    // jmp [rip+0] → continueAddr (+0x2D5E1A)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .invalid: xor rcx, rcx (null → cmovne won't fire, test rcx,rcx → jz exits loop)
                    int invalidStart = off;
                    cave[jzInvalidOff] = static_cast<std::uint8_t>(invalidStart - (jzInvalidOff + 1));
                    cave[off++] = 0x48; cave[off++] = 0x31; cave[off++] = 0xC9; // xor rcx, rcx
                    // jmp [rip+0] → epilogueAddr (bail out of function entirely)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &epilogueAddr, 8); off += 8;

                    logger::info("PatchHotSpotN: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) — exact fit
                    DWORD oldProt;
                    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotN: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 5, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        FlushInstructionCache(GetCurrentProcess(), site, 5);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(epilogueAddr), "PatchN", site, 5);
                        logger::info("PatchHotSpotN: +0x2D5E15 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch O: +0x2D5DCD ──────────────────────────────────────
            // Bucket occupancy check: cmp qword [rsi+8], 0.
            // RSI = rbp + (hash & (cap-1)) * 16. If RBP was stale/corrupted
            // (survived PatchL's validation but contained freed memory),
            // RSI points to garbage → ACCESS_VIOLATION at [rsi+8].
            // CATCH-ALL redirect makes it worse (redirects RSI to sentinel,
            // sentinel+8 has engine-written garbage → cascading corruption).
            // Fix: validate RSI high dword. Invalid → epilogue.
            // Overwrite 5 bytes: 48 83 7E 08 00 (exact JMP rel32 fit).
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2D5DCD);

                logger::info("PatchHotSpotO: bytes at +0x2D5DCD: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4]);

                // Expected: 48 83 7E 08 00
                if (site[0] != 0x48 || site[1] != 0x83 || site[2] != 0x7E ||
                    site[3] != 0x08 || site[4] != 0x00) {
                    logger::error("PatchHotSpotO: unexpected bytes — NOT patching");
                } else {
                    // Continue after the 5-byte cmp: +0x2D5DD2 (the je instruction)
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x2D5DD2);
                    std::uint64_t epilogueAddr = static_cast<std::uint64_t>(base + 0x2D5EE5);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(56));
                    int off = 0;

                    // 1. Validate RSI — canonical range check
                    // mov r10, rsi
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xF2;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // dec r10d
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA;
                    // cmp r10d, 0x7F
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F;
                    // ja .invalid
                    cave[off++] = 0x77;
                    int jzInvalidOff = off;
                    cave[off++] = 0x00; // patched below

                    // 2. Original instruction: cmp qword [rsi+8], 0
                    cave[off++] = 0x48; cave[off++] = 0x83; cave[off++] = 0x7E;
                    cave[off++] = 0x08; cave[off++] = 0x00;

                    // 3. Continue at +0x2D5DD2 (je instruction uses ZF from cmp)
                    // jmp [rip+0] → continueAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .invalid: RSI is bogus → bail to epilogue
                    int invalidStart = off;
                    cave[jzInvalidOff] = static_cast<std::uint8_t>(invalidStart - (jzInvalidOff + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &epilogueAddr, 8); off += 8;

                    logger::info("PatchHotSpotO: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) — exact fit
                    DWORD oldProt;
                    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotO: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 5, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        FlushInstructionCache(GetCurrentProcess(), site, 5);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(epilogueAddr), "PatchO", site, 5);
                        logger::info("PatchHotSpotO: +0x2D5DCD patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch P: +0x2D0B90 — call [rax+0x120] ──────────────────────
            // Virtual function call where RAX can be corrupted sentinel data
            // (engine wrote ASCII text to sentinel vtable area). Validates
            // RAX is in canonical heap range before the call; if not, skip
            // the call and jump past it (the next instruction at +0x2D0B96
            // is a jmp that exits the block).
            // Original bytes: FF 90 20 01 00 00 (6 bytes)
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2D0B90);

                logger::info("PatchHotSpotP: bytes at +0x2D0B90: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                // Expected: FF 90 20 01 00 00  (call [rax+0x120])
                if (site[0] != 0xFF || site[1] != 0x90 || site[2] != 0x20 ||
                    site[3] != 0x01 || site[4] != 0x00 || site[5] != 0x00) {
                    logger::error("PatchHotSpotP: unexpected bytes — NOT patching");
                } else {
                    // After the 6-byte call: +0x2D0B96 is the next instruction
                    // (jmp +0x2D11B7 per disasm). Skip to there on bad RAX.
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x2D0B96);
                    // The call returns to +0x2D0B96, so on skip we go there too.
                    std::uint64_t skipAddr = continueAddr;

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(64));
                    int off = 0;

                    // 1. Validate RAX — canonical range check (high dword in [1, 0x7F])
                    // push r10
                    cave[off++] = 0x41; cave[off++] = 0x52;
                    // mov r10, rax
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xC2;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // dec r10d
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA;
                    // cmp r10d, 0x7F
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F;
                    // pop r10
                    cave[off++] = 0x41; cave[off++] = 0x5A;
                    // ja .invalid (skip call)
                    cave[off++] = 0x77;
                    int jaOffset = off;
                    cave[off++] = 0x00; // patched below

                    // 2. Valid RAX — execute original: call [rax+0x120]
                    cave[off++] = 0xFF; cave[off++] = 0x90;
                    cave[off++] = 0x20; cave[off++] = 0x01; cave[off++] = 0x00; cave[off++] = 0x00;

                    // 3. Jump to continuation (+0x2D0B96)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .invalid: RAX is bogus — skip call, xor eax,eax (return 0)
                    int invalidStart = off;
                    cave[jaOffset] = static_cast<std::uint8_t>(invalidStart - (jaOffset + 1));
                    // xor eax, eax
                    cave[off++] = 0x31; cave[off++] = 0xC0;
                    // jmp to continuation (+0x2D0B96)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &skipAddr, 8); off += 8;

                    logger::info("PatchHotSpotP: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 + NOP (5+1 = 6 bytes — exact fit)
                    DWORD oldProt;
                    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotP: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 6, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; // NOP pad
                        FlushInstructionCache(GetCurrentProcess(), site, 6);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(skipAddr), "PatchP", site, 6);
                        logger::info("PatchHotSpotP: +0x2D0B90 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch Q (x6): Bounded hash-chain traversal ─────────────────
            // All 6 BSTHashMap::find() instances walk a linked list in a
            // bucket chain.  Under Wine, chains can become circular (next
            // pointer loops back) causing an infinite tight loop with zero
            // exceptions — this is THE New Game freeze.
            //
            // Loop pattern (identical in all 6):
            //   cmp  [rcx], ebx         ; key match?
            //   je   <found>
            //   mov  rcx, [rcx+0x10]    ; next node
            //   cmp  rcx, [rdi+0x70]    ; sentinel check
            //   jne  <loop>
            //
            // Each cave reproduces the pre-loop + loop with r11d as a
            // 65536 iteration bound.  R11 is volatile (Windows x64 ABI).
            //
            // Sites found by binary scan of identical loop pattern:
            //   +0x1AF612 (in func +0x1AF5C0) — original freeze site
            //   +0x1A0EF1 (in func +0x1A0E80)
            //   +0x30C385, +0x30C4B2, +0x30C5D5, +0x30C702 (in func +0x30C2E0)
            {
                // All 6 loop sites: { loopRVA, preLoopStartRVA, patchSize,
                //                     notFoundRVA, foundRVA, preLoopBytes, name }
                struct HashChainSite {
                    std::uint32_t preLoopRVA;     // start of pre-loop check
                    std::uint32_t patchSize;      // bytes to overwrite
                    std::uint32_t notFoundRVA;    // "return null" target
                    std::uint32_t foundRVA;       // "found" target
                    const std::uint8_t* preLoopVerify;  // bytes to verify
                    int preLoopVerifyLen;
                    const char* name;
                };

                // Verification bytes at each pre-loop start
                // +0x1AF607: 48 39 74 CA 10 (cmp [rdx+rcx*8+0x10], rsi)
                static const std::uint8_t verify_1AF[] = { 0x48, 0x39, 0x74, 0xCA, 0x10 };
                // +0x1A0EE5: 48 83 7C C2 10 00 (cmp qword [rdx+rax*8+0x10], 0)
                static const std::uint8_t verify_1A0[] = { 0x48, 0x83, 0x7C, 0xC2, 0x10 };
                // +0x30C375: 48 83 7C C2 10 00 (same pattern for all 4 in the big func)
                static const std::uint8_t verify_30C[] = { 0x48, 0x83, 0x7C, 0xC2, 0x10 };

                // Each site: pre-loop bytes + lea + je/jz + loop body (cmp/je/mov/cmp/jne)
                // We patch from the initial bucket-empty check through the loop back-edge.
                HashChainSite sites[] = {
                    // +0x1AF607: 19 bytes (cmp rsi variant, short je)
                    { 0x1AF607, 19, 0x1AF620, 0x1AF633, verify_1AF, 5, "Q1" },
                    // +0x1A0EE5: 26 bytes (cmp 0 variant, short je, loop + jmp after)
                    { 0x1A0EE5, 26, 0x1A0F50, 0x1A0F01, verify_1A0, 5, "Q2" },
                    // +0x30C375: 30 bytes (cmp 0 variant, near je, loop + jmp after)
                    { 0x30C375, 30, 0x30C440, 0x30C398, verify_30C, 5, "Q3" },
                    // +0x30C4A2: 30 bytes
                    { 0x30C4A2, 30, 0x30C56D, 0x30C4C5, verify_30C, 5, "Q4" },
                    // +0x30C5C5: 30 bytes
                    { 0x30C5C5, 30, 0x30C690, 0x30C5E8, verify_30C, 5, "Q5" },
                    // +0x30C6F2: 30 bytes
                    { 0x30C6F2, 30, 0x30C7B9, 0x30C715, verify_30C, 5, "Q6" },
                };

                for (auto& s : sites) {
                    auto* site = reinterpret_cast<std::uint8_t*>(base + s.preLoopRVA);

                    logger::info("PatchHashChain-{}: bytes at +0x{:X}: "
                        "{:02X} {:02X} {:02X} {:02X} {:02X}",
                        s.name, s.preLoopRVA,
                        site[0], site[1], site[2], site[3], site[4]);

                    if (std::memcmp(site, s.preLoopVerify, s.preLoopVerifyLen) != 0) {
                        logger::error("PatchHashChain-{}: unexpected bytes — NOT patching", s.name);
                        continue;
                    }

                    auto returnNullAddr = static_cast<std::uint64_t>(base + s.notFoundRVA);
                    auto foundAddr      = static_cast<std::uint64_t>(base + s.foundRVA);

                    // Build the cave: copy the original pre-loop bytes into
                    // the cave, then add the bounded loop.
                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(128));
                    int off = 0;

                    // -- Pre-loop: copy original bytes up to the loop start --
                    // All variants have: bucket-empty-check + lea rcx + je/jz not-found
                    // We encode the cave generically: the loop body is always the same,
                    // and we emit the pre-loop as the original bytes minus the loop itself.
                    //
                    // For all variants, the last 12 bytes before patchEnd are the loop:
                    //   39 19              cmp [rcx], ebx
                    //   74 XX              je  found
                    //   48 8B 49 10        mov rcx, [rcx+0x10]
                    //   48 3B 4F 70        cmp rcx, [rdi+0x70]
                    //   75 F2              jne loop
                    // Some also have a trailing jmp/jne after the loop (2-5 bytes).
                    // We skip those since the cave handles not-found via jmp [rip].

                    int preLoopSize = s.patchSize - 12;
                    // For the 30-byte variants, there's a 5-byte jmp at the end
                    // (e9 XX XX XX XX) after the jne. For the 26-byte variant,
                    // there's a 2-byte jmp (eb XX). Subtract those too.
                    if (s.patchSize == 30) preLoopSize -= 5;  // trailing jmp rel32
                    else if (s.patchSize == 26) preLoopSize -= 2;  // trailing jmp rel8
                    // Now preLoopSize = bytes before 'cmp [rcx], ebx'

                    // Copy pre-loop bytes verbatim into cave
                    std::memcpy(&cave[off], site, preLoopSize);
                    off += preLoopSize;

                    // The last pre-loop instruction is a conditional jump to not-found.
                    // We need to fixup that jump target to point to our return_null label.
                    // For the short-je variant (patchSize=19): byte at off-1 is the rel8
                    // For the cmp-0 + short-je variant (patchSize=26): byte at off-1
                    // For the cmp-0 + near-jz variant (patchSize=30): bytes at off-4..off-1

                    // Mark where the initial "not found" je/jz needs to jump:
                    int jeNullFixupOff;
                    bool nearJump = false;
                    if (s.patchSize == 30) {
                        // near jz (0F 84 XX XX XX XX) — the last 6 bytes of pre-loop
                        // are the jz, so the rel32 is at off-4
                        jeNullFixupOff = off - 4;
                        nearJump = true;
                    } else {
                        // short je (74 XX) — rel8 is at off-1
                        jeNullFixupOff = off - 1;
                    }

                    // -- NEW: set iteration bound in r11d --
                    // mov r11d, 0x10000
                    cave[off++] = 0x41; cave[off++] = 0xBB;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x01; cave[off++] = 0x00;

                    // -- Loop body --
                    int loopTop = off;
                    // cmp [rcx], ebx
                    cave[off++] = 0x39; cave[off++] = 0x19;
                    // je found
                    cave[off++] = 0x74;
                    int jeFoundOff = off;
                    cave[off++] = 0x00;
                    // mov rcx, [rcx+0x10]
                    cave[off++] = 0x48; cave[off++] = 0x8B; cave[off++] = 0x49; cave[off++] = 0x10;
                    // cmp rcx, [rdi+0x70]
                    cave[off++] = 0x48; cave[off++] = 0x3B; cave[off++] = 0x4F; cave[off++] = 0x70;
                    // je return_null
                    cave[off++] = 0x74;
                    int jeNullOff2 = off;
                    cave[off++] = 0x00;
                    // dec r11d
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCB;
                    // jnz loop_top
                    cave[off++] = 0x75;
                    int loopBackOff = off;
                    cave[off++] = 0x00;
                    cave[loopBackOff] = static_cast<std::uint8_t>(
                        static_cast<std::int8_t>(loopTop - (loopBackOff + 1)));

                    // -- return_null label --
                    int returnNullStart = off;
                    // Fix up in-loop je to return_null
                    cave[jeNullOff2] = static_cast<std::uint8_t>(returnNullStart - (jeNullOff2 + 1));
                    // Fix up pre-loop je/jz to return_null
                    if (nearJump) {
                        auto rel32 = static_cast<std::int32_t>(returnNullStart - (jeNullFixupOff + 4));
                        std::memcpy(&cave[jeNullFixupOff], &rel32, 4);
                    } else {
                        cave[jeNullFixupOff] = static_cast<std::uint8_t>(returnNullStart - (jeNullFixupOff + 1));
                    }

                    // jmp [rip+0] → returnNullAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &returnNullAddr, 8); off += 8;

                    // -- found label --
                    int foundStart = off;
                    cave[jeFoundOff] = static_cast<std::uint8_t>(foundStart - (jeFoundOff + 1));
                    // jmp [rip+0] → foundAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &foundAddr, 8); off += 8;

                    logger::info("PatchHashChain-{}: cave {} bytes at 0x{:X}", s.name, off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch original site with JMP rel32 + NOPs
                    DWORD oldProt;
                    VirtualProtect(site, s.patchSize, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHashChain-{}: JMP too far ({}) — NOT patching", s.name, dist);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        for (std::uint32_t i = 5; i < s.patchSize; i++) site[i] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, s.patchSize);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(returnNullAddr), s.name, site, s.patchSize);
                        logger::info("PatchHashChain-{}: +0x{:X} patched — bounded hash-chain (max 65536)",
                            s.name, s.preLoopRVA);
                    }
                }
            }

            // ── Patch Q7: Bounded find loop in GetFormByNumericId ────────
            // The native GetFormByNumericId at +0x1E01A0 has an unbounded
            // find loop at +0x1E020C that walks bucket chains in the global
            // form map (Layout B: sentinel at [rbx+0x18]).  When our sharded
            // cache misses (new forms not yet cached), control falls through
            // to this native loop.  On corrupted chains → infinite loop →
            // freeze.  This is the LAST unbounded find loop that can trap
            // the main thread during New Game.
            //
            // Patch site: +0x1E0205 (21 bytes, through +0x1E0219)
            //   +0x1E0205: cmp qword [rax+0x10], 0    ; empty bucket?
            //   +0x1E020A: je +0x1E0220                ; → not found
            //   +0x1E020C: cmp [rax], edi              ; key match?
            //   +0x1E020E: je +0x1E021A                ; → found
            //   +0x1E0210: mov rax, [rax+0x10]         ; next node
            //   +0x1E0214: cmp rax, [rbx+0x18]         ; sentinel
            //   +0x1E0218: jne -0x0E                   ; → loop top
            //
            // Cave reproduces pre-loop + loop with r11d = 65536 bound.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x1E0205);
                static const std::uint8_t verify_Q7[] = { 0x48, 0x83, 0x78, 0x10, 0x00 };
                constexpr std::uint32_t patchSize = 21;

                logger::info("PatchHashChain-Q7: bytes at +0x1E0205: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4]);

                if (std::memcmp(site, verify_Q7, 5) != 0) {
                    logger::error("PatchHashChain-Q7: unexpected bytes — NOT patching");
                } else {
                    auto returnNullAddr = static_cast<std::uint64_t>(base + 0x1E0220);
                    auto foundAddr      = static_cast<std::uint64_t>(base + 0x1E021A);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(128));
                    int off = 0;

                    // Pre-loop: cmp qword [rax+0x10], 0  (48 83 78 10 00)
                    cave[off++] = 0x48; cave[off++] = 0x83;
                    cave[off++] = 0x78; cave[off++] = 0x10; cave[off++] = 0x00;
                    // je return_null (short, fixup later)
                    cave[off++] = 0x74;
                    int jeNullFixup1 = off;
                    cave[off++] = 0x00;

                    // mov r11d, 0x10000  (iteration bound)
                    cave[off++] = 0x41; cave[off++] = 0xBB;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x01; cave[off++] = 0x00;

                    // Loop body:
                    int loopTop = off;
                    // cmp [rax], edi  (39 38)
                    cave[off++] = 0x39; cave[off++] = 0x38;
                    // je found (short, fixup later)
                    cave[off++] = 0x74;
                    int jeFoundFixup = off;
                    cave[off++] = 0x00;
                    // mov rax, [rax+0x10]  (48 8B 40 10)
                    cave[off++] = 0x48; cave[off++] = 0x8B;
                    cave[off++] = 0x40; cave[off++] = 0x10;
                    // cmp rax, [rbx+0x18]  (48 3B 43 18)
                    cave[off++] = 0x48; cave[off++] = 0x3B;
                    cave[off++] = 0x43; cave[off++] = 0x18;
                    // je return_null (short, fixup later)
                    cave[off++] = 0x74;
                    int jeNullFixup2 = off;
                    cave[off++] = 0x00;
                    // dec r11d  (41 FF CB)
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCB;
                    // jnz loop_top
                    cave[off++] = 0x75;
                    int loopBackOff = off;
                    cave[off++] = 0x00;
                    cave[loopBackOff] = static_cast<std::uint8_t>(
                        static_cast<std::int8_t>(loopTop - (loopBackOff + 1)));

                    // return_null label:
                    int returnNullStart = off;
                    cave[jeNullFixup1] = static_cast<std::uint8_t>(returnNullStart - (jeNullFixup1 + 1));
                    cave[jeNullFixup2] = static_cast<std::uint8_t>(returnNullStart - (jeNullFixup2 + 1));
                    // jmp [rip+0] → returnNullAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &returnNullAddr, 8); off += 8;

                    // found label:
                    int foundStart = off;
                    cave[jeFoundFixup] = static_cast<std::uint8_t>(foundStart - (jeFoundFixup + 1));
                    // jmp [rip+0] → foundAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &foundAddr, 8); off += 8;

                    logger::info("PatchHashChain-Q7: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch original site
                    DWORD oldProt;
                    VirtualProtect(site, patchSize, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom   = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHashChain-Q7: JMP too far ({}) — NOT patching", dist);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        for (std::uint32_t i = 5; i < patchSize; i++) site[i] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, patchSize);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(returnNullAddr),
                            "Q7", site, patchSize);
                        logger::info("PatchHashChain-Q7: +0x1E0205 patched — bounded form-map find (max 65536)");
                    }
                }
            }
#endif
        }

        inline void ForceLoadAllForms()
        {
#ifdef SKYRIM_AE
            auto* tdh = RE::TESDataHandler::GetSingleton();
            if (!tdh) {
                logger::error("ForceLoadAllForms: TDH is null — cannot proceed"sv);
                return;
            }

            logger::info("=== ForceLoadAllForms (v1.22.27) ==="sv);

            // Log TDH state before reset
            logger::info("  TDH: loadingFiles={} hasDesiredFiles={} saveLoadGame={} clearingData={}",
                tdh->loadingFiles, tdh->hasDesiredFiles, tdh->saveLoadGame, tdh->clearingData);
            logger::info("  TDH: nextID=0x{:08X} activeFile={} AddForm={}",
                tdh->nextID,
                tdh->activeFile ? tdh->activeFile->fileName : "null",
                g_addFormCalls.load(std::memory_order_relaxed));

            // ==========================================================
            // CRITICAL: Reset all compile state before calling AE 13753.
            //
            // ManuallyCompileFiles (CloseTES hook) pre-sets file->compileIndex
            // on each TESFile and populates compiledFileCollection. When AE
            // 13753 runs its own compilation pass (calling AddCompileIndex per
            // form), the pre-set compileIndex causes TESForm::AddCompileIndex
            // to enter an infinite loop for some files (observed: Denizens of
            // Morthal - AI Overhaul Patch.esp looped 1.5 billion times).
            //
            // Root cause: TESForm::AddCompileIndex uses file->compileIndex to
            // build the global form slot. When the slot was pre-assigned by
            // ManuallyCompileFiles, the function's internal file iterator
            // cannot advance, producing an infinite loop.
            //
            // Fix: wipe all compile state so AE 13753 compiles from scratch.
            // This makes its compilation pass work cleanly with no conflicts.
            // ==========================================================
            std::size_t resetCount = 0;
            for (auto& file : tdh->files) {
                if (!file) continue;
                if (file->compileIndex != 0xFF) {
                    file->compileIndex = 0xFF;
                    file->smallFileCompileIndex = 0;
                    ++resetCount;
                }
            }
            auto prevReg = tdh->compiledFileCollection.files.size();
            auto prevEsl = tdh->compiledFileCollection.smallFiles.size();
            tdh->compiledFileCollection.files.clear();
            tdh->compiledFileCollection.smallFiles.clear();

            logger::info("  Compile state RESET: {} files cleared, compiledFileCollection was {} reg + {} ESL",
                resetCount, prevReg, prevEsl);

            // Set required flags
            tdh->loadingFiles = true;
            tdh->hasDesiredFiles = true;
            tdh->clearingData = false;

            // ==========================================================
            // VALIDATION BYPASS: NOP the abort jump in AE 13753
            //
            // At function offset +0x106, the instruction 74 09 (JE +9)
            // jumps to the abort path when a per-file validation function
            // (module+0x1C6E90) returns false. This aborts form loading
            // for ALL files even if only one file fails validation.
            //
            // We NOP this to 90 90, converting abort into skip-to-next.
            // The form loading loop at +0x11C then executes normally.
            // ==========================================================
            REL::Relocation<std::uintptr_t> fn13753{ REL::ID(13753) };
            auto fnAddr = fn13753.address();

            // Also log the function address for diagnostics
            logger::info("  AE 13753: addr=0x{:X} offset=0x{:X}",
                fnAddr, fnAddr - REL::Module::get().base());

            auto* patchAddr = reinterpret_cast<std::uint8_t*>(fnAddr + 0x106);

            // Verify the bytes match our disassembly
            if (patchAddr[0] != 0x74 || patchAddr[1] != 0x09) {
                logger::error("ForceLoadAllForms: UNEXPECTED bytes at AE 13753 +0x106: {:02X} {:02X} (expected 74 09)",
                    patchAddr[0], patchAddr[1]);
                logger::error("Binary layout mismatch — cannot apply validation bypass. Aborting.");
                return;
            }

            // Also verify surrounding context to make sure we have the right spot
            // Expected at +0x104: 84 C0 (test al, al) — the result check before the JE
            auto* contextAddr = reinterpret_cast<std::uint8_t*>(fnAddr + 0x104);
            if (contextAddr[0] != 0x84 || contextAddr[1] != 0xC0) {
                logger::error("ForceLoadAllForms: context mismatch at +0x104: {:02X} {:02X} (expected 84 C0)",
                    contextAddr[0], contextAddr[1]);
                logger::error("This is not the expected test-al-al before the JE. Aborting.");
                return;
            }

            logger::info("  Verified: +0x104=84 C0 (test al,al) +0x106=74 09 (je +9) — match!");

            // Save original bytes for restoration
            std::uint8_t origBytes[2] = { patchAddr[0], patchAddr[1] };

            // Apply the NOP patch
            DWORD oldProtect;
            if (!VirtualProtect(patchAddr, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                logger::error("ForceLoadAllForms: VirtualProtect failed (error={})", GetLastError());
                return;
            }
            patchAddr[0] = 0x90;  // NOP
            patchAddr[1] = 0x90;  // NOP
            VirtualProtect(patchAddr, 2, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), patchAddr, 2);

            logger::info("  PATCH APPLIED: AE 13753 +0x106 = 90 90 (validation abort → skip-to-next-file)");

            // Record counters before the call
            auto addFormBefore = g_addFormCalls.load(std::memory_order_relaxed);
            auto openTESBefore = g_openTESCalls.load(std::memory_order_relaxed);

            // Call AE 13753 with CORRECT 2-parameter signature
            // Param 1: TESDataHandler* (rcx)
            // Param 2: bool — false for initial loading, true for save loading (dl)
            using LoadFormsFn = void(*)(RE::TESDataHandler*, bool);
            REL::Relocation<LoadFormsFn> loadForms{ REL::ID(13753) };

            // v1.22.23: Install VEH to skip null dereferences inside AE 13753.
            // Large modlists contain ESL-range actor forms with null race/template
            // pointers. The VEH catches the AV, advances RIP past the bad instruction,
            // and continues — allowing the loading loop to proceed to the next form.
            // The CloseTES guard (g_forceLoadInProgress) prevents ManuallyCompileFiles
            // from invalidating compiledFileCollection while AE 13753 iterates it.
            g_vehSkipCount.store(0, std::memory_order_relaxed);
            g_forceLoadInProgress.store(true, std::memory_order_release);
            {
                VehGuard veh(1, ForceLoadVEH);

                logger::info(">>> Calling AE 13753(TDH*, false) with VEH null guard (v1.22.27) <<<");
                loadForms(tdh, false);
            }
            g_forceLoadInProgress.store(false, std::memory_order_release);
            auto skipCount = g_vehSkipCount.load(std::memory_order_relaxed);
            logger::info("  VEH skipped {} null dereferences during AE 13753", skipCount);

            auto addFormAfter = g_addFormCalls.load(std::memory_order_relaxed);
            auto openTESAfter = g_openTESCalls.load(std::memory_order_relaxed);

            // Restore original bytes immediately
            VirtualProtect(patchAddr, 2, PAGE_EXECUTE_READWRITE, &oldProtect);
            patchAddr[0] = origBytes[0];
            patchAddr[1] = origBytes[1];
            VirtualProtect(patchAddr, 2, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), patchAddr, 2);

            logger::info("  PATCH RESTORED: AE 13753 +0x106 = {:02X} {:02X}", origBytes[0], origBytes[1]);

            // Report results
            auto addFormDelta = addFormAfter - addFormBefore;
            auto openTESDelta = openTESAfter - openTESBefore;

            logger::info("=== ForceLoadAllForms (v1.22.27) RESULTS ==="sv);
            logger::info("  AddFormToDataHandler: {} → {} (+{})", addFormBefore, addFormAfter, addFormDelta);
            logger::info("  OpenTES calls: {} → {} (+{})", openTESBefore, openTESAfter, openTESDelta);
            logger::info("  TDH: nextID=0x{:08X} activeFile={}",
                tdh->nextID,
                tdh->activeFile ? tdh->activeFile->fileName : "null");
            logger::info("  TDH: loadingFiles={} compiledFiles={} compiledSmallFiles={}",
                tdh->loadingFiles,
                tdh->compiledFileCollection.files.size(),
                tdh->compiledFileCollection.smallFiles.size());

            // v1.22.22: Diagnostic — enumerate files that AE 13753 skipped (still at 0xFF).
            // These files were NOT loaded by the engine. Possible causes:
            //   (a) File not installed on disk (SomeFunc/existence check failed)
            //   (b) File skipped by per-file validation (SomeFunc at module+0x1C8E40)
            //   (c) File skipped by AnotherFunc (module+0x1C6E90) despite our NOP
            {
                std::vector<std::string> skippedFiles;
                std::size_t totalInTDH = 0;
                for (auto& file : tdh->files) {
                    if (!file) continue;
                    ++totalInTDH;
                    if (file->compileIndex == 0xFF && file->smallFileCompileIndex == 0) {
                        // Still uncompiled — AE 13753 skipped this file
                        skippedFiles.push_back(std::string(file->fileName ? file->fileName : "<null>"));
                    }
                }
                logger::info("  TDH->files total: {} | skipped by AE 13753: {}", totalInTDH, skippedFiles.size());
                if (!skippedFiles.empty()) {
                    logger::warn("=== FILES SKIPPED BY AE 13753 (compileIndex still 0xFF) ===");
                    // Log up to 100 skipped files to keep log manageable
                    std::size_t logLimit = std::min(skippedFiles.size(), std::size_t(100));
                    for (std::size_t i = 0; i < logLimit; ++i) {
                        logger::warn("  SKIPPED[{}]: {}", i, skippedFiles[i]);
                    }
                    if (skippedFiles.size() > 100) {
                        logger::warn("  ... and {} more skipped files (truncated)", skippedFiles.size() - 100);
                    }
                    logger::warn("=== END SKIPPED FILES LIST ===");
                }
            }

            if (addFormDelta > 100) {
                logger::info(">>> SUCCESS! Engine loaded {} forms via native form loading! <<<", addFormDelta);
            } else if (addFormDelta > 0) {
                logger::warn("Partial result: {} forms loaded (may need further investigation)", addFormDelta);
            } else {
                logger::error("NO forms loaded — validation bypass alone is insufficient");
            }

            // ==========================================================
            // v1.22.36: Hex dump AE 13753 validation loop for future analysis.
            // The per-file validation at +0xE0 to +0x120 determines which
            // files get loaded. Dumping bytes helps reverse-engineer the
            // skip logic that blocks 1787 files under Wine.
            // ==========================================================
            {
                auto* fnBytes = reinterpret_cast<const std::uint8_t*>(fnAddr);
                std::string hexDump;
                for (int i = 0xC0; i < 0x140; ++i) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "%02X ", fnBytes[i]);
                    hexDump += buf;
                    if ((i + 1) % 16 == 0) {
                        logger::info("  AE13753+0x{:03X}: {}", i - 15, hexDump);
                        hexDump.clear();
                    }
                }
                if (!hexDump.empty()) {
                    logger::info("  AE13753+0x{:03X}: {}", 0x130, hexDump);
                }
            }

            // ==========================================================
            // v1.22.36: Multi-pass form loading.
            //
            // After the first AE 13753 call, 1787 files are skipped
            // (compileIndex still 0xFF). The per-file validation function
            // (SomeFunc at module+0x1C8E40) rejects them, possibly because
            // their masters weren't compiled yet on the first pass.
            //
            // Strategy: re-compile skipped files and call AE 13753 again.
            // On subsequent passes, more masters are available, so more
            // dependent files may pass validation.
            //
            // Repeat up to 3 additional passes or until no progress.
            // ==========================================================
            for (int pass = 2; pass <= 4; ++pass) {
                // Count currently-skipped files
                std::size_t skippedCount = 0;
                for (auto& file : tdh->files) {
                    if (!file) continue;
                    if (file->compileIndex == 0xFF && file->smallFileCompileIndex == 0)
                        ++skippedCount;
                }

                if (skippedCount == 0) {
                    logger::info("  Multi-pass: all files compiled after pass {} — no more passes needed", pass - 1);
                    break;
                }

                logger::info("=== Multi-pass loading: pass {} ({} files still skipped) ===", pass, skippedCount);

                // Re-compile skipped files (ManuallyCompileFiles is idempotent —
                // only processes files with compileIndex=0xFF)
                ManuallyCompileFiles();

                // Ensure loading state
                tdh->loadingFiles = true;
                tdh->hasDesiredFiles = true;

                // Record counters before pass
                auto addFormBefore2 = g_addFormCalls.load(std::memory_order_relaxed);

                // Call AE 13753 again
                g_vehSkipCount.store(0, std::memory_order_relaxed);
                g_forceLoadInProgress.store(true, std::memory_order_release);
                {
                    VehGuard veh(1, ForceLoadVEH);

                    logger::info("  Calling AE 13753 pass {} ...", pass);
                    loadForms(tdh, false);
                }
                g_forceLoadInProgress.store(false, std::memory_order_release);

                auto addFormDelta2 = g_addFormCalls.load(std::memory_order_relaxed) - addFormBefore2;
                auto vehSkips2 = g_vehSkipCount.load(std::memory_order_relaxed);

                // Count still-skipped files after this pass
                std::size_t stillSkipped = 0;
                for (auto& file : tdh->files) {
                    if (!file) continue;
                    if (file->compileIndex == 0xFF && file->smallFileCompileIndex == 0)
                        ++stillSkipped;
                }

                logger::info("  Pass {} result: {} new forms, {} VEH skips, {} → {} still skipped",
                    pass, addFormDelta2, vehSkips2, skippedCount, stillSkipped);

                if (stillSkipped >= skippedCount) {
                    logger::info("  No progress — stopping multi-pass loading");
                    break;
                }
            }

            // Final skipped files count
            {
                std::size_t finalSkipped = 0;
                for (auto& file : tdh->files) {
                    if (!file) continue;
                    if (file->compileIndex == 0xFF && file->smallFileCompileIndex == 0)
                        ++finalSkipped;
                }
                logger::info("  Final: {} files still skipped after all passes", finalSkipped);
            }

            // ==========================================================
            // v1.22.27: Form Reference Resolution (InitItemImpl)
            //
            // After loading forms, call InitItemImpl() on every form to
            // resolve stored form IDs into live pointers.
            //
            // v1.22.36: Multi-pass InitItem — retry faulted forms after
            // the first pass, since more references may be resolvable.
            // ==========================================================
            if (addFormDelta > 100) {
                logger::info("=== InitItemImpl Phase (v1.22.36 — multi-pass) ==="sv);

                auto* saveLoadGame = RE::BGSSaveLoadGame::GetSingleton();
                if (saveLoadGame) {
                    saveLoadGame->globalFlags.set(
                        RE::BGSSaveLoadGame::GlobalFlags::kInitingForms);
                    logger::info("  Set kInitingForms flag");
                }

                auto initStart = std::chrono::high_resolution_clock::now();

                // Collect image bounds for vtable validation
                auto imgBase = REL::Module::get().base();
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imgBase);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(imgBase + dos->e_lfanew);
                auto imgEnd = imgBase + nt->OptionalHeader.SizeOfImage;

                // Scan the FULL global form hash table
                const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                std::uint64_t initCount = 0;
                std::uint64_t skipCount = 0;
                std::vector<RE::TESForm*> faultedForms; // Track faulted forms for retry

                if (formMap) {
                    auto* mapRaw = reinterpret_cast<const std::uint8_t*>(formMap);
                    auto capacity = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x0C);
                    auto sentinel = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x18);
                    auto entriesPtr = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x28);

                    logger::info("  Hash table: capacity={}, sentinel=0x{:X}, entries=0x{:X}",
                        capacity, sentinel, entriesPtr);

                    if (capacity > 0 && capacity <= 0x1000000 && entriesPtr >= 0x10000) {
                        auto* entryBase = reinterpret_cast<const std::uint8_t*>(entriesPtr);

                        for (std::uint32_t i = 0; i < capacity; ++i) {
                            auto* entry = entryBase + i * 24;

                            if (i % 4096 == 0) {
                                if (IsBadReadPtr(entry, static_cast<UINT_PTR>(
                                    std::min(std::size_t((capacity - i) * 24), std::size_t(4096 * 24)))))
                                    break;
                            }

                            auto formPtr = *reinterpret_cast<const std::uintptr_t*>(entry + 8);
                            if (formPtr == 0 || formPtr == sentinel ||
                                formPtr < 0x10000 || formPtr > 0x7FFFFFFFFFFF)
                            {
                                ++skipCount;
                                continue;
                            }

                            auto* formObj = reinterpret_cast<const std::uint8_t*>(formPtr);
                            if (IsBadReadPtr(formObj, 0x20)) {
                                ++skipCount;
                                continue;
                            }
                            auto vtable = *reinterpret_cast<const std::uintptr_t*>(formObj);
                            if (vtable < imgBase || vtable >= imgEnd) {
                                ++skipCount;
                                continue;
                            }

                            auto* form = reinterpret_cast<RE::TESForm*>(
                                const_cast<std::uint8_t*>(formObj));
                            if (SafeInitItem(form)) {
                                ++initCount;
                            } else {
                                ++skipCount;
                                faultedForms.push_back(form);
                                auto faulted = g_initFaultCount.fetch_add(1, std::memory_order_relaxed);
                                if (faulted < 20) {
                                    logger::warn("  InitItem faulted on form 0x{:08X} (type={}, vtable=0x{:X})",
                                        form->GetFormID(),
                                        std::to_underlying(form->GetFormType()),
                                        vtable);
                                } else if (faulted == 20) {
                                    logger::warn("  InitItem: suppressing further fault messages (20 logged)");
                                }
                            }
                        }
                    } else {
                        logger::error("  Hash table layout invalid — skipping InitItemImpl");
                    }
                } else {
                    logger::error("  TESForm::GetAllForms() returned null — skipping InitItemImpl");
                }

                auto pass1Elapsed = std::chrono::high_resolution_clock::now() - initStart;
                auto pass1Ms = std::chrono::duration_cast<std::chrono::milliseconds>(pass1Elapsed).count();
                logger::info("  InitItem pass 1: {} initialized, {} faulted, {}ms",
                    initCount, faultedForms.size(), pass1Ms);

                // v1.22.36: Retry faulted forms (up to 3 retry passes).
                // After pass 1 resolves many references, previously-faulting
                // forms may now succeed because their dependencies are resolved.
                for (int retryPass = 2; retryPass <= 4 && !faultedForms.empty(); ++retryPass) {
                    std::vector<RE::TESForm*> stillFaulted;
                    std::uint64_t retryOk = 0;

                    for (auto* form : faultedForms) {
                        if (SafeInitItem(form)) {
                            ++retryOk;
                        } else {
                            stillFaulted.push_back(form);
                        }
                    }

                    logger::info("  InitItem pass {}: {} succeeded, {} still faulted",
                        retryPass, retryOk, stillFaulted.size());

                    if (retryOk == 0) {
                        logger::info("  No progress — stopping InitItem retry");
                        break;
                    }
                    faultedForms = std::move(stillFaulted);
                }

                auto totalElapsed = std::chrono::high_resolution_clock::now() - initStart;
                auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(totalElapsed).count();

                if (saveLoadGame) {
                    saveLoadGame->globalFlags.reset(
                        RE::BGSSaveLoadGame::GlobalFlags::kInitingForms);
                    logger::info("  Cleared kInitingForms flag");
                }

                auto faultCount = g_initFaultCount.load(std::memory_order_relaxed);
                logger::info("  InitItemImpl total: {} initialized, {} permanently faulted, {}ms",
                    initCount, faultedForms.size(), totalMs);

                // ==========================================================
                // v1.22.36: Mark permanently faulted forms as kDeleted.
                //
                // The 179 faulted ActorCharacter forms have null internal
                // pointers (race, base NPC, etc) that cause null-dereference
                // cascades at MULTIPLE engine sites during New Game (AI
                // package evaluation, etc). Patching individual crash sites
                // is whack-a-mole — fixing one just moves the crash.
                //
                // Setting kDeleted (formFlags bit 0x20) tells the engine to
                // skip these forms in ALL processing: AI, rendering, physics,
                // cell attachment, save serialization, etc. The characters
                // simply won't spawn, which is far better than crashing.
                //
                // We also set kDisabled (0x800) as belt-and-suspenders — some
                // engine paths check kDisabled but not kDeleted.
                // ==========================================================
                if (!faultedForms.empty()) {
                    constexpr std::uint32_t kDeleted  = 0x20;
                    constexpr std::uint32_t kDisabled = 0x800;
                    std::size_t flaggedCount = 0;

                    for (auto* form : faultedForms) {
                        if (!form) continue;
                        auto oldFlags = form->formFlags;
                        form->formFlags |= kDeleted | kDisabled;
                        ++flaggedCount;

                        if (flaggedCount <= 10) {
                            logger::warn("  kDeleted+kDisabled: form 0x{:08X} type={} flags 0x{:08X} → 0x{:08X}",
                                form->GetFormID(),
                                std::to_underlying(form->GetFormType()),
                                oldFlags,
                                form->formFlags);
                        }
                    }

                    logger::info("  Flagged {} permanently faulted forms as kDeleted+kDisabled", flaggedCount);
                }

                // v1.22.36: Allocate zero page for null-form substitution.
                // ── Sentinel form infrastructure ──
                // 1. Stub function page (PAGE_EXECUTE_READ): "xor eax,eax; ret"
                g_stubFuncPage = VirtualAlloc(nullptr, 0x1000,
                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (g_stubFuncPage) {
                    auto* code = reinterpret_cast<std::uint8_t*>(g_stubFuncPage);
                    // Return 0: stub function for sentinel form vtable calls.
                    // Returning 1 caused secondary faults (test byte [1+0x40]).
                    // Returning 0 makes callers take "not found" branches cleanly.
                    code[0] = 0x31; code[1] = 0xC0; // xor eax, eax
                    code[2] = 0xC3;                  // ret
                    DWORD oldProt = 0;
                    VirtualProtect(g_stubFuncPage, 0x1000, PAGE_EXECUTE_READ, &oldProt);
                    logger::info("  Stub function at 0x{:X}",
                        reinterpret_cast<std::uintptr_t>(g_stubFuncPage));
                }

                // 2. Stub vtable (PAGE_READONLY): 512 entries → stub function
                g_stubVtable = VirtualAlloc(nullptr, 0x1000,
                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (g_stubVtable && g_stubFuncPage) {
                    auto* vt = reinterpret_cast<DWORD64*>(g_stubVtable);
                    auto stubAddr = reinterpret_cast<DWORD64>(g_stubFuncPage);
                    for (int i = 0; i < 512; ++i)
                        vt[i] = stubAddr;
                    DWORD oldProt = 0;
                    VirtualProtect(g_stubVtable, 0x1000, PAGE_READONLY, &oldProt);
                    logger::info("  Stub vtable at 0x{:X} (512 entries)",
                        reinterpret_cast<std::uintptr_t>(g_stubVtable));
                }

                // 3. Sentinel form page (PAGE_READONLY, 64KB):
                // 2b. Low null guard — try to map readable memory at address 0.
                // On Wine, VirtualAlloc at very low addresses may succeed because
                // Wine's memory management is more permissive than real Windows.
                // If this works, [null+offset] accesses read zeros directly
                // without triggering any VEH faults — zero overhead.
                {
                    // Try addresses from lowest to highest until one works.
                    // NtAllocateVirtualMemory with explicit address may bypass
                    // Wine's allocation granularity enforcement.
                    using NtAVM_t = LONG(WINAPI*)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
                    auto NtAVM = reinterpret_cast<NtAVM_t>(
                        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAllocateVirtualMemory"));

                    bool mapped = false;
                    if (NtAVM) {
                        // Try to allocate 64KB starting at page 0 (address 0x0)
                        void* baseAddr = reinterpret_cast<void*>(static_cast<ULONG_PTR>(1));
                        SIZE_T regionSize = 0x10000; // 64KB — covers all known offsets (max 0x7D40)
                        LONG status = NtAVM(GetCurrentProcess(), &baseAddr, 0, &regionSize,
                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                        if (status >= 0 && baseAddr != nullptr &&
                            reinterpret_cast<ULONG_PTR>(baseAddr) < 0x1000) {
                            g_lowNullGuard = baseAddr;
                            g_lowNullGuardEnd = reinterpret_cast<DWORD64>(baseAddr) + regionSize;
                            memset(baseAddr, 0, regionSize);

                            // Write sentinel data so null-form reads get sane values:
                            auto* page = reinterpret_cast<std::uint8_t*>(baseAddr);
                            if (g_stubVtable) {
                                auto vtAddr = reinterpret_cast<DWORD64>(g_stubVtable);
                                memcpy(page + 0x00, &vtAddr, 8); // vtable at offset 0
                            }
                            auto flags = static_cast<std::uint32_t>(0x20); // kDeleted
                            memcpy(page + 0x10, &flags, 4);
                            if (g_stubFuncPage) {
                                auto stubAddr = reinterpret_cast<DWORD64>(g_stubFuncPage);
                                memcpy(page + 0x4B8, &stubAddr, 8);
                            }

                            // Make it PAGE_READONLY to prevent corruption
                            DWORD oldP = 0;
                            VirtualProtect(baseAddr, regionSize, PAGE_READONLY, &oldP);

                            mapped = true;
                            logger::info("  LOW NULL GUARD: mapped {} bytes at 0x{:X} (PAGE_READONLY) — "
                                "null-form dereferences will read zeros with ZERO VEH overhead!",
                                regionSize, reinterpret_cast<std::uintptr_t>(baseAddr));
                        } else {
                            logger::info("  Low null guard: NtAllocateVirtualMemory at addr 1 returned "
                                "status=0x{:X}, baseAddr=0x{:X} — trying VirtualAlloc fallbacks",
                                (unsigned)status, reinterpret_cast<std::uintptr_t>(baseAddr));
                        }
                    }

                    if (!mapped) {
                        // Fallback: try VirtualAlloc at increasing addresses
                        for (ULONG_PTR tryAddr : {(ULONG_PTR)0x1000, (ULONG_PTR)0x10000}) {
                            auto* p = VirtualAlloc(reinterpret_cast<void*>(tryAddr), 0x10000,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                            if (p && reinterpret_cast<ULONG_PTR>(p) <= 0x10000) {
                                g_lowNullGuard = p;
                                g_lowNullGuardEnd = reinterpret_cast<DWORD64>(p) + 0x10000;
                                memset(p, 0, 0x10000);
                                auto* page = reinterpret_cast<std::uint8_t*>(p);
                                if (g_stubVtable) {
                                    auto vtAddr = reinterpret_cast<DWORD64>(g_stubVtable);
                                    memcpy(page + 0x00, &vtAddr, 8);
                                }
                                auto flags2 = static_cast<std::uint32_t>(0x20);
                                memcpy(page + 0x10, &flags2, 4);
                                if (g_stubFuncPage) {
                                    auto stubAddr = reinterpret_cast<DWORD64>(g_stubFuncPage);
                                    memcpy(page + 0x4B8, &stubAddr, 8);
                                }
                                DWORD oldP = 0;
                                VirtualProtect(p, 0x10000, PAGE_READONLY, &oldP);
                                mapped = true;
                                logger::info("  LOW NULL GUARD: VirtualAlloc at 0x{:X} succeeded! "
                                    "Null-form floods eliminated.", reinterpret_cast<std::uintptr_t>(p));
                                break;
                            }
                        }
                    }

                    if (!mapped) {
                        logger::info("  Low null guard: could not map low memory — "
                            "will rely on VEH + code cave patches for null-form handling");
                    }
                }

                //    Fake TESForm with kDeleted flag, valid vtable, stub pointers.
                //    Engine reads kDeleted and skips; virtual calls → stub → ret 0.
                g_zeroPage = VirtualAlloc(nullptr, 0x10000,
                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (g_zeroPage) {
                    memset(g_zeroPage, 0, 0x10000);
                    auto* page = reinterpret_cast<std::uint8_t*>(g_zeroPage);

                    // Vtable pointer at offset 0x0 → stub vtable
                    if (g_stubVtable) {
                        auto vtAddr = reinterpret_cast<DWORD64>(g_stubVtable);
                        memcpy(page + 0x00, &vtAddr, 8);
                    }

                    // formFlags at offset 0x10 = kDeleted (0x20)
                    auto flags = static_cast<std::uint32_t>(0x20);
                    memcpy(page + 0x10, &flags, 4);

                    // Direct function pointer fields that the engine dereferences:
                    // offset 0x4B8 (seen at +0x2ED880: call [rax+0x4B8])
                    if (g_stubFuncPage) {
                        auto stubAddr = reinterpret_cast<DWORD64>(g_stubFuncPage);
                        memcpy(page + 0x4B8, &stubAddr, 8);
                    }

                    // v1.22.58: PAGE_READWRITE — let the engine write to the
                    // sentinel freely. The write-skip VEH flood (~33K/sec under
                    // PAGE_READONLY at +0x2D67C8) consumed 100% CPU on New Game.
                    // Vtable/formFlags corruption is now handled by watchdog repair
                    // (every 10s) + CASE 0/EXT-CALL-RET for interim vtable crashes.
                    // This eliminates ALL write-related VEH overhead.

                    g_zeroPageBase = reinterpret_cast<DWORD64>(g_zeroPage);
                    g_zeroPageEnd = g_zeroPageBase + 0x10000;
                    logger::info("  Sentinel form page at 0x{:X} (PAGE_READWRITE, kDeleted=0x20, vtable=0x{:X})",
                        g_zeroPageBase,
                        g_stubVtable ? reinterpret_cast<std::uintptr_t>(g_stubVtable) : 0);
                } else {
                    logger::error("  Failed to allocate sentinel form page!");
                }

                // Install persistent form-reference fixup VEH
                g_formFixupCount.store(0, std::memory_order_relaxed);
                g_formFixupActive.store(true, std::memory_order_release);
                g_vehFormFixup = AddVectoredExceptionHandler(1, FormReferenceFixupVEH);
                logger::info("  Installed persistent FormReferenceFixupVEH");

                // v1.22.36: Binary patch at 29846+0x95 with form resolution.
                PatchFormPointerValidation();

                // v1.22.36: Binary patches at hot VEH addresses with null checks.
                PatchHotSpotNullChecks();

                // v1.22.36: Install first-chance crash logger VEH.
                // This logs crash info to Data/SKSE/Plugins/ for guaranteed
                // capture even when spdlog hasn't flushed.
                g_vehCrashLogger = AddVectoredExceptionHandler(1, CrashLoggerVEH);
                g_crashLoggerActive.store(true, std::memory_order_release);
                logger::info("  Installed CrashLoggerVEH (first-chance, writes to SKSE log dir)");

                // v1.22.77: Cache our DLL module range for fault recovery.
                // If game jumps into our DLL via corrupted sentinel vtable,
                // we can recover by popping the return address.
                {
                    HMODULE hSelf = nullptr;
                    if (GetModuleHandleExA(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&CrashLoggerVEH), &hSelf)) {
                        g_dllBase = reinterpret_cast<DWORD64>(hSelf);
                        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hSelf);
                        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                            g_dllBase + dos->e_lfanew);
                        g_dllEnd = g_dllBase + nt->OptionalHeader.SizeOfImage;
                        logger::info("  DLL module range: 0x{:X} - 0x{:X}", g_dllBase, g_dllEnd);
                    }
                }

                // v1.22.72: Capture main thread handle via DuplicateHandle on
                // pseudo-handle. OpenThread works initially but Wine revokes
                // THREAD_SUSPEND_RESUME access after the first call (err=5).
                // DuplicateHandle on GetCurrentThread() inherits full access.
                g_mainThreadId = GetCurrentThreadId();
                if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                                    GetCurrentProcess(), &g_mainThreadHandle,
                                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                    FALSE, 0)) {
                    logger::info("  Captured main thread handle via DuplicateHandle (tid={})", g_mainThreadId);
                } else {
                    // Fallback to OpenThread
                    g_mainThreadHandle = OpenThread(
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                        FALSE, g_mainThreadId);
                    if (g_mainThreadHandle) {
                        logger::info("  Captured main thread handle via OpenThread (tid={})", g_mainThreadId);
                    } else {
                        logger::warn("  Failed to capture main thread handle (err={})", GetLastError());
                    }
                }

                // v1.22.36: Watchdog thread — logs VEH counters every 10 seconds.
                // v1.22.46: Also samples main thread RIP when counters are stable
                // (freeze detection — helps pinpoint infinite loops).
                std::thread([]() {
                    int tick = 0;
                    std::uint64_t prevZp = 0, prevWs = 0, prevCa = 0;
                    int prevFi = 0;
                    int stableTicks = 0;     // how many ticks counters unchanged
                    auto imgBase = REL::Module::get().base();

                    while (true) {
                        Sleep(10000);
                        tick++;

                        auto curZp = g_zeroPageUseCount.load(std::memory_order_relaxed);
                        auto curWs = g_zeroPageWriteSkips.load(std::memory_order_relaxed);
                        auto curFi = g_formIdSkipCount.load(std::memory_order_relaxed);
                        auto curCa = g_catchAllCount.load(std::memory_order_relaxed);

                        // v1.22.76: Track stable ticks for freeze detection.
                        // Counters stable AND non-zero = game is frozen in a tight loop.
                        if (curZp == prevZp && curWs == prevWs &&
                            curFi == prevFi && curCa == prevCa &&
                            (curZp > 0 || curFi > 0)) {
                            stableTicks++;
                        } else {
                            stableTicks = 0;
                        }

                        prevZp = curZp; prevWs = curWs;
                        prevFi = curFi; prevCa = curCa;

                        // v1.22.76: Install INT3 probes after 3 stable ticks (30s freeze).
                        // Writes 0xCC at candidate loop entry points. VEH catches the
                        // breakpoint and logs which loop the main thread is stuck in.
                        if (stableTicks >= 3 && !g_probesInstalled.load(std::memory_order_acquire)) {
                            int installed = 0;
                            for (int i = 0; i < g_numProbes; ++i) {
                                auto* site = reinterpret_cast<std::uint8_t*>(imgBase + g_probes[i].offset);
                                DWORD oldProt;
                                if (VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
                                    g_probes[i].origByte = *site;
                                    *site = 0xCC;  // INT3
                                    FlushInstructionCache(GetCurrentProcess(), site, 1);
                                    g_probes[i].active.store(true, std::memory_order_release);
                                    installed++;
                                    // Do NOT restore protection (Wine page reversion)
                                }
                            }
                            g_probesInstalled.store(true, std::memory_order_release);
                            FILE* pf = nullptr;
                            fopen_s(&pf, g_crashLogPath, "a");
                            if (pf) {
                                fprintf(pf, "PROBES: installed %d/%d INT3 probes at tick %d (stable=%d)\n",
                                    installed, g_numProbes, tick, stableTicks);
                                fflush(pf);
                                fclose(pf);
                            }
                        }

                        // v1.22.64: Aggressively zero sentinel page to break garbage
                        // pointer chains. Engine writes garbage to sentinel (it's RW),
                        // and code that reads a "next pointer" from sentinel gets a
                        // random value like 0x7D00000000 → second-hop fault → CATCH-ALL.
                        // By zeroing the entire page and re-writing only critical fields,
                        // any "next pointer" read returns 0, which existing code cave
                        // null checks catch without VEH involvement.
                        if (g_zeroPage) {
                            auto* page = reinterpret_cast<std::uint8_t*>(g_zeroPage);
                            // Zero entire first 0x500 bytes (covers all form field offsets)
                            memset(page, 0, 0x500);
                            // Re-write critical fields
                            if (g_stubVtable) {
                                auto vtAddr = reinterpret_cast<DWORD64>(g_stubVtable);
                                memcpy(page + 0x00, &vtAddr, 8);
                            }
                            auto flags = static_cast<std::uint32_t>(0x20); // kDeleted
                            memcpy(page + 0x10, &flags, 4);
                            if (g_stubFuncPage) {
                                auto stubAddr = reinterpret_cast<DWORD64>(g_stubFuncPage);
                                memcpy(page + 0x4B8, &stubAddr, 8);
                            }
                        }

                        // v1.22.62: Verify code cave patches still have JMP opcode.
                        // Wine may revert file-backed code pages; re-apply if needed.
                        VerifyAndRepairCavePatches();

                        FILE* f = nullptr;
                        fopen_s(&f, g_crashLogPath, "a");
                        if (f) {
                            fprintf(f, "WATCHDOG #%d (t=%ds): zp=%llu ws=%llu nc=%d fi=%d ca=%llu cf=%llu er=%llu",
                                tick, tick * 10,
                                curZp, curWs,
                                g_nullSkipCount.load(std::memory_order_relaxed),
                                curFi, curCa,
                                (unsigned long long)g_caveFaultCount.load(std::memory_order_relaxed),
                                (unsigned long long)g_execRecoverCount.load(std::memory_order_relaxed));

                            // v1.22.72: Sample main thread RIP on every tick.
                            // Try SuspendThread first; if Wine blocks it (err=5),
                            // fall back to GetThreadContext without suspend (racy
                            // but still useful for detecting tight loops/deadlocks).
                            if (g_mainThreadHandle) {
                                bool suspended = false;
                                DWORD suspCount = SuspendThread(g_mainThreadHandle);
                                if (suspCount != (DWORD)-1) {
                                    suspended = true;
                                }

                                CONTEXT ctx = {};
                                ctx.ContextFlags = CONTEXT_CONTROL;
                                if (GetThreadContext(g_mainThreadHandle, &ctx)) {
                                    DWORD64 rip = ctx.Rip;
                                    DWORD64 rsp = ctx.Rsp;
                                    if (rip >= (DWORD64)imgBase && rip < (DWORD64)imgBase + 0x4000000) {
                                        fprintf(f, " RIP=+0x%llX RSP=0x%llX",
                                            (unsigned long long)(rip - imgBase),
                                            (unsigned long long)rsp);
                                    } else {
                                        fprintf(f, " RIP=0x%llX RSP=0x%llX",
                                            (unsigned long long)rip,
                                            (unsigned long long)rsp);
                                    }
                                    if (!suspended) fprintf(f, "(unsuspended)");
                                    auto* rspPtr = reinterpret_cast<DWORD64*>(rsp);
                                    if (!IsBadReadPtr(rspPtr, 64)) {
                                        fprintf(f, " STK=[");
                                        for (int i = 0; i < 8; ++i) {
                                            DWORD64 val = rspPtr[i];
                                            if (val >= (DWORD64)imgBase &&
                                                val < (DWORD64)imgBase + 0x4000000) {
                                                fprintf(f, "+0x%llX ",
                                                    (unsigned long long)(val - imgBase));
                                            }
                                        }
                                        fprintf(f, "]");
                                    }
                                } else {
                                    fprintf(f, " RIP=FAIL(susp=%d,err=%lu)",
                                        suspended ? 1 : 0, GetLastError());
                                }

                                if (suspended) ResumeThread(g_mainThreadHandle);
                            } else {
                                fprintf(f, " RIP=NO_HANDLE");
                            }

                            fprintf(f, "\n");
                            fflush(f);
                            fclose(f);
                        }
                    }
                }).detach();
                logger::info("  Started watchdog thread (10s interval, RIP sampling enabled)");

                // v1.22.76: Auto-New-Game REMOVED — the resetGame/fullReset path
                // triggers ClearData which is a DIFFERENT code path from manual
                // UI New Game and causes different freezes. Testing must be manual.

                logger::info("=== END InitItemImpl Phase (v1.22.36) ==="sv);
            }

            // v1.22.52/58: Ensure sentinel page integrity.
            // Page is PAGE_READWRITE since v1.22.58 — just refresh critical fields.
            if (g_zeroPage) {
                // Page is already PAGE_READWRITE since v1.22.58

                auto* zpBytes = reinterpret_cast<std::uint8_t*>(g_zeroPage);
                auto stubVtable = reinterpret_cast<DWORD64>(g_stubVtable);
                std::memcpy(&zpBytes[0x0000], &stubVtable, 8); // vtable
                auto flags = static_cast<std::uint32_t>(0x20);
                std::memcpy(&zpBytes[0x0010], &flags, 4); // kDeleted
                if (g_stubFuncPage) {
                    auto stubAddr = reinterpret_cast<DWORD64>(g_stubFuncPage);
                    std::memcpy(&zpBytes[0x04B8], &stubAddr, 8);
                }

                // v1.22.58: Keep PAGE_READWRITE — no restore to PAGE_READONLY.
                // Watchdog repairs critical fields every 10s tick.
                g_zpWritable.store(false, std::memory_order_relaxed);
                logger::info("  Sentinel page verified (vtable+flags+stubs refreshed, PAGE_READWRITE)");
            }

            logger::info("=== END ForceLoadAllForms (v1.22.36) ==="sv);

            // v1.22.61: Dump unpacked .text section for offline RE.
            // SkyrimSE.exe uses SteamStub DRM — the .text section is
            // encrypted on disk but unpacked in memory at runtime.
            // Write the live .text bytes to a file for capstone analysis.
            {
                auto imgBase = REL::Module::get().base();
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imgBase);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(imgBase + dos->e_lfanew);
                const auto* sec = IMAGE_FIRST_SECTION(nt);

                for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
                    if (std::memcmp(sec[i].Name, ".text", 5) == 0 &&
                        sec[i].VirtualAddress == 0x1000) {
                        auto textVA = imgBase + sec[i].VirtualAddress;
                        auto textSize = sec[i].Misc.VirtualSize;

                        // Write to same directory as crash log
                        std::string dumpPath(g_crashLogPath);
                        auto lastSlash = dumpPath.find_last_of("\\/");
                        if (lastSlash != std::string::npos)
                            dumpPath = dumpPath.substr(0, lastSlash + 1);
                        dumpPath += "SkyrimSE_text_dump.bin";

                        FILE* df = nullptr;
                        fopen_s(&df, dumpPath.c_str(), "wb");
                        if (df) {
                            fwrite(reinterpret_cast<const void*>(textVA), 1, textSize, df);
                            fclose(df);
                            logger::info("  .text dump: {} bytes → {}", textSize, dumpPath);
                        } else {
                            logger::warn("  .text dump: failed to open {}", dumpPath);
                        }
                        break;
                    }
                }
            }
#else
            logger::info("ForceLoadAllForms: SE build — skipping (AE-only fix)"sv);
#endif
        }
    }

    inline void Install()
    {
        detail::InitPaths();
        detail::ReplaceFormMapFunctions();

        logger::info("installed form caching patch"sv);
    }
}
