#pragma once

#include "form_caching_globals.h"

namespace Patches::FormCaching
{
    namespace detail
    {
        // Forward declaration — defined in form_caching_hooks.h.
        // VEH handlers call this to resolve form IDs found in registers.
        inline RE::TESForm* TESForm_GetFormByNumericId(RE::FormID a_formId);

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
            if (!g_forceLoadInProgress.load(std::memory_order_acquire))
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
            if (!g_formFixupActive.load(std::memory_order_acquire))
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
            if (!g_crashLoggerActive.load(std::memory_order_acquire))
                return EXCEPTION_CONTINUE_SEARCH;

            auto code = pep->ExceptionRecord->ExceptionCode;

            // v1.22.76: INT3 probe handler — catch breakpoints from freeze detection.
            if (code == EXCEPTION_BREAKPOINT) {
                auto rip = pep->ContextRecord->Rip;
                static auto sImgBase2 = REL::Module::get().base();
                for (int i = 0; i < g_numProbes; ++i) {
                    if (g_probes[i].active.load(std::memory_order_acquire) &&
                        rip == sImgBase2 + g_probes[i].offset) {
                        // Log the probe hit with full register context
                        VehLog("PROBE-HIT: +0x%llX",
                            (unsigned long long)g_probes[i].offset);
                        VehLog(" RAX=0x%llX RCX=0x%llX RDX=0x%llX RDI=0x%llX RSI=0x%llX R14=0x%llX RBP=0x%llX R8=0x%llX R9=0x%llX R15=0x%llX",
                            (unsigned long long)pep->ContextRecord->Rax,
                            (unsigned long long)pep->ContextRecord->Rcx,
                            (unsigned long long)pep->ContextRecord->Rdx,
                            (unsigned long long)pep->ContextRecord->Rdi,
                            (unsigned long long)pep->ContextRecord->Rsi,
                            (unsigned long long)pep->ContextRecord->R14,
                            (unsigned long long)pep->ContextRecord->Rbp,
                            (unsigned long long)pep->ContextRecord->R8,
                            (unsigned long long)pep->ContextRecord->R9,
                            (unsigned long long)pep->ContextRecord->R15);
                        // Log return address from stack
                        auto* rspPtr = reinterpret_cast<DWORD64*>(pep->ContextRecord->Rsp);
                        if (IsReadableMemory(rspPtr, 64)) {
                            VehLog(" STK=[");
                            for (int s = 0; s < 8; ++s) {
                                DWORD64 val = rspPtr[s];
                                if (val >= (DWORD64)sImgBase2 &&
                                    val < (DWORD64)sImgBase2 + 0x4000000) {
                                    VehLog("+0x%llX ", (unsigned long long)(val - sImgBase2));
                                }
                            }
                            VehLog("]");
                        }
                        VehLog("\n");
                        // Restore original byte and mark as fired
                        auto* site = reinterpret_cast<std::uint8_t*>(sImgBase2 + g_probes[i].offset);
                        *site = g_probes[i].origByte;
                        FlushInstructionCache(GetCurrentProcess(), site, 1);
                        g_probes[i].active.store(false, std::memory_order_release);
                        g_probes[i].fired.store(true, std::memory_order_release);
                        // Re-execute the original instruction (RIP stays at breakpoint addr)
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
                // Not our probe — pass to other handlers
                return EXCEPTION_CONTINUE_SEARCH;
            }

            // Log non-AV exceptions (stack overflow, illegal instruction, etc.)
            if (code != EXCEPTION_ACCESS_VIOLATION) {
                static std::atomic<int> s_nonAvCount{ 0 };
                auto cnt = s_nonAvCount.fetch_add(1, std::memory_order_relaxed);
                if (cnt < 5) {
                    auto imgBase = REL::Module::get().base();
                    auto rip2 = pep->ContextRecord->Rip;
                    VehLog("NON-AV EXCEPTION #%d: code=0x%08lX RIP=0x%llX (+0x%llX)\n",
                        cnt + 1, code, rip2, rip2 - imgBase);
                }
                return EXCEPTION_CONTINUE_SEARCH;
            }

            ULONG_PTR targetAddr = 0;
            ULONG_PTR accessType = 0; // 0=read, 1=write, 8=DEP
            if (pep->ExceptionRecord->NumberParameters >= 2) {
                accessType = pep->ExceptionRecord->ExceptionInformation[0];
                targetAddr = pep->ExceptionRecord->ExceptionInformation[1];
            }

            // v1.22.79: Install INT3 probes on the first post-loading AV.
            // kNewGame fires AFTER the engine freeze, so we can't wait for it.
            // Instead, install probes at the very first access violation that
            // happens in the game image after the counters were all zero (i.e.,
            // after the main menu was stable). This catches the freeze on its
            // first iteration through any probed loop.
            if (!g_probesInstalled.load(std::memory_order_acquire) &&
                g_formFixupActive.load(std::memory_order_acquire)) {
                auto imgBase2 = REL::Module::get().base();
                int installed = 0;
                for (int i = 0; i < g_numProbes; ++i) {
                    auto* site = reinterpret_cast<std::uint8_t*>(imgBase2 + g_probes[i].offset);
                    DWORD oldProt;
                    if (VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
                        g_probes[i].origByte = *site;
                        *site = 0xCC;
                        FlushInstructionCache(GetCurrentProcess(), site, 1);
                        g_probes[i].active.store(true, std::memory_order_release);
                        installed++;
                    }
                }
                g_probesInstalled.store(true, std::memory_order_release);

                // v1.22.86: Cache invalidation on New Game REMOVED.
                // ESM forms persist — clearing the cache caused 900+ native
                // map lookups → freeze on corrupted bucket chains.

                VehLog("PROBES: installed %d/%d INT3 probes on first VEH event\n"
                       "CACHE-CLEAR: flagged for lazy invalidation\n",
                    installed, g_numProbes);
            }

            // ─────────────────────────────────────────────────────────────
            // CASE W: Write to zero page — skip the instruction.
            // v1.22.43: Sentinel page is PAGE_READONLY to prevent vtable
            // corruption. All write attempts are caught here and silently
            // skipped, preserving the sentinel's vtable/flags/stubs.
            // ─────────────────────────────────────────────────────────────
            if (accessType == 1 && g_zeroPage &&
                targetAddr >= g_zeroPageBase &&
                targetAddr < g_zeroPageEnd) {
                auto rip2 = pep->ContextRecord->Rip;
                const auto* ripBytes2 = reinterpret_cast<const std::uint8_t*>(rip2);
                if (IsReadableMemory(ripBytes2, 16)) {
                    auto instrLen = x86_instr_len(ripBytes2);
                    pep->ContextRecord->Rip += instrLen;

                    auto count = g_zeroPageWriteSkips.fetch_add(1, std::memory_order_relaxed);
                    if (count < 200) {
                        VehLog("ZERO-WRITE-SKIP #%d: RIP=+0x%llX target=0x%llX +%dB\n",
                            count + 1, rip2 - REL::Module::get().base(),
                            (unsigned long long)targetAddr, instrLen);
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
                if (IsReadableMemory(rspPtr, 8)) {
                    pep->ContextRecord->Rip = *rspPtr;
                    pep->ContextRecord->Rsp += 8;
                    pep->ContextRecord->Rax = 0;

                    auto count = g_nullSkipCount.fetch_add(1, std::memory_order_relaxed);
                    if (count < 50) {
                        VehLog("NULL-CALL-RET #%d: RIP=0x%llX returning to 0x%llX\n",
                            count + 1, rip, *rspPtr);
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            // Faults outside SkyrimSE.exe image
            if (rip < sImgBase || rip >= sImgEnd) {
                // v1.22.43: If RIP is an invalid address (high kernel-space
                // or very low), the engine jumped here via a corrupted vtable
                // in the zero page. The CALL already pushed a return address.
                // Pop it and return 0, same as CASE 0 for null calls.
                bool isInvalidCodeAddr = (rip > 0x00007FFFFFFFFFFFULL) || (rip < 0x10000);
                if (isInvalidCodeAddr) {
                    auto* rspPtr = reinterpret_cast<DWORD64*>(pep->ContextRecord->Rsp);
                    if (IsReadableMemory(rspPtr, 8)) {
                        DWORD64 retAddr = *rspPtr;
                        pep->ContextRecord->Rip = retAddr;
                        pep->ContextRecord->Rsp += 8;
                        pep->ContextRecord->Rax = 0;

                        // v1.22.43: No vtable repair needed — page is PAGE_READONLY

                        static std::atomic<int> s_extFixCount{ 0 };
                        auto cnt2 = s_extFixCount.fetch_add(1, std::memory_order_relaxed);
                        if (cnt2 < 50) {
                            VehLog("EXT-CALL-RET #%d: RIP=0x%llX → returning to 0x%llX RAX=0\n",
                                cnt2 + 1, rip, retAddr);
                        }
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }

                // v1.22.54: Check if fault is inside one of our code caves.
                // This happens when garbage form pointers (e.g., 0x3C003C00,
                // 0x9E50...) pass the inline null check but aren't valid memory.
                // Redirect to the cave's null/skip path.
                {
                    int numCaves = g_numCavePatches.load(std::memory_order_acquire);
                    for (int i = 0; i < numCaves; ++i) {
                        if (rip >= g_cavePatches[i].caveStart && rip < g_cavePatches[i].caveEnd) {
                            pep->ContextRecord->Rip = g_cavePatches[i].nullReturnAddr;
                            pep->ContextRecord->Rax = 0; // safe: null paths return 0 or ignore RAX

                            // v1.22.69→v1.22.77: Repair sentinel on every cave-fault.
                            RepairSentinel();

                            auto count = g_caveFaultCount.fetch_add(1, std::memory_order_relaxed);
                            if (count < 200) {
                                VehLog("CAVE-FAULT #%llu: RIP=0x%llX (%s) target=0x%llX → redirect 0x%llX\n",
                                    (unsigned long long)(count + 1), (unsigned long long)rip,
                                    g_cavePatches[i].name,
                                    (unsigned long long)targetAddr,
                                    (unsigned long long)g_cavePatches[i].nullReturnAddr);
                            }
                            return EXCEPTION_CONTINUE_EXECUTION;
                        }
                    }
                }

                // v1.22.55: Execute-fault recovery — corrupted function pointer
                // or vtable entry jumped to non-executable memory (heap, freed
                // page, etc.). When RIP == targetAddr, the CPU faulted trying
                // to fetch instruction bytes at that address. If the stack top
                // holds a valid return address in the game image, this was a
                // CALL through a bad pointer — pop the return addr and continue.
                if (rip == targetAddr && accessType != 1) {
                    auto* rspPtr2 = reinterpret_cast<DWORD64*>(pep->ContextRecord->Rsp);
                    if (IsReadableMemory(rspPtr2, 8)) {
                        DWORD64 retAddr = *rspPtr2;

                        // v1.22.64: Also accept return addresses inside our code caves.
                        // PatchA's cave calls [rax+0x4B8] — if the function pointer is
                        // garbage, the CPU faults here with retAddr pointing back into
                        // the cave, not the game image. Redirect to the cave's null path.
                        bool inGameImage = (retAddr >= sImgBase && retAddr < sImgEnd);
                        bool inCave = false;
                        int caveIdx = -1;
                        if (!inGameImage) {
                            int numCaves = g_numCavePatches.load(std::memory_order_acquire);
                            for (int i = 0; i < numCaves; ++i) {
                                if (retAddr >= g_cavePatches[i].caveStart && retAddr < g_cavePatches[i].caveEnd) {
                                    inCave = true;
                                    caveIdx = i;
                                    break;
                                }
                            }
                        }

                        if (inGameImage || inCave) {
                            if (inCave) {
                                // Return to cave's null/skip path instead of resuming mid-cave
                                pep->ContextRecord->Rip = g_cavePatches[caveIdx].nullReturnAddr;
                            } else {
                                pep->ContextRecord->Rip = retAddr;
                            }
                            pep->ContextRecord->Rsp += 8;
                            pep->ContextRecord->Rax = 0;

                            auto cnt3 = static_cast<int>(g_execRecoverCount.fetch_add(1, std::memory_order_relaxed));
                            if (cnt3 < 50) {
                                VehLog("EXT-EXEC-RECOVER #%d: RIP=0x%llX (bad call target) → returning to 0x%llX (%s%s) RAX=0\n",
                                    cnt3 + 1, rip,
                                    (unsigned long long)pep->ContextRecord->Rip,
                                    inCave ? "cave:" : "+0x",
                                    inCave ? g_cavePatches[caveIdx].name : "");
                                if (!inCave) {
                                    VehLog("  (offset +0x%llX)\n", retAddr - sImgBase);
                                }
                            }
                            return EXCEPTION_CONTINUE_EXECUTION;
                        }
                    }
                }

                // v1.22.77: Faults in our own DLL — game jumped here via
                // corrupted sentinel vtable/function pointer, or our hook
                // received garbage data. Recover by popping return address
                // (like a failed CALL) and returning 0 to caller.
                if (g_dllBase && rip >= g_dllBase && rip < g_dllEnd) {
                    auto* rspPtr = reinterpret_cast<DWORD64*>(pep->ContextRecord->Rsp);
                    if (IsReadableMemory(rspPtr, 8)) {
                        DWORD64 retAddr = *rspPtr;
                        pep->ContextRecord->Rip = retAddr;
                        pep->ContextRecord->Rsp += 8;
                        pep->ContextRecord->Rax = 0;
                        RepairSentinel();

                        static std::atomic<int> s_dllFaultCount{ 0 };
                        auto cnt = s_dllFaultCount.fetch_add(1, std::memory_order_relaxed);
                        if (cnt < 50) {
                            VehLog("DLL-FAULT-RECOVER #%d: RIP=0x%llX target=0x%llX RAX=0x%llX → returning to 0x%llX\n",
                                cnt + 1, rip,
                                (unsigned long long)targetAddr,
                                (unsigned long long)pep->ContextRecord->Rax,
                                (unsigned long long)retAddr);
                        }
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }

                // Faults in other DLLs (Wine, etc.) — can't handle
                static std::atomic<int> s_extCount{ 0 };
                auto cnt = s_extCount.fetch_add(1, std::memory_order_relaxed);
                if (cnt < 10) {
                    // Identify which module contains the faulting RIP
                    char modName[MAX_PATH] = "<unknown>";
                    HMODULE hMod = nullptr;
                    if (GetModuleHandleExA(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(rip), &hMod)) {
                        GetModuleFileNameA(hMod, modName, MAX_PATH);
                    }

                    VehLog("EXT-CRASH #%d: RIP=0x%llX (module: %s) target=0x%llX RAX=0x%llX\n",
                        cnt + 1, rip,
                        modName,
                        (unsigned long long)targetAddr,
                        (unsigned long long)pep->ContextRecord->Rax);
                }
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const auto* ripBytes = reinterpret_cast<const std::uint8_t*>(rip);

            // ─────────────────────────────────────────────────────────────
            // CASE 0.5 (v1.22.90): BST chain-follow sentinel recovery.
            //
            // When grow() steals entries (next = nullptr) and a concurrent
            // find() follows the null chain pointer, the MOV instruction
            // faults. Instead of redirecting to the zero page (which causes
            // an infinite loop — the find loop reads zero page data as entry
            // pointers and cycles forever), we detect the specific chain-
            // follow instruction pattern and set the destination register
            // to the BST sentinel value. This makes the loop's sentinel
            // comparison match → loop exits cleanly → find returns "not found".
            //
            // Pattern: [REX] 8B ModRM disp8  where disp8 = 0x10 or 0x28
            //   e.g. 48 8B 52 10 = mov rdx, [rdx + 0x10]
            // ─────────────────────────────────────────────────────────────
            if (targetAddr < 0x10000 && g_bstSentinel.load(std::memory_order_acquire) &&
                rip >= sImgBase && rip < sImgEnd &&
                IsReadableMemory(ripBytes, 8)) {

                const auto* ip = ripBytes;
                int rex = 0;
                if ((*ip & 0xF0) == 0x40) { rex = *ip; ip++; }

                if (*ip == 0x8B) {  // MOV r, r/m
                    ip++;
                    int mod = (*ip >> 6) & 3;
                    int reg = ((*ip >> 3) & 7) | ((rex & 4) ? 8 : 0);  // dest (REX.R)
                    int rm  = (*ip & 7) | ((rex & 1) ? 8 : 0);          // src  (REX.B)
                    ip++;

                    bool hasSIB = (mod != 3 && (rm & 7) == 4);
                    if (!hasSIB && mod == 1) {  // [reg + disp8]
                        int disp = static_cast<std::int8_t>(*ip);

                        if (disp == 0x10 || disp == 0x28) {
                            // This is a BST chain-follow: mov reg, [reg + next_offset]
                            // Set destination register to sentinel so loop exits.
                            DWORD64 sentVal = reinterpret_cast<DWORD64>(g_bstSentinel.load(std::memory_order_relaxed));

                            // Map register index to CONTEXT field
                            DWORD64* regMap[] = {
                                &pep->ContextRecord->Rax, &pep->ContextRecord->Rcx,
                                &pep->ContextRecord->Rdx, &pep->ContextRecord->Rbx,
                                &pep->ContextRecord->Rsp, &pep->ContextRecord->Rbp,
                                &pep->ContextRecord->Rsi, &pep->ContextRecord->Rdi,
                                &pep->ContextRecord->R8,  &pep->ContextRecord->R9,
                                &pep->ContextRecord->R10, &pep->ContextRecord->R11,
                                &pep->ContextRecord->R12, &pep->ContextRecord->R13,
                                &pep->ContextRecord->R14, &pep->ContextRecord->R15,
                            };

                            if (reg < 16 && reg != 4) {  // don't touch RSP
                                *regMap[reg] = sentVal;
                                // Advance RIP past the instruction
                                auto instrLen = x86_instr_len(ripBytes);
                                pep->ContextRecord->Rip += instrLen;

                                static std::atomic<int> s_chainFixCount{ 0 };
                                auto cnt = s_chainFixCount.fetch_add(1, std::memory_order_relaxed);
                                if (cnt < 100) {
                                    VehLog("CHAIN-SENTINEL #%d: RIP=+0x%llX disp=%d dest=R%d → sentinel=0x%llX\n",
                                        cnt + 1, (unsigned long long)(rip - sImgBase),
                                        disp, reg,
                                        (unsigned long long)sentVal);
                                }
                                return EXCEPTION_CONTINUE_EXECUTION;
                            }
                        }
                    }
                }
            }

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

                // v1.22.77: Repair sentinel on every redirect. Sentinel is
                // PAGE_READWRITE so engine freely corrupts it with ASCII data;
                // zeroing here ensures the retried instruction sees clean fields.
                RepairSentinel();

                auto count = g_zeroPageUseCount.fetch_add(1, std::memory_order_relaxed);

                if (count < 600) {
                    VehLog("ZERO-PAGE #%d: RIP=+0x%llX target=0x%llX %s → zeroPage=0x%llX\n",
                        count + 1, rip - sImgBase,
                        (unsigned long long)targetAddr,
                        isNullAccess ? "null" : "high-invalid",
                        (unsigned long long)zp);
                    // Byte dump: first time we see each RIP, dump 32 bytes
                    {
                        int cnt = g_zeroDumpCount.load(std::memory_order_relaxed);
                        bool alreadyDumped = false;
                        for (int d = 0; d < cnt; ++d)
                            if (g_zeroDumpedRips[d] == rip) { alreadyDumped = true; break; }
                        if (!alreadyDumped && cnt < 32) {
                            int slot = g_zeroDumpCount.fetch_add(1, std::memory_order_relaxed);
                            if (slot < 32) {
                                g_zeroDumpedRips[slot] = rip;
                                VehLog("  BYTES[+0x%llX]:", rip - sImgBase);
                                if (IsReadableMemory(ripBytes, 32))
                                    for (int b = 0; b < 32; ++b) VehLog(" %02X", ripBytes[b]);
                                VehLog("\n");
                            }
                        }
                    }

                    // v1.22.85: Enhanced diagnostics for high-invalid
                    // targets — dump full register state + memory around
                    // corrupted pointer to identify corruption source.
                    if (isHighInvalid && count < 50) {
                        VehLog("  REGS: RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n",
                            (unsigned long long)pep->ContextRecord->Rax,
                            (unsigned long long)pep->ContextRecord->Rbx,
                            (unsigned long long)pep->ContextRecord->Rcx,
                            (unsigned long long)pep->ContextRecord->Rdx);
                        VehLog("        RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\n",
                            (unsigned long long)pep->ContextRecord->Rsi,
                            (unsigned long long)pep->ContextRecord->Rdi,
                            (unsigned long long)pep->ContextRecord->Rbp,
                            (unsigned long long)pep->ContextRecord->Rsp);
                        VehLog("        R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX\n",
                            (unsigned long long)pep->ContextRecord->R8,
                            (unsigned long long)pep->ContextRecord->R9,
                            (unsigned long long)pep->ContextRecord->R10,
                            (unsigned long long)pep->ContextRecord->R11);
                        VehLog("        R12=%016llX R13=%016llX R14=%016llX R15=%016llX\n",
                            (unsigned long long)pep->ContextRecord->R12,
                            (unsigned long long)pep->ContextRecord->R13,
                            (unsigned long long)pep->ContextRecord->R14,
                            (unsigned long long)pep->ContextRecord->R15);

                        // Dump stack (return addresses)
                        auto* stk = reinterpret_cast<DWORD64*>(pep->ContextRecord->Rsp);
                        if (IsReadableMemory(stk, 64)) {
                            VehLog("  STACK:");
                            for (int s = 0; s < 8; ++s)
                                VehLog(" %016llX", (unsigned long long)stk[s]);
                            VehLog("\n");
                        }

                        // For each register that looks like a valid
                        // pointer, dump 64 bytes at that address so we
                        // can see the data structure it points to.
                        const char* regNames[] = {
                            "RAX","RBX","RCX","RDX","RSI","RDI",
                            "R8","R9","R10","R11","R12","R13","R14","R15"
                        };
                        DWORD64 regVals[] = {
                            pep->ContextRecord->Rax, pep->ContextRecord->Rbx,
                            pep->ContextRecord->Rcx, pep->ContextRecord->Rdx,
                            pep->ContextRecord->Rsi, pep->ContextRecord->Rdi,
                            pep->ContextRecord->R8,  pep->ContextRecord->R9,
                            pep->ContextRecord->R10, pep->ContextRecord->R11,
                            pep->ContextRecord->R12, pep->ContextRecord->R13,
                            pep->ContextRecord->R14, pep->ContextRecord->R15
                        };
                        for (int r = 0; r < 14; ++r) {
                            DWORD64 rv = regVals[r];
                            // Valid user-mode heap pointer?
                            if (rv > 0x10000 && rv <= 0x00007FFFFFFFFFFFULL) {
                                auto* mem = reinterpret_cast<std::uint8_t*>(rv);
                                if (IsReadableMemory(mem, 64)) {
                                    VehLog("  MEM[%s=%016llX]:", regNames[r],
                                        (unsigned long long)rv);
                                    for (int b = 0; b < 64; ++b) {
                                        if (b % 16 == 0 && b > 0)
                                            VehLog("\n                              ");
                                        VehLog(" %02X", mem[b]);
                                    }
                                    // Also print as ASCII for string detection
                                    VehLog("\n  ASCII: \"");
                                    for (int b = 0; b < 64; ++b) {
                                        char c = (char)mem[b];
                                        VehLog("%c", (c >= 32 && c < 127) ? c : '.');
                                    }
                                    VehLog("\"\n");
                                }
                            }
                        }
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
                            VehLog("FORM-RESOLVE #%d: RIP=+0x%llX formID=0x%08X → 0x%llX\n",
                                count + 1, rip - sImgBase, formId,
                                reinterpret_cast<unsigned long long>(resolved));
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

                        // v1.22.77: Repair sentinel before retry
                        RepairSentinel();

                        auto count = g_formIdSkipCount.fetch_add(1, std::memory_order_relaxed);
                        if (count < 50) {
                            VehLog("FORM-ZEROPAGE #%d: RIP=+0x%llX formID=0x%08X NOT FOUND → zeroPage=0x%llX\n",
                                count + 1, rip - sImgBase, formId, (unsigned long long)zp);
                        }
                        return EXCEPTION_CONTINUE_EXECUTION; // Retry with zero page
                    }

                    // No zero page available — fall through to crash log
                    break;
                }
            }

            // ─────────────────────────────────────────────────────────────
            // CASE 3: Catch-all — redirect register to zero page + RETRY.
            // v1.22.44: Instead of skipping the instruction (which breaks
            // control flow and causes freezes), find the register that
            // holds the unmapped address, redirect it to the zero page,
            // and retry. The instruction reads zeros from the sentinel
            // and continues naturally.
            // ─────────────────────────────────────────────────────────────
            if (g_zeroPage) {
                DWORD64 zp = reinterpret_cast<DWORD64>(g_zeroPage);
                DWORD64* gprs[] = {
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

                // v1.22.47: Two-phase redirect strategy.
                // Phase 1: Find register within 1MB of target (single-register addressing).
                // Phase 2: Find largest pointer-like register (multi-register addressing
                //          like [r14 + rax*8] where base ptr is far from target).
                // Never fall back to skip+zero — always redirect + retry.
                bool patched = false;

                // Phase 1: Register close to target (handles [reg+offset] patterns)
                for (int i = 0; i < 15; ++i) {
                    DWORD64 val = *gprs[i];
                    if (val == 0 || val == zp) continue;
                    auto diff = static_cast<std::int64_t>(targetAddr) -
                                static_cast<std::int64_t>(val);
                    if (diff >= 0 && diff < 0x100000) { // Within 1MB
                        *gprs[i] = zp;
                        patched = true;
                        break;
                    }
                }

                // Phase 2: Redirect largest pointer-like register
                // (handles [base + index*scale] where base is unmapped)
                if (!patched) {
                    int bestIdx = -1;
                    DWORD64 bestVal = 0;
                    for (int i = 0; i < 15; ++i) {
                        DWORD64 val = *gprs[i];
                        // Must look like a pointer: > 64KB, not zero page,
                        // not in SkyrimSE.exe image, and <= target address
                        if (val > 0x10000 && val != zp &&
                            val <= targetAddr && val > bestVal) {
                            bestVal = val;
                            bestIdx = i;
                        }
                    }
                    if (bestIdx >= 0) {
                        *gprs[bestIdx] = zp;
                        patched = true;
                    }
                }

                // v1.22.77: Repair sentinel before retry
                RepairSentinel();

                auto count = g_catchAllCount.fetch_add(1, std::memory_order_relaxed);
                if (count < 200) {
                    VehLog("CATCH-ALL #%d: RIP=+0x%llX target=0x%llX patched=%s RAX=0x%llX R14=0x%llX\n",
                        count + 1, rip - sImgBase,
                        (unsigned long long)targetAddr,
                        patched ? "redirect" : "FAILED",
                        (unsigned long long)pep->ContextRecord->Rax,
                        (unsigned long long)pep->ContextRecord->R14);
                    // Byte dump: first time per unique RIP
                    {
                        int cnt = g_catchAllDumpCount.load(std::memory_order_relaxed);
                        bool alreadyDumped = false;
                        for (int d = 0; d < cnt; ++d)
                            if (g_catchAllDumpedRips[d] == rip) { alreadyDumped = true; break; }
                        if (!alreadyDumped && cnt < 32) {
                            int slot = g_catchAllDumpCount.fetch_add(1, std::memory_order_relaxed);
                            if (slot < 32) {
                                g_catchAllDumpedRips[slot] = rip;
                                VehLog("  BYTES[+0x%llX]:", rip - sImgBase);
                                if (IsReadableMemory(ripBytes, 32))
                                    for (int b = 0; b < 32; ++b) VehLog(" %02X", ripBytes[b]);
                                VehLog("\n");
                            }
                        }
                    }

                    // v1.22.85: Full register + memory dump for first 20 catch-alls
                    if (count < 20) {
                        VehLog("  REGS: RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n",
                            (unsigned long long)pep->ContextRecord->Rax,
                            (unsigned long long)pep->ContextRecord->Rbx,
                            (unsigned long long)pep->ContextRecord->Rcx,
                            (unsigned long long)pep->ContextRecord->Rdx);
                        VehLog("        RSI=%016llX RDI=%016llX R8 =%016llX R14=%016llX\n",
                            (unsigned long long)pep->ContextRecord->Rsi,
                            (unsigned long long)pep->ContextRecord->Rdi,
                            (unsigned long long)pep->ContextRecord->R8,
                            (unsigned long long)pep->ContextRecord->R14);
                        // Dump memory at RSI (common source struct) if valid
                        auto rsiVal = pep->ContextRecord->Rsi;
                        if (rsiVal > 0x10000 && rsiVal <= 0x00007FFFFFFFFFFFULL) {
                            auto* mem = reinterpret_cast<std::uint8_t*>(rsiVal);
                            if (IsReadableMemory(mem, 128)) {
                                VehLog("  MEM[RSI=%016llX]:", (unsigned long long)rsiVal);
                                for (int b = 0; b < 128; ++b) {
                                    if (b % 16 == 0 && b > 0)
                                        VehLog("\n                              ");
                                    VehLog(" %02X", mem[b]);
                                }
                                VehLog("\n  ASCII: \"");
                                for (int b = 0; b < 128; ++b) {
                                    char c = (char)mem[b];
                                    VehLog("%c", (c >= 32 && c < 127) ? c : '.');
                                }
                                VehLog("\"\n");
                            }
                        }
                    }
                }
                // Safety valve: after 10000 catch-all redirects, stop handling
                if (count < 10000)
                    return EXCEPTION_CONTINUE_EXECUTION;
            }

            // Only log first 20 non-handled crashes
            if (g_crashLogCount.fetch_add(1, std::memory_order_relaxed) >= 20)
                return EXCEPTION_CONTINUE_SEARCH;

            // Write crash details to SKSE log directory
            {
                // rip, targetAddr, sImgBase already computed above
                auto offset = rip - sImgBase;

                {
                    char verBuf[64];
                    snprintf(verBuf, sizeof(verBuf), "\n=== SSEEngineFixesForWine CRASH LOG (v%zu.%zu.%zu) ===\n",
                        Version::MAJOR, Version::MINOR, Version::PATCH);
                    VehLog("%s", verBuf);
                }
                VehLog("Exception: 0x%08lX at RIP=0x%llX (SkyrimSE.exe+0x%llX)\n",
                    code, rip, offset);
                VehLog("Target address: 0x%llX\n", (unsigned long long)targetAddr);
                VehLog("Registers:\n");
                VehLog("  RAX=0x%016llX  RBX=0x%016llX  RCX=0x%016llX  RDX=0x%016llX\n",
                    pep->ContextRecord->Rax, pep->ContextRecord->Rbx,
                    pep->ContextRecord->Rcx, pep->ContextRecord->Rdx);
                VehLog("  RSI=0x%016llX  RDI=0x%016llX  RBP=0x%016llX  RSP=0x%016llX\n",
                    pep->ContextRecord->Rsi, pep->ContextRecord->Rdi,
                    pep->ContextRecord->Rbp, pep->ContextRecord->Rsp);
                VehLog("  R8 =0x%016llX  R9 =0x%016llX  R10=0x%016llX  R11=0x%016llX\n",
                    pep->ContextRecord->R8, pep->ContextRecord->R9,
                    pep->ContextRecord->R10, pep->ContextRecord->R11);
                VehLog("  R12=0x%016llX  R13=0x%016llX  R14=0x%016llX  R15=0x%016llX\n",
                    pep->ContextRecord->R12, pep->ContextRecord->R13,
                    pep->ContextRecord->R14, pep->ContextRecord->R15);

                // Dump bytes at RIP for disassembly
                VehLog("Bytes at RIP:");
                if (IsReadableMemory(ripBytes, 16)) {
                    for (int i = 0; i < 16; ++i)
                        VehLog(" %02X", ripBytes[i]);
                } else {
                    VehLog(" <unreadable>");
                }
                VehLog("\n");

                // Stack dump (16 entries)
                VehLog("Stack (RSP):");
                auto* rspPtr = reinterpret_cast<const DWORD64*>(pep->ContextRecord->Rsp);
                if (IsReadableMemory(rspPtr, 128)) {
                    VehLog("\n");
                    for (int i = 0; i < 16; ++i)
                        VehLog("  [RSP+0x%02X] = 0x%016llX\n", i * 8, rspPtr[i]);
                } else {
                    VehLog(" <unreadable>\n");
                }

                VehLog("=== END CRASH LOG ===\n");
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
    } // namespace detail
} // namespace Patches::FormCaching
