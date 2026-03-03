#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <shared_mutex>
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

        struct ShardedCache
        {
            mutable std::shared_mutex mutex;
            std::unordered_map<std::uint32_t, RE::TESForm*> map;
        };

        inline ShardedCache g_formCache[256];
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

            g_hk_ClearData.call(a_self);

            // Dump state after ClearData too
            DumpFilesListState(a_self, "POST-ClearData");
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
        // v1.22.3: Hook TESFile::CloseTES — detect if files are closed
        // after catalog scan (would indicate catalog is a separate pass)
        // ================================================================
        inline SafetyHookInline g_hk_CloseTES;

        inline void TESFile_CloseTES(RE::TESFile* a_self, bool a_force)
        {
            auto count = g_closeTESCalls.fetch_add(1, std::memory_order_relaxed);
            if (count < 5 || (count > 0 && count % 1000 == 0)) {
                logger::info("  CloseTES #{}: file='{}' force={}",
                    count + 1, a_self ? a_self->fileName : "null", a_force);
            }
            g_hk_CloseTES.call(a_self, a_force);
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

        // Forward declarations for v1.22.11 compile caller scanner (diagnostic only)
        inline void ScanAndHookCompileCaller(std::uintptr_t addCompileIndexAddr);

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

            // v1.22.9: CompileFiles hooks REMOVED — caused crash (wrong RELOCATION_IDs)
            // SE→AE ID mapping is not a fixed offset; guessed AE IDs were wrong.
            // Instead, enhanced ClearData + InitFormData hooks dump full TDH state.

            // Log all hook addresses for verification
            logger::info("form caching: hook summary — GetForm=0x{:X} AddForm=0x{:X} OpenTES=0x{:X} AddCompileIdx=0x{:X} SeekNextForm=0x{:X} CloseTES=0x{:X}"sv,
                getForm.address(), AddForm.address(), OpenTES.address(),
                AddCompileIndex.address(), SeekNextForm.address(), CloseTES.address());

            // ================================================================
            // v1.22.11: Runtime code scanner — find callers of AddCompileIndex
            // The binary is encrypted on disk (Steam DRM), so we scan the
            // decrypted .text section in memory at runtime to find which
            // function calls AddCompileIndex. This identifies "CompileFiles"
            // (the engine function that assigns compile indices to all files).
            // We then hook that function to detect + fix the 600-file skip.
            // ================================================================
            ScanAndHookCompileCaller(AddCompileIndex.address());
        }

        // ================================================================
        // v1.22.11: Runtime code scanner + dynamic hook installer
        //
        // Problem: With 600+ compiled files under Wine, the engine skips
        // the entire compilation step (loadingFiles never becomes true,
        // AddCompileIndex is never called, no forms are loaded).
        //
        // Approach:
        // 1. Scan decrypted .text for CALL instructions targeting AddCompileIndex
        // 2. Find the containing function (= "CompileFiles") via Address Library
        // 3. Hook it to detect when compilation is skipped
        // 4. If skipped, manually assign compile indices and set loadingFiles
        // ================================================================
        inline SafetyHookInline g_hk_compileCaller{};
        inline std::uintptr_t g_compileCallerAddr = 0;
        inline std::uint64_t g_compileCallerAeId = 0;
        inline std::atomic<bool> g_manualCompileDone{ false };

        // Manual compilation: assign compile indices to all active files
        // and populate compiledFileCollection. This replicates what the
        // engine's CompileFiles does when it doesn't skip.
        inline void ManuallyCompileFiles()
        {
            auto* tdh = RE::TESDataHandler::GetSingleton();
            if (!tdh) {
                logger::error("ManuallyCompileFiles: TDH is null!"sv);
                return;
            }

            std::uint8_t nextRegIdx = 0;
            std::uint16_t nextEslIdx = 0;
            std::size_t regCount = 0;
            std::size_t eslCount = 0;
            std::size_t skippedInactive = 0;

            for (auto& file : tdh->files) {
                if (!file) continue;

                // Only compile active files (those enabled in plugins.txt / load order)
                // The engine would also skip files that aren't active
                // Note: Skyrim.esm and other always-on files have kActive set
                bool isActive = file->recordFlags.all(RE::TESFile::RecordFlag::kActive) ||
                                file->recordFlags.all(RE::TESFile::RecordFlag::kMaster);
                if (!isActive) {
                    ++skippedInactive;
                    continue;
                }

                if (file->IsLight()) {
                    // ESL/light plugin → compileIndex = 0xFE, smallFileCompileIndex = sequential
                    if (nextEslIdx <= 0xFFF) {
                        file->compileIndex = 0xFE;
                        file->smallFileCompileIndex = nextEslIdx++;
                        tdh->compiledFileCollection.smallFiles.push_back(file);
                        ++eslCount;
                    } else {
                        logger::warn("ManualCompile: ESL limit exceeded at '{}'", file->fileName);
                    }
                } else {
                    // Regular plugin → compileIndex = sequential (0-0xFD)
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

            logger::info("ManuallyCompileFiles: assigned {} reg + {} ESL = {} total (skipped {} inactive)",
                regCount, eslCount, regCount + eslCount, skippedInactive);
            logger::info("ManuallyCompileFiles: compiledFileCollection.files={} .smallFiles={}",
                tdh->compiledFileCollection.files.size(),
                tdh->compiledFileCollection.smallFiles.size());
            logger::info("ManuallyCompileFiles: loadingFiles set to TRUE"sv);
        }

        // Hook function for the compile caller (signature-agnostic using void* args)
        // x64 Windows: first 4 args in RCX, RDX, R8, R9; this pointer in RCX for member funcs
        // Returns void* to preserve whatever return value the original function has
        inline void* HookedCompileCaller(void* a_this, void* a2, void* a3, void* a4)
        {
            auto beforeCount = g_addCompileIndexCalls.load(std::memory_order_relaxed);
            logger::info(">>> CompileCaller ENTERED (AE {}, addr 0x{:X}), AddCompileIndex count before: {}",
                g_compileCallerAeId, g_compileCallerAddr, beforeCount);

            // Call the original function and preserve its return value
            auto result = g_hk_compileCaller.call<void*>(a_this, a2, a3, a4);

            auto afterCount = g_addCompileIndexCalls.load(std::memory_order_relaxed);
            logger::info(">>> CompileCaller RETURNED, AddCompileIndex count after: {} (delta: {})",
                afterCount, afterCount - beforeCount);

            // Check if compilation was skipped (no AddCompileIndex calls were made)
            if (afterCount == beforeCount) {
                // Check if there are active files that should have been compiled
                auto* tdh = RE::TESDataHandler::GetSingleton();
                if (tdh) {
                    std::size_t activeFiles = 0;
                    for (auto& file : tdh->files) {
                        if (file && (file->recordFlags.all(RE::TESFile::RecordFlag::kActive) ||
                                     file->recordFlags.all(RE::TESFile::RecordFlag::kMaster))) {
                            ++activeFiles;
                        }
                    }

                    bool collectionEmpty = tdh->compiledFileCollection.files.empty() &&
                                          tdh->compiledFileCollection.smallFiles.empty();

                    logger::warn(">>> COMPILE SKIPPED! active files: {}, collection empty: {}, loadingFiles: {}",
                        activeFiles, collectionEmpty, tdh->loadingFiles);

                    if (activeFiles > 0 && collectionEmpty && !tdh->loadingFiles &&
                        !g_manualCompileDone.exchange(true)) {
                        logger::warn(">>> TRIGGERING MANUAL COMPILATION FALLBACK <<<"sv);
                        ManuallyCompileFiles();
                    }
                }
            }

            return result;
        }

        inline void ScanAndHookCompileCaller(std::uintptr_t addCompileIndexAddr)
        {
            // Get module info for .text section scanning
            auto moduleBase = REL::Module::get().base();

            logger::info("=== RUNTIME CODE SCANNER ==="sv);
            logger::info("  Module base: 0x{:X}", moduleBase);
            logger::info("  AddCompileIndex addr: 0x{:X} (offset 0x{:X})",
                addCompileIndexAddr, addCompileIndexAddr - moduleBase);

            // Parse PE headers from memory to find .text section
            auto dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
            auto ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase + dosHeader->e_lfanew);
            auto sectionHeader = IMAGE_FIRST_SECTION(ntHeaders);
            auto numSections = ntHeaders->FileHeader.NumberOfSections;

            std::uintptr_t textStart = 0;
            std::size_t textSize = 0;

            for (int i = 0; i < numSections; ++i) {
                char name[9] = {};
                std::memcpy(name, sectionHeader[i].Name, 8);
                if (std::strcmp(name, ".text") == 0 && textStart == 0) {
                    textStart = moduleBase + sectionHeader[i].VirtualAddress;
                    textSize = sectionHeader[i].Misc.VirtualSize;
                    logger::info("  .text section: VA 0x{:X}, size 0x{:X} ({:.1f} MB)",
                        textStart, textSize, textSize / (1024.0 * 1024.0));
                    break;
                }
            }

            if (textStart == 0 || textSize == 0) {
                logger::error("  Failed to find .text section in memory!"sv);
                return;
            }

            // Scan for E8 (CALL rel32) and E9 (JMP rel32) targeting AddCompileIndex
            struct CallerInfo {
                std::uintptr_t callAddr;  // Address of the CALL/JMP instruction
                std::uintptr_t callRva;   // RVA from module base
                bool isJmp;               // true if JMP (tail call), false if CALL
            };
            std::vector<CallerInfo> callers;

            auto* textBytes = reinterpret_cast<const std::uint8_t*>(textStart);
            for (std::size_t i = 0; i + 5 <= textSize; ++i) {
                if (textBytes[i] == 0xE8 || textBytes[i] == 0xE9) {  // CALL or JMP rel32
                    std::int32_t rel32;
                    std::memcpy(&rel32, &textBytes[i + 1], 4);
                    auto callAddr = textStart + i;
                    auto target = callAddr + 5 + rel32;
                    if (target == addCompileIndexAddr) {
                        callers.push_back({
                            callAddr,
                            callAddr - moduleBase,
                            textBytes[i] == 0xE9
                        });
                    }
                }
            }

            logger::info("  Found {} CALL/JMP sites targeting AddCompileIndex:", callers.size());
            for (auto& c : callers) {
                logger::info("    {} at 0x{:X} (RVA 0x{:X})",
                    c.isJmp ? "JMP" : "CALL", c.callAddr, c.callRva);
            }

            if (callers.empty()) {
                logger::warn("  No CALL sites found! AddCompileIndex may be called indirectly."sv);
                return;
            }

            // Use Address Library Offset2ID to find containing functions
            REL::IDDatabase::Offset2ID offset2id;
            logger::info("  Offset2ID database loaded ({} entries)", offset2id.size());

            // For each caller, find the containing function
            struct ContainingFunc {
                std::uint64_t aeId;
                std::uintptr_t funcOffset;
                std::uintptr_t funcAddr;
                std::size_t callSiteCount;  // how many CALL sites this function has
            };
            std::vector<ContainingFunc> containingFuncs;

            for (auto& c : callers) {
                // Binary search for the first function with offset > callRva
                // Then back up one to get the function containing our call site
                auto it = std::upper_bound(
                    offset2id.begin(), offset2id.end(), c.callRva,
                    [](std::uint64_t rva, const auto& mapping) {
                        return rva < mapping.offset;
                    });

                if (it != offset2id.begin()) {
                    --it;  // Go back to the function that starts at or before our address
                    auto funcOffset = it->offset;
                    auto funcId = it->id;
                    auto funcAddr = moduleBase + funcOffset;
                    auto distFromStart = c.callRva - funcOffset;

                    logger::info("    -> Containing function: AE {} at offset 0x{:X} (+0x{:X} into func)",
                        funcId, funcOffset, distFromStart);

                    // Track unique containing functions
                    bool found = false;
                    for (auto& f : containingFuncs) {
                        if (f.aeId == funcId) {
                            f.callSiteCount++;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        containingFuncs.push_back({ funcId, funcOffset, funcAddr, 1 });
                    }
                }
            }

            logger::info("  Unique containing functions: {}", containingFuncs.size());
            for (auto& f : containingFuncs) {
                logger::info("    AE {} at 0x{:X}: {} CALL site(s)",
                    f.aeId, f.funcOffset, f.callSiteCount);
            }

            // v1.22.11: Originally hooked the function with the most CALL sites.
            // v1.22.12: REMOVED hook — hooking AE 11596 crashed the game because
            // the function signature (4 args, void* return) didn't match the actual
            // function. Instead, we detect "compilation skipped" from the monitor
            // thread in editor_id_cache.h and trigger manual compilation there.
            if (!containingFuncs.empty()) {
                auto& primary = *std::max_element(containingFuncs.begin(), containingFuncs.end(),
                    [](const auto& a, const auto& b) { return a.callSiteCount < b.callSiteCount; });
                g_compileCallerAddr = primary.funcAddr;
                g_compileCallerAeId = primary.aeId;

                logger::info("  Primary candidate: AE {} at 0x{:X} ({} CALL sites) — NOT hooking (monitor-based fallback instead)",
                    primary.aeId, primary.funcAddr, primary.callSiteCount);
            }

            logger::info("=== END RUNTIME CODE SCANNER ==="sv);
        }
    }

    inline void Install()
    {
        detail::ReplaceFormMapFunctions();

        logger::info("installed form caching patch"sv);
    }
}
