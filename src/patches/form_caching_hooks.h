#pragma once

#include "form_caching_globals.h"
#include "form_caching_veh.h"

namespace Patches::FormCaching
{
    namespace detail
    {

        // v1.22.90: grow/rehash hooks — passthrough + sentinel capture.
        // BSTHashMap::grow is NOT thread-safe under Wine. When grow() steals
        // entries (sets next = nullptr), concurrent find() threads follow the
        // null chain pointer. The deallocate call inside grow is NOP'd (see
        // Install()) so old arrays stay allocated with next = nullptr.
        // The first grow call captures the BST sentinel pointer and writes it
        // into the low null guard page, so null chain follows read sentinel
        // and the find loop exits cleanly.
        inline SafetyHookInline g_hk_growA{};
        inline SafetyHookInline g_hk_growB{};

        // grow signature: __fastcall(this) — rcx = BSTHashMap inner struct
        // Layout: +0x04 = capacity (uint32), +0x10 = sentinel ptr, +0x20 = buckets pointer
        inline void __fastcall BSTHashMap_grow_A(void* a_this)
        {
            g_growCallCount.fetch_add(1, std::memory_order_relaxed);

            // Capture BST sentinel on first call (atomic CAS — first writer wins)
            if (!g_bstSentinel.load(std::memory_order_acquire)) {
                auto* sentinel = *reinterpret_cast<void**>(
                    reinterpret_cast<std::uint8_t*>(a_this) + 0x10);
                if (sentinel) {
                    void* expected = nullptr;
                    if (g_bstSentinel.compare_exchange_strong(expected, sentinel, std::memory_order_release, std::memory_order_relaxed)) {
                        logger::info("v1.22.90: Captured BST sentinel = 0x{:X}",
                            reinterpret_cast<std::uintptr_t>(sentinel));
                        PatchNullPageWithSentinel();
                    }
                }
            }

            // Passthrough — call original grow (deallocate is NOP'd)
            g_hk_growA.call<void>(a_this);
        }

        inline void __fastcall BSTHashMap_grow_B(void* a_this)
        {
            g_growCallCount.fetch_add(1, std::memory_order_relaxed);

            if (!g_bstSentinel.load(std::memory_order_acquire)) {
                auto* sentinel = *reinterpret_cast<void**>(
                    reinterpret_cast<std::uint8_t*>(a_this) + 0x10);
                if (sentinel) {
                    void* expected = nullptr;
                    if (g_bstSentinel.compare_exchange_strong(expected, sentinel, std::memory_order_release, std::memory_order_relaxed)) {
                        logger::info("v1.22.90: Captured BST sentinel = 0x{:X} (grow_B)",
                            reinterpret_cast<std::uintptr_t>(sentinel));
                        PatchNullPageWithSentinel();
                    }
                }
            }

            g_hk_growB.call<void>(a_this);
        }

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
                // Contended — spin with pause + backoff (Sleep(0) yields after 64 spins)
                g_setAtContentionCount.fetch_add(1, std::memory_order_relaxed);
                int spins = 0;
                do {
                    _mm_pause();
                    if (++spins > 64) { Sleep(0); spins = 0; }
                } while (g_hashMapSetAtLock.test_and_set(std::memory_order_acquire));
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
                int spins = 0;
                do {
                    _mm_pause();
                    if (++spins > 64) { Sleep(0); spins = 0; }
                } while (g_hashMapSetAtLock.test_and_set(std::memory_order_acquire));
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

            // v1.22.86: After kDataLoaded, the cache is authoritative.
            // All ESM forms were cached during loading (AddFormToDataHandler).
            // New forms from New Game come through SetAt hooks.
            // NEVER fall through to native BSTHashMap::find — its bucket chains
            // are corrupted by grow/rehash races under Wine (freed bucket arrays
            // get reused for string allocations → ASCII data read as pointers →
            // infinite loop in unpatched find sites → freeze).
            if (g_cacheAuthoritative.load(std::memory_order_acquire)) {
                return nullptr;
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

                        // v1.22.91: Capture BST sentinel from form map inner struct.
                        // inner = formMap + 8, sentinel is at inner + 0x10 = formMap + 0x18.
                        // This is the game's BSTScatterTableSentinel address.
                        if (!g_bstSentinel.load(std::memory_order_acquire)) {
                            auto* sentPtr = *reinterpret_cast<void**>(
                                reinterpret_cast<std::uint8_t*>(inner) + 0x10);
                            if (sentPtr) {
                                void* expected = nullptr;
                                if (g_bstSentinel.compare_exchange_strong(expected, sentPtr, std::memory_order_release, std::memory_order_relaxed)) {
                                    logger::info("v1.22.91: Captured BST sentinel = 0x{:X} from form map",
                                        reinterpret_cast<std::uintptr_t>(sentPtr));
                                    PatchNullPageWithSentinel();
                                }
                            }
                        }
                    }
                }
            }

            // During loading only: fall through to native GetFormByNumericId.
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

        // VEH handlers (x86_instr_len, ForceLoadVEH, FormReferenceFixupVEH,
        // CrashLoggerVEH, SafeInitItem, classifyInstruction) are defined in
        // form_caching_veh.h — do NOT duplicate them here.

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
        inline std::atomic<bool> g_pluginsTxtParsed{false};
        inline std::atomic<bool> g_pluginsTxtLoaded{false};

        inline void EnsurePluginsTxtLoaded()
        {
            if (g_pluginsTxtLoaded.load(std::memory_order_acquire)) return;
            g_pluginsTxtLoaded.store(true, std::memory_order_release);

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
                    g_pluginsTxtParsed.store(true, std::memory_order_release);
                }
            }
            CloseHandle(hFile);

            logger::info("ManuallyCompileFiles: plugins.txt parsed={}, {} enabled, {} disabled",
                g_pluginsTxtParsed.load(std::memory_order_relaxed), g_enabledNames.size(), g_disabledNames.size());
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
                if (g_pluginsTxtParsed.load(std::memory_order_acquire)) {
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

    } // namespace detail
} // namespace Patches::FormCaching
