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
// v1.19.0: Diagnostics + solution
// v1.18.0 VQ scan found only 194 forms (364K vtable-valid objects but almost none
// pass FormID/formType checks). This means ESM forms either have a different layout
// or are stored in structures we haven't examined.
//
// This version:
// 1. Tests GameLookupFormByID to see if the engine CAN find forms
// 2. Scans TDH+0xD00..+0x1800 for BSTArray<TESForm*> patterns
// 3. For every VQ vtable hit, specifically searches for FormID 0x12E46
// 4. SEH-protected GetFormEditorID to prevent false-positive crashes

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

        // SEH-isolated GetFormEditorID — prevents crash on false positives
        // MSVC: __try/__except cannot coexist with C++ destructors in same function
        inline const char* SafeGetEditorID(RE::TESForm* form) noexcept
        {
            __try {
                return form->GetFormEditorID();
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        // ================================================================
        // Check if a pointer looks like a TESForm
        // ================================================================
        inline bool IsLikelyForm(const void* ptr, ImageBounds img)
        {
            if (!ptr || IsBadReadPtr(ptr, 0x20)) return false;
            auto vtable = *reinterpret_cast<const std::uintptr_t*>(ptr);
            if (vtable < img.base || vtable >= img.end) return false;
            if (vtable + 0x200 >= img.end) return false;
            // Check vtable[0] is a code pointer
            auto vf0 = *reinterpret_cast<const std::uintptr_t*>(vtable);
            if (vf0 < img.base || vf0 >= img.end) return false;
            return true;
        }

        // ================================================================
        // PHASE 0: Key experiments — can the game find forms?
        // ================================================================
        inline void RunExperiments()
        {
            auto img = GetImageBounds();
            logger::info("  image: 0x{:X} - 0x{:X} ({:.1f}MB)",
                img.base, img.end,
                static_cast<double>(img.end - img.base) / (1024.0 * 1024.0));

            // Experiment A: GameLookupFormByID for key FormIDs
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
                        auto vtable = *reinterpret_cast<std::uintptr_t*>(form);
                        auto storedId = *reinterpret_cast<std::uint32_t*>(
                            reinterpret_cast<std::uintptr_t>(form) + 0x14);
                        logger::info("  GameLookup({}, 0x{:X}): {:p} vtable=0x{:X} storedId=0x{:X}",
                            t.name, t.id, (void*)form, vtable, storedId);
                    } else {
                        logger::info("  GameLookup({}, 0x{:X}): NULL", t.name, t.id);
                    }
                }
            }

            // Experiment B: CommonLib formArrays — log sizes for key types
            {
                auto* tdh = RE::TESDataHandler::GetSingleton();
                if (tdh) {
                    logger::info("  TDH address: {:p}", (void*)tdh);
                    // Try accessing formArrays through CommonLib's typed API
                    // FormType enum indices from CommonLib:
                    // Keyword=0x04, Weapon=0x29, Armor=0x1A, NPC=0x2B, Cell=0x3C, Ref=0x3D
                    struct FATtest { std::uint8_t type; const char* name; };
                    FATtest fatests[] = {
                        { 0x04, "Keyword" }, { 0x29, "Weapon" }, { 0x1A, "Armor" },
                        { 0x2B, "NPC" }, { 0x3C, "Cell" }, { 0x3D, "Ref" },
                    };
                    auto tdhAddr = reinterpret_cast<std::uintptr_t>(tdh);
                    // formArrays at +0x010 per CommonLib, each BSTArray is 0x18 bytes
                    for (auto& fa : fatests) {
                        auto offset = 0x010 + static_cast<std::size_t>(fa.type) * 0x18;
                        auto* arrBase = reinterpret_cast<const std::uint8_t*>(tdhAddr + offset);
                        if (IsBadReadPtr(arrBase, 0x18)) {
                            logger::info("  formArrays[{}]: UNREADABLE at +0x{:X}", fa.name, offset);
                            continue;
                        }
                        auto dataPtr = *reinterpret_cast<const std::uintptr_t*>(arrBase);
                        auto cap = *reinterpret_cast<const std::uint32_t*>(arrBase + 0x08);
                        auto sz = *reinterpret_cast<const std::uint32_t*>(arrBase + 0x10);
                        logger::info("  formArrays[{}] at +0x{:X}: data=0x{:X} cap={} size={}",
                            fa.name, offset, dataPtr, cap, sz);
                    }
                }
            }

            // Experiment C: Scan TDH+0xD00..+0x1800 for BSTArray-like patterns
            {
                auto* tdh = RE::TESDataHandler::GetSingleton();
                if (!tdh) return;
                auto tdhAddr = reinterpret_cast<std::uintptr_t>(tdh);
                logger::info("  TDH deep scan (+0xD00..+0x1800):"sv);

                std::size_t arraysFound = 0;
                std::size_t arraysWithForms = 0;
                std::size_t totalFormsInArrays = 0;

                for (std::size_t off = 0xD00; off + 0x18 <= 0x1800; off += 0x18) {
                    auto* arrBase = reinterpret_cast<const std::uint8_t*>(tdhAddr + off);
                    if (IsBadReadPtr(arrBase, 0x18)) continue;

                    auto dataPtr = *reinterpret_cast<const std::uintptr_t*>(arrBase);
                    auto cap = *reinterpret_cast<const std::uint32_t*>(arrBase + 0x08);
                    auto sz = *reinterpret_cast<const std::uint32_t*>(arrBase + 0x10);

                    // Is this a valid BSTArray? data!=0, 0 < size <= cap < 10M
                    if (dataPtr == 0 || cap == 0 || sz == 0) continue;
                    if (sz > cap || cap > 10000000) continue;
                    if (IsBadReadPtr(reinterpret_cast<const void*>(dataPtr), 8)) continue;

                    ++arraysFound;

                    // Try reading first 3 entries as pointers, check if they're TESForm
                    std::size_t formCount = 0;
                    std::size_t checkCount = std::min(sz, std::uint32_t(3));
                    for (std::size_t i = 0; i < checkCount; ++i) {
                        auto entryAddr = dataPtr + i * 8;
                        if (IsBadReadPtr(reinterpret_cast<const void*>(entryAddr), 8)) break;
                        auto ptr = *reinterpret_cast<const std::uintptr_t*>(entryAddr);
                        if (ptr == 0) continue;
                        if (IsLikelyForm(reinterpret_cast<const void*>(ptr), img)) {
                            ++formCount;
                        }
                    }

                    if (formCount > 0) {
                        ++arraysWithForms;
                        totalFormsInArrays += sz;
                        // Log this array
                        if (arraysWithForms <= 10) {
                            // Read first entry's FormID
                            auto firstPtr = *reinterpret_cast<const std::uintptr_t*>(dataPtr);
                            std::uint32_t firstFormId = 0;
                            std::uint8_t firstFormType = 0xFF;
                            if (firstPtr && !IsBadReadPtr(reinterpret_cast<const void*>(firstPtr), 0x20)) {
                                firstFormId = *reinterpret_cast<const std::uint32_t*>(firstPtr + 0x14);
                                firstFormType = *reinterpret_cast<const std::uint8_t*>(firstPtr + 0x1A);
                            }
                            logger::info("    +0x{:X}: {} entries (cap {}), first form: id=0x{:X} type=0x{:02X}",
                                off, sz, cap, firstFormId, firstFormType);
                        }
                    }
                }

                logger::info("  TDH scan: {} arrays, {} with forms, {} total form entries",
                    arraysFound, arraysWithForms, totalFormsInArrays);

                // If we found form arrays, iterate ALL of them to collect forms
                if (arraysWithForms > 0 && totalFormsInArrays > 200) {
                    logger::info("  Collecting forms from TDH arrays..."sv);
                    std::size_t collected = 0;
                    std::size_t dupes = 0;

                    for (std::size_t off = 0xD00; off + 0x18 <= 0x1800; off += 0x18) {
                        auto* arrBase = reinterpret_cast<const std::uint8_t*>(tdhAddr + off);
                        if (IsBadReadPtr(arrBase, 0x18)) continue;

                        auto dataPtr = *reinterpret_cast<const std::uintptr_t*>(arrBase);
                        auto cap = *reinterpret_cast<const std::uint32_t*>(arrBase + 0x08);
                        auto sz = *reinterpret_cast<const std::uint32_t*>(arrBase + 0x10);
                        if (dataPtr == 0 || cap == 0 || sz == 0) continue;
                        if (sz > cap || cap > 10000000) continue;

                        // Check first entry
                        if (IsBadReadPtr(reinterpret_cast<const void*>(dataPtr), sz * 8)) continue;

                        for (std::uint32_t i = 0; i < sz; ++i) {
                            auto ptr = *reinterpret_cast<const std::uintptr_t*>(dataPtr + i * 8);
                            if (ptr == 0) continue;
                            if (!IsLikelyForm(reinterpret_cast<const void*>(ptr), img)) continue;

                            auto fid = *reinterpret_cast<const std::uint32_t*>(ptr + 0x14);
                            if (fid == 0 || fid >= 0x05000000) continue;

                            auto* form = reinterpret_cast<RE::TESForm*>(ptr);
                            auto [it, ok] = g_formById.try_emplace(fid, form);
                            if (ok) ++collected;
                            else ++dupes;
                        }
                    }

                    logger::info("  TDH collection: {} new forms, {} duplicates", collected, dupes);

                    // Check for target
                    auto it = g_formById.find(0x12E46);
                    if (it != g_formById.end()) {
                        logger::info("  FOUND defaultUnarmedWeap 0x12E46 at {:p} (from TDH)!",
                            (void*)it->second);
                    }
                }
            }
        }

        // ================================================================
        // VQ scan with target-specific search + relaxed filters
        // ================================================================
        inline std::unordered_map<RE::FormID, RE::TESForm*> VirtualQueryScan()
        {
            std::unordered_map<RE::FormID, RE::TESForm*> result;
            result.reserve(100000);

            auto img = GetImageBounds();

            MEMORY_BASIC_INFORMATION mbi{};
            std::uintptr_t scanAddr = 0x10000;
            std::size_t regions = 0;
            std::size_t bytesScanned = 0;
            std::size_t vtableHits = 0;
            std::size_t targetFound = 0;       // 0x12E46 found at +0x14
            std::size_t targetAtOther = 0;     // 0x12E46 found at other offsets

            auto t0 = std::chrono::high_resolution_clock::now();

            while (VirtualQuery(reinterpret_cast<void*>(scanAddr), &mbi, sizeof(mbi)))
            {
                auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                auto regionSize = mbi.RegionSize;
                auto regionEnd = regionBase + regionSize;

                if (regionEnd <= regionBase) break;
                scanAddr = regionEnd;
                ++regions;

                if (regions % 500 == 0) {
                    auto elapsed = std::chrono::high_resolution_clock::now() - t0;
                    if (elapsed > std::chrono::seconds(20)) {
                        logger::warn("  VQ: 20s limit, {}MB, {} forms", bytesScanned >> 20, result.size());
                        break;
                    }
                }

                if (mbi.State != MEM_COMMIT) continue;
                if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
                DWORD readBits = PAGE_READONLY | PAGE_READWRITE |
                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                    PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
                if (!(mbi.Protect & readBits)) continue;

                // Skip image section (forms are heap-allocated)
                if (regionBase >= img.base && regionBase < img.end) continue;

                // NO size limit — scan all regions
                auto* base = reinterpret_cast<const std::uint8_t*>(regionBase);
                bytesScanned += regionSize;

                for (std::size_t off = 0; off + 0x20 <= regionSize; off += 8)
                {
                    auto vtable = *reinterpret_cast<const std::uintptr_t*>(base + off);
                    if (vtable < img.base || vtable >= img.end) continue;
                    if (vtable + 0x200 >= img.end) continue;

                    auto* vt = reinterpret_cast<const std::uintptr_t*>(vtable);
                    auto vf0 = vt[0], vf1 = vt[1], vf2 = vt[2];
                    if (vf0 < img.base || vf0 >= img.end) continue;
                    if (vf1 < img.base || vf1 >= img.end) continue;
                    if (vf2 < img.base || vf2 >= img.end) continue;

                    ++vtableHits;

                    // SPECIFIC TARGET SEARCH: check +0x14 for 0x12E46 regardless of formType
                    auto formId14 = *reinterpret_cast<const std::uint32_t*>(base + off + 0x14);
                    if (formId14 == 0x12E46) {
                        ++targetFound;
                        if (targetFound <= 3) {
                            auto ft = *(base + off + 0x1A);
                            logger::info("  ** 0x12E46 at +0x14: {:p} vtable=0x{:X} formType=0x{:02X}",
                                (void*)(base + off), vtable, ft);
                        }
                    }

                    // Also check other offsets for 0x12E46
                    for (int delta : {0x10, 0x18, 0x1C, 0x20}) {
                        if (off + delta + 4 > regionSize) break;
                        auto candidate = *reinterpret_cast<const std::uint32_t*>(base + off + delta);
                        if (candidate == 0x12E46 && delta != 0x14) {
                            ++targetAtOther;
                            if (targetAtOther <= 3) {
                                logger::info("  ** 0x12E46 at +0x{:X}: {:p} vtable=0x{:X}",
                                    delta, (void*)(base + off), vtable);
                            }
                        }
                    }

                    // Collect form with relaxed filter: no formType check
                    if (formId14 > 0 && formId14 < 0x05000000) {
                        result.try_emplace(formId14,
                            reinterpret_cast<RE::TESForm*>(
                                const_cast<std::uint8_t*>(base + off)));
                    }
                }
            }

            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - t0).count();

            logger::info("  VQ: {} regions, {}MB, {} vtable hits, {} unique forms, {}ms",
                regions, bytesScanned >> 20, vtableHits, result.size(), ms);
            logger::info("  VQ target: 0x12E46 found {} times at +0x14, {} at other offsets",
                targetFound, targetAtOther);

            return result;
        }

        // ================================================================
        // Fallback: scan BSTHashMap entries (from v1.17.0)
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
        // Main population routine
        // ================================================================
        inline void PopulateCache()
        {
            logger::info("editor ID cache v1.19.0: experiments + TDH scan"sv);

            auto img = GetImageBounds();

            // ==============================================================
            // PHASE 0: Key experiments
            // ==============================================================
            RunExperiments();

            // ==============================================================
            // PHASE 1: VQ scan (relaxed: no formType filter, no size limit)
            // ==============================================================
            auto vqForms = VirtualQueryScan();
            logger::info("  Phase 1: {} forms from VQ", vqForms.size());

            // Merge VQ results into g_formById (TDH scan may have added forms)
            for (auto& [id, form] : vqForms) {
                g_formById.try_emplace(id, form);
            }

            // Also scan hash table
            auto htForms = ScanHashTableEntries();
            for (auto& [id, form] : htForms) {
                g_formById.try_emplace(id, form);
            }
            logger::info("  Total forms collected: {}", g_formById.size());

            // Check for target
            {
                auto it = g_formById.find(0x12E46);
                if (it != g_formById.end()) {
                    logger::info("  FOUND defaultUnarmedWeap 0x12E46 at {:p}!",
                        (void*)it->second);
                } else {
                    logger::warn("  NOT FOUND defaultUnarmedWeap 0x12E46"sv);
                }
            }

            // ==============================================================
            // PHASE 2: Build editor ID cache (SEH-protected)
            // ==============================================================
            EditorIdMap newCache;
            std::size_t withEditorId = 0;
            std::size_t emptyEditorId = 0;
            std::size_t sehCaught = 0;
            std::size_t badString = 0;

            for (auto& [formId, form] : g_formById) {
                if (!form) continue;

                // Use SEH-protected wrapper
                const char* eid = SafeGetEditorID(form);
                if (!eid) { ++emptyEditorId; continue; }
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
                if (withEditorId <= 20) {
                    logger::info("    '{}' -> 0x{:08X}", std::string_view(eid, len), formId);
                }
            }

            logger::info("  Phase 2: {} editor IDs, {} empty, {} SEH caught, {} bad string",
                withEditorId, emptyEditorId, sehCaught, badString);

            // Hardcoded fallback
            static const std::pair<const char*, RE::FormID> knownIds[] = {
                { "PlayerRef", 0x14 }, { "Player", 0x07 }, { "Gold001", 0x3B },
                { "defaultUnarmedWeap", 0x12E46 }, { "WeapTypeUnarmed", 0x13F8D },
                { "Torch01", 0x1D4EC },
            };
            for (auto& [eid, fid] : knownIds) {
                if (newCache.find(eid) != newCache.end()) continue;
                auto it = g_formById.find(fid);
                if (it != g_formById.end() && it->second) {
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

            if (g_editorIdCache.empty()) {
                logger::warn("editor ID cache: EMPTY, will retry later"sv);
                return;
            }

            // Insert into game's editor-ID-to-form map
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

            // Insert forms into form-by-ID map
            if (g_formById.size() > 200) {
                const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                if (formMap) {
                    const RE::BSWriteLockGuard wl{ formLock };
                    std::size_t inserted = 0;
                    for (auto& [formId, form] : g_formById) {
                        if (!form) continue;
                        auto [it, ok] = formMap->emplace(formId, form);
                        if (ok) ++inserted;
                    }
                    logger::info("  repopulated form-by-ID map: +{} entries", inserted);
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
        logger::info("editor ID cache patch enabled (v1.19.0 experiments + TDH scan)"sv);
    }
}
