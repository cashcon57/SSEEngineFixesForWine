#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "tree_lod_reference_caching.h"

// Wine-compatible form caching — replaces tbb::concurrent_hash_map with
// std::shared_mutex + std::unordered_map (256 shards by master index)

// notes on form caching
// 14688 - SetAt - inserts form to map, replaces if it already exists
// 14710 - RemoveAt - removes form from map
// g_FormMap xrefs
// 13689 - TESDataHandler::dtor - clears entire form map, called by TES::dtor, this only happens on game shutdown
// 13754 - TESDataHandler::ClearData - clears entire form map, called on game shutdown but also on Main::PerformGameReset, hook to clear our cache
// 13785 - HotLoadPlugin command handler - no one should be using this, so don't worry about it
// 14593 - TESForm::ctor - calls RemoveAt and SetAt, so handled by those hooks
// 14594 - TESForm::dtor_1 - deallocs the form map if there are zero forms
// 14617 - TESForm::GetFormByNumericId - form lookup, hooked
// 14627 - TESForm::RemoveFromDataStructures - calls RemoveAt, handled by that hook
// 14666 - TESForm::SetFormId - changes formid of form, if form is NOT temporary, removes old id from map and adds new one, calls SetAt/RemoveAt
// 441564 - TESForm::ReleaseFormDataStructures - deletes form map, inlined in TESForm dtors
// 14669 - TESForm::InitializeFormDataStructures - creates new empty form map, hook to clear our cache
// 14703 - TESForm::dtor_2 - deallocs the form map if there are zero forms
// 22839 - ConsoleFunc::Help - inlined form reader
// 22869 - ConsoleFunc::TestCode - inlined form reader
// 35865 - LoadGameCleanup - inlined form reader

namespace Patches::FormCaching
{
    namespace detail
    {
        // ================================================================
        // v1.22.0: Loading pipeline instrumentation counters
        // These track calls to critical loading functions to determine
        // whether forms are ever created during the 51-second load.
        // ================================================================
        inline std::atomic<std::uint64_t> g_clearDataCalls{ 0 };
        inline std::atomic<std::uint64_t> g_initFormDataCalls{ 0 };
        inline std::atomic<std::uint64_t> g_addFormCalls{ 0 };
        inline std::atomic<std::uint64_t> g_addFormNullCalls{ 0 };
        inline std::atomic<std::uint64_t> g_openTESCalls{ 0 };
        inline std::atomic<std::uint64_t> g_openTESSuccesses{ 0 };
        inline std::atomic<std::uint64_t> g_addCompileIndexCalls{ 0 };
        inline std::atomic<std::uint64_t> g_seekNextFormCalls{ 0 };
        inline std::atomic<std::uint64_t> g_closeTESCalls{ 0 };
        inline std::atomic<std::uint64_t> g_seekNextSubrecordCalls{ 0 };
        inline std::atomic<std::uint64_t> g_readDataCalls{ 0 };
        inline std::atomic<std::uint64_t> g_readDataBytes{ 0 };
        inline std::atomic<bool> g_seekNextFormReturnedFalse{ false };

        // v1.22.23: ForceLoadAllForms VEH support
        // g_forceLoadInProgress: true while AE 13753 is executing — suppresses
        // CloseTES hook's ManuallyCompileFiles call (prevents compiledFileCollection
        // iterator invalidation while AE 13753 iterates it).
        // g_vehSkipCount: number of null dereferences skipped by ForceLoadVEH.
        inline std::atomic<bool> g_forceLoadInProgress{ false };
        inline std::atomic<std::uint64_t> g_vehSkipCount{ 0 };

        // v1.22.27: Form-reference fixup VEH — persistent after ForceLoadAllForms.
        // Catches access violations where a register holds a raw form ID
        // (< 0x10000000) instead of a resolved pointer, resolves it via
        // TESForm_GetFormByNumericId, and continues execution.
        inline std::atomic<bool> g_formFixupActive{ false };
        inline std::atomic<std::uint64_t> g_formFixupCount{ 0 };
        // v1.22.27: Count of forms that faulted during InitItem (SEH-caught)
        inline std::atomic<std::uint64_t> g_initFaultCount{ 0 };
        // v1.22.36: Sentinel form page — a fake TESForm that looks "deleted"
        // to the engine. VEH redirects null form pointers here. The engine
        // reads kDeleted from formFlags and skips processing entirely.
        //
        // Layout (PAGE_READONLY, 64KB):
        //   [0x0000]: vtable ptr → g_stubVtable (array of "ret 0" stubs)
        //   [0x0010]: formFlags = 0x20 (kDeleted)
        //   [0x0014]: formID = 0
        //   [0x001A]: formType = 0
        //   [0x04B8]: stub function ptr (for direct function pointer fields)
        //   All other bytes: 0
        //
        // A companion "stub function" page (PAGE_EXECUTE_READ) holds
        // `xor eax, eax; ret` — all virtual/function pointer calls return 0.
        // A "stub vtable" page (PAGE_READONLY) holds 512 pointers to the stub.
        inline void* g_zeroPage = nullptr;
        inline void* g_stubFuncPage = nullptr;
        inline void* g_stubVtable = nullptr;
        inline DWORD64 g_zeroPageBase = 0;
        inline DWORD64 g_zeroPageEnd = 0;
        inline std::atomic<std::uint64_t> g_zeroPageUseCount{ 0 };
        inline std::atomic<std::uint64_t> g_zeroPageWriteSkips{ 0 };
        inline std::atomic<std::uint64_t> g_catchAllCount{ 0 };
        // Legacy sentinel (kept for FormReferenceFixupVEH compatibility)
        alignas(64) inline std::uint8_t g_sentinelForm[0x400] = {};
        inline std::atomic<std::uint64_t> g_sentinelUseCount{ 0 };

        // v1.22.10: Enhanced ClearData diagnostic — dumps files list state
        // when ClearData fires, to see what the engine knew about before compile
        inline void DumpFilesListState(RE::TESDataHandler* a_self, const char* context)
        {
            if (!a_self) return;

            std::size_t fileCount = 0;
            std::size_t masterCount = 0;
            std::size_t smallFileCount = 0;
            std::size_t activeCount = 0;
            std::size_t compiledCount = 0;

            for (auto& file : a_self->files) {
                if (!file) continue;
                ++fileCount;
                if (file->recordFlags.all(RE::TESFile::RecordFlag::kMaster))
                    ++masterCount;
                if (file->recordFlags.all(RE::TESFile::RecordFlag::kSmallFile))
                    ++smallFileCount;
                if (file->recordFlags.all(RE::TESFile::RecordFlag::kActive))
                    ++activeCount;
                if (file->compileIndex != 0xFF)
                    ++compiledCount;

                // Log first 20 files and last 5
                if (fileCount <= 20) {
                    logger::info("  [{}] {} '{}' flags=0x{:08X} compIdx=0x{:02X} smallIdx=0x{:04X}",
                        context, fileCount, file->fileName,
                        file->recordFlags.underlying(),
                        file->compileIndex, file->smallFileCompileIndex);
                }
            }

            // Log last 5 if there are more than 25
            if (fileCount > 25) {
                std::size_t idx = 0;
                for (auto& file : a_self->files) {
                    if (!file) continue;
                    ++idx;
                    if (idx > fileCount - 5) {
                        logger::info("  [{}] {} '{}' flags=0x{:08X} compIdx=0x{:02X} smallIdx=0x{:04X}",
                            context, idx, file->fileName,
                            file->recordFlags.underlying(),
                            file->compileIndex, file->smallFileCompileIndex);
                    }
                }
            }

            auto& cc = a_self->compiledFileCollection;
            logger::info("  [{}] SUMMARY: {} files, {} master, {} small, {} active, {} compiled, "
                         "compiledCollection={} reg + {} ESL, loadingFiles={}",
                context, fileCount, masterCount, smallFileCount, activeCount, compiledCount,
                cc.files.size(), cc.smallFiles.size(), a_self->loadingFiles);
        }

        struct ShardedCache
        {
            mutable std::shared_mutex mutex;
            std::unordered_map<std::uint32_t, RE::TESForm*> map;
        };

        inline ShardedCache g_formCache[256];
        inline SafetyHookInline g_hk_GetFormByNumericId{};

        inline RE::TESForm* TESForm_GetFormByNumericId(RE::FormID a_formId)
        {
            const std::uint8_t  masterId = (a_formId & 0xFF000000) >> 24;
            const std::uint32_t baseId = (a_formId & 0x00FFFFFF);

            auto& shard = g_formCache[masterId];

            // lookup form in our cache first (shared/read lock)
            {
                std::shared_lock lock(shard.mutex);
                auto it = shard.map.find(baseId);
                if (it != shard.map.end()) {
                    return it->second;
                }
            }

            // Call the GAME's native GetFormByNumericId via trampoline.
            // This uses the game's actual BSTHashMap code with the correct struct
            // layout, bypassing CommonLibSSE-NG's broken BSTHashMap template.
            RE::TESForm* formPointer = g_hk_GetFormByNumericId.call<RE::TESForm*>(a_formId);

            if (formPointer != nullptr) {
                std::unique_lock lock(shard.mutex);
                shard.map.emplace(baseId, formPointer);
            }

            return formPointer;
        }

        // Call the game's native GetFormByNumericId directly (via trampoline).
        // Used by editor_id_cache.h for brute-force form enumeration.
        inline RE::TESForm* GameLookupFormByID(RE::FormID a_formId)
        {
            return g_hk_GetFormByNumericId.call<RE::TESForm*>(a_formId);
        }

#ifdef SKYRIM_AE
        inline SafetyHookInline g_hk_RemoveAt{};

        inline std::uint64_t FormMap_RemoveAt(RE::BSTHashMap<RE::FormID, RE::TESForm*>* a_self, RE::FormID* a_formIdPtr, void* a_prevValueFunctor)
        {
            const auto result = g_hk_RemoveAt.call<std::uint64_t>(a_self, a_formIdPtr, a_prevValueFunctor);

            const std::uint8_t  masterId = (*a_formIdPtr & 0xFF000000) >> 24;
            const std::uint32_t baseId = (*a_formIdPtr & 0x00FFFFFF);

            if (result == 1) {
                {
                    std::unique_lock lock(g_formCache[masterId].mutex);
                    g_formCache[masterId].map.erase(baseId);
                }
                TreeLodReferenceCaching::detail::RemoveCachedForm(baseId);
            }

            return result;
        }

        static inline REL::Relocation<bool(std::uintptr_t a_self, RE::FormID* a_formIdPtr, RE::TESForm** a_valueFunctor, void* a_unk)> orig_FormScatterTable_SetAt;

        // the functor stores the TESForm to set as the first field
        inline bool FormScatterTable_SetAt(std::uintptr_t a_self, RE::FormID* a_formIdPtr, RE::TESForm** a_valueFunctor, void* a_unk)
        {
            const std::uint8_t  masterId = (*a_formIdPtr & 0xFF000000) >> 24;
            const std::uint32_t baseId = (*a_formIdPtr & 0x00FFFFFF);

            RE::TESForm* formPointer = *a_valueFunctor;

            if (formPointer != nullptr) {
                std::unique_lock lock(g_formCache[masterId].mutex);
                g_formCache[masterId].map.insert_or_assign(baseId, formPointer);

                TreeLodReferenceCaching::detail::RemoveCachedForm(baseId);
            }

            return orig_FormScatterTable_SetAt(a_self, a_formIdPtr, a_valueFunctor, a_unk);
        }
#else
        inline SafetyHookInline g_hk_RemoveAt{};

        inline std::uint32_t FormMap_RemoveAt(std::uintptr_t a_self, std::uintptr_t a_arg2, std::uint32_t a_crc, RE::FormID* a_formIdPtr, void* a_prevValueFunctor)
        {
            const auto result = g_hk_RemoveAt.call<std::uint32_t>(a_self, a_arg2, a_crc, a_formIdPtr, a_prevValueFunctor);

            const std::uint8_t  masterId = (*a_formIdPtr & 0xFF000000) >> 24;
            const std::uint32_t baseId = (*a_formIdPtr & 0x00FFFFFF);

            if (result == 1) {
                {
                    std::unique_lock lock(g_formCache[masterId].mutex);
                    g_formCache[masterId].map.erase(baseId);
                }
                TreeLodReferenceCaching::detail::RemoveCachedForm(baseId);
            }

            return result;
        }

        static inline REL::Relocation<bool(std::uintptr_t a_self, std::uintptr_t a_arg2, std::uint32_t a_crc, RE::FormID* a_formIdPtr, RE::TESForm** a_valueFunctor, void* a_unk)> orig_FormScatterTable_SetAt;

        // the functor stores the TESForm to set as the first field
        inline bool FormScatterTable_SetAt(std::uintptr_t a_self, std::uintptr_t a_arg2, std::uint32_t a_crc, RE::FormID* a_formIdPtr, RE::TESForm** a_valueFunctor, void* a_unk)
        {
            const std::uint8_t  masterId = (*a_formIdPtr & 0xFF000000) >> 24;
            const std::uint32_t baseId = (*a_formIdPtr & 0x00FFFFFF);

            RE::TESForm* formPointer = *a_valueFunctor;

            if (formPointer != nullptr) {
                std::unique_lock lock(g_formCache[masterId].mutex);
                g_formCache[masterId].map.insert_or_assign(baseId, formPointer);

                TreeLodReferenceCaching::detail::RemoveCachedForm(baseId);
            }

            return orig_FormScatterTable_SetAt(a_self, a_arg2, a_crc, a_formIdPtr, a_valueFunctor, a_unk);
        }
#endif

        inline SafetyHookInline g_hk_ClearData;

        // the game does not lock the form table on these clears so we won't either
        // maybe fix later if it causes issues
        inline void TESDataHandler_ClearData(RE::TESDataHandler* a_self)
        {
            auto count = g_clearDataCalls.fetch_add(1, std::memory_order_relaxed) + 1;
            logger::info(">>> ClearData called (call #{}) — dumping state BEFORE clear"sv, count);

            // v1.22.10: Dump files list state before ClearData wipes it
            DumpFilesListState(a_self, "PRE-ClearData");

            for (auto& shard : g_formCache) {
                std::unique_lock lock(shard.mutex);
                shard.map.clear();
            }

            TreeLodReferenceCaching::detail::ClearCache();

            g_hk_ClearData.call(a_self);

            // Dump state after ClearData too
            DumpFilesListState(a_self, "POST-ClearData");
        }

        inline SafetyHookInline g_hk_InitializeFormDataStructures;

        inline void TESForm_InitializeFormDataStructures()
        {
            auto count = g_initFormDataCalls.fetch_add(1, std::memory_order_relaxed) + 1;
            logger::info(">>> InitializeFormDataStructures called (call #{})"sv, count);

            // v1.22.10: Check TDH state at init time
            auto* tdh = RE::TESDataHandler::GetSingleton();
            if (tdh) {
                DumpFilesListState(tdh, "PRE-InitFormData");
            }

            for (auto& shard : g_formCache) {
                std::unique_lock lock(shard.mutex);
                shard.map.clear();
            }

            TreeLodReferenceCaching::detail::ClearCache();

            g_hk_InitializeFormDataStructures.call();
        }

        // ================================================================
        // v1.22.0: AddFormToDataHandler hook — counts form registrations
        // This is the CRITICAL function: if this is never called, the
        // engine never creates forms from plugin files.
        // ================================================================
        inline SafetyHookInline g_hk_AddFormToDataHandler;

        inline bool TESDataHandler_AddFormToDataHandler(RE::TESDataHandler* a_self, RE::TESForm* a_form)
        {
            if (!a_form) {
                g_addFormNullCalls.fetch_add(1, std::memory_order_relaxed);
            } else {
                auto count = g_addFormCalls.fetch_add(1, std::memory_order_relaxed);
                // Log the first 20 forms added, then every 10000th
                if (count < 20 || (count > 0 && count % 10000 == 0)) {
                    auto formId = a_form->GetFormID();
                    auto formType = static_cast<std::uint8_t>(a_form->GetFormType());
                    logger::info("  AddForm #{}: formID=0x{:08X} type=0x{:02X}",
                        count + 1, formId, formType);
                }
            }
            return g_hk_AddFormToDataHandler.call<bool>(a_self, a_form);
        }

        // ================================================================
        // v1.22.1: TESFile::OpenTES hook — counts file opens
        // If files are never opened for reading, the loading loop didn't run.
        // ================================================================
        inline SafetyHookInline g_hk_OpenTES;

        inline bool TESFile_OpenTES(RE::TESFile* a_self, std::uint32_t a_mode, bool a_lock)
        {
            auto count = g_openTESCalls.fetch_add(1, std::memory_order_relaxed);
            bool result = g_hk_OpenTES.call<bool>(a_self, a_mode, a_lock);
            if (result) {
                auto successes = g_openTESSuccesses.fetch_add(1, std::memory_order_relaxed);
                if (successes < 10 || (successes > 0 && successes % 500 == 0)) {
                    logger::info("  OpenTES #{}: '{}' mode={} lock={} -> SUCCESS",
                        count + 1, a_self->fileName, a_mode, a_lock);
                }
            } else {
                logger::warn("  OpenTES #{}: '{}' mode={} lock={} -> FAILED",
                    count + 1, a_self->fileName, a_mode, a_lock);
            }
            return result;
        }

        // ================================================================
        // v1.22.2: Hook TESForm::AddCompileIndex — counts compile index assignments
        // If this is never called, compile indices are never assigned.
        // ================================================================
        inline SafetyHookInline g_hk_AddCompileIndex;

        inline void TESForm_AddCompileIndex(RE::FormID* a_id, RE::TESFile* a_file)
        {
            auto count = g_addCompileIndexCalls.fetch_add(1, std::memory_order_relaxed);
            if (count < 10 || (count > 0 && count % 50000 == 0)) {
                logger::info("  AddCompileIndex #{}: file='{}' formID=0x{:08X}",
                    count + 1, a_file ? a_file->fileName : "null",
                    a_id ? *a_id : 0);
            }
            g_hk_AddCompileIndex.call(a_id, a_file);
        }

        // ================================================================
        // v1.22.2: Hook TESFile::SeekNextForm — counts record iteration
        // This is called repeatedly during record reading. If it's never
        // called, the game never attempts to read any records.
        // ================================================================
        inline SafetyHookInline g_hk_SeekNextForm;

        inline bool TESFile_SeekNextForm(RE::TESFile* a_self, bool a_skipIgnored)
        {
            auto count = g_seekNextFormCalls.fetch_add(1, std::memory_order_relaxed);
            if (count < 5 || (count > 0 && count % 100000 == 0)) {
                logger::info("  SeekNextForm #{}: file='{}'",
                    count + 1, a_self ? a_self->fileName : "null");
            }
            return g_hk_SeekNextForm.call<bool>(a_self, a_skipIgnored);
        }

        // ================================================================
        // v1.22.31: x86-64 instruction length decoder.
        // Used by ForceLoadVEH and FormReferenceFixupVEH to advance RIP
        // past faulting instructions.
        // Handles REX prefix, legacy prefixes, ModRM, SIB, disp8/disp32,
        // and immediate operands (imm8/imm16/imm32).
        // ================================================================
        inline std::size_t x86_instr_len(const std::uint8_t* ip)
        {
            const auto* start = ip;

            // Consume REX prefix (40–4F)
            bool hasRex = false;
            if ((*ip & 0xF0) == 0x40) { hasRex = true; ++ip; }

            // Consume legacy prefixes (operand-size, address-size, REP, LOCK, segment)
            bool has66 = false;
            while (*ip == 0x66 || *ip == 0xF0 || *ip == 0xF2 || *ip == 0xF3 ||
                   *ip == 0x2E || *ip == 0x3E || *ip == 0x26 ||
                   *ip == 0x36 || *ip == 0x64 || *ip == 0x65) {
                if (*ip == 0x66) has66 = true;
                ++ip;
            }

            const auto op = *ip++;
            bool isTwoByteOp = false;

            // Two-byte escape (0F xx); three-byte escape (0F 38 xx / 0F 3A xx)
            if (op == 0x0F) {
                isTwoByteOp = true;
                const auto op2 = *ip++;
                if (op2 == 0x38 || op2 == 0x3A) ++ip; // third opcode byte
            }

            // All crashing memory-access instructions have a ModRM byte.
            const auto modrm = *ip++;
            const auto mod   = (modrm >> 6) & 0x3;
            const auto reg   = (modrm >> 3) & 0x7;
            const auto rm    = modrm & 0x7;

            // Displacement
            if (mod != 3) {
                if (rm == 4) ++ip;            // SIB byte
                if      (mod == 0 && rm == 5) ip += 4; // RIP-relative disp32
                else if (mod == 1)            ip += 1; // disp8
                else if (mod == 2)            ip += 4; // disp32
            }

            // Immediate operands — opcodes that include an imm after ModRM+disp
            if (!isTwoByteOp) {
                // F6 /0 = TEST r/m8, imm8
                if (op == 0xF6 && reg == 0) ip += 1;
                // F7 /0 = TEST r/m16/32, imm16/32
                else if (op == 0xF7 && reg == 0) ip += (has66 ? 2 : 4);
                // 80, 82 = op r/m8, imm8
                else if (op == 0x80 || op == 0x82) ip += 1;
                // 81 = op r/m16/32, imm16/32
                else if (op == 0x81) ip += (has66 ? 2 : 4);
                // 83 = op r/m16/32, imm8
                else if (op == 0x83) ip += 1;
                // C6 /0 = MOV r/m8, imm8
                else if (op == 0xC6 && reg == 0) ip += 1;
                // C7 /0 = MOV r/m16/32, imm16/32
                else if (op == 0xC7 && reg == 0) ip += (has66 ? 2 : 4);
                // 69 = IMUL r, r/m, imm16/32
                else if (op == 0x69) ip += (has66 ? 2 : 4);
                // 6B = IMUL r, r/m, imm8
                else if (op == 0x6B) ip += 1;
            }

            return static_cast<std::size_t>(ip - start);
        }

        // ================================================================
        // v1.22.23: Vectored Exception Handler — null-dereference guard
        // for AE 13753 (ForceLoadAllForms).
        //
        // Large modlists (GTS 1720+ plugins) contain ESL-range actor forms
        // whose race/template pointer is null. AE 13753's loading loop
        // crashes at `mov ecx, [rax+0x38]` (AE 37791+0x8D, RAX=0) when
        // it hits one of these forms.
        //
        // This VEH catches the null AV, decodes the instruction length,
        // advances RIP past the failing instruction, and continues.
        // The destination register is left zero, which is the same as if
        // the load had returned 0 — null checks downstream handle it.
        // ================================================================
        inline LONG CALLBACK ForceLoadVEH(PEXCEPTION_POINTERS pep)
        {
            // Only active during ForceLoadAllForms
            if (!g_forceLoadInProgress.load(std::memory_order_relaxed))
                return EXCEPTION_CONTINUE_SEARCH;

            // Only handle access violations
            if (pep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
                return EXCEPTION_CONTINUE_SEARCH;

            // Only handle null dereferences (target address < 0x10000)
            // to avoid masking legitimate faults in unrelated code
            ULONG_PTR targetAddr = 0;
            if (pep->ExceptionRecord->NumberParameters >= 2)
                targetAddr = pep->ExceptionRecord->ExceptionInformation[1];
            if (targetAddr >= 0x10000)
                return EXCEPTION_CONTINUE_SEARCH;

            // Advance RIP past the failing instruction
            const auto* rip = reinterpret_cast<const std::uint8_t*>(pep->ContextRecord->Rip);
            pep->ContextRecord->Rip += x86_instr_len(rip);

            g_vehSkipCount.fetch_add(1, std::memory_order_relaxed);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // ================================================================
        // v1.22.27: Form-Reference Fixup VEH
        //
        // After ForceLoadAllForms, some forms still contain raw form IDs
        // (32-bit values < 0x10000000) stored in 64-bit pointer fields.
        // These were set during TESForm::Load() when the referenced form
        // hadn't been loaded yet. On Windows, a post-load resolution
        // pass fixes these, but our ForceLoadAllForms path skips it.
        //
        // This VEH catches access violations where the target address
        // looks like a form ID (0x100 <= addr < 0x10000000), finds which
        // register holds that value, resolves it via our sharded cache,
        // and patches the register to the resolved pointer.
        //
        // Crash pattern: `test dword ptr [rax+0x28], 0x3FF`
        //   RAX = 0x5000823 → form ID 0x05000823
        //   target = RAX + 0x28 = 0x500084B
        //
        // This VEH stays installed permanently after ForceLoadAllForms.
        // ================================================================
        inline LONG CALLBACK FormReferenceFixupVEH(PEXCEPTION_POINTERS pep)
        {
            if (!g_formFixupActive.load(std::memory_order_relaxed))
                return EXCEPTION_CONTINUE_SEARCH;

            if (pep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
                return EXCEPTION_CONTINUE_SEARCH;

            // v1.22.29: Only handle faults within SkyrimSE.exe's code —
            // do not interfere with crash logger, Wine internals, or other DLLs.
            {
                static auto imgBase = REL::Module::get().base();
                static auto imgEnd = [&]() {
                    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imgBase);
                    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(imgBase + dos->e_lfanew);
                    return imgBase + nt->OptionalHeader.SizeOfImage;
                }();
                auto rip = pep->ContextRecord->Rip;
                if (rip < imgBase || rip >= imgEnd)
                    return EXCEPTION_CONTINUE_SEARCH;
            }

            // Get the faulting address
            ULONG_PTR targetAddr = 0;
            if (pep->ExceptionRecord->NumberParameters >= 2)
                targetAddr = pep->ExceptionRecord->ExceptionInformation[1];

            // v1.22.36: Handle null-pointer form dereferences.
            // After kDeleted flagging + code cave, the engine still hits
            // null form pointers (RAX=0) in the AI evaluation chain.
            // Pattern: `cmp byte ptr [rax+0x1A], 0x2B` where rax=0.
            // The target address is < 0x100 (null page + small offset).
            //
            // Instead of patching each crash site, skip the faulting
            // instruction. The destination register stays 0 / unchanged,
            // and downstream null checks handle the rest.
            if (targetAddr < 0x100) {
                static std::atomic<int> s_nullSkipCount{ 0 };
                auto count = s_nullSkipCount.fetch_add(1, std::memory_order_relaxed);

                const auto* rip = reinterpret_cast<const std::uint8_t*>(pep->ContextRecord->Rip);
                auto instrLen = x86_instr_len(rip);
                pep->ContextRecord->Rip += instrLen;

                if (count < 50) {
                    logger::warn("FormFixupVEH: null-ptr skip #{}: RIP=0x{:X} (+{}B) target=0x{:X} RAX=0x{:X}",
                        count + 1, pep->ContextRecord->Rip - instrLen, instrLen,
                        targetAddr, pep->ContextRecord->Rax);
                } else if (count == 50) {
                    logger::warn("FormFixupVEH: suppressing null-ptr skip messages (50 logged)");
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // Form IDs are 32-bit values: master index (0x00-0xFE) in top byte.
            // Valid range: [0x100, 0xFF000000). Under Wine/64-bit, heap addresses
            // are >> 0x100000000 (e.g., 0x248...), so the 32-bit range is safe.
            // The faulting address may be offset from the base register
            // (e.g., [rax+0x28]), so we also check all registers below.
            // Reject real pointers (>= 0xFF000000).
            if (targetAddr >= 0xFF000000)
                return EXCEPTION_CONTINUE_SEARCH;

            // Scan all general-purpose registers for a value that looks like a form ID.
            // The register holding the form ID will be the base of the memory access.
            // x86-64 register order in CONTEXT: Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8-R15
            DWORD64* regs[] = {
                &pep->ContextRecord->Rax,
                &pep->ContextRecord->Rcx,
                &pep->ContextRecord->Rdx,
                &pep->ContextRecord->Rbx,
                // Skip Rsp — never holds a form ID
                &pep->ContextRecord->Rbp,
                &pep->ContextRecord->Rsi,
                &pep->ContextRecord->Rdi,
                &pep->ContextRecord->R8,
                &pep->ContextRecord->R9,
                &pep->ContextRecord->R10,
                &pep->ContextRecord->R11,
                &pep->ContextRecord->R12,
                &pep->ContextRecord->R13,
                &pep->ContextRecord->R14,
                &pep->ContextRecord->R15,
            };
            const char* regNames[] = {
                "RAX", "RCX", "RDX", "RBX", "RBP", "RSI", "RDI",
                "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
            };

            for (int i = 0; i < 15; ++i) {
                DWORD64 val = *regs[i];

                // Must look like a form ID: [0x100, 0xFF000000)
                if (val < 0x100 || val >= 0xFF000000)
                    continue;

                // The target address should be at or near this register value
                // (within a reasonable struct offset, say +/- 0x1000)
                auto diff = static_cast<std::int64_t>(targetAddr) - static_cast<std::int64_t>(val);
                if (diff < -0x1000 || diff > 0x1000)
                    continue;

                // This register holds what looks like a form ID. Resolve it.
                auto formId = static_cast<RE::FormID>(val);
                auto* resolved = TESForm_GetFormByNumericId(formId);

                if (resolved) {
                    auto count = g_formFixupCount.fetch_add(1, std::memory_order_relaxed);
                    if (count < 20) {
                        logger::warn("FormFixupVEH: {}=0x{:X} (formID 0x{:08X}) → 0x{:X} at RIP=0x{:X}",
                            regNames[i], val, formId,
                            reinterpret_cast<std::uintptr_t>(resolved),
                            pep->ContextRecord->Rip);
                    } else if (count == 20) {
                        logger::warn("FormFixupVEH: suppressing further log messages (20 logged, more occurring)");
                    }
                    *regs[i] = reinterpret_cast<DWORD64>(resolved);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }

                // v1.22.36: Form not in cache — let the crash happen cleanly.
                // Previous approaches (sentinel, instruction skip, zero+ZF)
                // all caused cascading failures downstream. A clean crash
                // at the original site gives better diagnostic information.
                auto sentCount = g_sentinelUseCount.fetch_add(1, std::memory_order_relaxed);
                if (sentCount < 50) {
                    logger::warn("FormFixupVEH: {}=0x{:X} (formID 0x{:08X}) NOT FOUND at RIP=0x{:X} — passing through",
                        regNames[i], val, formId, pep->ContextRecord->Rip);
                } else if (sentCount == 50) {
                    logger::warn("FormFixupVEH: suppressing pass-through log messages (50 logged)");
                }
                return EXCEPTION_CONTINUE_SEARCH;
            }

            // No register held a form ID — this is a different kind of crash
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // ================================================================
        // v1.22.36: First-chance crash logger VEH.
        //
        // Catches access violations and writes diagnostic info
        // (crash address, exception code, registers, module offset)
        // directly to a file. Uses a known game directory path.
        //
        // Only active after g_crashLoggerActive is set (post form loading).
        // Logs once per crash site, then returns EXCEPTION_CONTINUE_SEARCH.
        // ================================================================
        inline std::atomic<bool> g_crashLoggerActive{ false };
        inline std::atomic<int> g_crashLogCount{ 0 };
        inline std::atomic<int> g_nullSkipCount{ 0 };
        inline std::atomic<int> g_formIdSkipCount{ 0 };

        // Classify an x86 instruction at `ip` for VEH skip handling.
        // Returns: 'C' = CMP/TEST, 'F' = CALL indirect, 'O' = other
        inline char classifyInstruction(const std::uint8_t* ip)
        {
            // Skip prefixes (REX 40-4F, LOCK F0, 66, F2, F3, segment)
            while ((*ip >= 0x40 && *ip <= 0x4F) || *ip == 0xF0 ||
                   *ip == 0x66 || *ip == 0xF2 || *ip == 0xF3 ||
                   *ip == 0x2E || *ip == 0x3E || *ip == 0x26 ||
                   *ip == 0x36 || *ip == 0x64 || *ip == 0x65) {
                ++ip;
            }

            auto op = *ip;

            // CMP/TEST group: 38-3D, 80-83 /7(CMP), F6-F7 /0(TEST), 84-85
            if (op >= 0x38 && op <= 0x3D) return 'C';
            if (op == 0x84 || op == 0x85) return 'C';
            if (op == 0x80 || op == 0x82 || op == 0x83) {
                auto reg = (ip[1] >> 3) & 0x7;
                if (reg == 7) return 'C'; // /7 = CMP
            }
            if (op == 0x81) {
                auto reg = (ip[1] >> 3) & 0x7;
                if (reg == 7) return 'C'; // /7 = CMP
            }
            if (op == 0xF6 || op == 0xF7) {
                auto reg = (ip[1] >> 3) & 0x7;
                if (reg == 0) return 'C'; // /0 = TEST
            }
            // Two-byte: 0F BA /4(BT), 0F A3(BT), etc — also comparisons
            if (op == 0x0F) {
                auto op2 = ip[1];
                if (op2 == 0xBA || op2 == 0xA3 || op2 == 0xAB ||
                    op2 == 0xB3 || op2 == 0xBB) return 'C';
            }

            // CALL indirect: FF /2
            if (op == 0xFF) {
                auto reg = (ip[1] >> 3) & 0x7;
                if (reg == 2) return 'F'; // /2 = CALL
            }

            return 'O';
        }

        inline LONG CALLBACK CrashLoggerVEH(PEXCEPTION_POINTERS pep)
        {
            if (!g_crashLoggerActive.load(std::memory_order_relaxed))
                return EXCEPTION_CONTINUE_SEARCH;

            auto code = pep->ExceptionRecord->ExceptionCode;

            // Log non-AV exceptions (stack overflow, illegal instruction, etc.)
            if (code != EXCEPTION_ACCESS_VIOLATION) {
                static std::atomic<int> s_nonAvCount{ 0 };
                auto cnt = s_nonAvCount.fetch_add(1, std::memory_order_relaxed);
                if (cnt < 5) {
                    FILE* f = nullptr;
                    fopen_s(&f, "C:\\SSEEngineFixesForWine_crash.log", "a");
                    if (f) {
                        auto imgBase = REL::Module::get().base();
                        auto rip2 = pep->ContextRecord->Rip;
                        fprintf(f, "NON-AV EXCEPTION #%d: code=0x%08lX RIP=0x%llX (+0x%llX)\n",
                            cnt + 1, code, rip2, rip2 - imgBase);
                        fflush(f);
                        fclose(f);
                    }
                }
                return EXCEPTION_CONTINUE_SEARCH;
            }

            ULONG_PTR targetAddr = 0;
            ULONG_PTR accessType = 0; // 0=read, 1=write, 8=DEP
            if (pep->ExceptionRecord->NumberParameters >= 2) {
                accessType = pep->ExceptionRecord->ExceptionInformation[0];
                targetAddr = pep->ExceptionRecord->ExceptionInformation[1];
            }

            // ─────────────────────────────────────────────────────────────
            // CASE W: Write to zero page — skip the instruction.
            // The zero page is PAGE_READONLY. When the engine tries to
            // write to a "form" that's actually our zero page, we skip
            // the write instruction to keep the page pristine (all zeros).
            // This prevents cross-contamination between VEH invocations.
            // ─────────────────────────────────────────────────────────────
            if (accessType == 1 && g_zeroPage &&
                targetAddr >= g_zeroPageBase &&
                targetAddr < g_zeroPageEnd) {
                auto rip2 = pep->ContextRecord->Rip;
                const auto* ripBytes2 = reinterpret_cast<const std::uint8_t*>(rip2);
                if (!IsBadReadPtr(ripBytes2, 16)) {
                    auto instrLen = x86_instr_len(ripBytes2);
                    pep->ContextRecord->Rip += instrLen;

                    auto count = g_zeroPageWriteSkips.fetch_add(1, std::memory_order_relaxed);
                    if (count < 200) {
                        const char* logPath = "C:\\SSEEngineFixesForWine_crash.log";
                        FILE* f2 = nullptr;
                        fopen_s(&f2, logPath, "a");
                        if (f2) {
                            fprintf(f2, "ZERO-WRITE-SKIP #%d: RIP=+0x%llX target=0x%llX +%dB\n",
                                count + 1, rip2 - REL::Module::get().base(),
                                (unsigned long long)targetAddr, instrLen);
                            fflush(f2);
                            fclose(f2);
                        }
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            // Compute SkyrimSE.exe image bounds (cached)
            static auto sImgBase = REL::Module::get().base();
            static auto sImgEnd = [&]() {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(sImgBase);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(sImgBase + dos->e_lfanew);
                return sImgBase + nt->OptionalHeader.SizeOfImage;
            }();
            auto rip = pep->ContextRecord->Rip;

            // ─────────────────────────────────────────────────────────────
            // CASE 0: Null code execution (RIP < 0x10000)
            // Happens when CALL jumped to a null function pointer (e.g.,
            // vtable read from zero page returned 0). The CALL already
            // pushed the return address onto the stack.
            // Pop the return address, set RAX=0 (null return), continue.
            // ─────────────────────────────────────────────────────────────
            if (rip < 0x10000) {
                auto* rspPtr = reinterpret_cast<DWORD64*>(pep->ContextRecord->Rsp);
                if (!IsBadReadPtr(rspPtr, 8)) {
                    pep->ContextRecord->Rip = *rspPtr;
                    pep->ContextRecord->Rsp += 8;
                    pep->ContextRecord->Rax = 0;

                    auto count = g_nullSkipCount.fetch_add(1, std::memory_order_relaxed);
                    if (count < 50) {
                        const char* logPath = "C:\\SSEEngineFixesForWine_crash.log";
                        FILE* f2 = nullptr;
                        fopen_s(&f2, logPath, "a");
                        if (f2) {
                            fprintf(f2, "NULL-CALL-RET #%d: RIP=0x%llX returning to 0x%llX\n",
                                count + 1, rip, *rspPtr);
                            fflush(f2);
                            fclose(f2);
                        }
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            // Log (but don't handle) faults outside SkyrimSE.exe image
            if (rip < sImgBase || rip >= sImgEnd) {
                static std::atomic<int> s_extCount{ 0 };
                auto cnt = s_extCount.fetch_add(1, std::memory_order_relaxed);
                if (cnt < 10) {
                    const char* logPath = "C:\\SSEEngineFixesForWine_crash.log";
                    FILE* f = nullptr;
                    fopen_s(&f, logPath, "a");
                    if (f) {
                        fprintf(f, "EXT-CRASH #%d: RIP=0x%llX (outside SkyrimSE.exe 0x%llX-0x%llX) target=0x%llX RAX=0x%llX\n",
                            cnt + 1, rip, sImgBase, sImgEnd,
                            (unsigned long long)targetAddr,
                            (unsigned long long)pep->ContextRecord->Rax);
                        fflush(f);
                        fclose(f);
                    }
                }
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const auto* ripBytes = reinterpret_cast<const std::uint8_t*>(rip);

            // ─────────────────────────────────────────────────────────────
            // CASE 1: Invalid pointer dereference — zero page substitution
            //
            // Covers TWO scenarios:
            //  a) Null pointer: targetAddr < 0x10000 (register is near 0)
            //  b) Garbage pointer: targetAddr > 0x7FFFFFFFFFFF (above
            //     user-mode VA space on Windows x64, e.g. 0xFFFFFFFF00000008)
            //
            // These arise because the engine reads zero from the zero page,
            // then combines it with other registers (OR, shift, etc.)
            // producing invalid kernel-space addresses.
            //
            // Instead of skipping instructions (which corrupts control flow),
            // replace the bad register with g_zeroPage and RETRY the
            // instruction. The engine reads zeros from every field and
            // takes "nothing here" branches naturally.
            // ─────────────────────────────────────────────────────────────
            bool isNullAccess = (targetAddr < 0x10000);
            bool isHighInvalid = (targetAddr > 0x00007FFFFFFFFFFFULL);
            if ((isNullAccess || isHighInvalid) && g_zeroPage) {
                DWORD64 zp = reinterpret_cast<DWORD64>(g_zeroPage);
                DWORD64* gprs[] = {
                    &pep->ContextRecord->Rax,
                    &pep->ContextRecord->Rcx,
                    &pep->ContextRecord->Rdx,
                    &pep->ContextRecord->Rbx,
                    // Skip RSP
                    &pep->ContextRecord->Rbp,
                    &pep->ContextRecord->Rsi,
                    &pep->ContextRecord->Rdi,
                    &pep->ContextRecord->R8,
                    &pep->ContextRecord->R9,
                    &pep->ContextRecord->R10,
                    &pep->ContextRecord->R11,
                    &pep->ContextRecord->R12,
                    &pep->ContextRecord->R13,
                    &pep->ContextRecord->R14,
                    &pep->ContextRecord->R15,
                };

                bool patched = false;
                if (isNullAccess) {
                    // Null pointer: find register near zero
                    for (int i = 0; i < 15; ++i) {
                        DWORD64 val = *gprs[i];
                        if (val < 0x100 && targetAddr >= val &&
                            targetAddr - val < 0x10000) {
                            *gprs[i] = zp + val;
                            patched = true;
                            break;
                        }
                    }
                } else {
                    // High-invalid: find register with invalid high bits
                    // that when offset-adjusted matches targetAddr
                    for (int i = 0; i < 15; ++i) {
                        DWORD64 val = *gprs[i];
                        if (val <= 0x00007FFFFFFFFFFFULL)
                            continue; // Valid user-mode address, skip
                        auto diff = static_cast<std::int64_t>(targetAddr) -
                                    static_cast<std::int64_t>(val);
                        if (diff >= -0x10000 && diff <= 0x10000) {
                            *gprs[i] = zp; // Replace with zero page base
                            patched = true;
                            break;
                        }
                    }
                }

                if (!patched) {
                    // Fallback: set RAX to zero page (most common target)
                    pep->ContextRecord->Rax = zp;
                    patched = true;
                }

                auto count = g_zeroPageUseCount.fetch_add(1, std::memory_order_relaxed);
                if (count < 200) {
                    const char* logPath = "C:\\SSEEngineFixesForWine_crash.log";
                    FILE* f2 = nullptr;
                    fopen_s(&f2, logPath, "a");
                    if (f2) {
                        fprintf(f2, "ZERO-PAGE #%d: RIP=+0x%llX target=0x%llX %s → zeroPage=0x%llX\n",
                            count + 1, rip - sImgBase,
                            (unsigned long long)targetAddr,
                            isNullAccess ? "null" : "high-invalid",
                            (unsigned long long)zp);
                        // Byte dump: first time we see each RIP, dump 32 bytes
                        static DWORD64 s_dumpedRips[32] = {};
                        static int s_dumpCount = 0;
                        bool alreadyDumped = false;
                        for (int d = 0; d < s_dumpCount; ++d)
                            if (s_dumpedRips[d] == rip) { alreadyDumped = true; break; }
                        if (!alreadyDumped && s_dumpCount < 32) {
                            s_dumpedRips[s_dumpCount++] = rip;
                            fprintf(f2, "  BYTES[+0x%llX]:", rip - sImgBase);
                            if (!IsBadReadPtr(ripBytes, 32))
                                for (int b = 0; b < 32; ++b) fprintf(f2, " %02X", ripBytes[b]);
                            fprintf(f2, "\n");
                        }
                        fflush(f2);
                        fclose(f2);
                    }
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // ─────────────────────────────────────────────────────────────
            // CASE 2: Form-ID-as-pointer dereference (targetAddr < 0xFF000000)
            // Wine leaves raw form IDs (32-bit values) in 64-bit pointer fields.
            // Scan registers for a value that looks like a form ID and is close
            // to the target address. Try to resolve via GetFormByNumericId.
            // If resolved: patch register, retry instruction.
            // If NOT resolved: skip instruction (form doesn't exist).
            // ─────────────────────────────────────────────────────────────
            if (targetAddr < 0xFF000000ULL) {
                DWORD64* regs[] = {
                    &pep->ContextRecord->Rax,
                    &pep->ContextRecord->Rcx,
                    &pep->ContextRecord->Rdx,
                    &pep->ContextRecord->Rbx,
                    &pep->ContextRecord->Rbp,
                    &pep->ContextRecord->Rsi,
                    &pep->ContextRecord->Rdi,
                    &pep->ContextRecord->R8,
                    &pep->ContextRecord->R9,
                    &pep->ContextRecord->R10,
                    &pep->ContextRecord->R11,
                    &pep->ContextRecord->R12,
                    &pep->ContextRecord->R13,
                    &pep->ContextRecord->R14,
                    &pep->ContextRecord->R15,
                };

                for (int i = 0; i < 15; ++i) {
                    DWORD64 val = *regs[i];
                    if (val < 0x100 || val >= 0xFF000000ULL)
                        continue;
                    // Skip values that point into our zero page — not form IDs!
                    if (g_zeroPage && val >= g_zeroPageBase && val < g_zeroPageEnd)
                        continue;
                    auto diff = static_cast<std::int64_t>(targetAddr) - static_cast<std::int64_t>(val);
                    if (diff < -0x1000 || diff > 0x1000)
                        continue;

                    // Found a register that looks like a form ID
                    auto formId = static_cast<RE::FormID>(val);
                    auto* resolved = TESForm_GetFormByNumericId(formId);

                    if (resolved) {
                        // Resolved! Replace register with real pointer, retry instruction
                        *regs[i] = reinterpret_cast<DWORD64>(resolved);
                        auto count = g_formIdSkipCount.fetch_add(1, std::memory_order_relaxed);
                        if (count < 20) {
                            const char* logPath = "C:\\SSEEngineFixesForWine_crash.log";
                            FILE* f2 = nullptr;
                            fopen_s(&f2, logPath, "a");
                            if (f2) {
                                fprintf(f2, "FORM-RESOLVE #%d: RIP=+0x%llX formID=0x%08X → 0x%llX\n",
                                    count + 1, rip - sImgBase, formId,
                                    reinterpret_cast<unsigned long long>(resolved));
                                fflush(f2);
                                fclose(f2);
                            }
                        }
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }

                    // Form not found in hash table — replace register with
                    // zero page pointer so the engine reads zeros from all
                    // fields and takes natural "nothing" branches.
                    // This avoids instruction skipping which corrupts control flow.
                    if (g_zeroPage) {
                        DWORD64 zp = reinterpret_cast<DWORD64>(g_zeroPage);
                        auto offset = static_cast<std::int64_t>(targetAddr) - static_cast<std::int64_t>(val);
                        *regs[i] = zp; // Point to zero page

                        auto count = g_formIdSkipCount.fetch_add(1, std::memory_order_relaxed);
                        if (count < 50) {
                            const char* logPath = "C:\\SSEEngineFixesForWine_crash.log";
                            FILE* f2 = nullptr;
                            fopen_s(&f2, logPath, "a");
                            if (f2) {
                                fprintf(f2, "FORM-ZEROPAGE #%d: RIP=+0x%llX formID=0x%08X NOT FOUND → zeroPage=0x%llX\n",
                                    count + 1, rip - sImgBase, formId, (unsigned long long)zp);
                                fflush(f2);
                                fclose(f2);
                            }
                        }
                        return EXCEPTION_CONTINUE_EXECUTION; // Retry with zero page
                    }

                    // No zero page available — fall through to crash log
                    break;
                }
            }

            // ─────────────────────────────────────────────────────────────
            // CASE 3: Catch-all — skip instruction + zero result register.
            // Handles remaining AVs that fall through all other cases:
            //  - Out-of-bounds array access (e.g., [R14+RAX*8] past image)
            //  - Unmapped heap/stack addresses
            //  - Any other unresolvable access
            //
            // We skip the faulting instruction and zero RAX (the most common
            // result register). This is "last resort" recovery — imprecise
            // but better than crashing. The engine may null-check downstream.
            // ─────────────────────────────────────────────────────────────
            {
                auto instrLen = x86_instr_len(ripBytes);
                if (instrLen > 0 && instrLen <= 15) {
                    pep->ContextRecord->Rip += instrLen;
                    pep->ContextRecord->Rax = 0;

                    auto count = g_catchAllCount.fetch_add(1, std::memory_order_relaxed);
                    if (count < 200) {
                        const char* logPath = "C:\\SSEEngineFixesForWine_crash.log";
                        FILE* f2 = nullptr;
                        fopen_s(&f2, logPath, "a");
                        if (f2) {
                            fprintf(f2, "CATCH-ALL #%d: RIP=+0x%llX +%dB target=0x%llX RAX=0x%llX\n",
                                count + 1, rip - sImgBase, instrLen,
                                (unsigned long long)targetAddr,
                                (unsigned long long)pep->ContextRecord->Rax);
                            // Byte dump: first time per unique RIP
                            static DWORD64 s_dumpedRips[32] = {};
                            static int s_dumpCount = 0;
                            bool alreadyDumped = false;
                            for (int d = 0; d < s_dumpCount; ++d)
                                if (s_dumpedRips[d] == rip) { alreadyDumped = true; break; }
                            if (!alreadyDumped && s_dumpCount < 32) {
                                s_dumpedRips[s_dumpCount++] = rip;
                                fprintf(f2, "  BYTES[+0x%llX]:", rip - sImgBase);
                                if (!IsBadReadPtr(ripBytes, 32))
                                    for (int b = 0; b < 32; ++b) fprintf(f2, " %02X", ripBytes[b]);
                                fprintf(f2, "\n");
                            }
                            fflush(f2);
                            fclose(f2);
                        }
                    }
                    // Safety valve: after 10000 catch-all skips, stop handling
                    // to prevent infinite loops from starving the process
                    if (count < 10000)
                        return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            // Only log first 20 non-handled crashes
            if (g_crashLogCount.fetch_add(1, std::memory_order_relaxed) >= 20)
                return EXCEPTION_CONTINUE_SEARCH;

            // Write to C:\ root (guaranteed known Wine path)
            {
                const char* logPath = "C:\\SSEEngineFixesForWine_crash.log";
                FILE* f = nullptr;
                fopen_s(&f, logPath, "a");
                if (f) {
                    // rip, targetAddr, sImgBase already computed above
                    auto offset = rip - sImgBase;
                    bool inModule = true; // We already verified RIP is in SkyrimSE.exe

                    fprintf(f, "\n=== SSEEngineFixesForWine CRASH LOG (v1.22.36) ===\n");
                    fprintf(f, "Exception: 0x%08lX at RIP=0x%llX (SkyrimSE.exe+0x%llX)\n",
                        code, rip, offset);
                    fprintf(f, "Target address: 0x%llX\n", (unsigned long long)targetAddr);
                    fprintf(f, "Registers:\n");
                    fprintf(f, "  RAX=0x%016llX  RBX=0x%016llX  RCX=0x%016llX  RDX=0x%016llX\n",
                        pep->ContextRecord->Rax, pep->ContextRecord->Rbx,
                        pep->ContextRecord->Rcx, pep->ContextRecord->Rdx);
                    fprintf(f, "  RSI=0x%016llX  RDI=0x%016llX  RBP=0x%016llX  RSP=0x%016llX\n",
                        pep->ContextRecord->Rsi, pep->ContextRecord->Rdi,
                        pep->ContextRecord->Rbp, pep->ContextRecord->Rsp);
                    fprintf(f, "  R8 =0x%016llX  R9 =0x%016llX  R10=0x%016llX  R11=0x%016llX\n",
                        pep->ContextRecord->R8, pep->ContextRecord->R9,
                        pep->ContextRecord->R10, pep->ContextRecord->R11);
                    fprintf(f, "  R12=0x%016llX  R13=0x%016llX  R14=0x%016llX  R15=0x%016llX\n",
                        pep->ContextRecord->R12, pep->ContextRecord->R13,
                        pep->ContextRecord->R14, pep->ContextRecord->R15);

                    // Dump bytes at RIP for disassembly
                    fprintf(f, "Bytes at RIP:");
                    if (!IsBadReadPtr(ripBytes, 16)) {
                        for (int i = 0; i < 16; ++i)
                            fprintf(f, " %02X", ripBytes[i]);
                    } else {
                        fprintf(f, " <unreadable>");
                    }
                    fprintf(f, "\n");

                    // Stack dump (16 entries)
                    fprintf(f, "Stack (RSP):");
                    auto* rspPtr = reinterpret_cast<const DWORD64*>(pep->ContextRecord->Rsp);
                    if (!IsBadReadPtr(rspPtr, 128)) {
                        fprintf(f, "\n");
                        for (int i = 0; i < 16; ++i)
                            fprintf(f, "  [RSP+0x%02X] = 0x%016llX\n", i * 8, rspPtr[i]);
                    } else {
                        fprintf(f, " <unreadable>\n");
                    }

                    fprintf(f, "=== END CRASH LOG ===\n");
                    fflush(f);
                    fclose(f);
                }
            }

            // Also try to flush spdlog
            try { spdlog::default_logger()->flush(); } catch (...) {}

            return EXCEPTION_CONTINUE_SEARCH;
        }

        // ================================================================
        // v1.22.27: SEH-isolated InitItem — prevents crash on forms with
        // null internal fields (e.g., Character forms with null race ptr).
        // MSVC: __try/__except cannot coexist with C++ destructors, so
        // this must be a separate function (same pattern as SafeGetEditorID).
        // ================================================================
        inline bool SafeInitItem(RE::TESForm* form) noexcept
        {
            __try {
                form->InitItem();
                return true;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Forward declarations (defined after CloseTES hook)
        inline void ManuallyCompileFiles();
        inline std::atomic<bool> g_manualCompileDone{ false };

        // ================================================================
        // v1.22.3: Hook TESFile::CloseTES — detect if files are closed
        // after catalog scan (would indicate catalog is a separate pass)
        //
        // v1.22.15: MAIN-THREAD COMPILATION TRIGGER
        // Under Wine with 600+ files, CompileFiles is skipped entirely.
        // The form loading decision is made on the main thread AFTER the
        // catalog scan. Previously, ManuallyCompileFiles ran from a
        // background monitor thread — too late (the decision was already
        // made). Now we trigger it from this CloseTES hook which runs on
        // the main thread DURING the catalog scan. We call it repeatedly
        // (idempotent) to compile files as they're cataloged, and keep
        // re-asserting loadingFiles=true so the engine sees it when it
        // checks on the main thread after the catalog scan ends.
        // ================================================================
        inline SafetyHookInline g_hk_CloseTES;

        inline void TESFile_CloseTES(RE::TESFile* a_self, bool a_force)
        {
            auto count = g_closeTESCalls.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 5 || (count % 1000 == 0)) {
                logger::info("  CloseTES #{}: file='{}' force={}",
                    count, a_self ? a_self->fileName : "null", a_force);
            }
            g_hk_CloseTES.call(a_self, a_force);

            // --- v1.22.15: Main-thread compilation trigger ---
            // Static locals are safe here: CloseTES is called from the main thread only.
            static bool s_firstCompileDone = false;
            static std::size_t s_lastCompileAtClose = 0;

            // After first compile: keep re-asserting loadingFiles=true on the main
            // thread until form loading actually starts (OpenTES > 0). This ensures
            // the engine's main-thread check sees it before the form loading decision.
            if (s_firstCompileDone) {
                if (g_openTESCalls.load(std::memory_order_relaxed) == 0) {
                    auto* tdh = RE::TESDataHandler::GetSingleton();
                    if (tdh) {
                        tdh->loadingFiles = true;
                    }
                }

                // Re-compile every 1000 CloseTES calls to pick up newly-cataloged files.
                // SUPPRESSED during ForceLoadAllForms: AE 13753 iterates compiledFileCollection
                // and calling ManuallyCompileFiles here would clear+rebuild it (iterator invalidation).
                if (!g_forceLoadInProgress.load(std::memory_order_relaxed) &&
                    count >= s_lastCompileAtClose + 1000) {
                    s_lastCompileAtClose = count;
                    ManuallyCompileFiles();
                }
                return;
            }

            // First compile: trigger when enough catalog work is done.
            // Check every 200 CloseTES calls starting from 2000 (enough files to matter).
            // Conditions: no normal compilation, no form loading yet, TDH exists with files.
            if (count >= 2000 && count % 200 == 0 &&
                g_addCompileIndexCalls.load(std::memory_order_relaxed) == 0 &&
                g_openTESCalls.load(std::memory_order_relaxed) == 0)
            {
                auto* tdh = RE::TESDataHandler::GetSingleton();
                if (tdh && !tdh->loadingFiles) {
                    // Count current files
                    std::size_t fileCount = 0;
                    for (auto& file : tdh->files) {
                        if (file) ++fileCount;
                    }

                    // Trigger when we have substantial files (>100) and have seen
                    // enough CloseTES calls to have processed them (>= fileCount)
                    if (fileCount > 100 && count >= fileCount) {
                        logger::warn(">>> MAIN-THREAD COMPILE TRIGGER: CloseTES #{}, {} files cataloged, "
                                     "0 AddCompileIndex, 0 OpenTES — triggering ManuallyCompileFiles <<<",
                            count, fileCount);

                        // Prevent monitor thread from also triggering
                        g_manualCompileDone.store(true, std::memory_order_release);

                        ManuallyCompileFiles();

                        s_firstCompileDone = true;
                        s_lastCompileAtClose = count;

                        logger::info(">>> MAIN-THREAD COMPILE DONE: loadingFiles={}, "
                                     "compiledCollection={} reg + {} ESL <<<",
                            tdh->loadingFiles,
                            tdh->compiledFileCollection.files.size(),
                            tdh->compiledFileCollection.smallFiles.size());
                    }
                }
            }
        }

        // ================================================================
        // v1.22.3: Hook TESFile::SeekNextSubrecord — counts subrecord
        // iteration. The loading pass reads subrecords within each form.
        // A high count means the game is reading form data, not just headers.
        // ================================================================
        inline SafetyHookInline g_hk_SeekNextSubrecord;

        inline bool TESFile_SeekNextSubrecord(RE::TESFile* a_self)
        {
            g_seekNextSubrecordCalls.fetch_add(1, std::memory_order_relaxed);
            return g_hk_SeekNextSubrecord.call<bool>(a_self);
        }

        // ================================================================
        // v1.22.3: Hook TESFile::ReadData — counts bytes read from files.
        // Shows total I/O volume (header-only = small, full load = huge).
        // ================================================================
        inline SafetyHookInline g_hk_ReadData;

        inline bool TESFile_ReadData(RE::TESFile* a_self, void* a_buf, std::uint32_t a_inLength)
        {
            g_readDataCalls.fetch_add(1, std::memory_order_relaxed);
            g_readDataBytes.fetch_add(a_inLength, std::memory_order_relaxed);
            return g_hk_ReadData.call<bool>(a_self, a_buf, a_inLength);
        }

        // v1.22.16: AE 11596 hook REMOVED — confirmed never called during initial loading.
        // The initial data load uses a different code path than AE 11596.

        // v1.22.17: Multi-target .text scanner REMOVED in v1.22.18 — served its
        // purpose identifying AE 13698 (form loading, 54 AddForm call sites) and
        // AE 13753 (loading orchestration, 2 OpenTES calls). Scanner caused
        // zero-activity startup (possible Offset2ID allocation interference).

        inline void ReplaceFormMapFunctions()
        {
            const REL::Relocation getForm{ RELOCATION_ID(14461, 14617) };
            g_hk_GetFormByNumericId = safetyhook::create_inline(getForm.address(), TESForm_GetFormByNumericId);

            const REL::Relocation RemoveAt{ RELOCATION_ID(14514, 14710) };
            g_hk_RemoveAt = safetyhook::create_inline(RemoveAt.address(), FormMap_RemoveAt);

            // DISABLED: SetAt callsite hooks may corrupt TESForm::ctor with wrong
            // offsets for AE 1.6.1170, preventing all form creation after SKSE loads.
            // The 166 pre-existing forms (created before SKSE) are the only ones that
            // survive, while all ESM form creation silently fails.
            // TODO: Verify correct offsets for AE 1.6.1170 before re-enabling.
            logger::info("form caching: SetAt callsite hooks DISABLED (investigating form creation failure)"sv);
#if 0
            // there is one call that is not the form table so we will callsite hook
#ifdef SKYRIM_AE
            constexpr std::array todoSetAt = {
                std::pair(14593, 0x2B0),
                std::pair(14593, 0x301),
                std::pair(14666, 0xFE)
            };
#else
            constexpr std::array todoSetAt = {
                std::pair(14438, 0x1DE),
                std::pair(14438, 0x214),
                std::pair(14508, 0x16C),
                std::pair(14508, 0x1A2)
            };
#endif

            for (auto& [id, offset] : todoSetAt) {
                REL::Relocation target{ REL::ID(id), offset };
                orig_FormScatterTable_SetAt = target.write_call<5>(FormScatterTable_SetAt);
            }
#endif

            const REL::Relocation ClearData{ RELOCATION_ID(13646, 13754) };
            g_hk_ClearData = safetyhook::create_inline(ClearData.address(), TESDataHandler_ClearData);

            const REL::Relocation InitializeFormDataStructures{ RELOCATION_ID(14511, 14669) };
            g_hk_InitializeFormDataStructures = safetyhook::create_inline(InitializeFormDataStructures.address(), TESForm_InitializeFormDataStructures);

            // v1.22.0: Hook AddFormToDataHandler to count form registrations
            const REL::Relocation AddForm{ RELOCATION_ID(13597, 13693) };
            g_hk_AddFormToDataHandler = safetyhook::create_inline(AddForm.address(), TESDataHandler_AddFormToDataHandler);
            logger::info("form caching: AddFormToDataHandler hook installed (counting form registrations)"sv);

            // v1.22.1: Hook TESFile::OpenTES to count file opens
            const REL::Relocation OpenTES{ RELOCATION_ID(13855, 13931) };
            g_hk_OpenTES = safetyhook::create_inline(OpenTES.address(), TESFile_OpenTES);
            logger::info("form caching: OpenTES hook at 0x{:X} (offset 0x{:X})"sv,
                OpenTES.address(), OpenTES.address() - REL::Module::get().base());

            // v1.22.2: Hook AddCompileIndex to count compile index assignments
            const REL::Relocation AddCompileIndex{ RELOCATION_ID(14509, 14667) };
            g_hk_AddCompileIndex = safetyhook::create_inline(AddCompileIndex.address(), TESForm_AddCompileIndex);
            logger::info("form caching: AddCompileIndex hook at 0x{:X} (offset 0x{:X})"sv,
                AddCompileIndex.address(), AddCompileIndex.address() - REL::Module::get().base());

            // v1.22.2: Hook SeekNextForm to count record iteration
            const REL::Relocation SeekNextForm{ RELOCATION_ID(13894, 13979) };
            g_hk_SeekNextForm = safetyhook::create_inline(SeekNextForm.address(), TESFile_SeekNextForm);
            logger::info("form caching: SeekNextForm hook at 0x{:X} (offset 0x{:X})"sv,
                SeekNextForm.address(), SeekNextForm.address() - REL::Module::get().base());

            // v1.22.3: Hook CloseTES to detect file close after catalog
            const REL::Relocation CloseTES{ RELOCATION_ID(13857, 13933) };
            g_hk_CloseTES = safetyhook::create_inline(CloseTES.address(), TESFile_CloseTES);
            logger::info("form caching: CloseTES hook at 0x{:X} (offset 0x{:X})"sv,
                CloseTES.address(), CloseTES.address() - REL::Module::get().base());

            // v1.22.3: Hook SeekNextSubrecord to count subrecord reads
            const REL::Relocation SeekNextSubrecord{ RELOCATION_ID(13903, 13990) };
            g_hk_SeekNextSubrecord = safetyhook::create_inline(SeekNextSubrecord.address(), TESFile_SeekNextSubrecord);
            logger::info("form caching: SeekNextSubrecord hook at 0x{:X}"sv, SeekNextSubrecord.address());

            // v1.22.3: Hook ReadData to count bytes read
            const REL::Relocation ReadData{ RELOCATION_ID(13904, 13991) };
            g_hk_ReadData = safetyhook::create_inline(ReadData.address(), TESFile_ReadData);
            logger::info("form caching: ReadData hook at 0x{:X}"sv, ReadData.address());

            // Log all hook addresses for verification
            logger::info("form caching: hook summary — GetForm=0x{:X} AddForm=0x{:X} OpenTES=0x{:X} AddCompileIdx=0x{:X} SeekNextForm=0x{:X} CloseTES=0x{:X}"sv,
                getForm.address(), AddForm.address(), OpenTES.address(),
                AddCompileIndex.address(), SeekNextForm.address(), CloseTES.address());

            // v1.22.17 scanner results (now removed — caused startup hang):
            //   AE 13698 at 0x1B1D30: 54 AddFormToDataHandler calls = FORM LOADING FUNCTION
            //   AE 13753 at 0x1B96E0: 2 OpenTES calls = LOADING ORCHESTRATION
            //   AE 13785 at 0x1BE460: 3 OpenTES calls = HotLoadPlugin (irrelevant)
            // See ForceLoadAllForms() for the actual fix.
        }

        // Cached plugins.txt data (parsed once, reused across idempotent calls)
        inline std::set<std::string> g_enabledNames;   // lowercase enabled plugin names
        inline std::set<std::string> g_disabledNames;  // lowercase disabled plugin names
        inline bool g_pluginsTxtParsed = false;
        inline bool g_pluginsTxtLoaded = false;

        inline void EnsurePluginsTxtLoaded()
        {
            if (g_pluginsTxtLoaded) return;
            g_pluginsTxtLoaded = true;

            WCHAR localAppData[MAX_PATH] = {};
            HRESULT hr = SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData);
            if (hr != S_OK) return;

            std::wstring pluginsPath = localAppData;
            pluginsPath += L"\\Skyrim Special Edition\\Plugins.txt";

            HANDLE hFile = CreateFileW(pluginsPath.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

            if (hFile == INVALID_HANDLE_VALUE) return;

            DWORD fileSize = GetFileSize(hFile, NULL);
            if (fileSize > 0 && fileSize < 10 * 1024 * 1024) {
                std::vector<char> buf(fileSize + 1, 0);
                DWORD bytesRead = 0;
                if (ReadFile(hFile, buf.data(), fileSize, &bytesRead, NULL) && bytesRead > 0) {
                    std::string content(buf.data(), bytesRead);
                    std::istringstream stream(content);
                    std::string line;
                    while (std::getline(stream, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line.empty() || line[0] == '#') continue;

                        if (line[0] == '*') {
                            std::string name = line.substr(1);
                            std::transform(name.begin(), name.end(), name.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            g_enabledNames.insert(std::move(name));
                        } else {
                            std::string name = line;
                            std::transform(name.begin(), name.end(), name.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            g_disabledNames.insert(std::move(name));
                        }
                    }
                    g_pluginsTxtParsed = true;
                }
            }
            CloseHandle(hFile);

            logger::info("ManuallyCompileFiles: plugins.txt parsed={}, {} enabled, {} disabled",
                g_pluginsTxtParsed, g_enabledNames.size(), g_disabledNames.size());
        }

        // Manual compilation: assign compile indices to all enabled files
        // and populate compiledFileCollection. This replicates what the
        // engine's CompileFiles does when it doesn't skip.
        //
        // v1.22.14: The kActive flag is set BY the compilation process, so it's
        // always 0 when compilation is skipped. Instead, we parse plugins.txt
        // to determine which files are enabled (same source the engine uses).
        //
        // v1.22.15: IDEMPOTENT — safe to call multiple times. Skips files that
        // already have a compile index assigned (compileIndex != 0xFF). This
        // allows incremental compilation during the catalog scan: each call
        // processes any newly-cataloged files since the previous call.
        inline void ManuallyCompileFiles()
        {
            auto* tdh = RE::TESDataHandler::GetSingleton();
            if (!tdh) {
                logger::error("ManuallyCompileFiles: TDH is null!"sv);
                return;
            }

            // Parse plugins.txt once (cached for subsequent calls)
            EnsurePluginsTxtLoaded();

            // --- Find current max compile indices (for idempotent re-entry) ---
            std::uint8_t nextRegIdx = 0;
            std::uint16_t nextEslIdx = 0;

            for (auto& file : tdh->files) {
                if (!file || file->compileIndex == 0xFF) continue;
                if (file->compileIndex == 0xFE) {
                    // ESL — track highest smallFileCompileIndex
                    if (file->smallFileCompileIndex >= nextEslIdx)
                        nextEslIdx = file->smallFileCompileIndex + 1;
                } else {
                    // Regular — track highest compileIndex
                    if (file->compileIndex >= nextRegIdx)
                        nextRegIdx = file->compileIndex + 1;
                }
            }

            // --- Assign compile indices to NEW files only ---
            std::size_t regCount = 0;
            std::size_t eslCount = 0;
            std::size_t skippedAlready = 0;
            std::size_t skippedDisabled = 0;

            for (auto& file : tdh->files) {
                if (!file) continue;

                // Skip files already compiled (idempotent guard)
                if (file->compileIndex != 0xFF) {
                    ++skippedAlready;
                    continue;
                }

                // Determine if this file should be compiled
                bool shouldCompile;
                if (g_pluginsTxtParsed) {
                    std::string lowerName(file->fileName);
                    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    bool isEnabled = g_enabledNames.count(lowerName) > 0;
                    bool isDisabled = g_disabledNames.count(lowerName) > 0;
                    shouldCompile = isEnabled || !isDisabled;
                } else {
                    shouldCompile = true;  // fallback: compile everything
                }

                if (!shouldCompile) {
                    ++skippedDisabled;
                    continue;
                }

                if (file->IsLight()) {
                    if (nextEslIdx <= 0xFFF) {
                        file->compileIndex = 0xFE;
                        file->smallFileCompileIndex = nextEslIdx++;
                        tdh->compiledFileCollection.smallFiles.push_back(file);
                        ++eslCount;
                    } else {
                        logger::warn("ManualCompile: ESL limit exceeded at '{}'", file->fileName);
                    }
                } else {
                    if (nextRegIdx <= 0xFD) {
                        file->compileIndex = nextRegIdx++;
                        tdh->compiledFileCollection.files.push_back(file);
                        ++regCount;
                    } else {
                        logger::warn("ManualCompile: regular plugin limit exceeded at '{}'", file->fileName);
                    }
                }
            }

            // Set loadingFiles = true so the engine proceeds with form loading
            tdh->loadingFiles = true;

            logger::info("ManuallyCompileFiles: assigned {} reg + {} ESL = {} NEW (skipped {} already-compiled, {} disabled)",
                regCount, eslCount, regCount + eslCount, skippedAlready, skippedDisabled);
            logger::info("ManuallyCompileFiles: compiledFileCollection.files={} .smallFiles={} | loadingFiles=TRUE",
                tdh->compiledFileCollection.files.size(),
                tdh->compiledFileCollection.smallFiles.size());
        }

        // ================================================================
        // v1.22.21: ForceLoadAllForms — VALIDATION BYPASS + CLEAN COMPILE STATE
        //
        // Called at kDataLoaded when form loading was skipped (0 AddForm
        // calls). Under Wine with 600+ plugins, the engine skips
        // CompileFiles and consequently all form loading.
        //
        // BREAKTHROUGH (v1.22.19 hex dump analysis):
        // AE 13753 is the engine's form loading orchestrator. It has:
        //   1. Correct signature: void(TESDataHandler*, bool)
        //      (v1.22.18 called with wrong 1-param signature!)
        //   2. Validation loop at +0xE0 iterates ALL files in TDH->files:
        //      - Calls SomeFunc (module+0x1C8E40): false → skip file
        //      - Calls AnotherFunc (module+0x1C6E90): false → ABORT ALL
        //   3. Abort instruction at +0x106: JE (74 09) → abort path
        //   4. Form loading loop at +0x11C runs ONLY if validation passes
        //
        // Fix: Temporarily NOP the abort jump (74 09 → 90 90) to convert
        // the abort into a skip-to-next-file. Then call AE 13753 with
        // the correct 2-param signature. The engine's own form loading
        // loop at +0x11C then runs, loading all forms natively.
        // ================================================================
        // ================================================================
        // v1.22.36: Binary patch at AE 29846+0x95 — targeted crash fix
        // with form ID resolution.
        //
        // The instruction `test dword ptr [rax+0x28], 0x3FF` crashes when
        // RAX holds a raw form ID (< 0x1'00000000) instead of a pointer.
        //
        // Code cave logic:
        //   1. Check if RAX high 32 bits != 0 → valid pointer → original TEST
        //   2. If form ID: call TESForm::GetFormByNumericId to resolve
        //   3. If resolved: replace RAX, execute original TEST
        //   4. If not resolved: xor eax,eax + ZF=1, skip
        //
        // v1.22.36: Now resolves form IDs instead of just zeroing RAX.
        // This prevents cascading null-pointer failures downstream.
        // ================================================================
        inline void PatchFormPointerValidation()
        {
#ifdef SKYRIM_AE
            REL::Relocation<std::uintptr_t> fn29846{ REL::ID(29846) };
            auto* patchSite = reinterpret_cast<std::uint8_t*>(fn29846.address() + 0x95);
            auto* returnSite = patchSite + 7; // +0x9C = after the 7-byte TEST

            logger::info("PatchFormPointerValidation: bytes at 29846+0x95: "
                "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                patchSite[0], patchSite[1], patchSite[2], patchSite[3],
                patchSite[4], patchSite[5], patchSite[6], patchSite[7]);

            if (patchSite[0] != 0xF7 || patchSite[1] != 0x40 || patchSite[2] != 0x28) {
                logger::error("PatchFormPointerValidation: unexpected bytes — NOT patching");
                return;
            }

            // Get the address of TESForm::GetFormByNumericId (AE 14617)
            REL::Relocation<std::uintptr_t> fnGetForm{ REL::ID(14617) };
            auto getFormAddr = fnGetForm.address();
            logger::info("PatchFormPointerValidation: GetFormByNumericId at 0x{:X}", getFormAddr);

            // Allocate code cave from SKSE trampoline (near SkyrimSE.exe for rel32)
            auto caveSize = 256;
            auto& trampoline = SKSE::GetTrampoline();
            auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(caveSize));
            auto returnAddr = reinterpret_cast<std::uint64_t>(returnSite);

            int off = 0;

            // ---- CHECK IF RAX IS A VALID POINTER ----
            // push r11
            cave[off++] = 0x41; cave[off++] = 0x53;
            // mov r11, rax
            cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xC3;
            // shr r11, 32
            cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEB; cave[off++] = 0x20;
            // test r11, r11
            cave[off++] = 0x4D; cave[off++] = 0x85; cave[off++] = 0xDB;
            // pop r11
            cave[off++] = 0x41; cave[off++] = 0x5B;
            // jz .form_id
            cave[off++] = 0x74;
            int jzOffset = off;
            cave[off++] = 0x00;

            // ---- VALID POINTER PATH ----
            // test dword ptr [rax+28h], 3FFh
            cave[off++] = 0xF7; cave[off++] = 0x40; cave[off++] = 0x28;
            cave[off++] = 0xFF; cave[off++] = 0x03; cave[off++] = 0x00; cave[off++] = 0x00;
            // jmp [rip+xx] → returnSite
            cave[off++] = 0xFF; cave[off++] = 0x25;
            cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
            std::memcpy(&cave[off], &returnAddr, 8); off += 8;

            // ---- FORM ID PATH: resolve via GetFormByNumericId ----
            int formIdPathStart = off;
            cave[jzOffset] = static_cast<std::uint8_t>(formIdPathStart - (jzOffset + 1));

            // Save volatile registers (preserve caller state for the call)
            // push rcx
            cave[off++] = 0x51;
            // push rdx
            cave[off++] = 0x52;
            // push r8
            cave[off++] = 0x41; cave[off++] = 0x50;
            // push r9
            cave[off++] = 0x41; cave[off++] = 0x51;
            // push r10
            cave[off++] = 0x41; cave[off++] = 0x52;
            // push r11
            cave[off++] = 0x41; cave[off++] = 0x53;
            // push rax (save original form ID)
            cave[off++] = 0x50;
            // sub rsp, 0x28 (shadow space 0x20 + 8 alignment: 7 pushes=56, +0x28=96, 96%16=0)
            cave[off++] = 0x48; cave[off++] = 0x83; cave[off++] = 0xEC; cave[off++] = 0x28;

            // mov ecx, eax (form ID → first arg, zero-extended)
            cave[off++] = 0x89; cave[off++] = 0xC1;

            // mov rax, <GetFormByNumericId address>
            cave[off++] = 0x48; cave[off++] = 0xB8;
            std::memcpy(&cave[off], &getFormAddr, 8); off += 8;

            // call rax
            cave[off++] = 0xFF; cave[off++] = 0xD0;

            // test rax, rax (check if form was found)
            cave[off++] = 0x48; cave[off++] = 0x85; cave[off++] = 0xC0;

            // jz .not_found
            cave[off++] = 0x74;
            int jzNotFoundOff = off;
            cave[off++] = 0x00;

            // ---- FOUND: RAX = resolved TESForm* ----
            // add rsp, 0x30 (0x28 shadow + 8 skip saved rax)
            cave[off++] = 0x48; cave[off++] = 0x83; cave[off++] = 0xC4; cave[off++] = 0x30;
            // pop r11
            cave[off++] = 0x41; cave[off++] = 0x5B;
            // pop r10
            cave[off++] = 0x41; cave[off++] = 0x5A;
            // pop r9
            cave[off++] = 0x41; cave[off++] = 0x59;
            // pop r8
            cave[off++] = 0x41; cave[off++] = 0x58;
            // pop rdx
            cave[off++] = 0x5A;
            // pop rcx
            cave[off++] = 0x59;
            // RAX holds resolved pointer — execute original TEST
            // test dword ptr [rax+28h], 3FFh
            cave[off++] = 0xF7; cave[off++] = 0x40; cave[off++] = 0x28;
            cave[off++] = 0xFF; cave[off++] = 0x03; cave[off++] = 0x00; cave[off++] = 0x00;
            // jmp [rip+xx] → returnSite
            cave[off++] = 0xFF; cave[off++] = 0x25;
            cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
            std::memcpy(&cave[off], &returnAddr, 8); off += 8;

            // ---- NOT FOUND: zero RAX as last resort ----
            int notFoundStart = off;
            cave[jzNotFoundOff] = static_cast<std::uint8_t>(notFoundStart - (jzNotFoundOff + 1));
            // add rsp, 0x30 (0x28 shadow + 8 skip saved rax)
            cave[off++] = 0x48; cave[off++] = 0x83; cave[off++] = 0xC4; cave[off++] = 0x30;
            // pop r11
            cave[off++] = 0x41; cave[off++] = 0x5B;
            // pop r10
            cave[off++] = 0x41; cave[off++] = 0x5A;
            // pop r9
            cave[off++] = 0x41; cave[off++] = 0x59;
            // pop r8
            cave[off++] = 0x41; cave[off++] = 0x58;
            // pop rdx
            cave[off++] = 0x5A;
            // pop rcx
            cave[off++] = 0x59;
            // xor eax, eax (RAX=0, ZF=1)
            cave[off++] = 0x31; cave[off++] = 0xC0;
            // jmp [rip+xx] → returnSite
            cave[off++] = 0xFF; cave[off++] = 0x25;
            cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
            std::memcpy(&cave[off], &returnAddr, 8); off += 8;

            logger::info("PatchFormPointerValidation: code cave built, {} bytes at 0x{:X}",
                off, reinterpret_cast<std::uintptr_t>(cave));

            // Patch the original site: 5-byte JMP rel32 + 2 NOPs
            DWORD oldProtect;
            VirtualProtect(patchSite, 7, PAGE_EXECUTE_READWRITE, &oldProtect);

            auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
            auto jmpFrom = reinterpret_cast<std::intptr_t>(patchSite + 5);
            auto rel32 = static_cast<std::int32_t>(jmpTarget - jmpFrom);

            auto actualDist = jmpTarget - jmpFrom;
            if (actualDist > INT32_MAX || actualDist < INT32_MIN) {
                logger::error("PatchFormPointerValidation: JMP distance too large ({} bytes) — NOT patching", actualDist);
                VirtualProtect(patchSite, 7, oldProtect, &oldProtect);
                return;
            }

            patchSite[0] = 0xE9;
            std::memcpy(&patchSite[1], &rel32, 4);
            patchSite[5] = 0x90;
            patchSite[6] = 0x90;

            VirtualProtect(patchSite, 7, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), patchSite, 7);

            logger::info("PatchFormPointerValidation: patched 29846+0x95 → code cave (rel32={})", rel32);
#endif
        }

        // v1.22.36: Binary patches at hot VEH addresses with inline null checks.
        // Eliminates VEH overhead for the most-hit crash sites by checking for
        // null/sentinel form pointers before the faulting instruction.
        inline void PatchHotSpotNullChecks()
        {
#ifdef SKYRIM_AE
            auto base = REL::Module::get().base();
            auto& trampoline = SKSE::GetTrampoline();

            // ── Patch A: +0x2ED880 — null check before call [rax+0x4B8] ──
            // Original: FF 90 B8 04 00 00 (6 bytes)
            // If RAX is null or in sentinel page, skip the call and return 0.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2ED880);
                auto returnAddr = reinterpret_cast<std::uint64_t>(site + 6);

                logger::info("PatchHotSpotA: bytes at +0x2ED880: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                if (site[0] != 0xFF || site[1] != 0x90 || site[2] != 0xB8 ||
                    site[3] != 0x04 || site[4] != 0x00 || site[5] != 0x00) {
                    logger::error("PatchHotSpotA: unexpected bytes — NOT patching");
                } else {
                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(80));
                    int off = 0;

                    // test rax, rax
                    cave[off++] = 0x48; cave[off++] = 0x85; cave[off++] = 0xC0;
                    // jz .null_form
                    cave[off++] = 0x74;
                    int jzOff = off;
                    cave[off++] = 0x00; // placeholder

                    // Check sentinel range using R11 (volatile, destroyed by call)
                    if (g_zeroPage) {
                        // mov r11, <sentinelBase>
                        cave[off++] = 0x49; cave[off++] = 0xBB;
                        std::memcpy(&cave[off], &g_zeroPageBase, 8); off += 8;
                        // cmp rax, r11
                        cave[off++] = 0x4C; cave[off++] = 0x39; cave[off++] = 0xD8;
                        // jb .do_call
                        cave[off++] = 0x72;
                        int jbCallOff = off;
                        cave[off++] = 0x00;
                        // add r11, 0x10000
                        cave[off++] = 0x49; cave[off++] = 0x81; cave[off++] = 0xC3;
                        cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x01; cave[off++] = 0x00;
                        // cmp rax, r11
                        cave[off++] = 0x4C; cave[off++] = 0x39; cave[off++] = 0xD8;
                        // jb .null_form (in sentinel range)
                        cave[off++] = 0x72;
                        int jbNullOff = off;
                        cave[off++] = 0x00;

                        // .do_call:
                        int doCallStart = off;
                        cave[jbCallOff] = static_cast<std::uint8_t>(doCallStart - (jbCallOff + 1));

                        // Original: call qword ptr [rax+0x4B8]
                        cave[off++] = 0xFF; cave[off++] = 0x90;
                        cave[off++] = 0xB8; cave[off++] = 0x04; cave[off++] = 0x00; cave[off++] = 0x00;
                        // jmp [rip+0] → returnSite
                        cave[off++] = 0xFF; cave[off++] = 0x25;
                        cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                        std::memcpy(&cave[off], &returnAddr, 8); off += 8;

                        // .null_form:
                        int nullStart = off;
                        cave[jzOff] = static_cast<std::uint8_t>(nullStart - (jzOff + 1));
                        cave[jbNullOff] = static_cast<std::uint8_t>(nullStart - (jbNullOff + 1));
                    } else {
                        // No sentinel — just null check
                        // Original: call qword ptr [rax+0x4B8]
                        cave[off++] = 0xFF; cave[off++] = 0x90;
                        cave[off++] = 0xB8; cave[off++] = 0x04; cave[off++] = 0x00; cave[off++] = 0x00;
                        // jmp [rip+0] → returnSite
                        cave[off++] = 0xFF; cave[off++] = 0x25;
                        cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                        std::memcpy(&cave[off], &returnAddr, 8); off += 8;

                        // .null_form:
                        int nullStart = off;
                        cave[jzOff] = static_cast<std::uint8_t>(nullStart - (jzOff + 1));
                    }

                    // xor eax, eax
                    cave[off++] = 0x31; cave[off++] = 0xC0;
                    // jmp [rip+0] → returnSite
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &returnAddr, 8); off += 8;

                    logger::info("PatchHotSpotA: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + NOP (1 byte)
                    DWORD oldProt;
                    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotA: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 6, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90;
                        VirtualProtect(site, 6, oldProt, &oldProt);
                        FlushInstructionCache(GetCurrentProcess(), site, 6);
                        logger::info("PatchHotSpotA: +0x2ED880 patched (rel32={})", rel32);
                    }
                }
            }

            // ── Patch B: +0x32D146 — null check before cmp byte [rax+0x1A] ──
            // Original: 80 78 1A 2B 4C 0F 44 D0 (8 bytes)
            // cmp byte [rax+0x1A], 0x2B; cmovz r10, rax
            // If RAX is null, skip both instructions (type won't match anyway).
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x32D146);
                auto returnAddr = reinterpret_cast<std::uint64_t>(site + 8);

                logger::info("PatchHotSpotB: bytes at +0x32D146: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3],
                    site[4], site[5], site[6], site[7]);

                if (site[0] != 0x80 || site[1] != 0x78 || site[2] != 0x1A || site[3] != 0x2B ||
                    site[4] != 0x4C || site[5] != 0x0F || site[6] != 0x44 || site[7] != 0xD0) {
                    logger::error("PatchHotSpotB: unexpected bytes — NOT patching");
                } else {
                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(48));
                    int off = 0;

                    // test rax, rax
                    cave[off++] = 0x48; cave[off++] = 0x85; cave[off++] = 0xC0;
                    // jz .skip
                    cave[off++] = 0x74;
                    int jzOff = off;
                    cave[off++] = 0x00;
                    // Original: cmp byte [rax+0x1A], 0x2B
                    cave[off++] = 0x80; cave[off++] = 0x78; cave[off++] = 0x1A; cave[off++] = 0x2B;
                    // Original: cmovz r10, rax
                    cave[off++] = 0x4C; cave[off++] = 0x0F; cave[off++] = 0x44; cave[off++] = 0xD0;
                    // .skip:
                    int skipStart = off;
                    cave[jzOff] = static_cast<std::uint8_t>(skipStart - (jzOff + 1));
                    // jmp [rip+0] → returnSite
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &returnAddr, 8); off += 8;

                    logger::info("PatchHotSpotB: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 3 NOPs
                    DWORD oldProt;
                    VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotB: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 8, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; site[6] = 0x90; site[7] = 0x90;
                        VirtualProtect(site, 8, oldProt, &oldProt);
                        FlushInstructionCache(GetCurrentProcess(), site, 8);
                        logger::info("PatchHotSpotB: +0x32D146 patched (rel32={})", rel32);
                    }
                }
            }

            logger::info("PatchHotSpotNullChecks: done");
#endif
        }

        inline void ForceLoadAllForms()
        {
#ifdef SKYRIM_AE
            auto* tdh = RE::TESDataHandler::GetSingleton();
            if (!tdh) {
                logger::error("ForceLoadAllForms: TDH is null — cannot proceed"sv);
                return;
            }

            logger::info("=== ForceLoadAllForms (v1.22.27) ==="sv);

            // Log TDH state before reset
            logger::info("  TDH: loadingFiles={} hasDesiredFiles={} saveLoadGame={} clearingData={}",
                tdh->loadingFiles, tdh->hasDesiredFiles, tdh->saveLoadGame, tdh->clearingData);
            logger::info("  TDH: nextID=0x{:08X} activeFile={} AddForm={}",
                tdh->nextID,
                tdh->activeFile ? tdh->activeFile->fileName : "null",
                g_addFormCalls.load(std::memory_order_relaxed));

            // ==========================================================
            // CRITICAL: Reset all compile state before calling AE 13753.
            //
            // ManuallyCompileFiles (CloseTES hook) pre-sets file->compileIndex
            // on each TESFile and populates compiledFileCollection. When AE
            // 13753 runs its own compilation pass (calling AddCompileIndex per
            // form), the pre-set compileIndex causes TESForm::AddCompileIndex
            // to enter an infinite loop for some files (observed: Denizens of
            // Morthal - AI Overhaul Patch.esp looped 1.5 billion times).
            //
            // Root cause: TESForm::AddCompileIndex uses file->compileIndex to
            // build the global form slot. When the slot was pre-assigned by
            // ManuallyCompileFiles, the function's internal file iterator
            // cannot advance, producing an infinite loop.
            //
            // Fix: wipe all compile state so AE 13753 compiles from scratch.
            // This makes its compilation pass work cleanly with no conflicts.
            // ==========================================================
            std::size_t resetCount = 0;
            for (auto& file : tdh->files) {
                if (!file) continue;
                if (file->compileIndex != 0xFF) {
                    file->compileIndex = 0xFF;
                    file->smallFileCompileIndex = 0;
                    ++resetCount;
                }
            }
            auto prevReg = tdh->compiledFileCollection.files.size();
            auto prevEsl = tdh->compiledFileCollection.smallFiles.size();
            tdh->compiledFileCollection.files.clear();
            tdh->compiledFileCollection.smallFiles.clear();

            logger::info("  Compile state RESET: {} files cleared, compiledFileCollection was {} reg + {} ESL",
                resetCount, prevReg, prevEsl);

            // Set required flags
            tdh->loadingFiles = true;
            tdh->hasDesiredFiles = true;
            tdh->clearingData = false;

            // ==========================================================
            // VALIDATION BYPASS: NOP the abort jump in AE 13753
            //
            // At function offset +0x106, the instruction 74 09 (JE +9)
            // jumps to the abort path when a per-file validation function
            // (module+0x1C6E90) returns false. This aborts form loading
            // for ALL files even if only one file fails validation.
            //
            // We NOP this to 90 90, converting abort into skip-to-next.
            // The form loading loop at +0x11C then executes normally.
            // ==========================================================
            REL::Relocation<std::uintptr_t> fn13753{ REL::ID(13753) };
            auto fnAddr = fn13753.address();

            // Also log the function address for diagnostics
            logger::info("  AE 13753: addr=0x{:X} offset=0x{:X}",
                fnAddr, fnAddr - REL::Module::get().base());

            auto* patchAddr = reinterpret_cast<std::uint8_t*>(fnAddr + 0x106);

            // Verify the bytes match our disassembly
            if (patchAddr[0] != 0x74 || patchAddr[1] != 0x09) {
                logger::error("ForceLoadAllForms: UNEXPECTED bytes at AE 13753 +0x106: {:02X} {:02X} (expected 74 09)",
                    patchAddr[0], patchAddr[1]);
                logger::error("Binary layout mismatch — cannot apply validation bypass. Aborting.");
                return;
            }

            // Also verify surrounding context to make sure we have the right spot
            // Expected at +0x104: 84 C0 (test al, al) — the result check before the JE
            auto* contextAddr = reinterpret_cast<std::uint8_t*>(fnAddr + 0x104);
            if (contextAddr[0] != 0x84 || contextAddr[1] != 0xC0) {
                logger::error("ForceLoadAllForms: context mismatch at +0x104: {:02X} {:02X} (expected 84 C0)",
                    contextAddr[0], contextAddr[1]);
                logger::error("This is not the expected test-al-al before the JE. Aborting.");
                return;
            }

            logger::info("  Verified: +0x104=84 C0 (test al,al) +0x106=74 09 (je +9) — match!");

            // Save original bytes for restoration
            std::uint8_t origBytes[2] = { patchAddr[0], patchAddr[1] };

            // Apply the NOP patch
            DWORD oldProtect;
            if (!VirtualProtect(patchAddr, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                logger::error("ForceLoadAllForms: VirtualProtect failed (error={})", GetLastError());
                return;
            }
            patchAddr[0] = 0x90;  // NOP
            patchAddr[1] = 0x90;  // NOP
            VirtualProtect(patchAddr, 2, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), patchAddr, 2);

            logger::info("  PATCH APPLIED: AE 13753 +0x106 = 90 90 (validation abort → skip-to-next-file)");

            // Record counters before the call
            auto addFormBefore = g_addFormCalls.load(std::memory_order_relaxed);
            auto openTESBefore = g_openTESCalls.load(std::memory_order_relaxed);

            // Call AE 13753 with CORRECT 2-parameter signature
            // Param 1: TESDataHandler* (rcx)
            // Param 2: bool — false for initial loading, true for save loading (dl)
            using LoadFormsFn = void(*)(RE::TESDataHandler*, bool);
            REL::Relocation<LoadFormsFn> loadForms{ REL::ID(13753) };

            // v1.22.23: Install VEH to skip null dereferences inside AE 13753.
            // Large modlists contain ESL-range actor forms with null race/template
            // pointers. The VEH catches the AV, advances RIP past the bad instruction,
            // and continues — allowing the loading loop to proceed to the next form.
            // The CloseTES guard (g_forceLoadInProgress) prevents ManuallyCompileFiles
            // from invalidating compiledFileCollection while AE 13753 iterates it.
            g_vehSkipCount.store(0, std::memory_order_relaxed);
            g_forceLoadInProgress.store(true, std::memory_order_release);
            auto* vehHandle = AddVectoredExceptionHandler(1, ForceLoadVEH);

            logger::info(">>> Calling AE 13753(TDH*, false) with VEH null guard (v1.22.27) <<<");
            loadForms(tdh, false);

            RemoveVectoredExceptionHandler(vehHandle);
            g_forceLoadInProgress.store(false, std::memory_order_release);
            auto skipCount = g_vehSkipCount.load(std::memory_order_relaxed);
            logger::info("  VEH skipped {} null dereferences during AE 13753", skipCount);

            auto addFormAfter = g_addFormCalls.load(std::memory_order_relaxed);
            auto openTESAfter = g_openTESCalls.load(std::memory_order_relaxed);

            // Restore original bytes immediately
            VirtualProtect(patchAddr, 2, PAGE_EXECUTE_READWRITE, &oldProtect);
            patchAddr[0] = origBytes[0];
            patchAddr[1] = origBytes[1];
            VirtualProtect(patchAddr, 2, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), patchAddr, 2);

            logger::info("  PATCH RESTORED: AE 13753 +0x106 = {:02X} {:02X}", origBytes[0], origBytes[1]);

            // Report results
            auto addFormDelta = addFormAfter - addFormBefore;
            auto openTESDelta = openTESAfter - openTESBefore;

            logger::info("=== ForceLoadAllForms (v1.22.27) RESULTS ==="sv);
            logger::info("  AddFormToDataHandler: {} → {} (+{})", addFormBefore, addFormAfter, addFormDelta);
            logger::info("  OpenTES calls: {} → {} (+{})", openTESBefore, openTESAfter, openTESDelta);
            logger::info("  TDH: nextID=0x{:08X} activeFile={}",
                tdh->nextID,
                tdh->activeFile ? tdh->activeFile->fileName : "null");
            logger::info("  TDH: loadingFiles={} compiledFiles={} compiledSmallFiles={}",
                tdh->loadingFiles,
                tdh->compiledFileCollection.files.size(),
                tdh->compiledFileCollection.smallFiles.size());

            // v1.22.22: Diagnostic — enumerate files that AE 13753 skipped (still at 0xFF).
            // These files were NOT loaded by the engine. Possible causes:
            //   (a) File not installed on disk (SomeFunc/existence check failed)
            //   (b) File skipped by per-file validation (SomeFunc at module+0x1C8E40)
            //   (c) File skipped by AnotherFunc (module+0x1C6E90) despite our NOP
            {
                std::vector<std::string> skippedFiles;
                std::size_t totalInTDH = 0;
                for (auto& file : tdh->files) {
                    if (!file) continue;
                    ++totalInTDH;
                    if (file->compileIndex == 0xFF && file->smallFileCompileIndex == 0) {
                        // Still uncompiled — AE 13753 skipped this file
                        skippedFiles.push_back(std::string(file->fileName ? file->fileName : "<null>"));
                    }
                }
                logger::info("  TDH->files total: {} | skipped by AE 13753: {}", totalInTDH, skippedFiles.size());
                if (!skippedFiles.empty()) {
                    logger::warn("=== FILES SKIPPED BY AE 13753 (compileIndex still 0xFF) ===");
                    // Log up to 100 skipped files to keep log manageable
                    std::size_t logLimit = std::min(skippedFiles.size(), std::size_t(100));
                    for (std::size_t i = 0; i < logLimit; ++i) {
                        logger::warn("  SKIPPED[{}]: {}", i, skippedFiles[i]);
                    }
                    if (skippedFiles.size() > 100) {
                        logger::warn("  ... and {} more skipped files (truncated)", skippedFiles.size() - 100);
                    }
                    logger::warn("=== END SKIPPED FILES LIST ===");
                }
            }

            if (addFormDelta > 100) {
                logger::info(">>> SUCCESS! Engine loaded {} forms via native form loading! <<<", addFormDelta);
            } else if (addFormDelta > 0) {
                logger::warn("Partial result: {} forms loaded (may need further investigation)", addFormDelta);
            } else {
                logger::error("NO forms loaded — validation bypass alone is insufficient");
            }

            // ==========================================================
            // v1.22.36: Hex dump AE 13753 validation loop for future analysis.
            // The per-file validation at +0xE0 to +0x120 determines which
            // files get loaded. Dumping bytes helps reverse-engineer the
            // skip logic that blocks 1787 files under Wine.
            // ==========================================================
            {
                auto* fnBytes = reinterpret_cast<const std::uint8_t*>(fnAddr);
                std::string hexDump;
                for (int i = 0xC0; i < 0x140; ++i) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "%02X ", fnBytes[i]);
                    hexDump += buf;
                    if ((i + 1) % 16 == 0) {
                        logger::info("  AE13753+0x{:03X}: {}", i - 15, hexDump);
                        hexDump.clear();
                    }
                }
                if (!hexDump.empty()) {
                    logger::info("  AE13753+0x{:03X}: {}", 0x130, hexDump);
                }
            }

            // ==========================================================
            // v1.22.36: Multi-pass form loading.
            //
            // After the first AE 13753 call, 1787 files are skipped
            // (compileIndex still 0xFF). The per-file validation function
            // (SomeFunc at module+0x1C8E40) rejects them, possibly because
            // their masters weren't compiled yet on the first pass.
            //
            // Strategy: re-compile skipped files and call AE 13753 again.
            // On subsequent passes, more masters are available, so more
            // dependent files may pass validation.
            //
            // Repeat up to 3 additional passes or until no progress.
            // ==========================================================
            for (int pass = 2; pass <= 4; ++pass) {
                // Count currently-skipped files
                std::size_t skippedCount = 0;
                for (auto& file : tdh->files) {
                    if (!file) continue;
                    if (file->compileIndex == 0xFF && file->smallFileCompileIndex == 0)
                        ++skippedCount;
                }

                if (skippedCount == 0) {
                    logger::info("  Multi-pass: all files compiled after pass {} — no more passes needed", pass - 1);
                    break;
                }

                logger::info("=== Multi-pass loading: pass {} ({} files still skipped) ===", pass, skippedCount);

                // Re-compile skipped files (ManuallyCompileFiles is idempotent —
                // only processes files with compileIndex=0xFF)
                ManuallyCompileFiles();

                // Ensure loading state
                tdh->loadingFiles = true;
                tdh->hasDesiredFiles = true;

                // Record counters before pass
                auto addFormBefore2 = g_addFormCalls.load(std::memory_order_relaxed);

                // Call AE 13753 again
                g_vehSkipCount.store(0, std::memory_order_relaxed);
                g_forceLoadInProgress.store(true, std::memory_order_release);
                auto* vehHandle2 = AddVectoredExceptionHandler(1, ForceLoadVEH);

                logger::info("  Calling AE 13753 pass {} ...", pass);
                loadForms(tdh, false);

                RemoveVectoredExceptionHandler(vehHandle2);
                g_forceLoadInProgress.store(false, std::memory_order_release);

                auto addFormDelta2 = g_addFormCalls.load(std::memory_order_relaxed) - addFormBefore2;
                auto vehSkips2 = g_vehSkipCount.load(std::memory_order_relaxed);

                // Count still-skipped files after this pass
                std::size_t stillSkipped = 0;
                for (auto& file : tdh->files) {
                    if (!file) continue;
                    if (file->compileIndex == 0xFF && file->smallFileCompileIndex == 0)
                        ++stillSkipped;
                }

                logger::info("  Pass {} result: {} new forms, {} VEH skips, {} → {} still skipped",
                    pass, addFormDelta2, vehSkips2, skippedCount, stillSkipped);

                if (stillSkipped >= skippedCount) {
                    logger::info("  No progress — stopping multi-pass loading");
                    break;
                }
            }

            // Final skipped files count
            {
                std::size_t finalSkipped = 0;
                for (auto& file : tdh->files) {
                    if (!file) continue;
                    if (file->compileIndex == 0xFF && file->smallFileCompileIndex == 0)
                        ++finalSkipped;
                }
                logger::info("  Final: {} files still skipped after all passes", finalSkipped);
            }

            // ==========================================================
            // v1.22.27: Form Reference Resolution (InitItemImpl)
            //
            // After loading forms, call InitItemImpl() on every form to
            // resolve stored form IDs into live pointers.
            //
            // v1.22.36: Multi-pass InitItem — retry faulted forms after
            // the first pass, since more references may be resolvable.
            // ==========================================================
            if (addFormDelta > 100) {
                logger::info("=== InitItemImpl Phase (v1.22.36 — multi-pass) ==="sv);

                auto* saveLoadGame = RE::BGSSaveLoadGame::GetSingleton();
                if (saveLoadGame) {
                    saveLoadGame->globalFlags.set(
                        RE::BGSSaveLoadGame::GlobalFlags::kInitingForms);
                    logger::info("  Set kInitingForms flag");
                }

                auto initStart = std::chrono::high_resolution_clock::now();

                // Collect image bounds for vtable validation
                auto imgBase = REL::Module::get().base();
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imgBase);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(imgBase + dos->e_lfanew);
                auto imgEnd = imgBase + nt->OptionalHeader.SizeOfImage;

                // Scan the FULL global form hash table
                const auto& [formMap, formLock] = RE::TESForm::GetAllForms();
                std::uint64_t initCount = 0;
                std::uint64_t skipCount = 0;
                std::vector<RE::TESForm*> faultedForms; // Track faulted forms for retry

                if (formMap) {
                    auto* mapRaw = reinterpret_cast<const std::uint8_t*>(formMap);
                    auto capacity = *reinterpret_cast<const std::uint32_t*>(mapRaw + 0x0C);
                    auto sentinel = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x18);
                    auto entriesPtr = *reinterpret_cast<const std::uintptr_t*>(mapRaw + 0x28);

                    logger::info("  Hash table: capacity={}, sentinel=0x{:X}, entries=0x{:X}",
                        capacity, sentinel, entriesPtr);

                    if (capacity > 0 && capacity <= 0x1000000 && entriesPtr >= 0x10000) {
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
                                formPtr < 0x10000 || formPtr > 0x7FFFFFFFFFFF)
                            {
                                ++skipCount;
                                continue;
                            }

                            auto* formObj = reinterpret_cast<const std::uint8_t*>(formPtr);
                            if (IsBadReadPtr(formObj, 0x20)) {
                                ++skipCount;
                                continue;
                            }
                            auto vtable = *reinterpret_cast<const std::uintptr_t*>(formObj);
                            if (vtable < imgBase || vtable >= imgEnd) {
                                ++skipCount;
                                continue;
                            }

                            auto* form = reinterpret_cast<RE::TESForm*>(
                                const_cast<std::uint8_t*>(formObj));
                            if (SafeInitItem(form)) {
                                ++initCount;
                            } else {
                                ++skipCount;
                                faultedForms.push_back(form);
                                auto faulted = g_initFaultCount.fetch_add(1, std::memory_order_relaxed);
                                if (faulted < 20) {
                                    logger::warn("  InitItem faulted on form 0x{:08X} (type={}, vtable=0x{:X})",
                                        form->GetFormID(),
                                        std::to_underlying(form->GetFormType()),
                                        vtable);
                                } else if (faulted == 20) {
                                    logger::warn("  InitItem: suppressing further fault messages (20 logged)");
                                }
                            }
                        }
                    } else {
                        logger::error("  Hash table layout invalid — skipping InitItemImpl");
                    }
                } else {
                    logger::error("  TESForm::GetAllForms() returned null — skipping InitItemImpl");
                }

                auto pass1Elapsed = std::chrono::high_resolution_clock::now() - initStart;
                auto pass1Ms = std::chrono::duration_cast<std::chrono::milliseconds>(pass1Elapsed).count();
                logger::info("  InitItem pass 1: {} initialized, {} faulted, {}ms",
                    initCount, faultedForms.size(), pass1Ms);

                // v1.22.36: Retry faulted forms (up to 3 retry passes).
                // After pass 1 resolves many references, previously-faulting
                // forms may now succeed because their dependencies are resolved.
                for (int retryPass = 2; retryPass <= 4 && !faultedForms.empty(); ++retryPass) {
                    std::vector<RE::TESForm*> stillFaulted;
                    std::uint64_t retryOk = 0;

                    for (auto* form : faultedForms) {
                        if (SafeInitItem(form)) {
                            ++retryOk;
                        } else {
                            stillFaulted.push_back(form);
                        }
                    }

                    logger::info("  InitItem pass {}: {} succeeded, {} still faulted",
                        retryPass, retryOk, stillFaulted.size());

                    if (retryOk == 0) {
                        logger::info("  No progress — stopping InitItem retry");
                        break;
                    }
                    faultedForms = std::move(stillFaulted);
                }

                auto totalElapsed = std::chrono::high_resolution_clock::now() - initStart;
                auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(totalElapsed).count();

                if (saveLoadGame) {
                    saveLoadGame->globalFlags.reset(
                        RE::BGSSaveLoadGame::GlobalFlags::kInitingForms);
                    logger::info("  Cleared kInitingForms flag");
                }

                auto faultCount = g_initFaultCount.load(std::memory_order_relaxed);
                logger::info("  InitItemImpl total: {} initialized, {} permanently faulted, {}ms",
                    initCount, faultedForms.size(), totalMs);

                // ==========================================================
                // v1.22.36: Mark permanently faulted forms as kDeleted.
                //
                // The 179 faulted ActorCharacter forms have null internal
                // pointers (race, base NPC, etc) that cause null-dereference
                // cascades at MULTIPLE engine sites during New Game (AI
                // package evaluation, etc). Patching individual crash sites
                // is whack-a-mole — fixing one just moves the crash.
                //
                // Setting kDeleted (formFlags bit 0x20) tells the engine to
                // skip these forms in ALL processing: AI, rendering, physics,
                // cell attachment, save serialization, etc. The characters
                // simply won't spawn, which is far better than crashing.
                //
                // We also set kDisabled (0x800) as belt-and-suspenders — some
                // engine paths check kDisabled but not kDeleted.
                // ==========================================================
                if (!faultedForms.empty()) {
                    constexpr std::uint32_t kDeleted  = 0x20;
                    constexpr std::uint32_t kDisabled = 0x800;
                    std::size_t flaggedCount = 0;

                    for (auto* form : faultedForms) {
                        if (!form) continue;
                        auto oldFlags = form->formFlags;
                        form->formFlags |= kDeleted | kDisabled;
                        ++flaggedCount;

                        if (flaggedCount <= 10) {
                            logger::warn("  kDeleted+kDisabled: form 0x{:08X} type={} flags 0x{:08X} → 0x{:08X}",
                                form->GetFormID(),
                                std::to_underlying(form->GetFormType()),
                                oldFlags,
                                form->formFlags);
                        }
                    }

                    logger::info("  Flagged {} permanently faulted forms as kDeleted+kDisabled", flaggedCount);
                }

                // v1.22.36: Allocate zero page for null-form substitution.
                // ── Sentinel form infrastructure ──
                // 1. Stub function page (PAGE_EXECUTE_READ): "xor eax,eax; ret"
                g_stubFuncPage = VirtualAlloc(nullptr, 0x1000,
                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (g_stubFuncPage) {
                    auto* code = reinterpret_cast<std::uint8_t*>(g_stubFuncPage);
                    // Return 0: stub function for sentinel form vtable calls.
                    // Returning 1 caused secondary faults (test byte [1+0x40]).
                    // Returning 0 makes callers take "not found" branches cleanly.
                    code[0] = 0x31; code[1] = 0xC0; // xor eax, eax
                    code[2] = 0xC3;                  // ret
                    DWORD oldProt = 0;
                    VirtualProtect(g_stubFuncPage, 0x1000, PAGE_EXECUTE_READ, &oldProt);
                    logger::info("  Stub function at 0x{:X}",
                        reinterpret_cast<std::uintptr_t>(g_stubFuncPage));
                }

                // 2. Stub vtable (PAGE_READONLY): 512 entries → stub function
                g_stubVtable = VirtualAlloc(nullptr, 0x1000,
                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (g_stubVtable && g_stubFuncPage) {
                    auto* vt = reinterpret_cast<DWORD64*>(g_stubVtable);
                    auto stubAddr = reinterpret_cast<DWORD64>(g_stubFuncPage);
                    for (int i = 0; i < 512; ++i)
                        vt[i] = stubAddr;
                    DWORD oldProt = 0;
                    VirtualProtect(g_stubVtable, 0x1000, PAGE_READONLY, &oldProt);
                    logger::info("  Stub vtable at 0x{:X} (512 entries)",
                        reinterpret_cast<std::uintptr_t>(g_stubVtable));
                }

                // 3. Sentinel form page (PAGE_READONLY, 64KB):
                //    Fake TESForm with kDeleted flag, valid vtable, stub pointers.
                //    Engine reads kDeleted and skips; virtual calls → stub → ret 0.
                g_zeroPage = VirtualAlloc(nullptr, 0x10000,
                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (g_zeroPage) {
                    memset(g_zeroPage, 0, 0x10000);
                    auto* page = reinterpret_cast<std::uint8_t*>(g_zeroPage);

                    // Vtable pointer at offset 0x0 → stub vtable
                    if (g_stubVtable) {
                        auto vtAddr = reinterpret_cast<DWORD64>(g_stubVtable);
                        memcpy(page + 0x00, &vtAddr, 8);
                    }

                    // formFlags at offset 0x10 = kDeleted (0x20)
                    auto flags = static_cast<std::uint32_t>(0x20);
                    memcpy(page + 0x10, &flags, 4);

                    // Direct function pointer fields that the engine dereferences:
                    // offset 0x4B8 (seen at +0x2ED880: call [rax+0x4B8])
                    if (g_stubFuncPage) {
                        auto stubAddr = reinterpret_cast<DWORD64>(g_stubFuncPage);
                        memcpy(page + 0x4B8, &stubAddr, 8);
                    }

                    DWORD oldProt = 0;
                    VirtualProtect(g_zeroPage, 0x10000, PAGE_READONLY, &oldProt);
                    g_zeroPageBase = reinterpret_cast<DWORD64>(g_zeroPage);
                    g_zeroPageEnd = g_zeroPageBase + 0x10000;
                    logger::info("  Sentinel form page at 0x{:X} (PAGE_READONLY, kDeleted=0x20, vtable=0x{:X})",
                        g_zeroPageBase,
                        g_stubVtable ? reinterpret_cast<std::uintptr_t>(g_stubVtable) : 0);
                } else {
                    logger::error("  Failed to allocate sentinel form page!");
                }

                // Install persistent form-reference fixup VEH
                g_formFixupCount.store(0, std::memory_order_relaxed);
                g_formFixupActive.store(true, std::memory_order_release);
                AddVectoredExceptionHandler(1, FormReferenceFixupVEH);
                logger::info("  Installed persistent FormReferenceFixupVEH");

                // v1.22.36: Binary patch at 29846+0x95 with form resolution.
                PatchFormPointerValidation();

                // v1.22.36: Binary patches at hot VEH addresses with null checks.
                PatchHotSpotNullChecks();

                // v1.22.36: Install first-chance crash logger VEH.
                // This logs crash info to Data/SKSE/Plugins/ for guaranteed
                // capture even when spdlog hasn't flushed.
                AddVectoredExceptionHandler(1, CrashLoggerVEH);
                g_crashLoggerActive.store(true, std::memory_order_release);
                logger::info("  Installed CrashLoggerVEH (first-chance, writes to Data/SKSE/Plugins/)");

                // v1.22.36: Watchdog thread — logs VEH counters every 10 seconds
                // to diagnose freezes (infinite loop vs slow processing).
                std::thread([]() {
                    int tick = 0;
                    while (true) {
                        Sleep(10000);
                        tick++;
                        FILE* f = nullptr;
                        fopen_s(&f, "C:\\SSEEngineFixesForWine_crash.log", "a");
                        if (f) {
                            fprintf(f, "WATCHDOG #%d (t=%ds): zp=%llu ws=%llu nc=%d fi=%d ca=%llu\n",
                                tick, tick * 10,
                                g_zeroPageUseCount.load(std::memory_order_relaxed),
                                g_zeroPageWriteSkips.load(std::memory_order_relaxed),
                                g_nullSkipCount.load(std::memory_order_relaxed),
                                g_formIdSkipCount.load(std::memory_order_relaxed),
                                g_catchAllCount.load(std::memory_order_relaxed));
                            fflush(f);
                            fclose(f);
                        }
                    }
                }).detach();
                logger::info("  Started watchdog thread (10s interval)");

                // ==========================================================
                // Auto-New-Game for automated testing
                // If C:\auto_newgame.flag exists, use SendInput (the most
                // reliable Win32 input API) to send Enter to start New Game.
                // Also try multiple methods as fallbacks.
                // ==========================================================
                {
                    FILE* flagFile = nullptr;
                    fopen_s(&flagFile, "C:\\auto_newgame.flag", "r");
                    if (flagFile) {
                        fclose(flagFile);
                        logger::info("  auto_newgame.flag found — will auto-start New Game in 5s");
                        std::thread([]() {
                            Sleep(5000);
                            FILE* f = nullptr;
                            fopen_s(&f, "C:\\SSEEngineFixesForWine_crash.log", "a");

                            // Find game window
                            HWND hwnd = FindWindowA("Skyrim Special Edition", nullptr);
                            if (!hwnd) hwnd = FindWindowA(nullptr, "Skyrim Special Edition");
                            if (f) {
                                fprintf(f, "AUTO-NEWGAME: hwnd=%p\n", (void*)hwnd);
                                fflush(f);
                            }

                            if (hwnd) {
                                SetForegroundWindow(hwnd);
                                SetFocus(hwnd);
                                Sleep(500);
                            }

                            // Method 1: SendInput (most reliable for games)
                            INPUT inputs[2] = {};
                            inputs[0].type = INPUT_KEYBOARD;
                            inputs[0].ki.wVk = 0x0D;  // VK_RETURN
                            inputs[0].ki.wScan = 0x1C; // Enter scan code
                            inputs[1].type = INPUT_KEYBOARD;
                            inputs[1].ki.wVk = 0x0D;
                            inputs[1].ki.wScan = 0x1C;
                            inputs[1].ki.dwFlags = 0x0002; // KEYEVENTF_KEYUP
                            UINT sent = SendInput(2, inputs, sizeof(INPUT));
                            if (f) {
                                fprintf(f, "AUTO-NEWGAME: SendInput sent %u events\n", sent);
                                fflush(f);
                            }

                            // Method 2: Also try PostMessage with WM_CHAR (Enter = 0x0D)
                            if (hwnd) {
                                Sleep(200);
                                PostMessageA(hwnd, 0x0102, 0x0D, 0); // WM_CHAR, Enter
                            }

                            // Method 3: Also try keybd_event as last resort
                            Sleep(200);
                            keybd_event(0x0D, 0x1C, 0, 0);
                            Sleep(50);
                            keybd_event(0x0D, 0x1C, 0x0002, 0);

                            if (f) {
                                fprintf(f, "AUTO-NEWGAME: all input methods tried\n");
                                fflush(f); fclose(f);
                            }
                        }).detach();
                    }
                }

                logger::info("=== END InitItemImpl Phase (v1.22.36) ==="sv);
            }

            logger::info("=== END ForceLoadAllForms (v1.22.36) ==="sv);
#else
            logger::info("ForceLoadAllForms: SE build — skipping (AE-only fix)"sv);
#endif
        }
    }

    inline void Install()
    {
        detail::ReplaceFormMapFunctions();

        logger::info("installed form caching patch"sv);
    }
}
