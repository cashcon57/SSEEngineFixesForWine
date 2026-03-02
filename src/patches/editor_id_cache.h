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
// v1.20.0: Clean diagnostic + safe population (no VQ scan)
//
// FINDINGS (v1.14.0 → v1.19.0):
// - VQ scan finds ~210 objects with TESForm-like vtables, but calling
//   GetFormEditorID on them triggers R6025 (pure virtual function call)
//   because many are false positives with abstract vtable entries.
// - GameLookupFormByID returns NULL for all ESM forms at kDataLoaded.
// - formArrays in TESDataHandler all empty at kDataLoaded.
// - form-by-ID hash map has ~166 real engine forms.
// - Editor-ID hash map was NEVER checked — doing that now.
// - Immersive & Pure modlist works under Wine, GTS doesn't → mod-specific.
//
// This version:
// 1. Drops VQ scan entirely (caused R6025 from false positives)
// 2. Checks editor-ID map directly (first time!)
// 3. Pre-checks vtable[0x32] before GetFormEditorID calls
// 4. Installs _set_purecall_handler as safety net
// 5. Focuses on clean diagnostics to understand GTS-specific failure

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
        inline std::atomic<int> g_populationAttempts{ 0 };
        inline std::unordered_map<RE::FormID, RE::TESForm*> g_formById;

        // Purecall safety net — prevents R6025 from killing the process
        inline thread_local volatile bool g_purecallFlag = false;

        inline void __cdecl PurecallHandler()
        {
            g_purecallFlag = true;
            // NOTE: on MSVC, _purecall calls abort() after this returns.
            // On Wine's ucrtbase, behavior may differ. This is a best-effort
            // safety net; the real protection is the vtable pre-check.
        }

        inline RE::TESForm* LookupByEditorID(const std::string_view& a_editorID)
        {
            std::shared_lock lock(g_cacheMutex);
            auto it = g_editorIdCache.find(std::string(a_editorID));
            return it != g_editorIdCache.end() ? it->second : nullptr;
        }

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

        // SEH-isolated GetFormEditorID — prevents AV crash on bad pointers
        // MSVC: __try/__except cannot coexist with C++ destructors in same function
        inline const char* SafeGetEditorID(RE::TESForm* form) noexcept
        {
            __try {
                return form->GetFormEditorID();
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        // Full-safety GetFormEditorID: vtable pre-check + SEH
        // Returns nullptr if the form doesn't have a real GetFormEditorID override
        inline const char* SafeGetEditorIDChecked(RE::TESForm* form, ImageBounds img) noexcept
        {
            if (!form || IsBadReadPtr(form, 8)) return nullptr;

            // Read vtable pointer
            auto vtable = *reinterpret_cast<const std::uintptr_t*>(form);
            if (vtable < img.base || vtable >= img.end) return nullptr;

            // Pre-check: vtable[0x32] (GetFormEditorID) must be in game image.
            // If it points to _purecall in ucrtbase.dll, it's outside the image.
            auto* vtEntries = reinterpret_cast<const std::uintptr_t*>(vtable);
            if (IsBadReadPtr(vtEntries + 0x32, 8)) return nullptr;
            auto fnGetEditorID = vtEntries[0x32];
            if (fnGetEditorID < img.base || fnGetEditorID >= img.end) {
                return nullptr; // _purecall or CRT stub — skip
            }

            // SEH-protected call (handles remaining edge cases)
            return SafeGetEditorID(form);
        }

        // ================================================================
        // BSTHashMap raw reader — works on Wine where BSTHashMap layout
        // might have subtle differences but raw memory is readable
        // ================================================================
        struct HashMapStats {
            std::uint32_t capacity = 0;
            std::uint32_t freeSlots = 0;
            std::uint32_t used = 0;
        };

        inline HashMapStats ReadHashMapStats(const void* map)
        {
            HashMapStats stats{};
            if (!map || IsBadReadPtr(map, 0x30)) return stats;
            auto* raw = reinterpret_cast<const std::uint8_t*>(map);
            stats.capacity = *reinterpret_cast<const std::uint32_t*>(raw + 0x0C);
            stats.freeSlots = *reinterpret_cast<const std::uint32_t*>(raw + 0x10);
            if (stats.capacity > stats.freeSlots)
                stats.used = stats.capacity - stats.freeSlots;
            return stats;
        }

        // ================================================================
        // Diagnostic: dump state of all game form registries
        // ================================================================
        inline void DiagnosticDump()
        {
            auto img = GetImageBounds();
            logger::info("  image: 0x{:X} - 0x{:X} ({:.1f}MB)",
                img.base, img.end,
                static_cast<double>(img.end - img.base) / (1024.0 * 1024.0));

            // 1. Form-by-ID hash map
            {
                const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                if (formMap) {
                    auto stats = ReadHashMapStats(formMap);
                    logger::info("  form-by-ID map: ~{} entries (capacity={}, free={})",
                        stats.used, stats.capacity, stats.freeSlots);
                } else {
                    logger::warn("  form-by-ID map: NULL pointer!"sv);
                }
            }

            // 2. Editor-ID hash map (FIRST TIME checking this!)
            {
                const auto& [editorMap, editorLock] = RE::TESForm::GetAllFormsByEditorID();
                if (editorMap) {
                    auto stats = ReadHashMapStats(editorMap);
                    logger::info("  editor-ID map: ~{} entries (capacity={}, free={})",
                        stats.used, stats.capacity, stats.freeSlots);

                    // If the editor-ID map has entries, try looking up key editor IDs
                    if (stats.used > 0) {
                        logger::info("  editor-ID map has entries! Trying native lookups..."sv);
                    }
                } else {
                    logger::warn("  editor-ID map: NULL pointer!"sv);
                }
            }

            // 3. Native LookupByEditorID — uses game's editor-ID map directly
            {
                const char* testIds[] = {
                    "PlayerRef", "Player", "Gold001",
                    "defaultUnarmedWeap", "WeapTypeUnarmed", "Torch01",
                    "GameHour", "TimeScale", "Gammaybe",
                };
                for (auto eid : testIds) {
                    auto* form = RE::TESForm::LookupByEditorID(std::string_view(eid));
                    if (form) {
                        logger::info("  NativeLookup('{}'): {:p} formId=0x{:X} type=0x{:02X}",
                            eid, (void*)form, form->GetFormID(),
                            static_cast<std::uint8_t>(form->GetFormType()));
                    } else {
                        logger::info("  NativeLookup('{}'): NULL", eid);
                    }
                }
            }

            // 4. GameLookupFormByID for key forms
            {
                struct TestId { RE::FormID id; const char* name; };
                TestId tests[] = {
                    { 0x14,    "PlayerRef" },
                    { 0x07,    "Player" },
                    { 0x3B,    "Gold001" },
                    { 0x12E46, "defaultUnarmedWeap" },
                    { 0x13F8D, "WeapTypeUnarmed" },
                    { 0x1D4EC, "Torch01" },
                };
                for (auto& t : tests) {
                    auto* form = Patches::FormCaching::detail::GameLookupFormByID(t.id);
                    if (form) {
                        logger::info("  GameLookup({}, 0x{:X}): {:p}",
                            t.name, t.id, (void*)form);
                    } else {
                        logger::info("  GameLookup({}, 0x{:X}): NULL", t.name, t.id);
                    }
                }
            }

            // 5. formArrays sizes (raw offset approach)
            {
                auto* tdh = RE::TESDataHandler::GetSingleton();
                if (tdh) {
                    logger::info("  TDH address: {:p}", (void*)tdh);
                    auto tdhAddr = reinterpret_cast<std::uintptr_t>(tdh);
                    struct FA { std::uint8_t type; const char* name; };
                    FA types[] = {
                        { 0x04, "Keyword" }, { 0x29, "Weapon" }, { 0x1A, "Armor" },
                        { 0x2B, "NPC" }, { 0x3C, "Cell" }, { 0x3D, "Ref" },
                        { 0x05, "Action" }, { 0x15, "Static" }, { 0x31, "Quest" },
                    };
                    std::size_t totalForms = 0;
                    for (auto& fa : types) {
                        auto offset = 0x010 + static_cast<std::size_t>(fa.type) * 0x18;
                        auto* arrBase = reinterpret_cast<const std::uint8_t*>(tdhAddr + offset);
                        if (IsBadReadPtr(arrBase, 0x18)) continue;
                        auto dataPtr = *reinterpret_cast<const std::uintptr_t*>(arrBase);
                        auto cap = *reinterpret_cast<const std::uint32_t*>(arrBase + 0x08);
                        auto sz = *reinterpret_cast<const std::uint32_t*>(arrBase + 0x10);
                        totalForms += sz;
                        if (sz > 0 || cap > 0) {
                            logger::info("  formArrays[{}]: size={} cap={} data=0x{:X}",
                                fa.name, sz, cap, dataPtr);
                        }
                    }
                    if (totalForms == 0) {
                        logger::warn("  formArrays: ALL EMPTY (0 forms in checked types)"sv);
                    }
                }
            }

            // 6. Loaded file count (raw TDH scan — compiledFileCollection)
            {
                auto* tdh = RE::TESDataHandler::GetSingleton();
                if (tdh) {
                    // compiledFileCollection at TDH+0x0D48 (AE) contains:
                    //   +0x00: BSTArray<TESFile*> files
                    //   +0x18: BSTArray<TESFile*> smallFiles
                    // Each BSTArray is 0x18 bytes: data(8) + cap(4) + pad(4) + size(4)
                    auto tdhAddr = reinterpret_cast<std::uintptr_t>(tdh);
                    // Try a few likely offsets for the file arrays
                    // Standard Skyrim SE/AE: loadedMods at +0x0D38, loadedModCount at +0x0D50
                    // Just count non-zero entries in formArrays to estimate
                    std::size_t nonEmptyArrays = 0;
                    for (std::uint8_t t = 0; t < 0x8A; ++t) {
                        auto offset = 0x010 + static_cast<std::size_t>(t) * 0x18;
                        auto* arrBase = reinterpret_cast<const std::uint8_t*>(tdhAddr + offset);
                        if (IsBadReadPtr(arrBase, 0x18)) continue;
                        auto sz = *reinterpret_cast<const std::uint32_t*>(arrBase + 0x10);
                        if (sz > 0) ++nonEmptyArrays;
                    }
                    logger::info("  TDH formArrays: {}/138 types have entries", nonEmptyArrays);
                }
            }
        }

        // ================================================================
        // Scan BSTHashMap entries for real forms
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

            if (capacity == 0 || capacity > 0x1000000 || entriesPtr < 0x10000) return result;
            if (IsBadReadPtr(reinterpret_cast<const void*>(entriesPtr), 24)) return result;

            auto img = GetImageBounds();
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
                    formPtr < 0x10000 || formPtr > 0x7FFFFFFFFFFF) continue;
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
        // Scan the editor-ID hash map for existing entries
        // (po3_Tweaks or other plugins might have populated it)
        // ================================================================
        inline EditorIdMap ScanEditorIdMap(ImageBounds img)
        {
            EditorIdMap result;
            const auto& [editorMap, editorLock] = RE::TESForm::GetAllFormsByEditorID();
            if (!editorMap) return result;

            auto* mapRaw = reinterpret_cast<const std::uint8_t*>(editorMap);
            auto capacity = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x0C);
            auto sentinel = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x18);
            auto entriesPtr = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x28);

            if (capacity == 0 || capacity > 0x1000000 || entriesPtr < 0x10000) return result;
            if (IsBadReadPtr(reinterpret_cast<const void*>(entriesPtr), 24)) return result;

            // Editor-ID map entry: BSFixedString key(8 bytes) + TESForm* value(8 bytes) + next(8 bytes)
            // BSFixedString is 8 bytes: a pointer to a ref-counted string in the string pool
            auto* entryBase = reinterpret_cast<const std::uint8_t*>(entriesPtr);

            for (std::uint32_t i = 0; i < capacity; ++i) {
                auto* entry = entryBase + i * 24;
                if (i % 4096 == 0) {
                    if (IsBadReadPtr(entry, static_cast<UINT_PTR>(
                        std::min(std::size_t((capacity - i) * 24), std::size_t(4096 * 24)))))
                        break;
                }

                // BSFixedString key at offset 0
                auto keyPtr = *reinterpret_cast<const std::uintptr_t*>(entry);
                // TESForm* value at offset 8
                auto formPtr = *reinterpret_cast<const std::uintptr_t*>(entry + 8);

                if (formPtr == 0 || formPtr == sentinel ||
                    formPtr < 0x10000 || formPtr > 0x7FFFFFFFFFFF) continue;
                if (keyPtr == 0 || keyPtr < 0x10000) continue;

                // Validate form pointer
                if (IsBadReadPtr(reinterpret_cast<const void*>(formPtr), 0x20)) continue;
                auto vtable = *reinterpret_cast<const std::uintptr_t*>(formPtr);
                if (vtable < img.base || vtable >= img.end) continue;

                // Read the BSFixedString — it's a pointer to char data
                // BSFixedString::data() returns const char*
                if (IsBadReadPtr(reinterpret_cast<const void*>(keyPtr), 8)) continue;
                auto strPtr = reinterpret_cast<const char*>(keyPtr);
                if (IsBadReadPtr(strPtr, 1)) continue;
                if (strPtr[0] == '\0') continue;

                // Validate string
                std::size_t len = 0;
                bool valid = true;
                for (; len < 512 && strPtr[len] != '\0'; ++len) {
                    if (strPtr[len] < 0x20 || strPtr[len] > 0x7E) { valid = false; break; }
                }
                if (!valid || len == 0 || len >= 512) continue;

                auto* form = reinterpret_cast<RE::TESForm*>(const_cast<void*>(
                    reinterpret_cast<const void*>(formPtr)));
                result.try_emplace(std::string(strPtr, len), form);
            }

            return result;
        }

        // ================================================================
        // Main population routine
        // ================================================================
        inline void PopulateCache()
        {
            int attempt = ++g_populationAttempts;
            logger::info("editor ID cache v1.20.0: attempt {} (diagnostic + safe)"sv, attempt);

            auto img = GetImageBounds();

            // Install purecall handler as safety net
            auto prevHandler = _set_purecall_handler(PurecallHandler);

            // ==============================================================
            // PHASE 0: Diagnostics — understand the state of ALL registries
            // ==============================================================
            DiagnosticDump();

            // ==============================================================
            // PHASE 1: Collect forms from hash table ONLY (known-safe)
            // NO VQ scan — it finds false positives that trigger R6025.
            // ==============================================================
            auto htForms = ScanHashTableEntries();
            logger::info("  Phase 1: {} forms from form-by-ID hash table", htForms.size());

            for (auto& [id, form] : htForms) {
                g_formById.try_emplace(id, form);
            }

            // ==============================================================
            // PHASE 2: Check if editor-ID map already has entries
            // (po3_Tweaks hooks SetFormEditorID at kPostLoad — if ESM loading
            // completed, those entries should be here)
            // ==============================================================
            auto existingEditorIds = ScanEditorIdMap(img);
            logger::info("  Phase 2: {} entries in existing editor-ID map", existingEditorIds.size());

            if (existingEditorIds.size() > 100) {
                // Editor-ID map is populated! Use it directly.
                logger::info("  editor-ID map already populated — using existing entries"sv);
                if (existingEditorIds.size() <= 30) {
                    for (auto& [eid, form] : existingEditorIds) {
                        logger::info("    '{}' -> {:p}", eid, (void*)form);
                    }
                } else {
                    // Log a sample
                    std::size_t count = 0;
                    for (auto& [eid, form] : existingEditorIds) {
                        if (++count > 10) break;
                        logger::info("    '{}' -> {:p}", eid, (void*)form);
                    }
                    logger::info("    ... and {} more", existingEditorIds.size() - 10);
                }

                // Check for our target
                auto it = existingEditorIds.find("defaultUnarmedWeap");
                if (it != existingEditorIds.end()) {
                    logger::info("  FOUND 'defaultUnarmedWeap' in existing map: {:p}", (void*)it->second);
                } else {
                    logger::warn("  'defaultUnarmedWeap' NOT in existing editor-ID map"sv);
                }

                // Store in our cache
                {
                    std::unique_lock lock(g_cacheMutex);
                    g_editorIdCache = std::move(existingEditorIds);
                }
                g_cachePopulated.store(true);
                _set_purecall_handler(prevHandler);
                logger::info("editor ID cache: DONE ({} entries from existing map)"sv,
                    g_editorIdCache.size());
                return;
            }

            // ==============================================================
            // PHASE 3: Get editor IDs from hash table forms (safe)
            // These are real engine forms — vtable pre-check + SEH
            // ==============================================================
            EditorIdMap newCache;
            std::size_t withEditorId = 0;
            std::size_t skippedVtable = 0;
            std::size_t sehCaught = 0;
            std::size_t emptyEditorId = 0;
            std::size_t badString = 0;

            for (auto& [formId, form] : g_formById) {
                if (!form) continue;

                const char* eid = SafeGetEditorIDChecked(form, img);
                if (!eid) {
                    ++skippedVtable;
                    continue;
                }
                if (IsBadReadPtr(eid, 1)) { ++sehCaught; continue; }
                if (eid[0] == '\0') { ++emptyEditorId; continue; }

                // Validate string
                bool valid = true;
                std::size_t len = 0;
                for (; len < 512 && eid[len] != '\0'; ++len) {
                    if (eid[len] < 0x20 || eid[len] > 0x7E) { valid = false; break; }
                }
                if (!valid || len == 0 || len >= 512) { ++badString; continue; }

                newCache.try_emplace(std::string(eid, len), form);
                ++withEditorId;
                if (withEditorId <= 30) {
                    logger::info("    '{}' -> 0x{:08X}", std::string_view(eid, len), formId);
                }
            }

            logger::info("  Phase 3: {} editorIDs, {} vtableSkip, {} SEH, {} empty, {} badStr",
                withEditorId, skippedVtable, sehCaught, emptyEditorId, badString);

            // ==============================================================
            // PHASE 4: Store cache + populate game maps if we have entries
            // ==============================================================
            {
                std::unique_lock lock(g_cacheMutex);
                g_editorIdCache = std::move(newCache);
            }

            if (!g_editorIdCache.empty()) {
                // Populate game's editor-ID map
                const auto& [editorMap, editorLock] = RE::TESForm::GetAllFormsByEditorID();
                if (editorMap) {
                    const RE::BSWriteLockGuard wl{ editorLock };
                    std::shared_lock lock(g_cacheMutex);
                    std::size_t inserted = 0;
                    for (const auto& [editorId, form] : g_editorIdCache) {
                        RE::BSFixedString key(editorId.c_str());
                        auto [it, ok] = editorMap->emplace(std::move(key), form);
                        if (ok) ++inserted;
                    }
                    logger::info("  repopulated editor-ID map: +{} entries", inserted);
                }
                g_cachePopulated.store(true);
                logger::info("editor ID cache: DONE ({} entries)"sv, g_editorIdCache.size());
            } else {
                logger::warn("  editor ID cache: EMPTY — will retry at kPostLoadGame/kNewGame"sv);
            }

            // Restore purecall handler
            _set_purecall_handler(prevHandler);
            logger::info("editor ID cache: attempt {} complete"sv, attempt);
        }
    }

    inline void OnDataLoaded()
    {
        // On first call (kDataLoaded), always run.
        // On retry (kPostLoadGame/kNewGame), re-run if cache is empty or small.
        if (detail::g_cachePopulated.load() && detail::g_editorIdCache.size() > 100) {
            logger::info("editor ID cache: already populated ({} entries), skipping"sv,
                detail::g_editorIdCache.size());
            return;
        }

        // Clear previous attempt data for retry
        if (detail::g_populationAttempts.load() > 0) {
            detail::g_formById.clear();
            detail::g_cachePopulated.store(false);
        }

        detail::PopulateCache();
    }

    inline void Install()
    {
        logger::info("editor ID cache patch enabled (v1.20.0 diagnostic + safe)"sv);
    }
}
