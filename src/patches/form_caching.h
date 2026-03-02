#pragma once

#include <atomic>
#include <shared_mutex>
#include <unordered_map>

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
            logger::info(">>> ClearData called (call #{}) — wiping form caches"sv, count);

            for (auto& shard : g_formCache) {
                std::unique_lock lock(shard.mutex);
                shard.map.clear();
            }

            TreeLodReferenceCaching::detail::ClearCache();

            g_hk_ClearData.call(a_self);
        }

        inline SafetyHookInline g_hk_InitializeFormDataStructures;

        inline void TESForm_InitializeFormDataStructures()
        {
            auto count = g_initFormDataCalls.fetch_add(1, std::memory_order_relaxed) + 1;
            logger::info(">>> InitializeFormDataStructures called (call #{}) — resetting form map"sv, count);

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
            logger::info("form caching: OpenTES hook installed (counting file opens)"sv);
        }
    }

    inline void Install()
    {
        detail::ReplaceFormMapFunctions();

        logger::info("installed form caching patch"sv);
    }
}
