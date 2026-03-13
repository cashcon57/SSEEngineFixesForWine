#pragma once

#include "form_caching_globals.h"
#include "form_caching_veh.h"

namespace Patches::FormCaching
{
    namespace detail
    {
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
            {
                VehGuard veh(1, ForceLoadVEH);

                logger::info(">>> Calling AE 13753(TDH*, false) with VEH null guard (v1.22.27) <<<");
                loadForms(tdh, false);
            }
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
                {
                    VehGuard veh(1, ForceLoadVEH);

                    logger::info("  Calling AE 13753 pass {} ...", pass);
                    loadForms(tdh, false);
                }
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
                                if (!IsReadableMemory(entry, static_cast<size_t>(
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
                            if (!IsReadableMemory(formObj, 0x20)) {
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
                // 2b. Low null guard — try to map readable memory at address 0.
                // On Wine, VirtualAlloc at very low addresses may succeed because
                // Wine's memory management is more permissive than real Windows.
                // If this works, [null+offset] accesses read zeros directly
                // without triggering any VEH faults — zero overhead.
                {
                    // Try addresses from lowest to highest until one works.
                    // NtAllocateVirtualMemory with explicit address may bypass
                    // Wine's allocation granularity enforcement.
                    using NtAVM_t = LONG(WINAPI*)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
                    auto NtAVM = reinterpret_cast<NtAVM_t>(
                        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAllocateVirtualMemory"));

                    bool mapped = false;
                    if (NtAVM) {
                        // Try to allocate 64KB starting at page 0 (address 0x0)
                        void* baseAddr = reinterpret_cast<void*>(static_cast<ULONG_PTR>(1));
                        SIZE_T regionSize = 0x10000; // 64KB — covers all known offsets (max 0x7D40)
                        LONG status = NtAVM(GetCurrentProcess(), &baseAddr, 0, &regionSize,
                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                        if (status >= 0 && baseAddr != nullptr &&
                            reinterpret_cast<ULONG_PTR>(baseAddr) < 0x1000) {
                            g_lowNullGuard = baseAddr;
                            g_lowNullGuardEnd = reinterpret_cast<DWORD64>(baseAddr) + regionSize;
                            memset(baseAddr, 0, regionSize);

                            // Write sentinel data so null-form reads get sane values:
                            auto* page = reinterpret_cast<std::uint8_t*>(baseAddr);
                            if (g_stubVtable) {
                                auto vtAddr = reinterpret_cast<DWORD64>(g_stubVtable);
                                memcpy(page + 0x00, &vtAddr, 8); // vtable at offset 0
                            }
                            // v1.22.94: Write sentinel if available (includes kDeleted bit)
                            {
                                auto* sent = g_bstSentinel.load(std::memory_order_acquire);
                                if (sent) {
                                    auto sentAddr = reinterpret_cast<DWORD64>(sent);
                                    memcpy(page + 0x10, &sentAddr, 8);
                                    memcpy(page + 0x28, &sentAddr, 8);
                                    memcpy(page + 0x30, &sentAddr, 8);
                                } else {
                                    auto flags = static_cast<std::uint32_t>(0x20); // kDeleted
                                    memcpy(page + 0x10, &flags, 4);
                                }
                            }
                            if (g_stubFuncPage) {
                                auto stubAddr = reinterpret_cast<DWORD64>(g_stubFuncPage);
                                memcpy(page + 0x4B8, &stubAddr, 8);
                            }

                            // Make it PAGE_READONLY to prevent corruption
                            DWORD oldP = 0;
                            VirtualProtect(baseAddr, regionSize, PAGE_READONLY, &oldP);

                            mapped = true;
                            logger::info("  LOW NULL GUARD: mapped {} bytes at 0x{:X} (PAGE_READONLY) — "
                                "null-form dereferences will read zeros with ZERO VEH overhead!",
                                regionSize, reinterpret_cast<std::uintptr_t>(baseAddr));
                        } else {
                            logger::info("  Low null guard: NtAllocateVirtualMemory at addr 1 returned "
                                "status=0x{:X}, baseAddr=0x{:X} — trying VirtualAlloc fallbacks",
                                (unsigned)status, reinterpret_cast<std::uintptr_t>(baseAddr));
                        }
                    }

                    if (!mapped) {
                        // Fallback: try VirtualAlloc at increasing addresses
                        for (ULONG_PTR tryAddr : {(ULONG_PTR)0x1000, (ULONG_PTR)0x10000}) {
                            auto* p = VirtualAlloc(reinterpret_cast<void*>(tryAddr), 0x10000,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                            if (p && reinterpret_cast<ULONG_PTR>(p) <= 0x10000) {
                                g_lowNullGuard = p;
                                g_lowNullGuardEnd = reinterpret_cast<DWORD64>(p) + 0x10000;
                                memset(p, 0, 0x10000);
                                auto* page = reinterpret_cast<std::uint8_t*>(p);
                                if (g_stubVtable) {
                                    auto vtAddr = reinterpret_cast<DWORD64>(g_stubVtable);
                                    memcpy(page + 0x00, &vtAddr, 8);
                                }
                                auto flags2 = static_cast<std::uint32_t>(0x20);
                                memcpy(page + 0x10, &flags2, 4);
                                if (g_stubFuncPage) {
                                    auto stubAddr = reinterpret_cast<DWORD64>(g_stubFuncPage);
                                    memcpy(page + 0x4B8, &stubAddr, 8);
                                }
                                DWORD oldP = 0;
                                VirtualProtect(p, 0x10000, PAGE_READONLY, &oldP);
                                mapped = true;
                                logger::info("  LOW NULL GUARD: VirtualAlloc at 0x{:X} succeeded! "
                                    "Null-form floods eliminated.", reinterpret_cast<std::uintptr_t>(p));
                                break;
                            }
                        }
                    }

                    if (!mapped) {
                        logger::info("  Low null guard: could not map low memory — "
                            "will rely on VEH + code cave patches for null-form handling");
                    }
                }

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

                    // v1.22.58: PAGE_READWRITE — let the engine write to the
                    // sentinel freely. The write-skip VEH flood (~33K/sec under
                    // PAGE_READONLY at +0x2D67C8) consumed 100% CPU on New Game.
                    // Vtable/formFlags corruption is now handled by watchdog repair
                    // (every 10s) + CASE 0/EXT-CALL-RET for interim vtable crashes.
                    // This eliminates ALL write-related VEH overhead.

                    g_zeroPageBase = reinterpret_cast<DWORD64>(g_zeroPage);
                    g_zeroPageEnd = g_zeroPageBase + 0x10000;
                    logger::info("  Sentinel form page at 0x{:X} (PAGE_READWRITE, kDeleted=0x20, vtable=0x{:X})",
                        g_zeroPageBase,
                        g_stubVtable ? reinterpret_cast<std::uintptr_t>(g_stubVtable) : 0);

                    // v1.22.91: If BST sentinel was already captured (from
                    // GetFormByNumericId during loading), patch g_zeroPage now.
                    if (g_bstSentinel.load(std::memory_order_acquire)) {
                        PatchNullPageWithSentinel();
                    }
                } else {
                    logger::error("  Failed to allocate sentinel form page!");
                }

                // Install persistent form-reference fixup VEH
                g_formFixupCount.store(0, std::memory_order_relaxed);
                g_formFixupActive.store(true, std::memory_order_release);
                g_vehFormFixup = AddVectoredExceptionHandler(1, FormReferenceFixupVEH);
                logger::info("  Installed persistent FormReferenceFixupVEH");

                // v1.22.36: Binary patch at 29846+0x95 with form resolution.
                PatchFormPointerValidation();

                // v1.22.36: Binary patches at hot VEH addresses with null checks.
                PatchHotSpotNullChecks();

                // v1.22.36: Install first-chance crash logger VEH.
                // This logs crash info to Data/SKSE/Plugins/ for guaranteed
                // capture even when spdlog hasn't flushed.
                g_vehCrashLogger = AddVectoredExceptionHandler(1, CrashLoggerVEH);
                g_crashLoggerActive.store(true, std::memory_order_release);
                logger::info("  Installed CrashLoggerVEH (first-chance, writes to SKSE log dir)");

                // v1.22.77: Cache our DLL module range for fault recovery.
                // If game jumps into our DLL via corrupted sentinel vtable,
                // we can recover by popping the return address.
                {
                    HMODULE hSelf = nullptr;
                    if (GetModuleHandleExA(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&CrashLoggerVEH), &hSelf)) {
                        g_dllBase = reinterpret_cast<DWORD64>(hSelf);
                        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hSelf);
                        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                            g_dllBase + dos->e_lfanew);
                        g_dllEnd = g_dllBase + nt->OptionalHeader.SizeOfImage;
                        logger::info("  DLL module range: 0x{:X} - 0x{:X}", g_dllBase, g_dllEnd);
                    }
                }

                // v1.22.72: Capture main thread handle via DuplicateHandle on
                // pseudo-handle. OpenThread works initially but Wine revokes
                // THREAD_SUSPEND_RESUME access after the first call (err=5).
                // DuplicateHandle on GetCurrentThread() inherits full access.
                g_mainThreadId = GetCurrentThreadId();
                if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                                    GetCurrentProcess(), &g_mainThreadHandle,
                                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                    FALSE, 0)) {
                    logger::info("  Captured main thread handle via DuplicateHandle (tid={})", g_mainThreadId);
                } else {
                    // Fallback to OpenThread
                    g_mainThreadHandle = OpenThread(
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                        FALSE, g_mainThreadId);
                    if (g_mainThreadHandle) {
                        logger::info("  Captured main thread handle via OpenThread (tid={})", g_mainThreadId);
                    } else {
                        logger::warn("  Failed to capture main thread handle (err={})", GetLastError());
                    }
                }

                // v1.22.95: Capture main thread stack bounds from TEB for
                // direct stack scanning (works even when GetThreadContext fails).
                {
                    DWORD64 teb = __readgsqword(0x30);
                    g_mainThreadStackBase = *reinterpret_cast<DWORD64*>(teb + 0x08);
                    g_mainThreadStackLimit = *reinterpret_cast<DWORD64*>(teb + 0x10);
                    logger::info("  Main thread stack: base=0x{:X} limit=0x{:X} ({}KB)",
                        g_mainThreadStackBase, g_mainThreadStackLimit,
                        (g_mainThreadStackBase - g_mainThreadStackLimit) / 1024);
                }

                // v1.22.36: Watchdog thread — logs VEH counters every 10 seconds.
                // v1.22.46: Also samples main thread RIP when counters are stable
                // (freeze detection — helps pinpoint infinite loops).
                // v1.22.97: g_watchdogRunning flag allows clean shutdown.
                static std::atomic<bool> g_watchdogRunning{true};
                std::thread([]() {
                    int tick = 0;
                    std::uint64_t prevZp = 0, prevWs = 0, prevCa = 0;
                    int prevFi = 0;
                    int stableTicks = 0;     // how many ticks counters unchanged
                    int zeroTicks = 0;       // how many ticks ALL counter deltas are zero
                    std::uint64_t prevCf = 0, prevEr = 0;
                    auto imgBase = REL::Module::get().base();

                    while (g_watchdogRunning.load(std::memory_order_relaxed)) {
                        Sleep(10000);
                        tick++;

                        auto curZp = g_zeroPageUseCount.load(std::memory_order_relaxed);
                        auto curWs = g_zeroPageWriteSkips.load(std::memory_order_relaxed);
                        auto curFi = g_formIdSkipCount.load(std::memory_order_relaxed);
                        auto curCa = g_catchAllCount.load(std::memory_order_relaxed);

                        // v1.22.76: Track stable ticks for freeze detection.
                        // Counters stable AND non-zero = game is frozen in a tight loop.
                        if (curZp == prevZp && curWs == prevWs &&
                            curFi == prevFi && curCa == prevCa &&
                            (curZp > 0 || curFi > 0)) {
                            stableTicks++;
                        } else {
                            stableTicks = 0;
                        }
                        // v1.22.95: Also detect zero-DELTA freezes (valid-pointer
                        // circular chains that never trigger VEH). All counter
                        // deltas must be zero after kDataLoaded = stuck in a tight
                        // loop that touches no instrumented code.
                        auto curCf = g_caveFaultCount.load(std::memory_order_relaxed);
                        auto curEr = g_execRecoverCount.load(std::memory_order_relaxed);
                        if (curZp == prevZp && curWs == prevWs &&
                            curFi == prevFi && curCa == prevCa &&
                            curCf == prevCf && curEr == prevEr &&
                            g_cacheAuthoritative.load(std::memory_order_acquire)) {
                            zeroTicks++;
                        } else {
                            zeroTicks = 0;
                        }
                        prevCf = curCf; prevEr = curEr;

                        prevZp = curZp; prevWs = curWs;
                        prevFi = curFi; prevCa = curCa;

                        // v1.22.76: Install INT3 probes after 3 stable ticks (30s freeze).
                        // v1.22.95: Also trigger on 6 zero-counter ticks (60s) after kDataLoaded.
                        // Writes 0xCC at candidate loop entry points. VEH catches the
                        // breakpoint and logs which loop the main thread is stuck in.
                        if ((stableTicks >= 3 || zeroTicks >= 6) &&
                            !g_probesInstalled.load(std::memory_order_acquire)) {
                            int installed = 0;
                            for (int i = 0; i < g_numProbes; ++i) {
                                auto* site = reinterpret_cast<std::uint8_t*>(imgBase + g_probes[i].offset);
                                DWORD oldProt;
                                if (VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
                                    g_probes[i].origByte = *site;
                                    g_probes[i].active.store(true, std::memory_order_release);
                                    *site = 0xCC;  // INT3 — VEH already recognizes this probe
                                    FlushInstructionCache(GetCurrentProcess(), site, 1);
                                    installed++;
                                    // Do NOT restore protection (Wine page reversion)
                                }
                            }
                            g_probesInstalled.store(true, std::memory_order_release);
                            FILE* pf = nullptr;
                            fopen_s(&pf, g_crashLogPath, "a");
                            if (pf) {
                                fprintf(pf, "PROBES: installed %d/%d INT3 probes at tick %d (stable=%d)\n",
                                    installed, g_numProbes, tick, stableTicks);
                                fflush(pf);
                                fclose(pf);
                            }
                        }

                        // v1.22.64: Aggressively zero sentinel page to break garbage
                        // pointer chains. Engine writes garbage to sentinel (it's RW),
                        // and code that reads a "next pointer" from sentinel gets a
                        // random value like 0x7D00000000 → second-hop fault → CATCH-ALL.
                        // By zeroing the entire page and re-writing only critical fields,
                        // any "next pointer" read returns 0, which existing code cave
                        // null checks catch without VEH involvement.
                        // v1.22.97: Use shared RepairSentinel() instead of inline
                        // duplicate — prevents data race with VEH handler.
                        RepairSentinel();

                        // v1.22.62: Verify code cave patches still have JMP opcode.
                        // Wine may revert file-backed code pages; re-apply if needed.
                        VerifyAndRepairCavePatches();

                        FILE* f = nullptr;
                        fopen_s(&f, g_crashLogPath, "a");
                        if (f) {
                            fprintf(f, "WATCHDOG #%d (t=%ds): zp=%llu ws=%llu nc=%d fi=%d ca=%llu cf=%llu er=%llu zt=%d",
                                tick, tick * 10,
                                curZp, curWs,
                                g_nullSkipCount.load(std::memory_order_relaxed),
                                curFi, curCa,
                                (unsigned long long)curCf,
                                (unsigned long long)curEr,
                                zeroTicks);

                            // v1.22.72: Sample main thread RIP on every tick.
                            // Try SuspendThread first; if Wine blocks it (err=5),
                            // fall back to GetThreadContext without suspend (racy
                            // but still useful for detecting tight loops/deadlocks).
                            if (g_mainThreadHandle) {
                                bool suspended = false;
                                DWORD suspCount = SuspendThread(g_mainThreadHandle);
                                if (suspCount != (DWORD)-1) {
                                    suspended = true;
                                }

                                CONTEXT ctx = {};
                                ctx.ContextFlags = CONTEXT_CONTROL;
                                if (GetThreadContext(g_mainThreadHandle, &ctx)) {
                                    DWORD64 rip = ctx.Rip;
                                    DWORD64 rsp = ctx.Rsp;
                                    g_mainThreadLastRsp = rsp; // save for stack scan fallback
                                    if (rip >= (DWORD64)imgBase && rip < (DWORD64)imgBase + 0x4000000) {
                                        fprintf(f, " RIP=+0x%llX RSP=0x%llX",
                                            (unsigned long long)(rip - imgBase),
                                            (unsigned long long)rsp);
                                    } else {
                                        fprintf(f, " RIP=0x%llX RSP=0x%llX",
                                            (unsigned long long)rip,
                                            (unsigned long long)rsp);
                                    }
                                    if (!suspended) fprintf(f, "(unsuspended)");
                                    auto* rspPtr = reinterpret_cast<DWORD64*>(rsp);
                                    if (IsReadableMemory(rspPtr, 64)) {
                                        fprintf(f, " STK=[");
                                        for (int i = 0; i < 8; ++i) {
                                            DWORD64 val = rspPtr[i];
                                            if (val >= (DWORD64)imgBase &&
                                                val < (DWORD64)imgBase + 0x4000000) {
                                                fprintf(f, "+0x%llX ",
                                                    (unsigned long long)(val - imgBase));
                                            }
                                        }
                                        fprintf(f, "]");
                                    }
                                } else {
                                    fprintf(f, " RIP=FAIL(susp=%d,err=%lu)",
                                        suspended ? 1 : 0, GetLastError());
                                }

                                if (suspended) ResumeThread(g_mainThreadHandle);
                            } else {
                                fprintf(f, " RIP=NO_HANDLE");
                            }

                            // v1.22.96: Full-stack scan for .text return addresses.
                            // Previous version narrowed to RSP ± 0x2000, but Wine blocks
                            // GetThreadContext after 2-3 calls, making RSP stale.
                            // Now scans full stack and filters to .text section only
                            // (0x1000-0x174F000) to avoid data/resource false positives.
                            if (g_mainThreadStackBase > 0 && g_mainThreadStackLimit > 0) {
                                DWORD64 scanLo = g_mainThreadStackLimit;
                                DWORD64 scanHi = g_mainThreadStackBase;
                                // .text section bounds (from PE headers)
                                DWORD64 textLo = (DWORD64)imgBase + 0x1000;
                                DWORD64 textHi = (DWORD64)imgBase + 0x174F000;
                                fprintf(f, " STKSCAN=[");
                                int found = 0;
                                for (DWORD64 addr = scanLo; addr + 8 <= scanHi && found < 32; addr += 8) {
                                    DWORD64 val = *reinterpret_cast<DWORD64*>(addr);
                                    if (val >= textLo && val < textHi) {
                                        fprintf(f, "+0x%llX ",
                                            (unsigned long long)(val - (DWORD64)imgBase));
                                        found++;
                                    }
                                }
                                fprintf(f, "]");
                            }

                            fprintf(f, "\n");
                            fflush(f);
                            fclose(f);
                        }
                    }
                }).detach();
                logger::info("  Started watchdog thread (10s interval, RIP sampling enabled)");

                // v1.22.76: Auto-New-Game REMOVED — the resetGame/fullReset path
                // triggers ClearData which is a DIFFERENT code path from manual
                // UI New Game and causes different freezes. Testing must be manual.

                logger::info("=== END InitItemImpl Phase (v1.22.36) ==="sv);
            }

            // v1.22.52/58: Ensure sentinel page integrity.
            // Page is PAGE_READWRITE since v1.22.58 — just refresh critical fields.
            if (g_zeroPage) {
                // Page is already PAGE_READWRITE since v1.22.58

                auto* zpBytes = reinterpret_cast<std::uint8_t*>(g_zeroPage);
                auto stubVtable = reinterpret_cast<DWORD64>(g_stubVtable);
                std::memcpy(&zpBytes[0x0000], &stubVtable, 8); // vtable
                // v1.22.94: Write BST sentinel at +0x10 if available (also has kDeleted
                // bit set in low 32 bits: 0x41FD5F3C & 0x20 == 0x20). Previous versions
                // wrote a 4-byte kDeleted flag here, clobbering the sentinel pointer
                // that PatchNullPageWithSentinel() had written — causing chain walks
                // through g_zeroPage to never match sentinel and loop forever.
                {
                    auto* sent = g_bstSentinel.load(std::memory_order_acquire);
                    if (sent) {
                        auto sentAddr = reinterpret_cast<DWORD64>(sent);
                        std::memcpy(&zpBytes[0x0010], &sentAddr, 8);
                        std::memcpy(&zpBytes[0x0028], &sentAddr, 8);
                        std::memcpy(&zpBytes[0x0030], &sentAddr, 8);
                    } else {
                        auto flags = static_cast<std::uint32_t>(0x20);
                        std::memcpy(&zpBytes[0x0010], &flags, 4); // kDeleted fallback
                    }
                }
                if (g_stubFuncPage) {
                    auto stubAddr = reinterpret_cast<DWORD64>(g_stubFuncPage);
                    std::memcpy(&zpBytes[0x04B8], &stubAddr, 8);
                }

                // v1.22.58: Keep PAGE_READWRITE — no restore to PAGE_READONLY.
                // Watchdog repairs critical fields every 10s tick.
                g_zpWritable.store(false, std::memory_order_relaxed);
                logger::info("  Sentinel page verified (vtable+flags+stubs refreshed, PAGE_READWRITE)");
            }

            logger::info("=== END ForceLoadAllForms (v1.22.36) ==="sv);

            // v1.22.61: Dump unpacked .text section for offline RE.
            // SkyrimSE.exe uses SteamStub DRM — the .text section is
            // encrypted on disk but unpacked in memory at runtime.
            // Write the live .text bytes to a file for capstone analysis.
            {
                auto imgBase = REL::Module::get().base();
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imgBase);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(imgBase + dos->e_lfanew);
                const auto* sec = IMAGE_FIRST_SECTION(nt);

                for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
                    if (std::memcmp(sec[i].Name, ".text", 5) == 0 &&
                        sec[i].VirtualAddress == 0x1000) {
                        auto textVA = imgBase + sec[i].VirtualAddress;
                        auto textSize = sec[i].Misc.VirtualSize;

                        // Write to same directory as crash log
                        std::string dumpPath(g_crashLogPath);
                        auto lastSlash = dumpPath.find_last_of("\\/");
                        if (lastSlash != std::string::npos)
                            dumpPath = dumpPath.substr(0, lastSlash + 1);
                        dumpPath += "SkyrimSE_text_dump.bin";

                        FILE* df = nullptr;
                        fopen_s(&df, dumpPath.c_str(), "wb");
                        if (df) {
                            fwrite(reinterpret_cast<const void*>(textVA), 1, textSize, df);
                            fclose(df);
                            logger::info("  .text dump: {} bytes → {}", textSize, dumpPath);
                        } else {
                            logger::warn("  .text dump: failed to open {}", dumpPath);
                        }
                        break;
                    }
                }
            }
#else
            logger::info("ForceLoadAllForms: SE build — skipping (AE-only fix)"sv);
#endif
        }
    } // namespace detail
} // namespace Patches::FormCaching
