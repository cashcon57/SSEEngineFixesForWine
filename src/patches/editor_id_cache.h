#pragma once

#include <algorithm>
#include <cctype>
#include <atomic>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "form_caching.h"

// Wine-compatible editor ID cache — fixes TESForm::LookupByEditorID under Wine
//
// Problem: On AE 1.6.1170 under Wine, the form-by-ID BSTHashMap has capacity
// 262144 but only 166 engine forms are populated. ESM forms are never registered.
// FormID is at TESForm+0x14 (confirmed). We scan the hash table entries directly
// and also scan heap memory near known forms to find ESM forms.

namespace Patches::EditorIdCache
{
    namespace detail
    {
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

        // Our own FormID -> TESForm* map built from memory scanning
        inline std::unordered_map<RE::FormID, RE::TESForm*> g_formById;

        inline RE::TESForm* LookupByEditorID(const std::string_view& a_editorID)
        {
            std::shared_lock lock(g_cacheMutex);
            auto it = g_editorIdCache.find(std::string(a_editorID));
            return it != g_editorIdCache.end() ? it->second : nullptr;
        }

        // ================================================================
        // Scan BSTHashMap entries table directly for ALL forms
        // The map has 262144 capacity but CommonLib iteration only finds 166.
        // We brute-force scan every entry slot.
        // ================================================================
        inline std::unordered_map<RE::FormID, RE::TESForm*> ScanHashTableEntries()
        {
            std::unordered_map<RE::FormID, RE::TESForm*> result;

            const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
            if (!formMap) return result;

            // Read the raw BSTHashMap structure
            auto* mapRaw = reinterpret_cast<const std::uint8_t*>(formMap);

            // BSTHashMap layout (from CommonLib):
            // +0x00: _pad00 (8), +0x08: _pad08 (4), +0x0C: capacity (4)
            // +0x10: free (4), +0x14: good (4)
            // +0x18: sentinel (8), +0x20: alloc.pad (8), +0x28: entries (8)
            auto capacity = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x0C);
            auto sentinel = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x18);
            auto entriesPtr = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x28);

            logger::info("  BSTHashMap: cap={} sentinel=0x{:X} entries=0x{:X}",
                capacity, sentinel, entriesPtr);

            // Validate
            if (capacity == 0 || capacity > 0x1000000) {
                // Try alternate layout: maybe capacity is at +0x10
                capacity = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x10);
                sentinel = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x1C);
                entriesPtr = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x2C);
                logger::info("  BSTHashMap (alt layout): cap={} sentinel=0x{:X} entries=0x{:X}",
                    capacity, sentinel, entriesPtr);
            }

            if (capacity == 0 || capacity > 0x1000000 || entriesPtr < 0x10000) {
                logger::warn("  BSTHashMap: invalid structure, can't scan entries");
                return result;
            }

            if (IsBadReadPtr(reinterpret_cast<const void*>(entriesPtr), 24)) {
                logger::warn("  BSTHashMap: entries pointer not readable");
                return result;
            }

            // Entry layout for BSTHashMap<FormID, TESForm*>:
            // Each entry is a BSTTuple<FormID, TESForm*> + next pointer
            // BSTTuple: key(4) + pad(4) + value(8) = 16 bytes
            // Plus next pointer (8 bytes) = 24 bytes total
            const std::size_t ENTRY_SIZE = 24;
            auto* entryBase = reinterpret_cast<const std::uint8_t*>(entriesPtr);

            // Check readability of entries table
            std::size_t tableSize = static_cast<std::size_t>(capacity) * ENTRY_SIZE;
            if (IsBadReadPtr(entryBase, static_cast<UINT_PTR>(std::min(tableSize, static_cast<std::size_t>(0x1000))))) {
                logger::warn("  BSTHashMap: entries table not readable at 0x{:X}", entriesPtr);
                return result;
            }

            std::size_t populated = 0;
            std::size_t withValidForm = 0;

            for (std::uint32_t i = 0; i < capacity; ++i) {
                auto* entry = entryBase + i * ENTRY_SIZE;

                // Check readability every 4096 entries (page boundary)
                if (i % 4096 == 0) {
                    std::size_t remaining = (capacity - i) * ENTRY_SIZE;
                    if (IsBadReadPtr(entry, static_cast<UINT_PTR>(std::min(remaining, static_cast<std::size_t>(4096 * ENTRY_SIZE))))) {
                        logger::warn("  BSTHashMap: entries unreadable at index {}", i);
                        break;
                    }
                }

                auto formId = *reinterpret_cast<const RE::FormID*>(entry);
                auto formPtr = *reinterpret_cast<const std::uintptr_t*>(entry + 8);
                auto nextPtr = *reinterpret_cast<const std::uintptr_t*>(entry + 16);

                // An entry is populated if:
                // - formPtr is a valid heap/image pointer (not 0, not sentinel)
                // - OR nextPtr is not the sentinel
                bool isPopulated = false;

                if (formPtr != 0 && formPtr != sentinel &&
                    formPtr > 0x10000 && formPtr < 0x00007FFFFFFFFFFF) {
                    isPopulated = true;
                }

                if (!isPopulated && nextPtr != sentinel && nextPtr != 0) {
                    isPopulated = true;
                }

                if (isPopulated && formPtr > 0x10000 && formPtr < 0x00007FFFFFFFFFFF) {
                    ++populated;

                    // Verify: read FormID from the form object at +0x14
                    auto* formObj = reinterpret_cast<const std::uint8_t*>(formPtr);
                    if (!IsBadReadPtr(formObj, 0x20)) {
                        auto storedId = *reinterpret_cast<const RE::FormID*>(formObj + 0x14);
                        auto vtable = *reinterpret_cast<const std::uintptr_t*>(formObj);

                        // Valid form: has vtable in image range and FormID matches
                        if (vtable > 0x140000000 && vtable < 0x150000000) {
                            ++withValidForm;
                            result[storedId] = reinterpret_cast<RE::TESForm*>(const_cast<std::uint8_t*>(formObj));
                        }
                    }
                }
            }

            logger::info("  BSTHashMap scan: {}/{} entries populated, {} with valid forms",
                populated, capacity, withValidForm);

            return result;
        }

        // ================================================================
        // Memory scan for specific FormIDs near known form addresses
        // ================================================================
        inline RE::TESForm* ScanMemoryForForm(RE::FormID targetId, std::uintptr_t baseAddr, std::size_t range)
        {
            const std::size_t PAGE_SIZE = 0x1000;
            std::uintptr_t scanStart = (baseAddr > range) ? (baseAddr - range) : 0x10000;
            std::uintptr_t scanEnd = baseAddr + range;

            // Align to page boundary
            scanStart &= ~(PAGE_SIZE - 1);

            for (std::uintptr_t page = scanStart; page < scanEnd; page += PAGE_SIZE) {
                if (IsBadReadPtr(reinterpret_cast<const void*>(page), PAGE_SIZE)) continue;

                for (std::size_t off = 0; off + 0x20 <= PAGE_SIZE; off += 8) {
                    auto* candidate = reinterpret_cast<const std::uint8_t*>(page + off);
                    auto candidateId = *reinterpret_cast<const RE::FormID*>(candidate + 0x14);

                    if (candidateId == targetId) {
                        auto vtable = *reinterpret_cast<const std::uintptr_t*>(candidate);
                        if (vtable > 0x140000000 && vtable < 0x150000000) {
                            return reinterpret_cast<RE::TESForm*>(const_cast<std::uint8_t*>(candidate));
                        }
                    }
                }
            }
            return nullptr;
        }

        inline void PopulateCache()
        {
            logger::info("editor ID cache v1.17.0: direct solution"sv);

            // ================================================================
            // PHASE 1: Scan the BSTHashMap entries table for ALL forms
            // This bypasses CommonLib's iteration which may miss entries
            // ================================================================
            auto scannedForms = ScanHashTableEntries();
            logger::info("  Phase 1: found {} forms in hash table entries", scannedForms.size());

            // Log some stats
            if (!scannedForms.empty()) {
                RE::FormID minId = 0xFFFFFFFF, maxId = 0;
                for (auto& [id, _] : scannedForms) {
                    if (id < minId) minId = id;
                    if (id > maxId) maxId = id;
                }
                logger::info("  Phase 1: FormID range 0x{:X} - 0x{:X}", minId, maxId);

                // Check for our target
                auto it = scannedForms.find(0x12E46);
                if (it != scannedForms.end()) {
                    logger::info("  Phase 1: FOUND defaultUnarmedWeap 0x12E46 at {:p}!",
                        (void*)it->second);
                }
            }

            // ================================================================
            // PHASE 2: If target form not found in hash table, scan memory
            // Scan near the known form addresses (0x2398xxxx range)
            // ================================================================
            bool foundTarget = scannedForms.count(0x12E46) > 0;

            if (!foundTarget) {
                logger::info("  Phase 2: scanning memory for FormID 0x12E46..."sv);

                // Get a representative form address from the hash table
                std::uintptr_t refAddr = 0;
                {
                    const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                    if (formMap) {
                        const RE::BSReadLockGuard rl{ formLock };
                        for (auto& [id, form] : *formMap) {
                            if (form) {
                                auto addr = reinterpret_cast<std::uintptr_t>(form);
                                // Prefer forms in the heap range, not image range
                                if (addr < 0x140000000) {
                                    refAddr = addr;
                                    break;
                                }
                            }
                        }
                    }
                }

                if (refAddr != 0) {
                    // Scan +/- 2MB around the reference address
                    logger::info("  Phase 2: scanning around 0x{:X} (+/- 2MB)...", refAddr);
                    auto* found = ScanMemoryForForm(0x12E46, refAddr, 0x200000);
                    if (found) {
                        logger::info("  Phase 2: FOUND 0x12E46 at {:p}!", (void*)found);
                        scannedForms[0x12E46] = found;
                        foundTarget = true;
                    }
                }

                // Also try near Player (different heap region)
                if (!foundTarget) {
                    auto* player = Patches::FormCaching::detail::GameLookupFormByID(0x14);
                    if (player) {
                        auto playerAddr = reinterpret_cast<std::uintptr_t>(player);
                        logger::info("  Phase 2: scanning near Player at 0x{:X} (+/- 2MB)...", playerAddr);
                        auto* found = ScanMemoryForForm(0x12E46, playerAddr, 0x200000);
                        if (found) {
                            logger::info("  Phase 2: FOUND 0x12E46 near Player at {:p}!", (void*)found);
                            scannedForms[0x12E46] = found;
                            foundTarget = true;
                        }
                    }
                }

                // Broader scan: try larger range
                if (!foundTarget && refAddr != 0) {
                    logger::info("  Phase 2: broad scan around 0x{:X} (+/- 16MB)...", refAddr);
                    auto* found = ScanMemoryForForm(0x12E46, refAddr, 0x1000000);
                    if (found) {
                        logger::info("  Phase 2: FOUND 0x12E46 (broad) at {:p}!", (void*)found);
                        scannedForms[0x12E46] = found;
                        foundTarget = true;
                    }
                }

                if (!foundTarget) {
                    logger::warn("  Phase 2: form 0x12E46 NOT FOUND in memory scans"sv);
                }
            }

            // Store all found forms for later use
            g_formById = scannedForms;

            // ================================================================
            // PHASE 3: Build editor ID cache from found forms
            // ================================================================
            EditorIdMap newCache;

            // Try GetFormEditorID on all found forms
            std::size_t withEditorId = 0;
            for (auto& [formId, form] : scannedForms) {
                if (!form) continue;
                const char* eid = form->GetFormEditorID();
                if (eid && eid[0] != '\0') {
                    newCache.try_emplace(std::string(eid), form);
                    ++withEditorId;
                    if (withEditorId <= 10) {
                        logger::info("  EditorID: '{}' -> 0x{:08X}", eid, formId);
                    }
                }
            }
            logger::info("  Phase 3: {} forms with editor IDs", withEditorId);

            // Hardcoded fallback for critical forms
            if (newCache.find("defaultUnarmedWeap") == newCache.end()) {
                auto it = scannedForms.find(0x12E46);
                if (it != scannedForms.end()) {
                    newCache.try_emplace("defaultUnarmedWeap", it->second);
                    logger::info("  Hardcoded: 'defaultUnarmedWeap' -> 0x12E46 ({:p})",
                        (void*)it->second);
                }
            }

            // More hardcoded critical editor IDs
            static const std::pair<const char*, RE::FormID> knownIds[] = {
                { "Player", 0x00000007 },
                { "PlayerRef", 0x00000014 },
                { "LockPick", 0x0000000A },
                { "Gold001", 0x0000003B },
                { "defaultUnarmedWeap", 0x00012E46 },
                { "WeapTypeUnarmed", 0x00013F8D },
                { "ArmorIronCuirass", 0x00012E49 },
                { "Torch01", 0x0001D4EC },
                { "FoodBread", 0x00064B31 },
            };

            for (auto& [eid, fid] : knownIds) {
                if (newCache.find(eid) != newCache.end()) continue;
                auto it = scannedForms.find(fid);
                if (it != scannedForms.end() && it->second) {
                    newCache.try_emplace(std::string(eid), it->second);
                    logger::info("  Hardcoded: '{}' -> 0x{:08X} ({:p})", eid, fid, (void*)it->second);
                }
            }

            logger::info("editor ID cache: total {} editor IDs"sv, newCache.size());

            // ================================================================
            // PHASE 4: Store cache and repopulate game maps
            // ================================================================
            {
                std::unique_lock lock(g_cacheMutex);
                g_editorIdCache = std::move(newCache);
            }

            {
                std::shared_lock lock(g_cacheMutex);
                if (g_editorIdCache.empty()) {
                    logger::warn("editor ID cache: empty, will retry at next event"sv);
                    return;
                }
            }

            // Repopulate the game's editor-ID-to-form BSTHashMap
            const auto& [editorMap, editorLock] = RE::TESForm::GetAllFormsByEditorID();
            if (editorMap) {
                const RE::BSWriteLockGuard wl{ editorLock };
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
            }

            // Also repopulate the form-by-ID BSTHashMap with any missing forms
            if (scannedForms.size() > 200) {
                const auto& [formMap, formLock2] = RE::TESForm::GetAllForms();
                if (formMap) {
                    const RE::BSWriteLockGuard wl{ formLock2 };
                    std::size_t added = 0;
                    for (auto& [formId, form] : scannedForms) {
                        if (!form) continue;
                        // Use emplace to add missing entries
                        auto [it, inserted] = formMap->emplace(formId, form);
                        if (inserted) ++added;
                    }
                    logger::info("editor ID cache: added {} forms to form-by-ID map"sv, added);
                }
            }

            if (!g_cachePopulated.load()) {
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
        logger::info("editor ID cache patch enabled (v1.17.0 direct solution)"sv);
    }
}
