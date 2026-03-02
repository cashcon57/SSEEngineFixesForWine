#pragma once

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "form_caching.h"

// Wine-compatible editor ID cache — fixes TESForm::LookupByEditorID under Wine
//
// v1.18.0: Under Wine/AE 1.6.1170, the game's form-by-ID BSTHashMap has capacity
// 262144 but only 166 engine forms. ESM forms (weapons, armor, NPCs, etc.) exist
// in heap memory but are never registered in the lookup maps.
//
// Solution: Use VirtualQuery to scan ALL committed memory for TESForm objects,
// validated by vtable chain + FormID + formType. Then call GetFormEditorID() on
// each found form and populate the game's editor-ID-to-form BSTHashMap.

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
        inline std::unordered_map<RE::FormID, RE::TESForm*> g_formById;

        inline RE::TESForm* LookupByEditorID(const std::string_view& a_editorID)
        {
            std::shared_lock lock(g_cacheMutex);
            auto it = g_editorIdCache.find(std::string(a_editorID));
            return it != g_editorIdCache.end() ? it->second : nullptr;
        }

        // ================================================================
        // Get image bounds from PE header (computed once, cached)
        // ================================================================
        struct ImageBounds {
            std::uintptr_t base = 0;
            std::uintptr_t end = 0;
        };

        inline ImageBounds GetImageBounds()
        {
            static ImageBounds bounds = []() {
                ImageBounds b;
                b.base = REL::Module::get().base();
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b.base);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b.base + dos->e_lfanew);
                b.end = b.base + nt->OptionalHeader.SizeOfImage;
                return b;
            }();
            return bounds;
        }

        // ================================================================
        // VirtualQuery: scan ALL committed memory for TESForm objects
        //
        // Validation chain per candidate (at each 8-byte offset):
        //   1. vtable pointer in [imageBase, imageEnd)
        //   2. vtable has room for TESForm vfuncs (vtable + 0x200 < imageEnd)
        //   3. vtable[0], [1], [2] all point into image (real vtable, not random ptr)
        //   4. FormID at +0x14: non-zero, < 0x05000000
        //   5. formType at +0x1A: < 0x8A (FormType::Max)
        // ================================================================
        inline std::unordered_map<RE::FormID, RE::TESForm*> VirtualQueryScan()
        {
            std::unordered_map<RE::FormID, RE::TESForm*> result;
            result.reserve(100000);

            auto img = GetImageBounds();
            logger::info("  image: 0x{:X} - 0x{:X} ({:.1f}MB)",
                img.base, img.end,
                static_cast<double>(img.end - img.base) / (1024.0 * 1024.0));

            MEMORY_BASIC_INFORMATION mbi{};
            std::uintptr_t scanAddr = 0x10000;
            std::size_t regions = 0;
            std::size_t bytesScanned = 0;
            std::size_t vtableHits = 0;

            auto t0 = std::chrono::high_resolution_clock::now();

            while (VirtualQuery(reinterpret_cast<void*>(scanAddr), &mbi, sizeof(mbi)))
            {
                auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                auto regionSize = mbi.RegionSize;
                auto regionEnd = regionBase + regionSize;

                // Prevent overflow / infinite loop
                if (regionEnd <= regionBase) break;
                scanAddr = regionEnd;
                ++regions;

                // Time check every 500 regions
                if (regions % 500 == 0) {
                    auto elapsed = std::chrono::high_resolution_clock::now() - t0;
                    if (elapsed > std::chrono::seconds(20)) {
                        logger::warn("  VQ: 20s limit, {}MB scanned, {} forms",
                            bytesScanned >> 20, result.size());
                        break;
                    }
                }

                // Must be committed + readable
                if (mbi.State != MEM_COMMIT) continue;
                if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
                DWORD readBits = PAGE_READONLY | PAGE_READWRITE |
                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                    PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
                if (!(mbi.Protect & readBits)) continue;

                // Skip the image itself — TESForm objects live on the heap
                if (regionBase >= img.base && regionBase < img.end) continue;

                // Skip huge regions (>256MB) — likely file mappings
                if (regionSize > 256ULL * 1024 * 1024) continue;

                auto* base = reinterpret_cast<const std::uint8_t*>(regionBase);
                bytesScanned += regionSize;

                for (std::size_t off = 0; off + 0x20 <= regionSize; off += 8)
                {
                    // 1. vtable in image range
                    auto vtable = *reinterpret_cast<const std::uintptr_t*>(base + off);
                    if (vtable < img.base || vtable >= img.end) continue;

                    // 2. vtable must have room for TESForm's ~80+ virtual functions
                    if (vtable + 0x200 >= img.end) continue;

                    // 3. First 3 vtable entries must be valid code pointers
                    auto* vt = reinterpret_cast<const std::uintptr_t*>(vtable);
                    auto vf0 = vt[0], vf1 = vt[1], vf2 = vt[2];
                    if (vf0 < img.base || vf0 >= img.end) continue;
                    if (vf1 < img.base || vf1 >= img.end) continue;
                    if (vf2 < img.base || vf2 >= img.end) continue;

                    ++vtableHits;

                    // 4. FormID at +0x14: non-zero, reasonable range
                    auto formId = *reinterpret_cast<const std::uint32_t*>(base + off + 0x14);
                    if (formId == 0 || formId >= 0x05000000) continue;

                    // 5. formType at +0x1A: valid FormType
                    auto formType = *(base + off + 0x1A);
                    if (formType >= 0x8A) continue;

                    result.try_emplace(formId,
                        reinterpret_cast<RE::TESForm*>(
                            const_cast<std::uint8_t*>(base + off)));
                }
            }

            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - t0).count();

            logger::info("  VQ: {} regions, {}MB, {} vtable hits, {} unique forms, {}ms",
                regions, bytesScanned >> 20, vtableHits, result.size(), ms);

            return result;
        }

        // ================================================================
        // Fallback: scan the BSTHashMap entries directly (from v1.17.0)
        // Guaranteed to find the 166 engine forms even if VQ fails
        // ================================================================
        inline std::unordered_map<RE::FormID, RE::TESForm*> ScanHashTableEntries()
        {
            std::unordered_map<RE::FormID, RE::TESForm*> result;

            const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
            if (!formMap) return result;

            auto* mapRaw = reinterpret_cast<const std::uint8_t*>(formMap);
            auto capacity = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x0C);
            auto sentinel = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x18);
            auto entriesPtr = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x28);

            if (capacity == 0 || capacity > 0x1000000 || entriesPtr < 0x10000)
                return result;
            if (IsBadReadPtr(reinterpret_cast<const void*>(entriesPtr), 24))
                return result;

            const std::size_t ENTRY_SIZE = 24;
            auto* entryBase = reinterpret_cast<const std::uint8_t*>(entriesPtr);
            auto img = GetImageBounds();

            for (std::uint32_t i = 0; i < capacity; ++i) {
                auto* entry = entryBase + i * ENTRY_SIZE;

                if (i % 4096 == 0) {
                    std::size_t remaining = (capacity - i) * ENTRY_SIZE;
                    if (IsBadReadPtr(entry,
                        static_cast<UINT_PTR>(std::min(remaining, std::size_t(4096 * ENTRY_SIZE)))))
                        break;
                }

                auto formPtr = *reinterpret_cast<const std::uintptr_t*>(entry + 8);
                if (formPtr == 0 || formPtr == sentinel ||
                    formPtr < 0x10000 || formPtr > 0x00007FFFFFFFFFFF)
                    continue;

                auto* formObj = reinterpret_cast<const std::uint8_t*>(formPtr);
                if (IsBadReadPtr(formObj, 0x20)) continue;

                auto vtable = *reinterpret_cast<const std::uintptr_t*>(formObj);
                if (vtable < img.base || vtable >= img.end) continue;

                auto storedId = *reinterpret_cast<const RE::FormID*>(formObj + 0x14);
                result[storedId] = reinterpret_cast<RE::TESForm*>(
                    const_cast<std::uint8_t*>(formObj));
            }

            return result;
        }

        // ================================================================
        // Main cache population routine
        // ================================================================
        inline void PopulateCache()
        {
            logger::info("editor ID cache v1.18.0: VirtualQuery memory scan"sv);

            auto img = GetImageBounds();

            // ==============================================================
            // PHASE 1: VirtualQuery comprehensive scan for ALL TESForm objects
            // ==============================================================
            auto allForms = VirtualQueryScan();
            logger::info("  Phase 1: {} unique forms via VirtualQuery", allForms.size());

            // Fallback: if VQ found very few, also scan the hash table
            if (allForms.size() < 500) {
                logger::info("  Phase 1b: VQ found few forms, scanning hash table..."sv);
                auto htForms = ScanHashTableEntries();
                logger::info("  Phase 1b: {} forms from hash table", htForms.size());
                for (auto& [id, form] : htForms) {
                    allForms.try_emplace(id, form);
                }
                logger::info("  Phase 1 total: {} unique forms", allForms.size());
            }

            // Check for target form
            {
                auto it = allForms.find(0x12E46);
                if (it != allForms.end()) {
                    logger::info("  FOUND defaultUnarmedWeap 0x12E46 at {:p}",
                        (void*)it->second);
                } else {
                    logger::warn("  NOT FOUND defaultUnarmedWeap 0x12E46"sv);
                }
            }

            // Log FormID range + form type distribution
            if (!allForms.empty()) {
                RE::FormID minId = 0xFFFFFFFF, maxId = 0;
                std::unordered_map<std::uint8_t, std::size_t> typeCount;

                for (auto& [id, form] : allForms) {
                    if (id < minId) minId = id;
                    if (id > maxId) maxId = id;
                    auto ft = *reinterpret_cast<const std::uint8_t*>(
                        reinterpret_cast<std::uintptr_t>(form) + 0x1A);
                    typeCount[ft]++;
                }
                logger::info("  FormID range: 0x{:X} - 0x{:X}", minId, maxId);

                // Log types with >50 forms (sorted by count)
                std::vector<std::pair<std::uint8_t, std::size_t>> sorted(
                    typeCount.begin(), typeCount.end());
                std::sort(sorted.begin(), sorted.end(),
                    [](auto& a, auto& b) { return a.second > b.second; });
                for (auto& [ft, cnt] : sorted) {
                    if (cnt < 50) break;
                    logger::info("    type 0x{:02X}: {} forms", ft, cnt);
                }
            }

            g_formById = allForms;

            // ==============================================================
            // PHASE 2: Build editor ID cache using GetFormEditorID()
            // ==============================================================
            EditorIdMap newCache;
            std::size_t withEditorId = 0;
            std::size_t emptyEditorId = 0;
            std::size_t skippedBadVfunc = 0;
            std::size_t skippedBadString = 0;

            for (auto& [formId, form] : allForms) {
                if (!form) continue;

                // Validate vtable[0x32] (GetFormEditorID) is a real code pointer
                auto vtable = *reinterpret_cast<std::uintptr_t*>(form);
                auto vfuncSlot = vtable + 0x32 * 8; // index 50 = 0x190 from vtable
                if (vfuncSlot + 8 >= img.end) { ++skippedBadVfunc; continue; }
                auto getEditorIdFn = *reinterpret_cast<const std::uintptr_t*>(vfuncSlot);
                if (getEditorIdFn < img.base || getEditorIdFn >= img.end) {
                    ++skippedBadVfunc;
                    continue;
                }

                // Call GetFormEditorID — vtable validated, safe to call
                const char* eid = form->GetFormEditorID();
                if (!eid) { ++emptyEditorId; continue; }

                // Validate returned pointer (batch check, not per-char)
                if (IsBadReadPtr(eid, 4)) { ++skippedBadString; continue; }
                if (eid[0] == '\0') { ++emptyEditorId; continue; }

                // Validate string: ASCII printable, reasonable length
                bool valid = true;
                std::size_t len = 0;
                for (; len < 512 && eid[len] != '\0'; ++len) {
                    if (eid[len] < 0x20 || eid[len] > 0x7E) { valid = false; break; }
                }
                if (!valid || len == 0 || len >= 512) {
                    ++skippedBadString;
                    continue;
                }

                newCache.try_emplace(std::string(eid, len), form);
                ++withEditorId;

                if (withEditorId <= 20) {
                    logger::info("    '{}' -> 0x{:08X}", std::string_view(eid, len), formId);
                }
            }

            logger::info("  Phase 2: {} editor IDs, {} empty, {} bad vfunc, {} bad string",
                withEditorId, emptyEditorId, skippedBadVfunc, skippedBadString);

            // ==============================================================
            // Hardcoded fallback for critical editor IDs
            // ==============================================================
            static const std::pair<const char*, RE::FormID> knownIds[] = {
                { "PlayerRef",           0x00000014 },
                { "Player",              0x00000007 },
                { "LockPick",            0x0000000A },
                { "Gold001",             0x0000003B },
                { "defaultUnarmedWeap",  0x00012E46 },
                { "WeapTypeUnarmed",     0x00013F8D },
                { "Torch01",             0x0001D4EC },
            };

            for (auto& [eid, fid] : knownIds) {
                if (newCache.find(eid) != newCache.end()) continue;
                auto it = allForms.find(fid);
                if (it != allForms.end() && it->second) {
                    newCache.try_emplace(std::string(eid), it->second);
                    logger::info("  hardcoded: '{}' -> 0x{:08X} ({:p})",
                        eid, fid, (void*)it->second);
                }
            }

            logger::info("  editor ID cache: {} total entries", newCache.size());

            // ==============================================================
            // PHASE 3: Store cache + populate game maps
            // ==============================================================
            {
                std::unique_lock lock(g_cacheMutex);
                g_editorIdCache = std::move(newCache);
            }

            {
                std::shared_lock lock(g_cacheMutex);
                if (g_editorIdCache.empty()) {
                    logger::warn("editor ID cache: EMPTY, will retry later"sv);
                    return;
                }
            }

            // Insert into game's editor-ID-to-form BSTHashMap
            {
                const auto& [editorMap, editorLock] = RE::TESForm::GetAllFormsByEditorID();
                if (editorMap) {
                    const RE::BSWriteLockGuard wl{ editorLock };
                    std::shared_lock lock(g_cacheMutex);
                    editorMap->reserve(static_cast<std::uint32_t>(g_editorIdCache.size()));
                    std::size_t inserted = 0;
                    for (const auto& [editorId, form] : g_editorIdCache) {
                        RE::BSFixedString key(editorId.c_str());
                        auto [it, ok] = editorMap->emplace(std::move(key), form);
                        if (ok) ++inserted;
                    }
                    logger::info("  repopulated editor-ID map: {} entries", inserted);
                }
            }

            // Insert found forms into game's form-by-ID BSTHashMap
            if (allForms.size() > 200) {
                const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                if (formMap) {
                    const RE::BSWriteLockGuard wl{ formLock };
                    std::size_t inserted = 0;
                    for (auto& [formId, form] : allForms) {
                        if (!form) continue;
                        auto [it, ok] = formMap->emplace(formId, form);
                        if (ok) ++inserted;
                    }
                    logger::info("  repopulated form-by-ID map: +{} entries (total {})",
                        inserted, formMap->size());
                }
            }

            g_cachePopulated.store(true);
            logger::info("editor ID cache: DONE"sv);
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
        logger::info("editor ID cache patch enabled (v1.18.0 VirtualQuery scan)"sv);
    }
}
