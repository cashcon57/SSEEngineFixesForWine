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
// Problem: On AE 1.6.1170 under Wine/CrossOver:
//   1. The form-by-ID BSTHashMap only has ~166 engine forms (ESM forms missing)
//   2. TESDataHandler member offsets in CommonLib don't match this version
//   3. po3_Tweaks' GetFormEditorID hook doesn't work under Wine
//   4. The editor-ID-to-form BSTHashMap is completely empty
//
// Fix: Hook TESDataHandler::AddFormToDataHandler to capture ALL forms during
// ESM loading. Build editor ID cache from captured forms. Repopulate the
// game's editor-ID-to-form BSTHashMap.

namespace Patches::EditorIdCache
{
    namespace detail
    {
        // Case-insensitive hash (FNV-1a)
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

        // ================================================================
        // AddFormToDataHandler hook — captures ALL forms during ESM loading
        // ================================================================
        inline SafetyHookInline g_hk_AddForm{};
        inline std::mutex g_capturedFormsMutex;
        inline std::unordered_map<RE::FormID, RE::TESForm*> g_capturedForms;

        inline bool HookedAddFormToDataHandler(RE::TESDataHandler* a_self, RE::TESForm* a_form)
        {
            auto result = g_hk_AddForm.call<bool>(a_self, a_form);

            if (a_form) {
                std::lock_guard lock(g_capturedFormsMutex);
                g_capturedForms[a_form->GetFormID()] = a_form;
            }

            return result;
        }

        // Wine-safe editor ID lookup
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
            // DIAGNOSTIC PHASE
            // ================================================================

            // Captured forms from AddFormToDataHandler hook
            std::size_t capturedCount = 0;
            {
                std::lock_guard lock(g_capturedFormsMutex);
                capturedCount = g_capturedForms.size();
            }
            logger::info("  AddFormToDataHandler hook captured {} forms", capturedCount);

            // Test game native lookup
            {
                auto* player = Patches::FormCaching::detail::GameLookupFormByID(0x14);
                auto* weapon = Patches::FormCaching::detail::GameLookupFormByID(0x12E46);
                logger::info("  GameLookup: Player={:p}, defaultUnarmedWeap={:p}",
                    (void*)player, (void*)weapon);
            }

            // Test captured forms for the weapon
            {
                std::lock_guard lock(g_capturedFormsMutex);
                auto it = g_capturedForms.find(0x00012E46);
                if (it != g_capturedForms.end()) {
                    auto* form = it->second;
                    auto eid = form->GetFormEditorID();
                    logger::info("  Captured form 0x12E46: {:p} type={} editorID='{}'",
                        (void*)form, static_cast<int>(form->GetFormType()),
                        eid ? eid : "(null)");
                } else {
                    logger::info("  Form 0x12E46 NOT in captured forms");
                }
            }

            // GetAllForms map size
            {
                const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                if (formMap) {
                    logger::info("  GetAllForms .size(): {}", formMap->size());
                }
            }

            // Dump raw TESDataHandler memory to find actual member offsets
            {
                auto* dh = RE::TESDataHandler::GetSingleton();
                if (dh) {
                    logger::info("  TESDataHandler ptr={:p}", (void*)dh);

                    // Also log the RELOCATION address vs dereferenced value
                    static REL::Relocation<RE::TESDataHandler**> singleton{ REL::ID(VAR_NUM(514141, 400269)) };
                    logger::info("  TESDataHandler reloc addr={:p}, *reloc={:p}",
                        (void*)singleton.address(), (void*)*singleton);

                    auto* raw = reinterpret_cast<const std::uint8_t*>(dh);

                    // Scan for BSTArray patterns: ptr(8) + size(4) + capacity(4) = 16 bytes
                    // Look for non-zero size values that suggest form arrays
                    logger::info("  Scanning TESDataHandler for non-zero BSTArray (up to +0xF00)..."sv);

                    std::size_t arraysFound = 0;
                    for (std::size_t off = 0; off < 0xF00; off += 8) {
                        // BSTArray layout: data_ptr(8) + size(4) + capacity(4)
                        auto dataPtr = *reinterpret_cast<const std::uintptr_t*>(raw + off);
                        auto size = *reinterpret_cast<const std::uint32_t*>(raw + off + 8);
                        auto cap = *reinterpret_cast<const std::uint32_t*>(raw + off + 12);

                        // Valid BSTArray: pointer-like data ptr, size > 0, capacity >= size, capacity < 10M
                        if (dataPtr > 0x10000 && dataPtr < 0x00007FFFFFFFFFFF &&
                            size > 0 && cap >= size && cap < 10000000) {
                            if (arraysFound < 20) {
                                logger::info("    +0x{:03X}: BSTArray? ptr=0x{:X} size={} cap={}",
                                    off, dataPtr, size, cap);
                            }
                            ++arraysFound;
                        }
                    }
                    logger::info("  Found {} potential BSTArray entries in TESDataHandler"sv, arraysFound);

                    // Also look specifically for TESFile* arrays (loaded files)
                    // These would have size in range of loaded plugin count (1-1000)
                    logger::info("  Looking for loaded file arrays (size 1-1000)..."sv);
                    for (std::size_t off = 0; off < 0xF00; off += 8) {
                        auto dataPtr = *reinterpret_cast<const std::uintptr_t*>(raw + off);
                        auto size = *reinterpret_cast<const std::uint32_t*>(raw + off + 8);
                        auto cap = *reinterpret_cast<const std::uint32_t*>(raw + off + 12);

                        if (dataPtr > 0x10000 && dataPtr < 0x00007FFFFFFFFFFF &&
                            size > 0 && size <= 1000 && cap >= size && cap < 10000) {
                            // Could be a file array. Try reading the first element as a pointer.
                            auto firstElem = *reinterpret_cast<const std::uintptr_t*>(dataPtr);
                            if (firstElem > 0x10000 && firstElem < 0x00007FFFFFFFFFFF) {
                                // Try reading as TESFile — check if fileName offset has printable chars
                                auto* possibleFile = reinterpret_cast<const char*>(firstElem + 0x58);
                                bool printable = true;
                                for (int i = 0; i < 6 && possibleFile[i]; ++i) {
                                    if (possibleFile[i] < 0x20 || possibleFile[i] > 0x7E) {
                                        printable = false;
                                        break;
                                    }
                                }
                                if (printable && possibleFile[0] != '\0') {
                                    char namePreview[32] = {};
                                    std::memcpy(namePreview, possibleFile, 31);
                                    logger::info("    +0x{:03X}: file array? size={} cap={} first='{}'",
                                        off, size, cap, namePreview);
                                }
                            }
                        }
                    }
                }
            }

            // ================================================================
            // POPULATION PHASE: Use captured forms
            // ================================================================

            if (capturedCount > 0) {
                logger::info("editor ID cache: building from {} captured forms..."sv, capturedCount);

                std::lock_guard lock(g_capturedFormsMutex);
                std::size_t withEditorId = 0;

                for (auto& [formId, form] : g_capturedForms) {
                    if (!form) continue;

                    const char* eid = form->GetFormEditorID();
                    if (eid && eid[0] != '\0') {
                        newCache.try_emplace(std::string(eid), form);
                        ++withEditorId;
                        if (withEditorId <= 5) {
                            logger::info("  EditorID: '{}' -> 0x{:08X} type={}",
                                eid, formId, static_cast<int>(form->GetFormType()));
                        }
                    }
                }

                logger::info("editor ID cache: {} forms with editor IDs out of {} captured",
                    withEditorId, capturedCount);

                // If GetFormEditorID doesn't work (po3_Tweaks broken), try hardcoded
                // editor IDs for commonly needed vanilla forms
                if (withEditorId == 0) {
                    logger::warn("editor ID cache: GetFormEditorID not working, using hardcoded IDs"sv);

                    // Hardcoded editor ID -> FormID map for critical Skyrim.esm forms
                    // that mods commonly look up by editor ID
                    static const std::pair<const char*, RE::FormID> knownEditorIds[] = {
                        { "defaultUnarmedWeap", 0x00012E46 },
                        { "Player", 0x00000007 },
                        { "PlayerRef", 0x00000014 },
                        { "Gold001", 0x0000003B },
                        { "Iron Dagger", 0x0001397E },
                        { "ArmorIronCuirass", 0x00012E49 },
                        { "FoodBread", 0x00064B31 },
                        { "Torch01", 0x0001D4EC },
                        { "LockPick", 0x0000000A },
                        { "WeapTypeUnarmed", 0x00013F8D },
                    };

                    for (auto& [editorId, formId] : knownEditorIds) {
                        auto it = g_capturedForms.find(formId);
                        if (it != g_capturedForms.end() && it->second) {
                            newCache.try_emplace(std::string(editorId), it->second);
                            logger::info("  Hardcoded: '{}' -> 0x{:08X} ({:p})",
                                editorId, formId, (void*)it->second);
                        }
                    }

                    logger::info("editor ID cache: {} hardcoded editor IDs matched", newCache.size());
                }
            } else {
                // No forms captured by hook — fall back to game native lookup
                logger::warn("editor ID cache: no forms captured by AddFormToDataHandler hook"sv);

                // Last resort: try CommonLib iteration
                const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                if (formMap && formMap->size() > 0) {
                    const RE::BSReadLockGuard rl{ formLock };
                    std::size_t count = 0;
                    for (auto& [id, form] : *formMap) {
                        if (!form) continue;
                        ++count;
                        const char* eid = form->GetFormEditorID();
                        if (eid && eid[0] != '\0') {
                            newCache.try_emplace(std::string(eid), form);
                        }
                    }
                    logger::info("editor ID cache: CommonLib iteration: {} forms, {} editor IDs",
                        count, newCache.size());
                }
            }

            logger::info("editor ID cache: total {} editor IDs in cache"sv, newCache.size());

            // Store cache
            {
                std::unique_lock lock(g_cacheMutex);
                g_editorIdCache = std::move(newCache);
            }

            // If empty, allow retry
            {
                std::shared_lock lock(g_cacheMutex);
                if (g_editorIdCache.empty()) {
                    logger::warn("editor ID cache: no editor IDs found, will retry at next event"sv);
                    return;
                }
            }

            // ================================================================
            // REPOPULATION PHASE: Write to game's editor-ID-to-form map
            // ================================================================
            const auto& [editorMap, editorLock] = RE::TESForm::GetAllFormsByEditorID();
            if (editorMap) {
                const RE::BSWriteLockGuard wl{ editorLock };
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
                std::shared_lock lock(g_cacheMutex);
                if (!g_editorIdCache.empty()) {
                    g_cachePopulated.store(true);
                }
            }
        }
    }

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

        // Hook AddFormToDataHandler to capture ALL forms during ESM loading.
        // This fires for every form registered, giving us the complete form set.
        const REL::Relocation addForm{ REL::ID(VAR_NUM(13597, 13693)) };
        detail::g_hk_AddForm = safetyhook::create_inline(
            addForm.address(), detail::HookedAddFormToDataHandler);

        if (detail::g_hk_AddForm) {
            logger::info("  hooked AddFormToDataHandler at {:p}", (void*)addForm.address());
        } else {
            logger::warn("  FAILED to hook AddFormToDataHandler");
        }
    }
}
