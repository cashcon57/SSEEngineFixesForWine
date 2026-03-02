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
// Problem: Under Wine/CrossOver, the game's editor-ID-to-form BSTHashMap is
// empty at kDataLoaded on AE. Plugins like CoreImpactFramework that call
// LookupByEditorID crash because no forms can be found by editor ID.
//
// Fix: At kDataLoaded (before other plugins), scan FormID ranges using the
// game's native GetFormByNumericId (via SafetyHook trampoline), collect
// editor IDs via GetFormEditorID (works if po3_Tweaks is installed), and
// repopulate the game's editor-ID-to-form BSTHashMap with fresh BSFixedStrings
// created on the main thread for consistent pointer-based hashing.

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

            EditorIdMap newCache;

            // ================================================================
            // DIAGNOSTIC PHASE: Determine what APIs work under Wine
            // ================================================================

            // Test 1: Game's native GetFormByNumericId (via SafetyHook trampoline)
            // This calls the game's own compiled code, bypassing CommonLib entirely.
            bool gameNativeWorks = false;
            bool editorIdsAvailable = false;

            struct TestForm { RE::FormID id; const char* name; };
            constexpr TestForm testForms[] = {
                { 0x00000014, "Player" },
                { 0x00012E46, "defaultUnarmedWeap" },
                { 0x0000003B, "Gold001" },
                { 0x00000007, "PlayerRef" },
            };

            for (const auto& [testId, testName] : testForms) {
                auto* nativeForm = Patches::FormCaching::detail::GameLookupFormByID(testId);
                auto* commonlibForm = RE::TESForm::LookupByID(testId);

                if (nativeForm) {
                    gameNativeWorks = true;
                    const char* eid = nativeForm->GetFormEditorID();
                    bool hasEid = eid && eid[0] != '\0';
                    if (hasEid) editorIdsAvailable = true;

                    logger::info("  GameLookup(0x{:X} '{}') = {:p} type={} editorID='{}'",
                        testId, testName, (void*)nativeForm,
                        static_cast<int>(nativeForm->GetFormType()),
                        hasEid ? eid : "(null)");

                    if (commonlibForm != nativeForm) {
                        logger::warn("  CommonLib LookupByID(0x{:X}) = {:p} MISMATCH!",
                            testId, (void*)commonlibForm);
                    }
                } else {
                    logger::info("  GameLookup(0x{:X} '{}') = nullptr (CommonLib={:p})",
                        testId, testName, (void*)commonlibForm);
                }
            }

            // Test 2: CommonLib's LookupByEditorID (BSFixedString hash path)
            {
                auto* form = RE::TESForm::LookupByEditorID("defaultUnarmedWeap"sv);
                logger::info("  LookupByEditorID('defaultUnarmedWeap') = {:p}", (void*)form);
            }

            // Test 3: Our sharded form cache from form_caching.h
            {
                std::size_t shardTotal = 0;
                for (int i = 0; i < 256; ++i) {
                    auto& shard = Patches::FormCaching::detail::g_formCache[i];
                    std::shared_lock lock(shard.mutex);
                    shardTotal += shard.map.size();
                }
                logger::info("  Sharded form cache: {} forms", shardTotal);
            }

            // Test 4: CommonLib GetAllForms map
            {
                const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                logger::info("  GetAllForms map ptr: {:p}", (void*)formMap);
                if (formMap) {
                    logger::info("  GetAllForms .size(): {}", formMap->size());
                }
            }

            // Test 5: TESDataHandler info
            auto* dh = RE::TESDataHandler::GetSingleton();
            std::uint32_t regularFileCount = 0;
            std::uint32_t lightFileCount = 0;
            if (dh) {
                logger::info("  TESDataHandler ptr={:p}", (void*)dh);

                auto& files = dh->compiledFileCollection.files;
                auto& smallFiles = dh->compiledFileCollection.smallFiles;
                regularFileCount = static_cast<std::uint32_t>(files.size());
                lightFileCount = static_cast<std::uint32_t>(smallFiles.size());
                logger::info("  Loaded files: {} regular, {} light", regularFileCount, lightFileCount);

                for (std::size_t i = 0; i < std::min(files.size(), static_cast<std::size_t>(3)); ++i) {
                    if (files[i]) {
                        logger::info("    File[{}]: '{}'", i, files[i]->GetFilename());
                    }
                }

                // Form arrays summary
                std::size_t nonEmpty = 0;
                std::size_t totalForms = 0;
                for (std::uint32_t ft = 0; ft < static_cast<std::uint32_t>(RE::FormType::Max); ++ft) {
                    auto& arr = dh->GetFormArray(static_cast<RE::FormType>(ft));
                    if (arr.size() > 0) {
                        ++nonEmpty;
                        totalForms += arr.size();
                    }
                }
                logger::info("  TESDataHandler form arrays: {} non-empty, {} total forms", nonEmpty, totalForms);
            }

            // ================================================================
            // POPULATION PHASE: Build editor ID cache
            // ================================================================

            if (gameNativeWorks && editorIdsAvailable) {
                // Best case: game native lookup works AND editor IDs are available.
                // Scan FormID ranges to find all forms with editor IDs.
                logger::info("editor ID cache: scanning FormID ranges (native lookup + editor IDs work)..."sv);

                std::size_t totalFound = 0;
                std::size_t totalEditorIds = 0;

                // Scan regular masters
                std::uint32_t mastersToScan = std::min(regularFileCount > 0 ? regularFileCount : 10u, 256u);
                for (std::uint32_t master = 0; master < mastersToScan; ++master) {
                    std::uint32_t prefix = master << 24;
                    std::size_t consecutiveMisses = 0;
                    std::size_t masterFound = 0;

                    for (std::uint32_t base = 1; base <= 0x200000 && consecutiveMisses < 4096; ++base) {
                        auto* form = Patches::FormCaching::detail::GameLookupFormByID(prefix | base);
                        if (form) {
                            consecutiveMisses = 0;
                            ++masterFound;

                            const char* eid = form->GetFormEditorID();
                            if (eid && eid[0] != '\0') {
                                newCache.try_emplace(std::string(eid), form);
                                ++totalEditorIds;
                                if (totalEditorIds <= 5) {
                                    logger::info("  EditorID: '{}' -> 0x{:08X} type={}",
                                        eid, form->GetFormID(),
                                        static_cast<int>(form->GetFormType()));
                                }
                            }
                        } else {
                            ++consecutiveMisses;
                        }
                    }

                    totalFound += masterFound;
                    if (masterFound > 0 && master < 10) {
                        logger::info("  Master 0x{:02X}: {} forms", master, masterFound);
                    }
                }

                // Scan light plugins (FE prefix)
                for (std::uint32_t li = 0; li < lightFileCount; ++li) {
                    std::uint32_t prefix = 0xFE000000 | (li << 12);
                    std::size_t consecutiveMisses = 0;
                    std::size_t lightFound = 0;

                    for (std::uint32_t base = 0; base <= 0xFFF && consecutiveMisses < 256; ++base) {
                        auto* form = Patches::FormCaching::detail::GameLookupFormByID(prefix | base);
                        if (form) {
                            consecutiveMisses = 0;
                            ++lightFound;

                            const char* eid = form->GetFormEditorID();
                            if (eid && eid[0] != '\0') {
                                newCache.try_emplace(std::string(eid), form);
                                ++totalEditorIds;
                            }
                        } else {
                            ++consecutiveMisses;
                        }
                    }

                    totalFound += lightFound;
                }

                logger::info("editor ID cache: FormID scan complete: {} forms, {} editor IDs",
                    totalFound, totalEditorIds);

            } else if (gameNativeWorks && !editorIdsAvailable) {
                // Game lookup works but no editor IDs (po3_Tweaks hook not functional).
                // Fall back to TESDataHandler enumeration.
                logger::warn("editor ID cache: game lookup works but GetFormEditorID returns empty"sv);
                logger::info("editor ID cache: trying TESDataHandler enumeration..."sv);

                if (dh) {
                    std::size_t formsScanned = 0;
                    std::size_t editorIdsFound = 0;

                    for (std::uint32_t ft = 0; ft < static_cast<std::uint32_t>(RE::FormType::Max); ++ft) {
                        for (auto* form : dh->GetFormArray(static_cast<RE::FormType>(ft))) {
                            if (!form) continue;
                            ++formsScanned;
                            const char* eid = form->GetFormEditorID();
                            if (eid && eid[0] != '\0') {
                                newCache.try_emplace(std::string(eid), form);
                                ++editorIdsFound;
                            }
                        }
                    }

                    logger::info("editor ID cache: TESDataHandler: {} scanned, {} editor IDs",
                        formsScanned, editorIdsFound);
                }
            } else {
                // Game native lookup doesn't work either. Try CommonLib map iteration.
                logger::warn("editor ID cache: game native lookup failed, trying CommonLib iteration..."sv);

                const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                if (formMap) {
                    const RE::BSReadLockGuard rl{ formLock };
                    std::size_t formsFound = 0;
                    std::size_t editorIdsFound = 0;

                    for (auto& [id, form] : *formMap) {
                        if (!form) continue;
                        ++formsFound;
                        const char* eid = form->GetFormEditorID();
                        if (eid && eid[0] != '\0') {
                            newCache.try_emplace(std::string(eid), form);
                            ++editorIdsFound;
                        }
                    }

                    logger::info("editor ID cache: CommonLib iterator: {} forms, {} editor IDs",
                        formsFound, editorIdsFound);
                }
            }

            logger::info("editor ID cache: total {} editor IDs in cache"sv, newCache.size());

            // Store in our Wine-safe cache
            {
                std::unique_lock lock(g_cacheMutex);
                g_editorIdCache = std::move(newCache);
            }

            // If no editor IDs found, allow retry at next event
            {
                std::shared_lock lock(g_cacheMutex);
                if (g_editorIdCache.empty()) {
                    logger::warn("editor ID cache: no editor IDs found, will retry at next event"sv);
                    return;
                }
            }

            // ================================================================
            // REPOPULATION PHASE: Write back to game's editor-ID-to-form map
            // ================================================================
            //
            // All BSFixedStrings created here go through BSStringPool on the MAIN thread.
            // Future LookupByEditorID calls (also main thread during kDataLoaded) will
            // create BSFixedStrings that intern to the SAME pool entries, making the
            // pointer-based hash and equality work correctly.
            const auto& [editorMap, editorLock] = RE::TESForm::GetAllFormsByEditorID();
            if (editorMap) {
                const RE::BSWriteLockGuard wl{ editorLock };

                // Clear existing entries (releases old BSFixedString references)
                editorMap->clear();

                {
                    std::shared_lock lock(g_cacheMutex);
                    editorMap->reserve(static_cast<std::uint32_t>(g_editorIdCache.size()));

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
                logger::warn("editor ID cache: game's editor ID map pointer is null"sv);
                // Still mark as populated since our Wine-safe cache works
                std::shared_lock lock(g_cacheMutex);
                if (!g_editorIdCache.empty()) {
                    g_cachePopulated.store(true);
                }
            }
        }
    }

    // Called from kDataLoaded message handler
    inline void OnDataLoaded()
    {
        if (detail::g_cachePopulated.load()) {
            logger::info("editor ID cache: already populated, skipping"sv);
            return;
        }
        detail::PopulateCache();
    }

    inline void Install()
    {
        logger::info("editor ID cache patch enabled (DLL prefixed for early load order)"sv);
    }
}
