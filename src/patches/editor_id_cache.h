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
//   5. AddFormToDataHandler (REL::ID 13693) is never called (inlined by compiler)
//
// v1.15.0: Deep diagnostics to find where ESM forms are actually stored

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

        // Wine-safe editor ID lookup
        inline RE::TESForm* LookupByEditorID(const std::string_view& a_editorID)
        {
            std::shared_lock lock(g_cacheMutex);
            auto it = g_editorIdCache.find(std::string(a_editorID));
            return it != g_editorIdCache.end() ? it->second : nullptr;
        }

        // ================================================================
        // Hex dump helper
        // ================================================================
        inline void HexDump(const char* label, const void* ptr, std::size_t len)
        {
            auto* raw = reinterpret_cast<const std::uint8_t*>(ptr);
            for (std::size_t off = 0; off < len; off += 16) {
                std::string hex;
                std::string ascii;
                for (std::size_t i = 0; i < 16 && (off + i) < len; ++i) {
                    char buf[4];
                    std::snprintf(buf, sizeof(buf), "%02X ", raw[off + i]);
                    hex += buf;
                    ascii += (raw[off + i] >= 0x20 && raw[off + i] <= 0x7E)
                        ? static_cast<char>(raw[off + i]) : '.';
                }
                logger::info("  {} +0x{:04X}: {} | {}", label, off, hex, ascii);
            }
        }

        inline void PopulateCache()
        {
            logger::info("editor ID cache v1.15.0: deep diagnostics"sv);

            EditorIdMap newCache;

            // ================================================================
            // DIAGNOSTIC 1: Hex dump TESDataHandler first 0x100 bytes
            // ================================================================
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (dh) {
                logger::info("  TESDataHandler ptr={:p}", (void*)dh);
                HexDump("DH", dh, 0x100);

                // Also dump around expected compiledFileCollection offset (0xD70)
                logger::info("  TESDataHandler at +0xD70 (expected compiledFileCollection):");
                HexDump("DH+D70", reinterpret_cast<const char*>(dh) + 0xD70, 0x40);
            }

            // ================================================================
            // DIAGNOSTIC 2: Log ALL 166 FormIDs from the form-by-ID map
            // ================================================================
            {
                const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                if (formMap) {
                    const RE::BSReadLockGuard rl{ formLock };
                    logger::info("  form-by-ID map: size={}", formMap->size());

                    std::size_t logged = 0;
                    for (auto& [id, form] : *formMap) {
                        if (!form) continue;
                        if (logged < 30) {
                            logger::info("    FormID=0x{:08X} type={} ptr={:p}",
                                id, static_cast<int>(form->GetFormType()), (void*)form);
                        }
                        ++logged;
                    }
                    logger::info("  form-by-ID map: iterated {} forms total", logged);
                }
            }

            // ================================================================
            // DIAGNOSTIC 3: Dump raw BSTHashMap structure at REL::ID 400507
            // ================================================================
            {
                // The form-by-ID map is at this relocation
                static REL::Relocation<void*> formMapReloc{ REL::ID(VAR_NUM(514351, 400507)) };
                auto mapAddr = formMapReloc.address();
                logger::info("  form-by-ID map reloc: addr=0x{:X}", mapAddr);
                HexDump("FormMap", reinterpret_cast<void*>(mapAddr), 0x40);
            }

            // ================================================================
            // DIAGNOSTIC 4: Scan TESDataHandler for ANY non-zero qwords
            // This tells us which offsets have data at all
            // ================================================================
            if (dh) {
                auto* raw = reinterpret_cast<const std::uint64_t*>(dh);
                logger::info("  Scanning TESDataHandler for non-zero qwords (up to +0x1800)..."sv);

                std::size_t nonZeroCount = 0;
                for (std::size_t off = 0; off < 0x1800 / 8; ++off) {
                    if (raw[off] != 0) {
                        if (nonZeroCount < 50) {
                            logger::info("    +0x{:04X}: 0x{:016X}", off * 8, raw[off]);
                        }
                        ++nonZeroCount;
                    }
                }
                logger::info("  TESDataHandler: {} non-zero qwords in first 0x1800 bytes", nonZeroCount);
            }

            // ================================================================
            // DIAGNOSTIC 5: Check if forms exist at known FormIDs via
            // scanning entries directly in the BSTHashMap
            // ================================================================
            {
                // Read the BSTHashMap structure manually
                // BSTScatterTable layout (confirmed from CommonLib):
                //   +0x00: pad(8) + pad(4) + capacity(4) + free(4) + good(4)  = 0x18
                //   +0x18: sentinel(8)                                          = 0x20
                //   +0x20: allocator.pad(8) + allocator.entries(8)              = 0x30
                // Entry layout for <FormID, TESForm*>:
                //   value.first (FormID, 4) + pad(4) + value.second (TESForm*, 8) + next(8) = 24 bytes

                static REL::Relocation<void*> formMapReloc{ REL::ID(VAR_NUM(514351, 400507)) };
                auto* mapRaw = reinterpret_cast<const std::uint8_t*>(formMapReloc.address());

                auto capacity = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x0C);
                auto free = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x10);
                auto good = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x14);
                auto sentinel = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x18);
                auto entries = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x28);

                logger::info("  BSTHashMap manual read: cap={} free={} good={} sentinel=0x{:X} entries=0x{:X}",
                    capacity, free, good, sentinel, entries);

                // Look for FormIDs 0x14 and 0x12E46 in the entries
                if (entries > 0x10000 && capacity > 0 && capacity < 0x1000000) {
                    auto* entryBase = reinterpret_cast<const std::uint8_t*>(entries);
                    std::size_t entrySize = 24; // FormID(4) + pad(4) + TESForm*(8) + next(8)

                    std::size_t occupied = 0;
                    RE::FormID minId = 0xFFFFFFFF, maxId = 0;
                    bool foundPlayer = false, foundWeapon = false;

                    for (std::uint32_t i = 0; i < capacity; ++i) {
                        auto* entry = entryBase + i * entrySize;
                        auto formId = *reinterpret_cast<const RE::FormID*>(entry);
                        auto formPtr = *reinterpret_cast<const std::uintptr_t*>(entry + 8);
                        auto nextPtr = *reinterpret_cast<const std::uintptr_t*>(entry + 16);

                        // An occupied entry has a non-sentinel next pointer or a valid form pointer
                        if (formPtr != 0 && formPtr != sentinel && formPtr > 0x10000) {
                            ++occupied;
                            if (formId < minId) minId = formId;
                            if (formId > maxId) maxId = formId;
                            if (formId == 0x14) foundPlayer = true;
                            if (formId == 0x12E46) foundWeapon = true;
                        }
                    }

                    logger::info("  BSTHashMap entries: {}/{} occupied, IDs range 0x{:X}-0x{:X}",
                        occupied, capacity, minId, maxId);
                    logger::info("  Found Player(0x14)={}, defaultUnarmedWeap(0x12E46)={}",
                        foundPlayer, foundWeapon);
                }
            }

            // ================================================================
            // DIAGNOSTIC 6: Try TESDataHandler file access with raw pointer math
            // CommonLib says compiledFileCollection is at +0xD70
            // compiledFileCollection.files (BSTArray<TESFile*>) at +0xD70
            // compiledFileCollection.smallFiles (BSTArray<TESFile*>) at +0xD70+0x18
            // But since our offsets are wrong, scan for TESFile pointers differently:
            // - Get module base and size
            // - Look for BSTArrays where elements are pointers to objects
            //   that have a printable string at +0x58 (TESFile::fileName)
            // ================================================================
            if (dh) {
                // Broader scan: scan every 8-byte aligned offset in first 0x1800 bytes
                // looking for pointer-to-array-of-pointers-to-TESFile patterns
                auto* raw = reinterpret_cast<const std::uint8_t*>(dh);
                logger::info("  Scanning TESDataHandler 0x1800 bytes for TESFile arrays..."sv);

                for (std::size_t off = 0; off < 0x1800; off += 8) {
                    auto dataPtr = *reinterpret_cast<const std::uintptr_t*>(raw + off);
                    // Next 4 bytes could be size, next 4 capacity
                    auto size = *reinterpret_cast<const std::uint32_t*>(raw + off + 8);
                    auto cap = *reinterpret_cast<const std::uint32_t*>(raw + off + 12);

                    if (dataPtr < 0x10000 || dataPtr > 0x00007FFFFFFFFFFF) continue;
                    if (size == 0 || size > 10000 || cap < size || cap > 100000) continue;

                    // Check first element: is it a valid pointer?
                    if (IsBadReadPtr(reinterpret_cast<const void*>(dataPtr), 8)) continue;
                    auto firstElem = *reinterpret_cast<const std::uintptr_t*>(dataPtr);
                    if (firstElem < 0x10000 || firstElem > 0x00007FFFFFFFFFFF) continue;

                    // Check if first element looks like a TESFile (+0x58 = fileName)
                    if (IsBadReadPtr(reinterpret_cast<const void*>(firstElem + 0x58), 8)) continue;
                    const char* possibleName = reinterpret_cast<const char*>(firstElem + 0x58);

                    bool hasPrintable = false;
                    bool allPrintable = true;
                    for (int i = 0; i < 8 && possibleName[i]; ++i) {
                        if (possibleName[i] >= 0x20 && possibleName[i] <= 0x7E) {
                            hasPrintable = true;
                        } else {
                            allPrintable = false;
                            break;
                        }
                    }

                    if (hasPrintable && allPrintable && possibleName[0] != '\0') {
                        char namePreview[48] = {};
                        for (int i = 0; i < 47 && possibleName[i] && possibleName[i] >= 0x20; ++i) {
                            namePreview[i] = possibleName[i];
                        }
                        logger::info("    +0x{:04X}: ptr=0x{:X} size={} cap={} elem[0]+0x58='{}'",
                            off, dataPtr, size, cap, namePreview);
                    }
                }
            }

            // ================================================================
            // DIAGNOSTIC 7: Try scanning a WIDER range of addresses near the
            // form-by-ID map for other BSTHashMaps with large entry counts
            // The game might use a separate map for ESM forms
            // ================================================================
            {
                static REL::Relocation<void*> formMapReloc{ REL::ID(VAR_NUM(514351, 400507)) };
                auto baseAddr = formMapReloc.address();

                logger::info("  Scanning for large BSTHashMaps near form-by-ID map (base=0x{:X})...", baseAddr);

                // Scan +/- 0x1000 bytes around the form-by-ID map
                for (std::intptr_t delta = -0x1000; delta <= 0x1000; delta += 0x30) {
                    auto addr = baseAddr + delta;
                    auto* mapRaw = reinterpret_cast<const std::uint8_t*>(addr);

                    auto capacity = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x0C);
                    auto free = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x10);
                    auto good = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x14);
                    auto entriesPtr = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x28);

                    // Valid BSTHashMap: power-of-2 capacity, free+good=capacity, valid entries ptr
                    if (capacity >= 64 && capacity <= 0x1000000 &&
                        (capacity & (capacity - 1)) == 0 &&  // power of 2
                        free + good == capacity &&
                        entriesPtr > 0x10000 && entriesPtr < 0x00007FFFFFFFFFFF) {

                        logger::info("    delta={:+05X} (0x{:X}): cap={} free={} good={} entries=0x{:X}",
                            delta, addr, capacity, free, good, entriesPtr);
                    }
                }
            }

            // ================================================================
            // POPULATION PHASE: Try all available sources
            // ================================================================

            // Source 1: CommonLib iteration of form-by-ID map
            {
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

            // Source 2: If we found forms but no editor IDs, try hardcoded mapping
            if (newCache.empty()) {
                // Try to use GameLookupFormByID for known forms
                // Note: This only works for forms IN the BSTHashMap (engine forms)
                static const std::pair<const char*, RE::FormID> knownEditorIds[] = {
                    { "Player", 0x00000007 },
                    { "PlayerRef", 0x00000014 },
                    { "LockPick", 0x0000000A },
                    { "Gold001", 0x0000003B },
                    { "defaultUnarmedWeap", 0x00012E46 },
                    { "WeapTypeUnarmed", 0x00013F8D },
                };

                for (auto& [editorId, formId] : knownEditorIds) {
                    auto* form = Patches::FormCaching::detail::GameLookupFormByID(formId);
                    if (form) {
                        newCache.try_emplace(std::string(editorId), form);
                        logger::info("  Hardcoded via GameLookup: '{}' -> 0x{:08X} ({:p})",
                            editorId, formId, (void*)form);
                    }
                }
                logger::info("editor ID cache: {} hardcoded editor IDs resolved via GameLookup", newCache.size());
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
        logger::info("editor ID cache patch enabled (v1.15.0 deep diagnostics)"sv);
    }
}
