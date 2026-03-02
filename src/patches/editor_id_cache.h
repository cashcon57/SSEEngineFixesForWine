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
// v1.16.0: Corrected diagnostics — deref BSTHashMap pointer, find FormID offset,
// investigate TESDataHandler+0xD00 region, scan for real form storage

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

        inline RE::TESForm* LookupByEditorID(const std::string_view& a_editorID)
        {
            std::shared_lock lock(g_cacheMutex);
            auto it = g_editorIdCache.find(std::string(a_editorID));
            return it != g_editorIdCache.end() ? it->second : nullptr;
        }

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
            logger::info("editor ID cache v1.16.0: corrected diagnostics"sv);

            EditorIdMap newCache;

            // ================================================================
            // DIAGNOSTIC 1: Read the ACTUAL BSTHashMap (deref the pointer!)
            // REL::ID 400507 stores a POINTER TO the BSTHashMap, not the map itself
            // ================================================================
            RE::BSTHashMap<RE::FormID, RE::TESForm*>* actualFormMap = nullptr;
            {
                const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                actualFormMap = formMap;
                if (formMap) {
                    logger::info("  form-by-ID map object at {:p}", (void*)formMap);
                    HexDump("FormMap", formMap, 0x30);
                    logger::info("  form-by-ID map .size() = {}", formMap->size());
                }
            }

            // ================================================================
            // DIAGNOSTIC 2: Find FormID offset within TESForm
            // Use PlayerRef (FormID 0x14) as reference
            // ================================================================
            auto* playerRef = Patches::FormCaching::detail::GameLookupFormByID(0x14);
            std::ptrdiff_t formIdOffset = -1;
            if (playerRef) {
                logger::info("  PlayerRef at {:p}, GetFormID()=0x{:08X}, GetFormType()={}",
                    (void*)playerRef, playerRef->GetFormID(),
                    static_cast<int>(playerRef->GetFormType()));

                HexDump("PlayerRef", playerRef, 0x30);

                // Scan for FormID value 0x14 in the object's first 0x40 bytes
                auto* raw = reinterpret_cast<const std::uint8_t*>(playerRef);
                for (std::ptrdiff_t off = 0; off <= 0x38; off += 4) {
                    auto val = *reinterpret_cast<const std::uint32_t*>(raw + off);
                    if (val == 0x14) {
                        logger::info("  Found FormID 0x14 at PlayerRef+0x{:02X}", off);
                        if (formIdOffset < 0) formIdOffset = off;
                    }
                }

                // Verify with another known form from the map
                const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                if (formMap) {
                    const RE::BSReadLockGuard rl{ formLock };
                    for (auto& [id, form] : *formMap) {
                        if (!form || id == 0x14) continue;
                        if (formIdOffset >= 0) {
                            auto storedId = *reinterpret_cast<const std::uint32_t*>(
                                reinterpret_cast<const std::uint8_t*>(form) + formIdOffset);
                            if (storedId == id) {
                                logger::info("  Verified FormID offset 0x{:02X}: form 0x{:08X} at {:p} matches",
                                    formIdOffset, id, (void*)form);
                            } else {
                                logger::warn("  FormID offset 0x{:02X} MISMATCH: expected 0x{:08X}, got 0x{:08X}",
                                    formIdOffset, id, storedId);
                            }
                        }
                        break;
                    }
                }
            }

            // ================================================================
            // DIAGNOSTIC 3: Investigate TESDataHandler+0xD00 region
            // This is where non-zero data starts (v1.15.0 showed 0x000-0xCFF all zeros)
            // ================================================================
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (dh) {
                auto* dhRaw = reinterpret_cast<const std::uint8_t*>(dh);
                logger::info("  TESDataHandler at {:p}", (void*)dh);
                logger::info("  Hex dump TESDataHandler +0xD00 to +0xDE0:");
                HexDump("DH+D00", dhRaw + 0xD00, 0xE0);

                // Dereference pointers at interesting offsets and dump what they point to
                struct PointerTarget {
                    std::size_t offset;
                    const char* label;
                };
                PointerTarget targets[] = {
                    { 0xD00, "DH+D00" },
                    { 0xD08, "DH+D08" },
                    { 0xD60, "DH+D60" },
                    { 0xD68, "DH+D68" },
                    { 0xDB0, "DH+DB0" },
                    { 0xDC0, "DH+DC0" },
                    { 0xEC0, "DH+EC0" },
                };

                for (auto& t : targets) {
                    auto ptr = *reinterpret_cast<const std::uintptr_t*>(dhRaw + t.offset);
                    if (ptr > 0x10000 && ptr < 0x00007FFFFFFFFFFF) {
                        if (!IsBadReadPtr(reinterpret_cast<const void*>(ptr), 0x30)) {
                            logger::info("  Deref {}: 0x{:X} ->", t.label, ptr);
                            HexDump(t.label, reinterpret_cast<const void*>(ptr), 0x30);
                        } else {
                            logger::info("  Deref {}: 0x{:X} -> UNREADABLE", t.label, ptr);
                        }
                    }
                }
            }

            // ================================================================
            // DIAGNOSTIC 4: TESDataHandler+0xDC0 analysis
            // v1.15.0 showed: +0xDC0=0x8659DF50 (form range), +0xDC8=0x800, +0xDD0=0x6B8
            // Could this be a BSTHashMap? Check: 0x800=2048 capacity, 0x6B8=1720
            // 2048 - 1720 = 328 (not 166). But let me read the ACTUAL structure
            // ================================================================
            if (dh) {
                auto* dhRaw = reinterpret_cast<const std::uint8_t*>(dh);

                // Read a potential BSTHashMap starting at various offsets near +0xDB0
                // BSTHashMap layout: pad(8) + pad(4) + cap(4) + free(4) + good(4) + sentinel(8) + alloc.pad(8) + entries(8)
                logger::info("  Trying BSTHashMap reads at various offsets in TESDataHandler..."sv);

                for (std::size_t start = 0xDA0; start <= 0xE00; start += 0x08) {
                    auto* mapRaw = dhRaw + start;
                    auto pad00 = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x00);
                    auto pad08 = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x08);
                    auto cap = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x0C);
                    auto free_ = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x10);
                    auto good = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x14);
                    auto sentinel = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x18);
                    auto allocPad = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x20);
                    auto entries = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x28);

                    // Check if this looks like a valid BSTHashMap
                    bool isPow2 = cap > 0 && (cap & (cap - 1)) == 0;
                    bool sizeMatch = (free_ + good == cap);

                    if (cap > 0 && isPow2 && sizeMatch && good > 10) {
                        logger::info("    +0x{:03X}: cap={} free={} good={} sentinel=0x{:X} entries=0x{:X}",
                            start, cap, free_, good, sentinel, entries);
                    }
                }
            }

            // ================================================================
            // DIAGNOSTIC 5: Scan for FormID 0x12E46 near known form addresses
            // The 166 forms are at 0x2398xxxx. Player is at 0x8659xxxx.
            // ESM forms might be in a nearby memory range.
            // ================================================================
            if (formIdOffset >= 0) {
                logger::info("  Scanning for FormID 0x12E46 using offset 0x{:02X}...", formIdOffset);

                // Scan memory ranges near known form addresses
                // Be VERY careful: only scan readable pages
                struct ScanRange {
                    std::uintptr_t start;
                    std::size_t size;
                    const char* label;
                };

                // Get the base range from known forms
                std::uintptr_t minFormAddr = 0xFFFFFFFFFFFFFFFF, maxFormAddr = 0;
                {
                    const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                    if (formMap) {
                        const RE::BSReadLockGuard rl{ formLock };
                        for (auto& [id, form] : *formMap) {
                            auto addr = reinterpret_cast<std::uintptr_t>(form);
                            if (addr < minFormAddr) minFormAddr = addr;
                            if (addr > maxFormAddr) maxFormAddr = addr;
                        }
                    }
                }

                logger::info("  Known form address range: 0x{:X} - 0x{:X}",
                    minFormAddr, maxFormAddr);

                // Scan a wider range around known forms
                // Forms are objects (probably 0x100-0x1000 bytes each), so step by 8 bytes
                // looking for the FormID at the known offset
                if (minFormAddr < maxFormAddr) {
                    // Scan from minFormAddr - 0x100000 to maxFormAddr + 0x100000
                    // But be careful with page boundaries
                    std::uintptr_t scanStart = (minFormAddr > 0x100000) ? (minFormAddr - 0x100000) : minFormAddr;
                    std::uintptr_t scanEnd = maxFormAddr + 0x100000;
                    std::size_t found = 0;

                    logger::info("  Scanning 0x{:X} - 0x{:X} for FormID 0x12E46 at offset +0x{:02X}...",
                        scanStart, scanEnd, formIdOffset);

                    // Step through in page-sized chunks, check readability
                    const std::size_t PAGE_SIZE = 0x1000;
                    for (std::uintptr_t page = scanStart & ~(PAGE_SIZE - 1); page < scanEnd; page += PAGE_SIZE) {
                        if (IsBadReadPtr(reinterpret_cast<const void*>(page), PAGE_SIZE)) continue;

                        // Scan this page for the FormID
                        for (std::size_t off = 0; off + formIdOffset + 4 <= PAGE_SIZE; off += 8) {
                            auto* candidate = reinterpret_cast<const std::uint8_t*>(page + off);
                            auto candidateId = *reinterpret_cast<const std::uint32_t*>(candidate + formIdOffset);

                            if (candidateId == 0x12E46) {
                                // Check if this looks like a valid form (has a vtable pointer)
                                auto vtable = *reinterpret_cast<const std::uintptr_t*>(candidate);
                                if (vtable > 0x140000000 && vtable < 0x150000000) {
                                    ++found;
                                    logger::info("    FOUND at 0x{:X}: vtable=0x{:X}",
                                        page + off, vtable);
                                    HexDump("Form0x12E46", candidate, 0x30);

                                    // Try calling GetFormType on it
                                    auto* asForm = reinterpret_cast<RE::TESForm*>(const_cast<std::uint8_t*>(candidate));
                                    try {
                                        auto formType = asForm->GetFormType();
                                        auto actualId = asForm->GetFormID();
                                        logger::info("    GetFormType()={}, GetFormID()=0x{:08X}",
                                            static_cast<int>(formType), actualId);
                                    } catch (...) {
                                        logger::warn("    GetFormType/GetFormID threw exception");
                                    }

                                    if (found >= 3) break;
                                }
                            }
                        }
                        if (found >= 3) break;
                    }

                    logger::info("  Scan complete: found {} candidates", found);

                    // Also try a broader scan if the first range didn't work
                    if (found == 0) {
                        // Try the 0x86xxxxxx range (where Player is)
                        auto playerAddr = reinterpret_cast<std::uintptr_t>(playerRef);
                        std::uintptr_t scan2Start = (playerAddr > 0x100000) ? (playerAddr - 0x100000) : playerAddr;
                        std::uintptr_t scan2End = playerAddr + 0x100000;

                        logger::info("  Scanning Player-range 0x{:X} - 0x{:X}...", scan2Start, scan2End);

                        for (std::uintptr_t page = scan2Start & ~(PAGE_SIZE - 1); page < scan2End; page += PAGE_SIZE) {
                            if (IsBadReadPtr(reinterpret_cast<const void*>(page), PAGE_SIZE)) continue;

                            for (std::size_t off = 0; off + formIdOffset + 4 <= PAGE_SIZE; off += 8) {
                                auto* candidate = reinterpret_cast<const std::uint8_t*>(page + off);
                                auto candidateId = *reinterpret_cast<const std::uint32_t*>(candidate + formIdOffset);

                                if (candidateId == 0x12E46) {
                                    auto vtable = *reinterpret_cast<const std::uintptr_t*>(candidate);
                                    if (vtable > 0x140000000 && vtable < 0x150000000) {
                                        ++found;
                                        logger::info("    FOUND at 0x{:X}: vtable=0x{:X}",
                                            page + off, vtable);
                                        HexDump("Form0x12E46", candidate, 0x30);
                                        if (found >= 3) break;
                                    }
                                }
                            }
                            if (found >= 3) break;
                        }
                        logger::info("  Player-range scan: found {} total", found);
                    }
                }
            }

            // ================================================================
            // POPULATION: Use whatever forms we can find
            // ================================================================
            {
                const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                if (formMap && formMap->size() > 0) {
                    const RE::BSReadLockGuard rl{ formLock };
                    for (auto& [id, form] : *formMap) {
                        if (!form) continue;
                        const char* eid = form->GetFormEditorID();
                        if (eid && eid[0] != '\0') {
                            newCache.try_emplace(std::string(eid), form);
                        }
                    }
                }
            }

            // Hardcoded fallback for forms in the BSTHashMap
            if (newCache.empty()) {
                static const std::pair<const char*, RE::FormID> knownEditorIds[] = {
                    { "Player", 0x00000007 },
                    { "PlayerRef", 0x00000014 },
                    { "LockPick", 0x0000000A },
                    { "Gold001", 0x0000003B },
                    { "defaultUnarmedWeap", 0x00012E46 },
                };

                for (auto& [editorId, formId] : knownEditorIds) {
                    auto* form = Patches::FormCaching::detail::GameLookupFormByID(formId);
                    if (form) {
                        newCache.try_emplace(std::string(editorId), form);
                        logger::info("  Hardcoded: '{}' -> 0x{:08X} ({:p})", editorId, formId, (void*)form);
                    }
                }
            }

            logger::info("editor ID cache: total {} editor IDs in cache"sv, newCache.size());

            {
                std::unique_lock lock(g_cacheMutex);
                g_editorIdCache = std::move(newCache);
            }

            {
                std::shared_lock lock(g_cacheMutex);
                if (g_editorIdCache.empty()) {
                    logger::warn("editor ID cache: no editor IDs found, will retry at next event"sv);
                    return;
                }
            }

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
        logger::info("editor ID cache patch enabled (v1.16.0 corrected diagnostics)"sv);
    }
}
