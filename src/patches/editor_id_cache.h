#pragma once

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "form_caching.h"
#include "memory_manager.h"
#include "../settings.h"

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
        // v1.21.3: Added relocation verification + brute-force form check
        // ================================================================
        inline void DiagnosticDump()
        {
            auto img = GetImageBounds();
            logger::info("  image: 0x{:X} - 0x{:X} ({:.1f}MB)",
                img.base, img.end,
                static_cast<double>(img.end - img.base) / (1024.0 * 1024.0));

            // ============================================================
            // 0. RELOCATION VERIFICATION — check if CommonLib IDs resolve
            //    to sane addresses on this AE version
            // ============================================================
            {
                // TDH singleton pointer
                REL::Relocation<void**> tdhReloc{ RELOCATION_ID(514141, 400269) };
                auto tdhRelocAddr = tdhReloc.address();
                logger::info("  RELOC TDH singleton: ID={} -> addr=0x{:X} (offset=0x{:X})",
#ifdef SKYRIM_AE
                    400269,
#else
                    514141,
#endif
                    tdhRelocAddr, tdhRelocAddr - img.base);

                // Check if relocation address is within image
                bool tdhInImage = (tdhRelocAddr >= img.base && tdhRelocAddr < img.end);
                logger::info("    reloc addr in image: {} (expected: YES for .data global)", tdhInImage);

                // Read the pointer stored there
                if (!IsBadReadPtr(reinterpret_cast<const void*>(tdhRelocAddr), 8)) {
                    auto tdhPtr = *reinterpret_cast<const std::uintptr_t*>(tdhRelocAddr);
                    bool ptrInImage = (tdhPtr >= img.base && tdhPtr < img.end);
                    logger::info("    dereferenced value: 0x{:X} (in image: {} — expected: NO for heap object)",
                        tdhPtr, ptrInImage);
                }

                // allForms map pointer
                REL::Relocation<void**> allFormsReloc{ RELOCATION_ID(514351, 400507) };
                auto allFormsRelocAddr = allFormsReloc.address();
                logger::info("  RELOC allForms: ID={} -> addr=0x{:X} (offset=0x{:X})",
#ifdef SKYRIM_AE
                    400507,
#else
                    514351,
#endif
                    allFormsRelocAddr, allFormsRelocAddr - img.base);

                if (!IsBadReadPtr(reinterpret_cast<const void*>(allFormsRelocAddr), 8)) {
                    auto mapPtr = *reinterpret_cast<const std::uintptr_t*>(allFormsRelocAddr);
                    bool mapInImage = (mapPtr >= img.base && mapPtr < img.end);
                    logger::info("    dereferenced value: 0x{:X} (in image: {})",
                        mapPtr, mapInImage);
                }

                // GetFormByNumericId function address (for comparison)
                REL::Relocation<void*> getFormReloc{ RELOCATION_ID(14461, 14617) };
                logger::info("  RELOC GetFormByNumericId: ID={} -> addr=0x{:X} (offset=0x{:X})",
#ifdef SKYRIM_AE
                    14617,
#else
                    14461,
#endif
                    getFormReloc.address(), getFormReloc.address() - img.base);
            }

            // 1. Form-by-ID hash map (via CommonLib)
            {
                const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                if (formMap) {
                    auto stats = ReadHashMapStats(formMap);
                    logger::info("  form-by-ID map (CommonLib): ~{} entries (capacity={}, free={}) at {:p}",
                        stats.used, stats.capacity, stats.freeSlots, (void*)formMap);
                } else {
                    logger::warn("  form-by-ID map: NULL pointer!"sv);
                }
            }

            // 2. Editor-ID hash map
            {
                const auto& [editorMap, editorLock] = RE::TESForm::GetAllFormsByEditorID();
                if (editorMap) {
                    auto stats = ReadHashMapStats(editorMap);
                    logger::info("  editor-ID map: ~{} entries (capacity={}, free={})",
                        stats.used, stats.capacity, stats.freeSlots);
                } else {
                    logger::warn("  editor-ID map: NULL pointer!"sv);
                }
            }

            // ============================================================
            // 3. BRUTE-FORCE FORM CHECK — use game's own GetFormByNumericId
            //    (via trampoline) to check if forms ACTUALLY exist.
            //    This bypasses CommonLib's potentially wrong relocation IDs.
            // ============================================================
            {
                logger::info("  --- brute-force form check via game function ---"sv);

                // Check specific known Skyrim.esm forms
                struct TestId { RE::FormID id; const char* name; };
                TestId specifics[] = {
                    { 0x14,    "PlayerRef" },
                    { 0x07,    "Player" },
                    { 0x3B,    "Gold001" },
                    { 0x1,     "Keyword:Null" },
                    { 0x2,     "LocationRefType:Null" },
                    { 0x12E46, "defaultUnarmedWeap" },
                    { 0x13F8D, "WeapTypeUnarmed" },
                    { 0x1D4EC, "Torch01" },
                };
                for (auto& t : specifics) {
                    auto* form = Patches::FormCaching::detail::GameLookupFormByID(t.id);
                    if (form) {
                        // Read form type from raw memory (offset 0x1A in TESForm)
                        auto formType = *reinterpret_cast<const std::uint8_t*>(
                            reinterpret_cast<const std::uint8_t*>(form) + 0x1A);
                        logger::info("  GameLookup({}, 0x{:X}): {:p} type=0x{:02X}",
                            t.name, t.id, (void*)form, formType);
                    } else {
                        logger::info("  GameLookup({}, 0x{:X}): NULL", t.name, t.id);
                    }
                }

                // Sweep ranges to count how many forms exist via game function
                // Range 0x001-0x0FF: hardcoded engine forms
                std::size_t found_0001_00FF = 0;
                for (RE::FormID id = 0x01; id <= 0xFF; ++id) {
                    if (Patches::FormCaching::detail::GameLookupFormByID(id))
                        ++found_0001_00FF;
                }
                logger::info("  GameLookup sweep 0x001-0x0FF: {}/255 found", found_0001_00FF);

                // Range 0x100-0xFFF: early Skyrim.esm forms
                std::size_t found_0100_0FFF = 0;
                for (RE::FormID id = 0x100; id <= 0xFFF; ++id) {
                    if (Patches::FormCaching::detail::GameLookupFormByID(id))
                        ++found_0100_0FFF;
                }
                logger::info("  GameLookup sweep 0x100-0xFFF: {}/3840 found", found_0100_0FFF);

                // Range 0x1000-0x1FFF: more Skyrim.esm forms
                std::size_t found_1000_1FFF = 0;
                for (RE::FormID id = 0x1000; id <= 0x1FFF; ++id) {
                    if (Patches::FormCaching::detail::GameLookupFormByID(id))
                        ++found_1000_1FFF;
                }
                logger::info("  GameLookup sweep 0x1000-0x1FFF: {}/4096 found", found_1000_1FFF);

                // Check if plugins are loaded (compile index 01 = Update.esm)
                std::size_t found_01 = 0;
                for (RE::FormID id = 0x01000000; id <= 0x01000FFF; ++id) {
                    if (Patches::FormCaching::detail::GameLookupFormByID(id))
                        ++found_01;
                }
                logger::info("  GameLookup sweep 0x01000000-0x01000FFF (Update.esm): {}/4096 found", found_01);

                // Compile index 02 = Dawnguard.esm
                std::size_t found_02 = 0;
                for (RE::FormID id = 0x02000000; id <= 0x02000FFF; ++id) {
                    if (Patches::FormCaching::detail::GameLookupFormByID(id))
                        ++found_02;
                }
                logger::info("  GameLookup sweep 0x02000000-0x02000FFF (Dawnguard.esm): {}/4096 found", found_02);

                // Compile index FE (light plugins / ESL)
                std::size_t found_FE = 0;
                for (RE::FormID id = 0xFE000000; id <= 0xFE000FFF; ++id) {
                    if (Patches::FormCaching::detail::GameLookupFormByID(id))
                        ++found_FE;
                }
                logger::info("  GameLookup sweep 0xFE000000-0xFE000FFF (ESL range): {}/4096 found", found_FE);

                // Total across all sweeps
                logger::info("  GameLookup total found: {}",
                    found_0001_00FF + found_0100_0FFF + found_1000_1FFF +
                    found_01 + found_02 + found_FE);
            }

            // 4. Native LookupByEditorID — uses CommonLib's editor-ID map
            {
                const char* testIds[] = {
                    "PlayerRef", "Player", "Gold001",
                    "defaultUnarmedWeap", "WeapTypeUnarmed", "Torch01",
                    "GameHour", "TimeScale",
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

            // 5. TESDataHandler diagnostics
            {
                auto* tdh = RE::TESDataHandler::GetSingleton();
                if (tdh) {
                    auto tdhAddr = reinterpret_cast<std::uintptr_t>(tdh);
                    bool tdhInImage = (tdhAddr >= img.base && tdhAddr < img.end);
                    logger::info("  TDH address: {:p} (in image: {} — expected: NO)", (void*)tdh, tdhInImage);

                    if (tdhInImage) {
                        logger::warn("  TDH appears to be in the game image! This likely means");
                        logger::warn("  RELOCATION_ID(514141, 400269) resolves to the WRONG address.");
                        logger::warn("  The TDH singleton pointer might be stale/wrong for AE 1.6.1170.");
                    }

                    // Try both raw offset AND CommonLib typed accessor for compiledFileCollection
                    logger::info("  --- compiledFileCollection (raw offset vs typed) ---"sv);

                    // Raw offset approach
                    if (!IsBadReadPtr(reinterpret_cast<const void*>(tdhAddr + 0xD70), 0x30)) {
                        auto regCount = *reinterpret_cast<const std::uint32_t*>(tdhAddr + 0xD80);
                        auto eslCount = *reinterpret_cast<const std::uint32_t*>(tdhAddr + 0xD98);
                        logger::info("    raw offset: {} regular, {} ESL-flagged", regCount, eslCount);
                    }

                    // CommonLib typed accessor
                    auto& files = tdh->compiledFileCollection.files;
                    auto& smallFiles = tdh->compiledFileCollection.smallFiles;
                    logger::info("    typed accessor: {} regular, {} ESL-flagged",
                        files.size(), smallFiles.size());

                    // Also check the `files` BSSimpleList (at offset 0xD60)
                    // This is the list of ALL loaded TESFile objects
                    logger::info("  --- TDH files list (BSSimpleList at +0xD60) ---"sv);
                    std::size_t fileListCount = 0;
                    for (auto& file : tdh->files) {
                        if (file && fileListCount < 10) {
                            logger::info("    file: '{}'", file->fileName);
                        }
                        ++fileListCount;
                    }
                    logger::info("    total files in list: {}", fileListCount);

                    // formArrays scan (sample a few types)
                    struct FA { std::uint8_t type; const char* name; };
                    FA types[] = {
                        { 0x04, "Keyword" }, { 0x29, "Weapon" }, { 0x1A, "Armor" },
                        { 0x2B, "NPC" }, { 0x3C, "Cell" }, { 0x3D, "Ref" },
                    };
                    std::size_t totalForms = 0;
                    for (auto& fa : types) {
                        auto& arr = tdh->GetFormArray(static_cast<RE::FormType>(fa.type));
                        totalForms += arr.size();
                        if (arr.size() > 0 || arr.capacity() > 0) {
                            logger::info("  formArrays[{}]: size={} cap={}", fa.name, arr.size(), arr.capacity());
                        }
                    }
                    if (totalForms == 0) {
                        logger::warn("  formArrays: ALL EMPTY (0 forms in checked types)"sv);
                    }

                    // Count non-empty arrays
                    std::size_t nonEmptyArrays = 0;
                    for (std::uint8_t t = 0; t < 0x8A; ++t) {
                        auto& arr = tdh->GetFormArray(static_cast<RE::FormType>(t));
                        if (arr.size() > 0) ++nonEmptyArrays;
                    }
                    logger::info("  TDH formArrays: {}/138 types have entries", nonEmptyArrays);

                    // loadingFiles
                    logger::info("  loadingFiles: {}", tdh->loadingFiles);
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

            // Log details of the forms we found (first 30)
            if (htForms.size() > 0 && htForms.size() <= 300) {
                std::size_t logged = 0;
                for (auto& [id, form] : htForms) {
                    if (++logged > 30) break;
                    logger::info("    formID=0x{:08X} type=0x{:02X} addr={:p}",
                        id, static_cast<std::uint8_t>(form->GetFormType()), (void*)form);
                }
                if (htForms.size() > 30) {
                    logger::info("    ... and {} more forms", htForms.size() - 30);
                }
            }

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

    // ================================================================
    // v1.22.3: Verify plugins.txt is readable from within the game process
    // Uses Win32 APIs (same path resolution the game would use) to read
    // and parse plugins.txt, logging the result for diagnostics.
    // ================================================================
    inline void VerifyPluginsTxt()
    {
        logger::info("=== PLUGINS.TXT VERIFICATION ==="sv);

        // Get %LOCALAPPDATA% path (same as the game uses)
        WCHAR localAppData[MAX_PATH] = {};
        HRESULT hr = SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData);
        if (hr != S_OK) {
            logger::error("  SHGetFolderPathW(CSIDL_LOCAL_APPDATA) failed: hr=0x{:X}", static_cast<unsigned>(hr));
            return;
        }

        // Log the resolved path
        char pathNarrow[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_UTF8, 0, localAppData, -1, pathNarrow, sizeof(pathNarrow), NULL, NULL);
        logger::info("  LOCALAPPDATA = '{}'", pathNarrow);

        std::wstring path = localAppData;
        path += L"\\Skyrim Special Edition\\Plugins.txt";

        char pathNarrow2[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, pathNarrow2, sizeof(pathNarrow2), NULL, NULL);
        logger::info("  plugins.txt path = '{}'", pathNarrow2);

        // Open the file using Win32 API (same as the game would)
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        if (hFile == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            logger::error("  CreateFileW FAILED: error={} ({})",
                err, err == 2 ? "FILE_NOT_FOUND" :
                     err == 3 ? "PATH_NOT_FOUND" :
                     err == 5 ? "ACCESS_DENIED" : "other");
            return;
        }

        DWORD fileSize = GetFileSize(hFile, NULL);
        logger::info("  file size: {} bytes", fileSize);

        if (fileSize == 0 || fileSize > 10 * 1024 * 1024) {
            logger::error("  file size abnormal, skipping read");
            CloseHandle(hFile);
            return;
        }

        // Read entire file
        std::vector<char> buf(fileSize + 1, 0);
        DWORD bytesRead = 0;
        if (!ReadFile(hFile, buf.data(), fileSize, &bytesRead, NULL)) {
            DWORD err = GetLastError();
            logger::error("  ReadFile FAILED: error={}", err);
            CloseHandle(hFile);
            return;
        }
        CloseHandle(hFile);
        logger::info("  read {} bytes successfully", bytesRead);

        // Analyze content
        std::string content(buf.data(), bytesRead);

        // Check line endings
        bool hasCR = content.find('\r') != std::string::npos;
        bool hasLF = content.find('\n') != std::string::npos;
        logger::info("  line endings: CR={} LF={} ({})",
            hasCR, hasLF,
            hasCR && hasLF ? "CRLF" : (hasLF ? "LF only" : (hasCR ? "CR only" : "none")));

        // Parse lines
        std::istringstream stream(content);
        std::string line;
        std::size_t totalLines = 0;
        std::size_t enabledCount = 0;
        std::size_t disabledCount = 0;
        std::size_t commentCount = 0;
        std::size_t emptyCount = 0;
        std::string firstEnabled, lastEnabled;

        while (std::getline(stream, line)) {
            // Strip \r if present (handles both LF and CRLF)
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            ++totalLines;

            if (line.empty()) { ++emptyCount; continue; }
            if (line[0] == '#') { ++commentCount; continue; }

            if (line[0] == '*') {
                ++enabledCount;
                std::string name = line.substr(1);
                if (firstEnabled.empty()) firstEnabled = name;
                lastEnabled = name;
            } else {
                ++disabledCount;
            }
        }

        logger::info("  parsed: {} lines, {} enabled, {} disabled, {} comments, {} empty",
            totalLines, enabledCount, disabledCount, commentCount, emptyCount);

        if (!firstEnabled.empty())
            logger::info("  first enabled: '{}'", firstEnabled);
        if (!lastEnabled.empty())
            logger::info("  last enabled: '{}'", lastEnabled);

        if (enabledCount == 0)
            logger::error("  >>> ZERO enabled plugins! The game has nothing to load!");
        else
            logger::info("  plugins.txt looks valid: {} active plugins", enabledCount);

        logger::info("=== END PLUGINS.TXT VERIFICATION ==="sv);
    }

    // ================================================================
    // v1.22.0: Loading Timeline Monitor
    // Periodically logs the state of form loading during the ~51 second
    // load period. This answers: "Are forms ever created during loading,
    // or are they never created at all?"
    // ================================================================
    namespace LoadingMonitor
    {
        inline std::atomic<bool> g_running{ false };
        inline std::thread g_thread;
        inline std::atomic<bool> g_tdhSeen{ false };
        inline std::atomic<bool> g_loadingFilesEverTrue{ false };

        inline void MonitorThread()
        {
            logger::info("=== LOADING MONITOR STARTED (2s, then 200ms after TDH) ==="sv);
            int tick = 0;
            auto startTime = std::chrono::high_resolution_clock::now();

            while (g_running.load(std::memory_order_relaxed)) {
                // Before TDH appears: 2s intervals. After TDH: 200ms intervals.
                int sleepMs = g_tdhSeen.load(std::memory_order_relaxed) ? 200 : 2000;
                int sleepChunks = sleepMs / 50;
                for (int i = 0; i < sleepChunks && g_running.load(std::memory_order_relaxed); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                if (!g_running.load(std::memory_order_relaxed)) break;

                ++tick;
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - startTime).count();
                logger::info("--- MONITOR tick {} ({:.1f}s) ---", tick, elapsed / 1000.0);

                // Form pipeline counters (all 10 hooks)
                logger::info("  AddForm:{} OpenTES:{}/{} CompileIdx:{} SeekForm:{} Close:{} Init:{} Subrec:{} RdData:{}({:.1f}MB)",
                    Patches::FormCaching::detail::g_addFormCalls.load(std::memory_order_relaxed),
                    Patches::FormCaching::detail::g_openTESSuccesses.load(std::memory_order_relaxed),
                    Patches::FormCaching::detail::g_openTESCalls.load(std::memory_order_relaxed),
                    Patches::FormCaching::detail::g_addCompileIndexCalls.load(std::memory_order_relaxed),
                    Patches::FormCaching::detail::g_seekNextFormCalls.load(std::memory_order_relaxed),
                    Patches::FormCaching::detail::g_closeTESCalls.load(std::memory_order_relaxed),
                    Patches::FormCaching::detail::g_initFormDataCalls.load(std::memory_order_relaxed),
                    Patches::FormCaching::detail::g_seekNextSubrecordCalls.load(std::memory_order_relaxed),
                    Patches::FormCaching::detail::g_readDataCalls.load(std::memory_order_relaxed),
                    static_cast<double>(Patches::FormCaching::detail::g_readDataBytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0));

                // allForms hash map size (read-only, safe for concurrent access)
                {
                    const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                    if (formMap) {
                        auto stats = detail::ReadHashMapStats(formMap);
                        logger::info("  allForms: ~{} entries (cap={}, free={})",
                            stats.used, stats.capacity, stats.freeSlots);
                    }
                }

                // TDH state — detect first appearance + high-freq loadingFiles check
                auto* tdh = RE::TESDataHandler::GetSingleton();
                if (tdh) {
                    if (!g_tdhSeen.exchange(true)) {
                        logger::info("  >>> TDH FIRST SEEN at {:.1f}s! addr={:p}",
                            elapsed / 1000.0, (void*)tdh);
                    }

                    // Check loadingFiles at every tick (200ms after TDH appears)
                    bool isLoading = tdh->loadingFiles;
                    if (isLoading && !g_loadingFilesEverTrue.exchange(true)) {
                        logger::info("  >>> loadingFiles BECAME TRUE at {:.1f}s! <<<", elapsed / 1000.0);
                    }

                    auto tdhAddr = reinterpret_cast<std::uintptr_t>(tdh);
                    if (!IsBadReadPtr(reinterpret_cast<const void*>(tdhAddr + 0xD70), 0x30)) {
                        auto regCount = *reinterpret_cast<const std::uint32_t*>(tdhAddr + 0xD80);
                        auto eslCount = *reinterpret_cast<const std::uint32_t*>(tdhAddr + 0xD98);

                        // Count files in BSSimpleList (only occasionally to avoid overhead)
                        std::size_t fileCount = 0;
                        if (tick % 5 == 0) {
                            for (auto& file : tdh->files) {
                                if (file) ++fileCount;
                            }
                        }

                        logger::info("  compiled: {} reg + {} ESL | loadingFiles: {} | files: {}",
                            regCount, eslCount, isLoading,
                            tick % 5 == 0 ? std::to_string(fileCount) : std::string("(skip)"));
                    }
                } else {
                    // TDH doesn't exist yet — check raw pointer to detect transitions
                    REL::Relocation<void**> tdhReloc{ RELOCATION_ID(514141, 400269) };
                    auto rawPtr = *reinterpret_cast<const std::uintptr_t*>(tdhReloc.address());
                    logger::info("  TDH: null (raw ptr at reloc: 0x{:X})", rawPtr);
                }

                // Memory allocator stats (if active)
                if (Settings::Memory::bReplaceAllocator.GetValue()) {
                    auto allocated = Patches::WineMemoryManager::detail::g_totalAllocated.load(std::memory_order_relaxed);
                    auto allocs = Patches::WineMemoryManager::detail::g_allocationCount.load(std::memory_order_relaxed);
                    auto fails = Patches::WineMemoryManager::detail::g_failCount.load(std::memory_order_relaxed);
                    logger::info("  memory: {:.1f}MB allocated, {}K allocs, {} fails",
                        static_cast<double>(allocated) / (1024.0 * 1024.0),
                        allocs / 1000, fails);
                }
            }

            logger::info("=== LOADING MONITOR STOPPED ==="sv);
        }

        inline void Start()
        {
            if (g_running.exchange(true)) return; // already running
            g_thread = std::thread(MonitorThread);
        }

        inline void Stop()
        {
            g_running.store(false, std::memory_order_relaxed);
            if (g_thread.joinable()) {
                g_thread.join();
            }
        }

        // Log final loading pipeline state at kDataLoaded
        inline void LogFinalState()
        {
            logger::info("========== LOADING PIPELINE FINAL STATE =========="sv);
            logger::info("  AddFormToDataHandler: {} total calls ({} null)",
                Patches::FormCaching::detail::g_addFormCalls.load(),
                Patches::FormCaching::detail::g_addFormNullCalls.load());
            logger::info("  OpenTES: {} calls ({} successes)",
                Patches::FormCaching::detail::g_openTESCalls.load(),
                Patches::FormCaching::detail::g_openTESSuccesses.load());
            logger::info("  AddCompileIndex: {} calls",
                Patches::FormCaching::detail::g_addCompileIndexCalls.load());
            logger::info("  SeekNextForm: {} calls",
                Patches::FormCaching::detail::g_seekNextFormCalls.load());
            logger::info("  ClearData: {} calls",
                Patches::FormCaching::detail::g_clearDataCalls.load());
            logger::info("  InitializeFormDataStructures: {} calls",
                Patches::FormCaching::detail::g_initFormDataCalls.load());
            logger::info("  CloseTES: {} calls",
                Patches::FormCaching::detail::g_closeTESCalls.load());
            logger::info("  SeekNextSubrecord: {} calls",
                Patches::FormCaching::detail::g_seekNextSubrecordCalls.load());
            logger::info("  ReadData: {} calls ({:.1f}MB total)",
                Patches::FormCaching::detail::g_readDataCalls.load(),
                static_cast<double>(Patches::FormCaching::detail::g_readDataBytes.load()) / (1024.0 * 1024.0));
            logger::info("  loadingFiles ever true: {}",
                g_loadingFilesEverTrue.load());

            // Check compile indices of files in TDH
            auto* tdh = RE::TESDataHandler::GetSingleton();
            if (tdh) {
                std::size_t fileCount = 0;
                std::size_t compiledRegular = 0;   // compileIndex != 0xFF
                std::size_t compiledESL = 0;        // compileIndex == 0xFE and smallFileCompileIndex assigned
                std::size_t uncompiledCount = 0;    // compileIndex == 0xFF

                for (auto& file : tdh->files) {
                    if (!file) continue;
                    ++fileCount;

                    // Use CommonLib typed accessor for compile indices
                    auto compileIndex = file->compileIndex;
                    auto smallIndex = file->smallFileCompileIndex;

                    if (compileIndex != 0xFF) {
                        // File was assigned a compile index
                        if (compileIndex == 0xFE) {
                            ++compiledESL;
                        } else {
                            ++compiledRegular;
                        }
                        if ((compiledRegular + compiledESL) <= 10) {
                            logger::info("    COMPILED: '{}' compIdx=0x{:02X} smallIdx=0x{:04X}",
                                file->fileName, compileIndex, smallIndex);
                        }
                    } else {
                        ++uncompiledCount;
                        if (uncompiledCount <= 5) {
                            logger::info("    UNCOMPILED: '{}' compIdx=0xFF smallIdx=0x{:04X}",
                                file->fileName, smallIndex);
                        }
                    }
                }

                logger::info("  Files: {} total, {} compiled-regular, {} compiled-ESL, {} uncompiled (0xFF)",
                    fileCount, compiledRegular, compiledESL, uncompiledCount);

                if ((compiledRegular + compiledESL) == 0 && fileCount > 0) {
                    logger::warn("  >>> ZERO files have compile indices! The loading loop NEVER ASSIGNED them."sv);
                    logger::warn("  >>> This means the engine never processed plugin records."sv);
                }
            }

            logger::info("=================================================="sv);
        }
    }

    inline void Install()
    {
        logger::info("editor ID cache patch enabled (v1.20.0 diagnostic + safe)"sv);
    }
}
