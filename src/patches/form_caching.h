#pragma once

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
            for (auto& shard : g_formCache) {
                std::unique_lock lock(shard.mutex);
                shard.map.clear();
            }

            TreeLodReferenceCaching::detail::ClearCache();

            g_hk_InitializeFormDataStructures.call();
        }

        inline void ReplaceFormMapFunctions()
        {
            const REL::Relocation getForm{ RELOCATION_ID(14461, 14617) };
            g_hk_GetFormByNumericId = safetyhook::create_inline(getForm.address(), TESForm_GetFormByNumericId);

            const REL::Relocation RemoveAt{ RELOCATION_ID(14514, 14710) };
            g_hk_RemoveAt = safetyhook::create_inline(RemoveAt.address(), FormMap_RemoveAt);

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

            const REL::Relocation ClearData{ RELOCATION_ID(13646, 13754) };
            g_hk_ClearData = safetyhook::create_inline(ClearData.address(), TESDataHandler_ClearData);

            const REL::Relocation InitializeFormDataStructures{ RELOCATION_ID(14511, 14669) };
            g_hk_InitializeFormDataStructures = safetyhook::create_inline(InitializeFormDataStructures.address(), TESForm_InitializeFormDataStructures);
        }
    }

    inline void Install()
    {
        detail::ReplaceFormMapFunctions();

        logger::info("installed form caching patch"sv);
    }
}
