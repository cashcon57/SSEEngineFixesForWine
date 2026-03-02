#pragma once

#include <algorithm>
#include <cctype>
#include <atomic>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "form_caching.h"

// Wine-compatible editor ID cache — fixes TESForm::LookupByEditorID under Wine
//
// Problem: BSTHashMap<BSFixedString, TESForm*> for editor ID lookups uses
// BSCRC32<BSFixedString> which hashes the data POINTER (not string content),
// and BSFixedString::operator== compares data POINTERS (not string content).
// Both rely on BSStringPool correctly interning identical strings to the same
// pool entry. Under Wine, cross-thread interning fails — the same string
// created on different threads gets different pool entries, breaking both
// the hash and equality, so every LookupByEditorID returns null.
//
// Fix: After data loading (kDataLoaded), iterate all forms on the main thread,
// read their editor IDs via GetFormEditorID() (reads from each form's own
// member storage, not the broken map), then clear and repopulate the game's
// BSTHashMap with fresh BSFixedString entries all created on the main thread.
// This ensures BSStringPool consistency for keys and future lookups.
//
// Form enumeration uses our own sharded form cache (from form_caching.h)
// which intercepts every FormScatterTable_SetAt call during form loading.
// This cache is guaranteed complete, unlike GetAllForms() BSTHashMap (broken
// under Wine) or TESDataHandler::formArrays[] (struct layout mismatch on AE).
//
// Requires po3_Tweaks with "Load EditorIDs = true" so that forms actually
// retain their editor IDs. Without it, most forms return "" from
// GetFormEditorID() and the cache will be small.

namespace Patches::EditorIdCache
{
    namespace detail
    {
        // Case-insensitive hash (FNV-1a) for editor ID strings
        struct CaseInsensitiveHash
        {
            std::size_t operator()(const std::string& s) const noexcept
            {
                std::size_t h = 14695981039346656037ULL;
                for (char c : s) {
                    h ^= static_cast<std::size_t>(std::tolower(static_cast<unsigned char>(c)));
                    h *= 1099511628211ULL;
                }
                return h;
            }
        };

        struct CaseInsensitiveEqual
        {
            bool operator()(const std::string& a, const std::string& b) const noexcept
            {
                return a.size() == b.size() &&
                    std::equal(a.begin(), a.end(), b.begin(), [](char ca, char cb) {
                        return std::tolower(static_cast<unsigned char>(ca)) ==
                               std::tolower(static_cast<unsigned char>(cb));
                    });
            }
        };

        using EditorIdMap = std::unordered_map<std::string, RE::TESForm*, CaseInsensitiveHash, CaseInsensitiveEqual>;

        inline std::shared_mutex g_cacheMutex;
        inline EditorIdMap g_editorIdCache;
        inline std::atomic<bool> g_cachePopulated{ false };

        // Wine-safe editor ID lookup (bypasses BSTHashMap entirely)
        inline RE::TESForm* LookupByEditorID(const std::string_view& a_editorID)
        {
            std::shared_lock lock(g_cacheMutex);
            auto it = g_editorIdCache.find(std::string(a_editorID));
            return it != g_editorIdCache.end() ? it->second : nullptr;
        }

        inline void PopulateCache()
        {
            logger::info("editor ID cache: building from loaded forms..."sv);

            // Phase 1: Enumerate ALL loaded forms from our sharded form cache.
            //
            // We use FormCaching::detail::g_formCache[256] which intercepts every
            // FormScatterTable_SetAt call during form loading. This sharded cache
            // (std::unordered_map with std::shared_mutex per shard) is guaranteed
            // to contain ALL registered forms regardless of Wine hash issues.
            //
            // Previous attempts that FAILED under Wine:
            //  - GetAllForms() BSTHashMap: only 166 forms (broken scatter table)
            //  - TESDataHandler::formArrays[]: 0 forms (struct layout mismatch on AE)
            //
            // GetFormEditorID() is a virtual call that reads from each form's own
            // member storage (e.g. BGSKeyword::formEditorID), NOT from the broken
            // editor ID BSTHashMap. This works regardless of Wine hash issues.
            EditorIdMap newCache;
            std::size_t formsScanned = 0;
            std::size_t editorIdsFound = 0;

            for (std::uint32_t shardIdx = 0; shardIdx < 256; ++shardIdx) {
                auto& shard = FormCaching::detail::g_formCache[shardIdx];
                std::shared_lock lock(shard.mutex);
                for (const auto& [baseId, form] : shard.map) {
                    if (!form)
                        continue;
                    ++formsScanned;

                    const char* editorId = form->GetFormEditorID();
                    if (editorId && editorId[0] != '\0') {
                        newCache.try_emplace(std::string(editorId), form);
                        ++editorIdsFound;
                    }
                }
            }

            logger::info("editor ID cache: scanned {} forms, found {} with editor IDs"sv,
                formsScanned, editorIdsFound);

            if (editorIdsFound < 100 && formsScanned > 1000) {
                logger::warn("editor ID cache: very few editor IDs found ({} / {} forms)"sv,
                    editorIdsFound, formsScanned);
                logger::warn("editor ID cache: ensure po3_Tweaks is loaded with 'Load EditorIDs = true'"sv);
            }

            // Store in our Wine-safe cache
            {
                std::unique_lock lock(g_cacheMutex);
                g_editorIdCache = std::move(newCache);
            }

            // Phase 2: Repopulate the game's BSTHashMap with fresh BSFixedStrings.
            //
            // All BSFixedStrings created here go through BSStringPool on the MAIN thread.
            // Future LookupByEditorID calls (also on the main thread) will create
            // BSFixedStrings that intern to the SAME pool entries, making the pointer-based
            // hash (BSCRC32) and equality (operator==) work correctly.
            //
            // The clear() + insert sequence is safe because:
            //  - clear() on an empty-checked map just destroys entries, no hash/comparison
            //  - insert into a freshly-cleared map: find() returns end() immediately
            //    (empty() check), so no broken comparison is ever invoked during insertion
            //  - As entries accumulate, subsequent find() calls during insert use
            //    BSFixedStrings all created on this thread, so interning is consistent

            const auto& [editorMap, editorLock] = RE::TESForm::GetAllFormsByEditorID();
            if (editorMap) {
                // Take write lock for exclusive access during repopulation
                const RE::BSWriteLockGuard wl{ editorLock };

                // Count existing entries (diagnostic)
                std::size_t existingCount = 0;
                for ([[maybe_unused]] const auto& entry : *editorMap) {
                    ++existingCount;
                }
                logger::info("editor ID cache: game's editor ID map had {} entries before repopulation"sv, existingCount);

                // Clear (releases old BSFixedString references)
                editorMap->clear();

                // Reserve capacity upfront to avoid rehashing mid-insert
                {
                    std::shared_lock lock(g_cacheMutex);
                    editorMap->reserve(static_cast<std::uint32_t>(g_editorIdCache.size()));

                    // Re-insert with fresh BSFixedStrings
                    std::size_t repopulated = 0;
                    for (const auto& [editorId, form] : g_editorIdCache) {
                        RE::BSFixedString key(editorId.c_str());
                        editorMap->emplace(std::move(key), form);
                        ++repopulated;
                    }

                    logger::info("editor ID cache: repopulated game map with {} entries"sv, repopulated);
                }
                g_cachePopulated.store(true);
            } else {
                logger::warn("editor ID cache: game's editor ID map pointer is null!"sv);
                logger::warn("editor ID cache: Wine-safe cache is available but inlined LookupByEditorID in other plugins will still fail"sv);
            }
        }
    }

    // Called from kDataLoaded message handler
    inline void OnDataLoaded()
    {
        if (detail::g_cachePopulated.load()) {
            logger::info("editor ID cache: already populated, skipping kDataLoaded repopulation"sv);
            return;
        }
        detail::PopulateCache();
    }

    inline void Install()
    {
        logger::info("editor ID cache patch enabled (DLL prefixed for early load order)"sv);
    }
}
