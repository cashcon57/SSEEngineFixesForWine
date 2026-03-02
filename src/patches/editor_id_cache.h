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
// Problem: Under Wine, BSFixedString cross-thread interning fails, breaking
// BSTHashMap<BSFixedString, TESForm*> lookups (pointer-based hash/equality).
//
// Fix: At kDataLoaded, read the game's editor ID BSTHashMap by directly
// iterating its raw memory (bypassing CommonLibSSE-NG's template which has
// issues under Wine). Extract all (editorId, TESForm*) pairs, then clear
// and re-insert with fresh BSFixedStrings created on the main thread so
// future lookups produce consistent pointer-based hashes.

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

        // Check if a raw address looks like a valid heap/data pointer
        inline bool isLikelyPointer(std::uintptr_t val)
        {
            // Windows x64 user space: 0x10000 to ~0x7FFFFFFFFFFF
            return val >= 0x10000 && val < 0x00007FFFFFFFFFFF;
        }

        // Read a C string from a BSFixedString stored at the given memory address.
        // BSFixedString is just a pointer (8 bytes) to a BSStringPool::Entry.
        // We reinterpret_cast to const BSFixedString* and call data() to get the string.
        // This does NOT construct or destruct a BSFixedString — no ref counting issues.
        inline const char* readBSFixedStringData(const void* bsfixedstr_memory)
        {
            auto poolEntryAddr = *reinterpret_cast<const std::uintptr_t*>(bsfixedstr_memory);
            if (!isLikelyPointer(poolEntryAddr))
                return nullptr;

            auto* bsfs = reinterpret_cast<const RE::BSFixedString*>(bsfixedstr_memory);
            return bsfs->data();
        }

        // Raw memory iteration of the editor ID BSTHashMap.
        //
        // BSTHashMap<BSFixedString, TESForm*> layout (CommonLibSSE-NG BSTScatterTable):
        //   +0x00: _pad00 (uint64_t, 8 bytes)
        //   +0x08: _pad08 (uint32_t, 4 bytes)
        //   +0x0C: _capacity (uint32_t) — total slots, always power of 2
        //   +0x10: _free (uint32_t) — free slot count
        //   +0x14: _good (uint32_t) — last free index
        //   +0x18: _sentinel (entry_type*, 8 bytes) — end-of-chain marker
        //   +0x20: _allocator._pad00 (uint64_t, 8 bytes)
        //   +0x28: _allocator._entries (byte*, 8 bytes) — entry array pointer
        //
        // Entry layout (24 bytes):
        //   +0x00: BSFixedString key (8 bytes = pointer to BSStringPool::Entry)
        //   +0x08: TESForm* value (8 bytes)
        //   +0x10: entry_type* next (8 bytes)
        //          nullptr = empty slot, any non-null = occupied (sentinel or chain)
        inline std::size_t RawIterateEditorIdMap(EditorIdMap& outCache)
        {
            const auto& [editorMap, editorLock] = RE::TESForm::GetAllFormsByEditorID();
            if (!editorMap) {
                logger::error("editor ID cache: GetAllFormsByEditorID returned null map"sv);
                return 0;
            }

            auto* raw = reinterpret_cast<const std::uint8_t*>(editorMap);

            // === DIAGNOSTIC: Dump raw memory ===
            logger::info("=== Editor ID BSTHashMap raw memory (address {:p}) ===", (void*)editorMap);
            for (int i = 0; i < 0x38; i += 8) {
                auto val = *reinterpret_cast<const std::uint64_t*>(raw + i);
                logger::info("  +0x{:02X}: 0x{:016X}", i, val);
            }

            // Read at CommonLib expected offsets
            auto capacity = *reinterpret_cast<const std::uint32_t*>(raw + 0x0C);
            auto freeCount = *reinterpret_cast<const std::uint32_t*>(raw + 0x10);
            auto good = *reinterpret_cast<const std::uint32_t*>(raw + 0x14);
            auto sentinelAddr = *reinterpret_cast<std::uintptr_t const*>(raw + 0x18);
            auto entriesAddr = *reinterpret_cast<std::uintptr_t const*>(raw + 0x28);

            logger::info("  CommonLib layout: capacity={}, free={}, good={}, size={}"sv,
                capacity, freeCount, good, capacity > freeCount ? capacity - freeCount : 0);
            logger::info("  sentinel=0x{:016X}, entries=0x{:016X}"sv, sentinelAddr, entriesAddr);
            logger::info("  DLL BSTScatterTableSentinel={:p}"sv,
                (void*)RE::detail::BSTScatterTableSentinel);

            bool layoutValid = capacity > 0 && capacity < 1000000 && isLikelyPointer(entriesAddr);

            // If primary layout invalid, try scanning for plausible alternative layouts
            if (!layoutValid) {
                logger::warn("  Primary layout invalid (cap={}, entries=0x{:X}), scanning alternatives..."sv,
                    capacity, entriesAddr);

                // Find power-of-2 uint32 as capacity and pointer-like uint64 as entries
                for (int eo = 0; eo <= 0x30 && !layoutValid; eo += 8) {
                    auto ptr = *reinterpret_cast<std::uintptr_t const*>(raw + eo);
                    if (!isLikelyPointer(ptr)) continue;

                    for (int co = 0; co <= 0x30; co += 4) {
                        if (co >= eo && co < eo + 8) continue; // overlaps with entries
                        auto cap = *reinterpret_cast<const std::uint32_t*>(raw + co);
                        if (cap > 0 && cap < 1000000 && (cap & (cap - 1)) == 0) {
                            logger::info("  Alt layout found: entries at +0x{:02X}, capacity at +0x{:02X}={}"sv,
                                eo, co, cap);
                            entriesAddr = ptr;
                            capacity = cap;
                            layoutValid = true;
                            break;
                        }
                    }
                }
            }

            if (!layoutValid) {
                logger::error("  Could not find valid BSTHashMap layout"sv);
                return 0;
            }

            // Iterate entries
            auto* entries = reinterpret_cast<const std::uint8_t*>(entriesAddr);
            constexpr std::size_t ENTRY_SIZE = 24;

            // Log first few entries for diagnostics
            for (std::uint32_t i = 0; i < std::min(capacity, static_cast<std::uint32_t>(5)); ++i) {
                auto* e = entries + (static_cast<std::size_t>(i) * ENTRY_SIZE);
                logger::info("  Entry[{}]: key=0x{:016X} value=0x{:016X} next=0x{:016X}"sv,
                    i,
                    *reinterpret_cast<const std::uint64_t*>(e + 0x00),
                    *reinterpret_cast<const std::uint64_t*>(e + 0x08),
                    *reinterpret_cast<const std::uint64_t*>(e + 0x10));
            }

            // Full scan
            std::size_t filledEntries = 0;
            std::size_t editorIdsFound = 0;

            for (std::uint32_t i = 0; i < capacity; ++i) {
                auto* entryRaw = entries + (static_cast<std::size_t>(i) * ENTRY_SIZE);
                auto nextAddr = *reinterpret_cast<std::uintptr_t const*>(entryRaw + 0x10);

                if (nextAddr == 0) continue; // empty slot
                ++filledEntries;

                auto* form = *reinterpret_cast<RE::TESForm* const*>(entryRaw + 0x08);
                if (!form) continue;

                const char* editorId = readBSFixedStringData(entryRaw + 0x00);
                if (editorId && editorId[0] != '\0') {
                    outCache.try_emplace(std::string(editorId), form);
                    ++editorIdsFound;
                    if (editorIdsFound <= 10) {
                        logger::info("  EditorID: '{}' -> FormID 0x{:08X}"sv,
                            editorId, form->GetFormID());
                    }
                }
            }

            logger::info("  Raw iteration: {} filled entries, {} editor IDs (of {} capacity)"sv,
                filledEntries, editorIdsFound, capacity);

            return editorIdsFound;
        }

        inline void PopulateCache()
        {
            logger::info("editor ID cache: building from loaded forms..."sv);

            // === Phase 1: Read editor IDs from the game's BSTHashMap via raw memory ===
            EditorIdMap newCache;
            std::size_t editorIdsFromRaw = RawIterateEditorIdMap(newCache);

            // === Diagnostic: Trampoline test with known FormIDs ===
            logger::info("=== Trampoline diagnostics ==="sv);
            const std::pair<RE::FormID, const char*> knownForms[] = {
                { 0x00000014, "PlayerRef" },
                { 0x0000000F, "Gold001" },
                { 0x00000007, "FormID_7" },
                { 0x00000001, "FormID_1" },
                { 0x00013938, "DragonPriest" },
            };
            for (auto& [fid, name] : knownForms) {
                auto* form = FormCaching::detail::GameLookupFormByID(fid);
                if (form) {
                    const char* eid = form->GetFormEditorID();
                    logger::info("  0x{:08X} ({}): FOUND type={} editorID='{}'",
                        fid, name, static_cast<int>(form->GetFormType()),
                        eid ? eid : "(null)");
                } else {
                    logger::info("  0x{:08X} ({}): NOT FOUND", fid, name);
                }
            }

            // === Diagnostic: Sharded cache count ===
            std::size_t shardedTotal = 0;
            for (auto& shard : FormCaching::detail::g_formCache) {
                std::shared_lock lock(shard.mutex);
                shardedTotal += shard.map.size();
            }
            logger::info("  Sharded form cache has {} total forms"sv, shardedTotal);

            // === Phase 1b: Fallback to brute-force FormID scan if raw iteration found nothing ===
            if (editorIdsFromRaw == 0) {
                logger::warn("editor ID cache: raw iteration found 0 editor IDs, trying brute-force scan..."sv);

                std::size_t formsScanned = 0;
                auto scanForm = [&](RE::FormID formId) {
                    auto* form = FormCaching::detail::GameLookupFormByID(formId);
                    if (!form) return false;
                    ++formsScanned;

                    const char* editorId = form->GetFormEditorID();
                    if (editorId && editorId[0] != '\0') {
                        newCache.try_emplace(std::string(editorId), form);
                    }
                    return true;
                };

                for (std::uint32_t masterIdx = 0; masterIdx <= 0xFD; ++masterIdx) {
                    const std::uint32_t prefix = masterIdx << 24;
                    std::uint32_t lastHit = 0;

                    for (std::uint32_t baseId = 0; baseId <= 0xFFF; ++baseId) {
                        if (scanForm(prefix | baseId))
                            lastHit = baseId;
                    }

                    if (lastHit >= 0xF00) {
                        std::uint32_t scanStart = 0x1000;
                        while (scanStart < 0x100000) {
                            bool foundAny = false;
                            for (std::uint32_t baseId = scanStart;
                                 baseId < scanStart + 0x1000 && baseId < 0x1000000;
                                 ++baseId)
                            {
                                if (scanForm(prefix | baseId)) {
                                    foundAny = true;
                                    lastHit = baseId;
                                }
                            }
                            if (!foundAny) break;
                            scanStart += 0x1000;
                        }
                    }
                }

                for (std::uint32_t eslIdx = 0; eslIdx < 0x1000; ++eslIdx) {
                    const std::uint32_t eslPrefix = 0xFE000000 | (eslIdx << 12);
                    for (std::uint32_t baseId = 0; baseId <= 0xFFF; ++baseId) {
                        scanForm(eslPrefix | baseId);
                    }
                }

                logger::info("editor ID cache: brute-force found {} forms, {} with editor IDs"sv,
                    formsScanned, newCache.size());
            }

            logger::info("editor ID cache: total {} editor IDs in cache"sv, newCache.size());

            // Store in our Wine-safe cache
            {
                std::unique_lock lock(g_cacheMutex);
                g_editorIdCache = std::move(newCache);
            }

            // === Phase 2: Repopulate the game's BSTHashMap with fresh BSFixedStrings ===
            //
            // All BSFixedStrings created here go through BSStringPool on the MAIN thread.
            // Future LookupByEditorID calls (also on the main thread) will create
            // BSFixedStrings that intern to the SAME pool entries, making the pointer-based
            // hash (BSCRC32) and equality (operator==) work correctly.
            const auto& [editorMap, editorLock] = RE::TESForm::GetAllFormsByEditorID();
            if (editorMap) {
                const RE::BSWriteLockGuard wl{ editorLock };

                // Clear existing entries (releases old BSFixedString references)
                editorMap->clear();

                {
                    std::shared_lock lock(g_cacheMutex);
                    if (!g_editorIdCache.empty()) {
                        editorMap->reserve(static_cast<std::uint32_t>(g_editorIdCache.size()));

                        std::size_t repopulated = 0;
                        for (const auto& [editorId, form] : g_editorIdCache) {
                            RE::BSFixedString key(editorId.c_str());
                            editorMap->emplace(std::move(key), form);
                            ++repopulated;
                        }
                        logger::info("editor ID cache: repopulated game map with {} entries"sv, repopulated);
                    } else {
                        logger::warn("editor ID cache: no editor IDs to repopulate"sv);
                    }
                }
                g_cachePopulated.store(true);
            } else {
                logger::warn("editor ID cache: game's editor ID map pointer is null"sv);
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
