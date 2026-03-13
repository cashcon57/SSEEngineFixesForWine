#pragma once

#include "form_caching_globals.h"

namespace Patches::FormCaching
{
    namespace detail
    {
        // v1.22.97: Common cave patch helpers — reduce boilerplate across 29 patches.
        inline bool EmitJmpRel32(std::uint8_t* site, std::uint8_t* cave, int patchSize) {
            auto dist = reinterpret_cast<std::intptr_t>(cave) - reinterpret_cast<std::intptr_t>(site + 5);
            if (dist > INT32_MAX || dist < INT32_MIN) return false;
            site[0] = 0xE9;
            auto rel32 = static_cast<std::int32_t>(dist);
            std::memcpy(&site[1], &rel32, 4);
            for (int i = 5; i < patchSize; i++) site[i] = 0x90;
            FlushInstructionCache(GetCurrentProcess(), site, patchSize);
            return true;
        }

        inline void EmitJmpAbs64(std::uint8_t* cave, int& off, std::uint64_t target) {
            cave[off++] = 0xFF; cave[off++] = 0x25;
            cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
            std::memcpy(&cave[off], &target, 8); off += 8;
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
            // The containing function's layout:
            //   +0x2ED880: call [rax+0x4B8]     ← PatchA (6 bytes)
            //   +0x2ED886: ...5 bytes...
            //   +0x2ED88B: test byte [rax+0x40], 0x01  ← PatchC (6 bytes)
            //   +0x2ED891: xor eax,eax; add rsp,0x28; ret  ← "return 0" epilogue
            // When the call fails (null RAX or bad call target), we must skip
            // to +0x2ED891 to return 0 from the function, NOT to +0x2ED886
            // which would execute with RAX=0 and freeze in an infinite loop.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2ED880);
                auto returnAddr = reinterpret_cast<std::uint64_t>(site + 6);
                // "return 0" epilogue: xor eax,eax; add rsp,0x28; ret
                auto nullAddr = static_cast<std::uint64_t>(base + 0x2ED891);

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

                    // xor eax, eax  (null path — skip entire function, return 0)
                    cave[off++] = 0x31; cave[off++] = 0xC0;
                    // jmp [rip+0] → nullAddr (+0x2ED891: xor eax,eax; add rsp,0x28; ret)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &nullAddr, 8); off += 8;

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
                        // NOTE: Do NOT restore original page protection — Wine reverts
                        // file-backed code pages when protection is restored, undoing patches.
                        FlushInstructionCache(GetCurrentProcess(), site, 6);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(nullAddr), "PatchA", site, 6);
                        logger::info("PatchHotSpotA: +0x2ED880 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
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
                        FlushInstructionCache(GetCurrentProcess(), site, 8);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(returnAddr), "PatchB", site, 8);
                        logger::info("PatchHotSpotB: +0x32D146 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch C: +0x2ED88B — null check before test byte [rax+0x40] ──
            // Original: F6 40 40 01 74 07 (6 bytes)
            //   test byte [rax+0x40], 0x01
            //   je +7  → +0x2ED898 (mov rax, [rax+0x128]; ret)
            // If je NOT taken: xor eax,eax; add rsp,0x28; ret (return 0)
            //
            // Function returns NULL if bit 0 is set, or [rax+0x128] if not.
            // When RAX is null/near-null (~0x7D00), the VEH redirect costs
            // ~50μs per call. At 42K/sec this consumes >100% of a CPU core
            // and freezes the game after New Game.
            //
            // Null behavior: return 0 (same as "flag set" path).
            // Sentinel page reads are harmless (no VEH), no sentinel check needed.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2ED88B);

                logger::info("PatchHotSpotC: bytes at +0x2ED88B: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                if (site[0] != 0xF6 || site[1] != 0x40 || site[2] != 0x40 ||
                    site[3] != 0x01 || site[4] != 0x74 || site[5] != 0x07) {
                    logger::error("PatchHotSpotC: unexpected bytes — NOT patching");
                } else {
                    // Return sites:
                    //   "return 0"   path: +0x2ED891 (xor eax,eax; add rsp,0x28; ret)
                    //   "get member" path: +0x2ED898 (mov rax,[rax+0x128]; add rsp,0x28; ret)
                    std::uint64_t returnZeroAddr = static_cast<std::uint64_t>(base + 0x2ED891);
                    std::uint64_t getMemberAddr  = static_cast<std::uint64_t>(base + 0x2ED898);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(48));
                    int off = 0;

                    // 64-bit validation: any 32-bit value (null, sentinel 0x7FD70000,
                    // form-ID-as-pointer like 0x3C003C00) has high dword == 0.
                    // Valid heap pointers are always > 4GB on x64.
                    // mov r10, rax
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xC2;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .return_zero
                    cave[off++] = 0x74;
                    int jbRetZeroOff = off;
                    cave[off++] = 0x00;

                    // Original: test byte [rax+0x40], 0x01
                    cave[off++] = 0xF6; cave[off++] = 0x40; cave[off++] = 0x40; cave[off++] = 0x01;
                    // je .get_member (ZF=1 → bit not set → return member)
                    cave[off++] = 0x74;
                    int jeGetMemOff = off;
                    cave[off++] = 0x00;

                    // .return_zero: (bit IS set, or null → return 0)
                    int retZeroStart = off;
                    cave[jbRetZeroOff] = static_cast<std::uint8_t>(retZeroStart - (jbRetZeroOff + 1));
                    // jmp [rip+0] → returnZeroAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &returnZeroAddr, 8); off += 8;

                    // .get_member:
                    int getMemStart = off;
                    cave[jeGetMemOff] = static_cast<std::uint8_t>(getMemStart - (jeGetMemOff + 1));
                    // jmp [rip+0] → getMemberAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &getMemberAddr, 8); off += 8;

                    logger::info("PatchHotSpotC: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + NOP (1 byte)
                    DWORD oldProt;
                    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotC: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 6, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 6);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(returnZeroAddr), "PatchC", site, 6);
                        logger::info("PatchHotSpotC: +0x2ED88B patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch D: +0x664A0B — null check before test byte [rbp+0x40] ──
            // Original: F6 45 40 01 74 05 (6 bytes)
            //   test byte [rbp+0x40], 0x01
            //   je +5  → +0x664A16 (test rbx, rbx; ...)
            // If je NOT taken: mov rsi, rbp; jmp +0x30 (use form)
            //
            // Main loop hot path — iterates forms and checks flag at +0x40.
            // When RBP is null (~0x7D00), VEH at 42K/sec freezes the game.
            //
            // Null behavior: take the je path (skip form, go to +0x664A16).
            // Sentinel page reads are harmless (no VEH), no sentinel check needed.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x664A0B);

                logger::info("PatchHotSpotD: bytes at +0x664A0B: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                if (site[0] != 0xF6 || site[1] != 0x45 || site[2] != 0x40 ||
                    site[3] != 0x01 || site[4] != 0x74 || site[5] != 0x05) {
                    logger::error("PatchHotSpotD: unexpected bytes — NOT patching");
                } else {
                    // Return sites:
                    //   je NOT taken: +0x664A11 (mov rsi, rbp — use this form)
                    //   je taken / null: +0x664A16 (test rbx, rbx — skip form)
                    std::uint64_t useFormAddr  = static_cast<std::uint64_t>(base + 0x664A11);
                    std::uint64_t skipFormAddr = static_cast<std::uint64_t>(base + 0x664A16);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(48));
                    int off = 0;

                    // 64-bit validation: high dword == 0 → invalid (null, sentinel, form ID)
                    // mov r10, rbp
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xEA;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .skip_form (32-bit value → take je path)
                    cave[off++] = 0x74;
                    int jbSkipOff = off;
                    cave[off++] = 0x00;

                    // Original: test byte [rbp+0x40], 0x01
                    cave[off++] = 0xF6; cave[off++] = 0x45; cave[off++] = 0x40; cave[off++] = 0x01;
                    // je .skip_form
                    cave[off++] = 0x74;
                    int jeSkipOff = off;
                    cave[off++] = 0x00;

                    // .use_form: (flag is set → use this form)
                    // jmp [rip+0] → useFormAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &useFormAddr, 8); off += 8;

                    // .skip_form: (flag not set, or null → skip)
                    int skipStart = off;
                    cave[jbSkipOff] = static_cast<std::uint8_t>(skipStart - (jbSkipOff + 1));
                    cave[jeSkipOff] = static_cast<std::uint8_t>(skipStart - (jeSkipOff + 1));
                    // jmp [rip+0] → skipFormAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &skipFormAddr, 8); off += 8;

                    logger::info("PatchHotSpotD: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + NOP (1 byte)
                    DWORD oldProt;
                    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotD: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 6, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 6);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(skipFormAddr), "PatchD", site, 6);
                        logger::info("PatchHotSpotD: +0x664A0B patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch E: +0x1AF7EF ──────────────────────────────────────
            // Hot site in a form ref-counting function.
            // Original bytes: 39 07 75 14 F0 FF 47 04 (8 bytes)
            //   cmp [rdi], eax     ; compare form type
            //   jne +0x14          ; if not equal, goto +0x1AF807
            //   lock inc [rdi+4]   ; atomic refcount++
            // When rdi is null/near-null (zero-paged form), this faults
            // ~47K/sec causing 100% CPU freeze on New Game.
            // Fix: inline null check → skip to function epilogue.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x1AF7EF);

                logger::info("PatchHotSpotE: bytes at +0x1AF7EF: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5], site[6], site[7]);

                if (site[0] != 0x39 || site[1] != 0x07 || site[2] != 0x75 ||
                    site[3] != 0x14 || site[4] != 0xF0 || site[5] != 0xFF ||
                    site[6] != 0x47 || site[7] != 0x04) {
                    logger::error("PatchHotSpotE: unexpected bytes — NOT patching");
                } else {
                    // Return sites:
                    //   epilogue (null or equal path): +0x1AF7F7 (mov rbp, [rsp+0x40])
                    //   jne target (not-equal path):   +0x1AF807
                    std::uint64_t epilogueAddr  = static_cast<std::uint64_t>(base + 0x1AF7F7);
                    std::uint64_t notEqualAddr  = static_cast<std::uint64_t>(base + 0x1AF807);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(48));
                    int off = 0;

                    // 64-bit validation: high dword == 0 → invalid (null, sentinel, form ID)
                    // mov r10, rdi
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xFA;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .null_path (32-bit value → skip to epilogue)
                    cave[off++] = 0x74;
                    int jbNullOff = off;
                    cave[off++] = 0x00;

                    // Original: cmp [rdi], eax
                    cave[off++] = 0x39; cave[off++] = 0x07;
                    // jne .not_equal
                    cave[off++] = 0x75;
                    int jneOff = off;
                    cave[off++] = 0x00;

                    // Equal path: lock inc dword [rdi+4]
                    cave[off++] = 0xF0; cave[off++] = 0xFF; cave[off++] = 0x47; cave[off++] = 0x04;

                    // .null_path: (also equal-path falls through here)
                    int nullStart = off;
                    cave[jbNullOff] = static_cast<std::uint8_t>(nullStart - (jbNullOff + 1));
                    // jmp [rip+0] → epilogueAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &epilogueAddr, 8); off += 8;

                    // .not_equal:
                    int notEqStart = off;
                    cave[jneOff] = static_cast<std::uint8_t>(notEqStart - (jneOff + 1));
                    // jmp [rip+0] → notEqualAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &notEqualAddr, 8); off += 8;

                    logger::info("PatchHotSpotE: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 3 NOPs (overwrite all 8 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotE: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 8, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; site[6] = 0x90; site[7] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 8);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(epilogueAddr), "PatchE", site, 8);
                        logger::info("PatchHotSpotE: +0x1AF7EF patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            logger::info("PatchHotSpotNullChecks: {} code caves registered", g_numCavePatches.load());

            // ── Patch F: +0x2EB95D ──────────────────────────────────────
            // Hot site in form flag-checking code (same area as Patch C).
            // Original bytes: F6 46 40 01 48 0F 44 F3 (8 bytes)
            //   test byte [rsi+0x40], 0x01  ; check form flag
            //   cmovz rsi, rbx              ; if flag clear, rsi = rbx
            // When rsi is null/near-null (0x7D00), [rsi+0x40] = 0x7D40
            // faults ~43K/sec on New Game.
            // Fix: inline null check → skip test, force cmovz (rsi=rbx).
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2EB95D);

                logger::info("PatchHotSpotF: bytes at +0x2EB95D: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5], site[6], site[7]);

                if (site[0] != 0xF6 || site[1] != 0x46 || site[2] != 0x40 ||
                    site[3] != 0x01 || site[4] != 0x48 || site[5] != 0x0F ||
                    site[6] != 0x44 || site[7] != 0xF3) {
                    logger::error("PatchHotSpotF: unexpected bytes — NOT patching");
                } else {
                    // After the 8 patched bytes, code continues at +0x2EB965:
                    //   4C 8B FB  mov r15, rbx
                    //   48 85 F6  test rsi, rsi
                    //   75 09     jne +9 → +0x2EB971
                    // Null path: rsi=rbx (rbx is the fallback), continue at +0x2EB965
                    // Normal path: test [rsi+0x40], cmovz, continue at +0x2EB965
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x2EB965);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(56));
                    int off = 0;

                    // 64-bit validation: high dword == 0 → invalid (null, sentinel, form ID)
                    // mov r10, rsi
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xF2;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .null_path (32-bit value → force rsi=rbx)
                    cave[off++] = 0x74;
                    int jbNullOff = off;
                    cave[off++] = 0x00;

                    // Original: test byte [rsi+0x40], 0x01
                    cave[off++] = 0xF6; cave[off++] = 0x46; cave[off++] = 0x40; cave[off++] = 0x01;
                    // Original: cmovz rsi, rbx
                    cave[off++] = 0x48; cave[off++] = 0x0F; cave[off++] = 0x44; cave[off++] = 0xF3;
                    // jmp continue
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .null_path: mov rsi, rbx (force fallback)
                    int nullStart = off;
                    cave[jbNullOff] = static_cast<std::uint8_t>(nullStart - (jbNullOff + 1));
                    cave[off++] = 0x48; cave[off++] = 0x89; cave[off++] = 0xDE; // mov rsi, rbx
                    // jmp continue
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    logger::info("PatchHotSpotF: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 3 NOPs (overwrite all 8 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotF: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 8, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; site[6] = 0x90; site[7] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 8);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(continueAddr), "PatchF", site, 8);
                        logger::info("PatchHotSpotF: +0x2EB95D patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch G: +0x179337 ──────────────────────────────────────
            // Linked list virtual call site. Iterates form nodes:
            //   mov rax, [rdi]     ; load vtable from form object
            //   mov rcx, rdi       ; this = rdi
            //   call [rax+0x08]    ; virtual function call (vtable slot 1)
            // When rdi = sentinel page (PAGE_READWRITE), engine writes
            // corrupt the vtable pointer (offset +0x00) to 0xFFFFFFFF.
            // call [0xFFFFFFFF+8] faults → CATCH-ALL flood → crash.
            // Fix: validate vtable is in SkyrimSE.exe image (high dword=1)
            // before calling. Invalid vtable → exit loop (not-found path).
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x179337);

                logger::info("PatchHotSpotG: bytes at +0x179337: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4],
                    site[5], site[6], site[7], site[8]);

                // Expected: 48 8B 07 48 8B CF FF 50 08
                //   mov rax, [rdi]; mov rcx, rdi; call [rax+0x08]
                if (site[0] != 0x48 || site[1] != 0x8B || site[2] != 0x07 ||
                    site[3] != 0x48 || site[4] != 0x8B || site[5] != 0xCF ||
                    site[6] != 0xFF || site[7] != 0x50 || site[8] != 0x08) {
                    logger::error("PatchHotSpotG: unexpected bytes — NOT patching");
                } else {
                    // After the 9 patched bytes, +0x179340:
                    //   3B C5        cmp eax, ebp     ; compare result
                    //   74 0B        je +0x0B         ; found → +0x17934F
                    //   48 8B 7F 08  mov rdi,[rdi+8]  ; next node
                    //   48 85 FF     test rdi, rdi    ; null check
                    //   75 EA        jne → loop       ; +0x179337
                    //   EB 43        jmp → +0x179392  ; not-found exit
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x179340);
                    // Loop exit (not-found): +0x17934D is "jmp +0x43" → +0x179392
                    std::uint64_t exitLoopAddr = static_cast<std::uint64_t>(base + 0x17934D);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(80));
                    int off = 0;

                    // 1. 64-bit validation: high dword == 0 → invalid (null, sentinel, form ID)
                    // mov r10, rdi
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xFA;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .exit_loop
                    cave[off++] = 0x74;
                    int jbExitOff = off;
                    cave[off++] = 0x00;

                    // 2. mov rax, [rdi]  — load vtable
                    cave[off++] = 0x48; cave[off++] = 0x8B; cave[off++] = 0x07;

                    // 3. Validate vtable: high dword must be 1 (SkyrimSE.exe image = 0x14XXXXXXX)
                    //    mov r10, rax
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xC2;
                    //    shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    //    cmp r10d, 1
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x01;
                    //    jne .exit_loop
                    cave[off++] = 0x75;
                    int jneExitOff = off;
                    cave[off++] = 0x00;

                    // 4. Vtable valid — original code: mov rcx, rdi; call [rax+0x08]
                    cave[off++] = 0x48; cave[off++] = 0x8B; cave[off++] = 0xCF;  // mov rcx, rdi
                    cave[off++] = 0xFF; cave[off++] = 0x50; cave[off++] = 0x08;  // call [rax+0x08]

                    // 5. jmp [rip+0] → continueAddr (+0x179340)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .exit_loop:
                    int exitStart = off;
                    cave[jbExitOff] = static_cast<std::uint8_t>(exitStart - (jbExitOff + 1));
                    cave[jneExitOff] = static_cast<std::uint8_t>(exitStart - (jneExitOff + 1));
                    // jmp [rip+0] → exitLoopAddr (+0x17934D, which is "jmp +0x43" → not-found)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &exitLoopAddr, 8); off += 8;

                    logger::info("PatchHotSpotG: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch site: JMP rel32 (5 bytes) + 4 NOPs (overwrite all 9 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 9, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotG: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 9, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; site[6] = 0x90; site[7] = 0x90; site[8] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 9);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(exitLoopAddr), "PatchG", site, 9);
                        logger::info("PatchHotSpotG: +0x179337 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch H: +0x1790A4 ──────────────────────────────────────
            // Array indexed access in form processing code.
            // Original bytes: 41 8B 04 C6  25 00 00 F0 03 (9 bytes)
            //   mov eax, [r14 + rax*8]  ; load from array using r14 as base
            //   and eax, 0x03F00000     ; mask upper bits
            // When r14 = sentinel page (0x7FD70000, only 64KB), the index
            // rax*8 exceeds the page boundary → fault at unmapped address.
            // The CATCH-ALL handler can't fix this (wrong register gets
            // redirected), leading to state corruption → later EXT-CRASH.
            // Fix: check if r14 is a valid 64-bit heap pointer (high dword
            // must be nonzero). Sentinel/null are 32-bit → skip with eax=0.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x1790A4);

                logger::info("PatchHotSpotH: bytes at +0x1790A4: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4],
                    site[5], site[6], site[7], site[8]);

                // Expected: 41 8B 04 C6 25 00 00 F0 03
                if (site[0] != 0x41 || site[1] != 0x8B || site[2] != 0x04 ||
                    site[3] != 0xC6 || site[4] != 0x25 || site[5] != 0x00 ||
                    site[6] != 0x00 || site[7] != 0xF0 || site[8] != 0x03) {
                    logger::error("PatchHotSpotH: unexpected bytes — NOT patching");
                } else {
                    // After the 9 patched bytes, code continues at +0x1790AD:
                    //   0B C1        or eax, ecx
                    //   89 03        mov [rbx], eax
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x1790AD);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(64));
                    int off = 0;

                    // 1. Check r14 is a valid 64-bit pointer (high dword must be nonzero)
                    //    mov r10, r14
                    cave[off++] = 0x4D; cave[off++] = 0x89; cave[off++] = 0xF2;
                    //    shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    //    test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    //    jz .skip (r14 is 32-bit address → sentinel or invalid)
                    cave[off++] = 0x74;
                    int jzSkipOff = off;
                    cave[off++] = 0x00;

                    // 2. R14 valid — original: mov eax, [r14 + rax*8]
                    cave[off++] = 0x41; cave[off++] = 0x8B; cave[off++] = 0x04; cave[off++] = 0xC6;
                    // Original: and eax, 0x03F00000
                    cave[off++] = 0x25; cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0xF0; cave[off++] = 0x03;

                    // 3. jmp [rip+0] → continueAddr (+0x1790AD)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .skip: r14 is sentinel/invalid — return eax=0
                    int skipStart = off;
                    cave[jzSkipOff] = static_cast<std::uint8_t>(skipStart - (jzSkipOff + 1));
                    //    xor eax, eax
                    cave[off++] = 0x31; cave[off++] = 0xC0;
                    //    jmp [rip+0] → continueAddr (+0x1790AD)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    logger::info("PatchHotSpotH: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch site: JMP rel32 (5 bytes) + 4 NOPs (overwrite all 9 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 9, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotH: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 9, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; site[6] = 0x90; site[7] = 0x90; site[8] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 9);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(continueAddr), "PatchH", site, 9);
                        logger::info("PatchHotSpotH: +0x1790A4 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch I: +0x2D5DE0 ──────────────────────────────────────
            // Hash table chain traversal — linked list walk comparing
            // node keys. This is the #1 crash site on New Game.
            // Original bytes: 48 39 08 0F 84 FC 00 00 00 (9 bytes)
            //   cmp [rax], rcx      ; compare node value with search key
            //   je +0xFC            ; → +0x2D5EE5 (found handler)
            // When rax = sentinel (0x7FD70000, PAGE_READWRITE), engine
            // writes corrupt the sentinel data — [rax] reads garbage
            // (0xFFFFFFFF etc.) which faults. The CATCH-ALL redirects
            // to sentinel but [sentinel+8] (next ptr) is also garbage,
            // creating an infinite fault loop (200+ events → crash).
            // Fix: 64-bit validate rax before dereference. Invalid →
            // exit loop to not-found path at +0x2D5DF3.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2D5DE0);

                logger::info("PatchHotSpotI: bytes at +0x2D5DE0: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4],
                    site[5], site[6], site[7], site[8]);

                // Expected: 48 39 08 0F 84 FC 00 00 00
                if (site[0] != 0x48 || site[1] != 0x39 || site[2] != 0x08 ||
                    site[3] != 0x0F || site[4] != 0x84 || site[5] != 0xFC ||
                    site[6] != 0x00 || site[7] != 0x00 || site[8] != 0x00) {
                    logger::error("PatchHotSpotI: unexpected bytes — NOT patching");
                } else {
                    // Return sites:
                    //   continue (after patched bytes): +0x2D5DE9 (mov rax, [rax+8])
                    //   found handler:                  +0x2D5EE5 (je +0xFC target)
                    //   not-found (loop exit):          +0x2D5DF3 (natural fall-through)
                    std::uint64_t continueAddr  = static_cast<std::uint64_t>(base + 0x2D5DE9);
                    std::uint64_t foundAddr     = static_cast<std::uint64_t>(base + 0x2D5EE5);
                    std::uint64_t notFoundAddr  = static_cast<std::uint64_t>(base + 0x2D5DF3);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(64));
                    int off = 0;

                    // 1. 64-bit validation: high dword == 0 → invalid
                    // mov r10, rax
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xC2;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .not_found
                    cave[off++] = 0x74;
                    int jzNotFoundOff = off;
                    cave[off++] = 0x00;

                    // 2. Original: cmp [rax], rcx
                    cave[off++] = 0x48; cave[off++] = 0x39; cave[off++] = 0x08;
                    // je .found_handler
                    cave[off++] = 0x74;
                    int jeFoundOff = off;
                    cave[off++] = 0x00;

                    // 3. Fall-through (not equal): jmp → continueAddr (+0x2D5DE9)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .found_handler:
                    int foundStart = off;
                    cave[jeFoundOff] = static_cast<std::uint8_t>(foundStart - (jeFoundOff + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &foundAddr, 8); off += 8;

                    // .not_found: exit loop
                    int notFoundStart = off;
                    cave[jzNotFoundOff] = static_cast<std::uint8_t>(notFoundStart - (jzNotFoundOff + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &notFoundAddr, 8); off += 8;

                    logger::info("PatchHotSpotI: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 4 NOPs (overwrite all 9 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 9, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotI: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 9, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; site[6] = 0x90; site[7] = 0x90; site[8] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 9);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(notFoundAddr), "PatchI", site, 9);
                        logger::info("PatchHotSpotI: +0x2D5DE0 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch J: +0x19CDF5 ──────────────────────────────────────
            // Form type check — movzx eax, byte [rdx+0x44] then cmp al, 3.
            // RDX is corrupted sentinel data. Safe default: xor eax, eax.
            // Overwrite 6 bytes: 0F B6 42 44 3C 03 (movzx + cmp al,3).
            // Cave executes both if valid. Continue at +0x19CDFB.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x19CDF5);

                logger::info("PatchHotSpotJ: bytes at +0x19CDF5: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                // Expected: 0F B6 42 44 3C 03
                if (site[0] != 0x0F || site[1] != 0xB6 || site[2] != 0x42 ||
                    site[3] != 0x44 || site[4] != 0x3C || site[5] != 0x03) {
                    logger::error("PatchHotSpotJ: unexpected bytes — NOT patching");
                } else {
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x19CDFB);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(48));
                    int off = 0;

                    // 64-bit validation: high dword == 0 → invalid
                    // mov r10, rdx
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xD2;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .invalid
                    cave[off++] = 0x74;
                    int jzInvalidOff = off;
                    cave[off++] = 0x00;

                    // .valid: original movzx eax, byte [rdx+0x44]
                    cave[off++] = 0x0F; cave[off++] = 0xB6; cave[off++] = 0x42; cave[off++] = 0x44;
                    // original cmp al, 3
                    cave[off++] = 0x3C; cave[off++] = 0x03;
                    // jmp [rip+0] → continueAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .invalid: xor eax, eax (safe default) then continue
                    int invalidStart = off;
                    cave[jzInvalidOff] = static_cast<std::uint8_t>(invalidStart - (jzInvalidOff + 1));
                    cave[off++] = 0x31; cave[off++] = 0xC0;
                    // cmp al, 3 (set flags consistently — al=0 so CF=1, ZF=0)
                    cave[off++] = 0x3C; cave[off++] = 0x03;
                    // jmp [rip+0] → continueAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    logger::info("PatchHotSpotJ: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 1 NOP (overwrite all 6 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotJ: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 6, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 6);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(continueAddr), "PatchJ", site, 6);
                        logger::info("PatchHotSpotJ: +0x19CDF5 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch K: +0x2B54D0 ──────────────────────────────────────
            // Reading formFlags — mov eax, [rcx+0x10] then shr eax, 0xA.
            // RCX is corrupted. Safe default: xor eax, eax.
            // Overwrite 6 bytes: 8B 41 10 C1 E8 0A (mov + shr).
            // Cave executes both if valid. Continue at +0x2B54D6.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2B54D0);

                logger::info("PatchHotSpotK: bytes at +0x2B54D0: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                // Expected: 8B 41 10 C1 E8 0A
                if (site[0] != 0x8B || site[1] != 0x41 || site[2] != 0x10 ||
                    site[3] != 0xC1 || site[4] != 0xE8 || site[5] != 0x0A) {
                    logger::error("PatchHotSpotK: unexpected bytes — NOT patching");
                } else {
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x2B54D6);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(48));
                    int off = 0;

                    // 64-bit validation: high dword == 0 → invalid
                    // mov r10, rcx
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xCA;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // test r10d, r10d
                    cave[off++] = 0x45; cave[off++] = 0x85; cave[off++] = 0xD2;
                    // jz .invalid
                    cave[off++] = 0x74;
                    int jzInvalidOff = off;
                    cave[off++] = 0x00;

                    // .valid: original mov eax, [rcx+0x10]
                    cave[off++] = 0x8B; cave[off++] = 0x41; cave[off++] = 0x10;
                    // original shr eax, 0xA
                    cave[off++] = 0xC1; cave[off++] = 0xE8; cave[off++] = 0x0A;
                    // jmp [rip+0] → continueAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .invalid: xor eax, eax (safe default, already 0 after shr)
                    int invalidStart = off;
                    cave[jzInvalidOff] = static_cast<std::uint8_t>(invalidStart - (jzInvalidOff + 1));
                    cave[off++] = 0x31; cave[off++] = 0xC0;
                    // jmp [rip+0] → continueAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    logger::info("PatchHotSpotK: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 1 NOP (overwrite all 6 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotK: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 6, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 6);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(continueAddr), "PatchK", site, 6);
                        logger::info("PatchHotSpotK: +0x2B54D0 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch L: +0x2D5DB0 ──────────────────────────────────────
            // Pointer read from structure — mov rbp, [rbx+0x20] then test rbp, rbp.
            // RBX may be corrupted (sentinel-like 0x7D000000XX passes 64-bit check
            // but [rbx+0x20] faults). VEH CAVE-FAULT then redirects with wrong flags.
            // Fix: invalid path and VEH fallback both jump to function epilogue
            // at +0x2D5EE5 (restores regs from stack, returns al=1) — safe regardless
            // of register/flag state.
            // Overwrite 7 bytes: 48 8B 6B 20 48 85 ED (mov + test).
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2D5DB0);

                logger::info("PatchHotSpotL: bytes at +0x2D5DB0: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5], site[6]);

                // Expected: 48 8B 6B 20 48 85 ED
                if (site[0] != 0x48 || site[1] != 0x8B || site[2] != 0x6B ||
                    site[3] != 0x20 || site[4] != 0x48 || site[5] != 0x85 ||
                    site[6] != 0xED) {
                    logger::error("PatchHotSpotL: unexpected bytes — NOT patching");
                } else {
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x2D5DB7);
                    // Function epilogue: restores rbx/rbp/rsi from stack, returns al=1
                    std::uint64_t epilogueAddr = static_cast<std::uint64_t>(base + 0x2D5EE5);

                    // Also need grow-table address for rbp==0 case
                    std::uint64_t growTableAddr = static_cast<std::uint64_t>(base + 0x2D5E32);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(96));
                    int off = 0;

                    // 1. Validate RBX — canonical range check.
                    //    high dword must be 1-128 (valid user-mode heap/image).
                    //    Rejects: 0 (null/sentinel), >128 (non-canonical/garbage text).
                    //    Catches ASCII text like 0x574E4D454D414C43 that old check missed.
                    // mov r10, rbx
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xDA;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // dec r10d  (0→0xFFFFFFFF, 1→0, 0x574E4D45→0x574E4D44)
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA;
                    // cmp r10d, 0x7F  (valid range [1..128] → dec'd [0..127])
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F;
                    // ja .invalid
                    cave[off++] = 0x77;
                    int jaInvalid1Off = off;
                    cave[off++] = 0x00; // patched below

                    // 2. Original: mov rbp, [rbx+0x20]
                    cave[off++] = 0x48; cave[off++] = 0x8B; cave[off++] = 0x6B; cave[off++] = 0x20;

                    // 3. Check rbp == 0 → grow table
                    // test rbp, rbp
                    cave[off++] = 0x48; cave[off++] = 0x85; cave[off++] = 0xED;
                    // jz .growTable
                    cave[off++] = 0x74;
                    int jzGrowOff = off;
                    cave[off++] = 0x00; // patched below

                    // 4. Validate RBP — same canonical range check
                    // mov r10, rbp
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xEA;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // dec r10d
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA;
                    // cmp r10d, 0x7F
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F;
                    // ja .invalid
                    cave[off++] = 0x77;
                    int jaInvalid2Off = off;
                    cave[off++] = 0x00; // patched below

                    // 5. RBP valid. Need ZF=0 for je at +0x2D5DB7 to NOT take.
                    //    After cmp r10d,0x7F where r10d<128, CF=1 but ZF=0. Good.
                    // jmp [rip+0] → continueAddr (+0x2D5DB7)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .growTable: RBP is zero — jump to grow-table path (+0x2D5E32)
                    int growStart = off;
                    cave[jzGrowOff] = static_cast<std::uint8_t>(growStart - (jzGrowOff + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &growTableAddr, 8); off += 8;

                    // .invalid: RBX or RBP failed canonical check → xor rax,rax + epilogue
                    int invalidStart = off;
                    cave[jaInvalid1Off] = static_cast<std::uint8_t>(invalidStart - (jaInvalid1Off + 1));
                    cave[jaInvalid2Off] = static_cast<std::uint8_t>(invalidStart - (jaInvalid2Off + 1));
                    // xor eax, eax (zero RAX — signal "not found" to caller)
                    cave[off++] = 0x31; cave[off++] = 0xC0;
                    // jmp [rip+0] → epilogueAddr (+0x2D5EE5)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &epilogueAddr, 8); off += 8;

                    logger::info("PatchHotSpotL: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) + 2 NOPs (overwrite all 7 bytes)
                    DWORD oldProt;
                    VirtualProtect(site, 7, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotL: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 7, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; site[6] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, 7);
                        // VEH CAVE-FAULT fallback also goes to epilogue (safe regardless of flags)
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(epilogueAddr), "PatchL", site, 7);
                        logger::info("PatchHotSpotL: +0x2D5DB0 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch M: REMOVED (v1.22.97) ─────────────────────────────
            // PatchM previously patched +0x2D0C33 (6 bytes, mov eax,[rsi+0x94]).
            // It overlapped with PatchR6 which also targets +0x2D0C33 but covers
            // 12 bytes (both the mov AND the sub eax,[rsi+0x98] at +0x2D0C39).
            // PatchM ran first and overwrote the bytes, so PatchR6's byte
            // verification always failed silently. Since R6 is strictly more
            // comprehensive (validates RSI, executes both loads, skips on invalid),
            // PatchM is now removed to let R6 apply.

            // ── Patch N: +0x2D5E15 ──────────────────────────────────────
            // Hash table bucket probe: cmp qword [rcx+8], 0.
            // RCX = rbp + index*16 where rbp may be stale/corrupted (stack addr
            // or sentinel). Causes CATCH-ALL flood (200 events → CTD).
            // If RCX high dword == 0 → treat bucket as empty (skip cmp,
            // set ZF=1 so cmovne at +0x2D5E1A doesn't fire).
            // Overwrite 5 bytes: 48 83 79 08 00. Continue at +0x2D5E1A.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2D5E15);

                logger::info("PatchHotSpotN: bytes at +0x2D5E15: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4]);

                // Expected: 48 83 79 08 00
                if (site[0] != 0x48 || site[1] != 0x83 || site[2] != 0x79 ||
                    site[3] != 0x08 || site[4] != 0x00) {
                    logger::error("PatchHotSpotN: unexpected bytes — NOT patching");
                } else {
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x2D5E1A);
                    // Function epilogue — safe bail-out if bucket walk is hopeless
                    std::uint64_t epilogueAddr = static_cast<std::uint64_t>(base + 0x2D5EE5);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(64));
                    int off = 0;

                    // Canonical range check: high dword must be 1-128
                    // mov r10, rcx
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xCA;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // dec r10d
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA;
                    // cmp r10d, 0x7F
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F;
                    // ja .invalid
                    cave[off++] = 0x77;
                    int jzInvalidOff = off;
                    cave[off++] = 0x00;

                    // .valid: original cmp qword [rcx+8], 0
                    cave[off++] = 0x48; cave[off++] = 0x83; cave[off++] = 0x79;
                    cave[off++] = 0x08; cave[off++] = 0x00;
                    // jmp [rip+0] → continueAddr (+0x2D5E1A)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .invalid: xor rcx, rcx (null → cmovne won't fire, test rcx,rcx → jz exits loop)
                    int invalidStart = off;
                    cave[jzInvalidOff] = static_cast<std::uint8_t>(invalidStart - (jzInvalidOff + 1));
                    cave[off++] = 0x48; cave[off++] = 0x31; cave[off++] = 0xC9; // xor rcx, rcx
                    // jmp [rip+0] → epilogueAddr (bail out of function entirely)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &epilogueAddr, 8); off += 8;

                    logger::info("PatchHotSpotN: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) — exact fit
                    DWORD oldProt;
                    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotN: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 5, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        FlushInstructionCache(GetCurrentProcess(), site, 5);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(epilogueAddr), "PatchN", site, 5);
                        logger::info("PatchHotSpotN: +0x2D5E15 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch O: +0x2D5DCD ──────────────────────────────────────
            // Bucket occupancy check: cmp qword [rsi+8], 0.
            // RSI = rbp + (hash & (cap-1)) * 16. If RBP was stale/corrupted
            // (survived PatchL's validation but contained freed memory),
            // RSI points to garbage → ACCESS_VIOLATION at [rsi+8].
            // CATCH-ALL redirect makes it worse (redirects RSI to sentinel,
            // sentinel+8 has engine-written garbage → cascading corruption).
            // Fix: validate RSI high dword. Invalid → epilogue.
            // Overwrite 5 bytes: 48 83 7E 08 00 (exact JMP rel32 fit).
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2D5DCD);

                logger::info("PatchHotSpotO: bytes at +0x2D5DCD: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4]);

                // Expected: 48 83 7E 08 00
                if (site[0] != 0x48 || site[1] != 0x83 || site[2] != 0x7E ||
                    site[3] != 0x08 || site[4] != 0x00) {
                    logger::error("PatchHotSpotO: unexpected bytes — NOT patching");
                } else {
                    // Continue after the 5-byte cmp: +0x2D5DD2 (the je instruction)
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x2D5DD2);
                    std::uint64_t epilogueAddr = static_cast<std::uint64_t>(base + 0x2D5EE5);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(56));
                    int off = 0;

                    // 1. Validate RSI — canonical range check
                    // mov r10, rsi
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xF2;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // dec r10d
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA;
                    // cmp r10d, 0x7F
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F;
                    // ja .invalid
                    cave[off++] = 0x77;
                    int jzInvalidOff = off;
                    cave[off++] = 0x00; // patched below

                    // 2. Original instruction: cmp qword [rsi+8], 0
                    cave[off++] = 0x48; cave[off++] = 0x83; cave[off++] = 0x7E;
                    cave[off++] = 0x08; cave[off++] = 0x00;

                    // 3. Continue at +0x2D5DD2 (je instruction uses ZF from cmp)
                    // jmp [rip+0] → continueAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .invalid: RSI is bogus → bail to epilogue
                    int invalidStart = off;
                    cave[jzInvalidOff] = static_cast<std::uint8_t>(invalidStart - (jzInvalidOff + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &epilogueAddr, 8); off += 8;

                    logger::info("PatchHotSpotO: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 (5 bytes) — exact fit
                    DWORD oldProt;
                    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotO: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 5, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        FlushInstructionCache(GetCurrentProcess(), site, 5);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(epilogueAddr), "PatchO", site, 5);
                        logger::info("PatchHotSpotO: +0x2D5DCD patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch P: +0x2D0B90 — call [rax+0x120] ──────────────────────
            // Virtual function call where RAX can be corrupted sentinel data
            // (engine wrote ASCII text to sentinel vtable area). Validates
            // RAX is in canonical heap range before the call; if not, skip
            // the call and jump past it (the next instruction at +0x2D0B96
            // is a jmp that exits the block).
            // Original bytes: FF 90 20 01 00 00 (6 bytes)
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2D0B90);

                logger::info("PatchHotSpotP: bytes at +0x2D0B90: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                // Expected: FF 90 20 01 00 00  (call [rax+0x120])
                if (site[0] != 0xFF || site[1] != 0x90 || site[2] != 0x20 ||
                    site[3] != 0x01 || site[4] != 0x00 || site[5] != 0x00) {
                    logger::error("PatchHotSpotP: unexpected bytes — NOT patching");
                } else {
                    // After the 6-byte call: +0x2D0B96 is the next instruction
                    // (jmp +0x2D11B7 per disasm). Skip to there on bad RAX.
                    std::uint64_t continueAddr = static_cast<std::uint64_t>(base + 0x2D0B96);
                    // The call returns to +0x2D0B96, so on skip we go there too.
                    std::uint64_t skipAddr = continueAddr;

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(64));
                    int off = 0;

                    // 1. Validate RAX — canonical range check (high dword in [1, 0x7F])
                    // push r10
                    cave[off++] = 0x41; cave[off++] = 0x52;
                    // mov r10, rax
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xC2;
                    // shr r10, 32
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    // dec r10d
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA;
                    // cmp r10d, 0x7F
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F;
                    // pop r10
                    cave[off++] = 0x41; cave[off++] = 0x5A;
                    // ja .invalid (skip call)
                    cave[off++] = 0x77;
                    int jaOffset = off;
                    cave[off++] = 0x00; // patched below

                    // 2. Valid RAX — execute original: call [rax+0x120]
                    cave[off++] = 0xFF; cave[off++] = 0x90;
                    cave[off++] = 0x20; cave[off++] = 0x01; cave[off++] = 0x00; cave[off++] = 0x00;

                    // 3. Jump to continuation (+0x2D0B96)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    // .invalid: RAX is bogus — skip call, xor eax,eax (return 0)
                    int invalidStart = off;
                    cave[jaOffset] = static_cast<std::uint8_t>(invalidStart - (jaOffset + 1));
                    // xor eax, eax
                    cave[off++] = 0x31; cave[off++] = 0xC0;
                    // jmp to continuation (+0x2D0B96)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &skipAddr, 8); off += 8;

                    logger::info("PatchHotSpotP: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch: JMP rel32 + NOP (5+1 = 6 bytes — exact fit)
                    DWORD oldProt;
                    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHotSpotP: JMP too far ({}) — NOT patching", dist);
                        VirtualProtect(site, 6, oldProt, &oldProt);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        site[5] = 0x90; // NOP pad
                        FlushInstructionCache(GetCurrentProcess(), site, 6);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(skipAddr), "PatchP", site, 6);
                        logger::info("PatchHotSpotP: +0x2D0B90 patched (rel32={}) verify={:02X}",
                            rel32, site[0]);
                    }
                }
            }

            // ── Patch Q (x6): Bounded hash-chain traversal ─────────────────
            // All 6 BSTHashMap::find() instances walk a linked list in a
            // bucket chain.  Under Wine, chains can become circular (next
            // pointer loops back) causing an infinite tight loop with zero
            // exceptions — this is THE New Game freeze.
            //
            // Loop pattern (identical in all 6):
            //   cmp  [rcx], ebx         ; key match?
            //   je   <found>
            //   mov  rcx, [rcx+0x10]    ; next node
            //   cmp  rcx, [rdi+0x70]    ; sentinel check
            //   jne  <loop>
            //
            // Each cave reproduces the pre-loop + loop with r11d as a
            // 65536 iteration bound.  R11 is volatile (Windows x64 ABI).
            //
            // Sites found by binary scan of identical loop pattern:
            //   +0x1AF612 (in func +0x1AF5C0) — original freeze site
            //   +0x1A0EF1 (in func +0x1A0E80)
            //   +0x30C385, +0x30C4B2, +0x30C5D5, +0x30C702 (in func +0x30C2E0)
            {
                // All 6 loop sites: { loopRVA, preLoopStartRVA, patchSize,
                //                     notFoundRVA, foundRVA, preLoopBytes, name }
                struct HashChainSite {
                    std::uint32_t preLoopRVA;     // start of pre-loop check
                    std::uint32_t patchSize;      // bytes to overwrite
                    std::uint32_t notFoundRVA;    // "return null" target
                    std::uint32_t foundRVA;       // "found" target
                    const std::uint8_t* preLoopVerify;  // bytes to verify
                    int preLoopVerifyLen;
                    const char* name;
                };

                // Verification bytes at each pre-loop start
                // +0x1AF607: 48 39 74 CA 10 (cmp [rdx+rcx*8+0x10], rsi)
                static const std::uint8_t verify_1AF[] = { 0x48, 0x39, 0x74, 0xCA, 0x10 };
                // +0x1A0EE5: 48 83 7C C2 10 00 (cmp qword [rdx+rax*8+0x10], 0)
                static const std::uint8_t verify_1A0[] = { 0x48, 0x83, 0x7C, 0xC2, 0x10 };
                // +0x30C375: 48 83 7C C2 10 00 (same pattern for all 4 in the big func)
                static const std::uint8_t verify_30C[] = { 0x48, 0x83, 0x7C, 0xC2, 0x10 };

                // Each site: pre-loop bytes + lea + je/jz + loop body (cmp/je/mov/cmp/jne)
                // We patch from the initial bucket-empty check through the loop back-edge.
                HashChainSite sites[] = {
                    // +0x1AF607: 19 bytes (cmp rsi variant, short je)
                    { 0x1AF607, 19, 0x1AF620, 0x1AF633, verify_1AF, 5, "Q1" },
                    // +0x1A0EE5: 26 bytes (cmp 0 variant, short je, loop + jmp after)
                    { 0x1A0EE5, 26, 0x1A0F50, 0x1A0F01, verify_1A0, 5, "Q2" },
                    // +0x30C375: 30 bytes (cmp 0 variant, near je, loop + jmp after)
                    { 0x30C375, 30, 0x30C440, 0x30C398, verify_30C, 5, "Q3" },
                    // +0x30C4A2: 30 bytes
                    { 0x30C4A2, 30, 0x30C56D, 0x30C4C5, verify_30C, 5, "Q4" },
                    // +0x30C5C5: 30 bytes
                    { 0x30C5C5, 30, 0x30C690, 0x30C5E8, verify_30C, 5, "Q5" },
                    // +0x30C6F2: 30 bytes
                    { 0x30C6F2, 30, 0x30C7B9, 0x30C715, verify_30C, 5, "Q6" },
                };

                for (auto& s : sites) {
                    auto* site = reinterpret_cast<std::uint8_t*>(base + s.preLoopRVA);

                    logger::info("PatchHashChain-{}: bytes at +0x{:X}: "
                        "{:02X} {:02X} {:02X} {:02X} {:02X}",
                        s.name, s.preLoopRVA,
                        site[0], site[1], site[2], site[3], site[4]);

                    if (std::memcmp(site, s.preLoopVerify, s.preLoopVerifyLen) != 0) {
                        logger::error("PatchHashChain-{}: unexpected bytes — NOT patching", s.name);
                        continue;
                    }

                    auto returnNullAddr = static_cast<std::uint64_t>(base + s.notFoundRVA);
                    auto foundAddr      = static_cast<std::uint64_t>(base + s.foundRVA);

                    // Build the cave: copy the original pre-loop bytes into
                    // the cave, then add the bounded loop.
                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(128));
                    int off = 0;

                    // -- Pre-loop: copy original bytes up to the loop start --
                    // All variants have: bucket-empty-check + lea rcx + je/jz not-found
                    // We encode the cave generically: the loop body is always the same,
                    // and we emit the pre-loop as the original bytes minus the loop itself.
                    //
                    // For all variants, the last 12 bytes before patchEnd are the loop:
                    //   39 19              cmp [rcx], ebx
                    //   74 XX              je  found
                    //   48 8B 49 10        mov rcx, [rcx+0x10]
                    //   48 3B 4F 70        cmp rcx, [rdi+0x70]
                    //   75 F2              jne loop
                    // Some also have a trailing jmp/jne after the loop (2-5 bytes).
                    // We skip those since the cave handles not-found via jmp [rip].

                    int preLoopSize = s.patchSize - 12;
                    // For the 30-byte variants, there's a 5-byte jmp at the end
                    // (e9 XX XX XX XX) after the jne. For the 26-byte variant,
                    // there's a 2-byte jmp (eb XX). Subtract those too.
                    if (s.patchSize == 30) preLoopSize -= 5;  // trailing jmp rel32
                    else if (s.patchSize == 26) preLoopSize -= 2;  // trailing jmp rel8
                    // Now preLoopSize = bytes before 'cmp [rcx], ebx'

                    // Copy pre-loop bytes verbatim into cave
                    std::memcpy(&cave[off], site, preLoopSize);
                    off += preLoopSize;

                    // The last pre-loop instruction is a conditional jump to not-found.
                    // We need to fixup that jump target to point to our return_null label.
                    // For the short-je variant (patchSize=19): byte at off-1 is the rel8
                    // For the cmp-0 + short-je variant (patchSize=26): byte at off-1
                    // For the cmp-0 + near-jz variant (patchSize=30): bytes at off-4..off-1

                    // Mark where the initial "not found" je/jz needs to jump:
                    int jeNullFixupOff;
                    bool nearJump = false;
                    if (s.patchSize == 30) {
                        // near jz (0F 84 XX XX XX XX) — the last 6 bytes of pre-loop
                        // are the jz, so the rel32 is at off-4
                        jeNullFixupOff = off - 4;
                        nearJump = true;
                    } else {
                        // short je (74 XX) — rel8 is at off-1
                        jeNullFixupOff = off - 1;
                    }

                    // -- NEW: set iteration bound in r11d --
                    // mov r11d, 0x10000
                    cave[off++] = 0x41; cave[off++] = 0xBB;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x01; cave[off++] = 0x00;

                    // -- Loop body --
                    int loopTop = off;
                    // cmp [rcx], ebx
                    cave[off++] = 0x39; cave[off++] = 0x19;
                    // je found
                    cave[off++] = 0x74;
                    int jeFoundOff = off;
                    cave[off++] = 0x00;
                    // mov rcx, [rcx+0x10]
                    cave[off++] = 0x48; cave[off++] = 0x8B; cave[off++] = 0x49; cave[off++] = 0x10;
                    // cmp rcx, [rdi+0x70]
                    cave[off++] = 0x48; cave[off++] = 0x3B; cave[off++] = 0x4F; cave[off++] = 0x70;
                    // je return_null
                    cave[off++] = 0x74;
                    int jeNullOff2 = off;
                    cave[off++] = 0x00;
                    // dec r11d
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCB;
                    // jnz loop_top
                    cave[off++] = 0x75;
                    int loopBackOff = off;
                    cave[off++] = 0x00;
                    cave[loopBackOff] = static_cast<std::uint8_t>(
                        static_cast<std::int8_t>(loopTop - (loopBackOff + 1)));

                    // -- return_null label --
                    int returnNullStart = off;
                    // Fix up in-loop je to return_null
                    cave[jeNullOff2] = static_cast<std::uint8_t>(returnNullStart - (jeNullOff2 + 1));
                    // Fix up pre-loop je/jz to return_null
                    if (nearJump) {
                        auto rel32 = static_cast<std::int32_t>(returnNullStart - (jeNullFixupOff + 4));
                        std::memcpy(&cave[jeNullFixupOff], &rel32, 4);
                    } else {
                        cave[jeNullFixupOff] = static_cast<std::uint8_t>(returnNullStart - (jeNullFixupOff + 1));
                    }

                    // jmp [rip+0] → returnNullAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &returnNullAddr, 8); off += 8;

                    // -- found label --
                    int foundStart = off;
                    cave[jeFoundOff] = static_cast<std::uint8_t>(foundStart - (jeFoundOff + 1));
                    // jmp [rip+0] → foundAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &foundAddr, 8); off += 8;

                    logger::info("PatchHashChain-{}: cave {} bytes at 0x{:X}", s.name, off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch original site with JMP rel32 + NOPs
                    DWORD oldProt;
                    VirtualProtect(site, s.patchSize, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHashChain-{}: JMP too far ({}) — NOT patching", s.name, dist);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        for (std::uint32_t i = 5; i < s.patchSize; i++) site[i] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, s.patchSize);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(returnNullAddr), s.name, site, s.patchSize);
                        logger::info("PatchHashChain-{}: +0x{:X} patched — bounded hash-chain (max 65536)",
                            s.name, s.preLoopRVA);
                    }
                }
            }

            // ── Patch Q7: Bounded find loop in GetFormByNumericId ────────
            // The native GetFormByNumericId at +0x1E01A0 has an unbounded
            // find loop at +0x1E020C that walks bucket chains in the global
            // form map (Layout B: sentinel at [rbx+0x18]).  When our sharded
            // cache misses (new forms not yet cached), control falls through
            // to this native loop.  On corrupted chains → infinite loop →
            // freeze.  This is the LAST unbounded find loop that can trap
            // the main thread during New Game.
            //
            // Patch site: +0x1E0205 (21 bytes, through +0x1E0219)
            //   +0x1E0205: cmp qword [rax+0x10], 0    ; empty bucket?
            //   +0x1E020A: je +0x1E0220                ; → not found
            //   +0x1E020C: cmp [rax], edi              ; key match?
            //   +0x1E020E: je +0x1E021A                ; → found
            //   +0x1E0210: mov rax, [rax+0x10]         ; next node
            //   +0x1E0214: cmp rax, [rbx+0x18]         ; sentinel
            //   +0x1E0218: jne -0x0E                   ; → loop top
            //
            // Cave reproduces pre-loop + loop with r11d = 65536 bound.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x1E0205);
                static const std::uint8_t verify_Q7[] = { 0x48, 0x83, 0x78, 0x10, 0x00 };
                constexpr std::uint32_t patchSize = 21;

                logger::info("PatchHashChain-Q7: bytes at +0x1E0205: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4]);

                if (std::memcmp(site, verify_Q7, 5) != 0) {
                    logger::error("PatchHashChain-Q7: unexpected bytes — NOT patching");
                } else {
                    auto returnNullAddr = static_cast<std::uint64_t>(base + 0x1E0220);
                    auto foundAddr      = static_cast<std::uint64_t>(base + 0x1E021A);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(128));
                    int off = 0;

                    // Pre-loop: cmp qword [rax+0x10], 0  (48 83 78 10 00)
                    cave[off++] = 0x48; cave[off++] = 0x83;
                    cave[off++] = 0x78; cave[off++] = 0x10; cave[off++] = 0x00;
                    // je return_null (short, fixup later)
                    cave[off++] = 0x74;
                    int jeNullFixup1 = off;
                    cave[off++] = 0x00;

                    // mov r11d, 0x10000  (iteration bound)
                    cave[off++] = 0x41; cave[off++] = 0xBB;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x01; cave[off++] = 0x00;

                    // Loop body:
                    int loopTop = off;
                    // cmp [rax], edi  (39 38)
                    cave[off++] = 0x39; cave[off++] = 0x38;
                    // je found (short, fixup later)
                    cave[off++] = 0x74;
                    int jeFoundFixup = off;
                    cave[off++] = 0x00;
                    // mov rax, [rax+0x10]  (48 8B 40 10)
                    cave[off++] = 0x48; cave[off++] = 0x8B;
                    cave[off++] = 0x40; cave[off++] = 0x10;
                    // cmp rax, [rbx+0x18]  (48 3B 43 18)
                    cave[off++] = 0x48; cave[off++] = 0x3B;
                    cave[off++] = 0x43; cave[off++] = 0x18;
                    // je return_null (short, fixup later)
                    cave[off++] = 0x74;
                    int jeNullFixup2 = off;
                    cave[off++] = 0x00;
                    // dec r11d  (41 FF CB)
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCB;
                    // jnz loop_top
                    cave[off++] = 0x75;
                    int loopBackOff = off;
                    cave[off++] = 0x00;
                    cave[loopBackOff] = static_cast<std::uint8_t>(
                        static_cast<std::int8_t>(loopTop - (loopBackOff + 1)));

                    // return_null label:
                    int returnNullStart = off;
                    cave[jeNullFixup1] = static_cast<std::uint8_t>(returnNullStart - (jeNullFixup1 + 1));
                    cave[jeNullFixup2] = static_cast<std::uint8_t>(returnNullStart - (jeNullFixup2 + 1));
                    // jmp [rip+0] → returnNullAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &returnNullAddr, 8); off += 8;

                    // found label:
                    int foundStart = off;
                    cave[jeFoundFixup] = static_cast<std::uint8_t>(foundStart - (jeFoundFixup + 1));
                    // jmp [rip+0] → foundAddr
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &foundAddr, 8); off += 8;

                    logger::info("PatchHashChain-Q7: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    // Patch original site
                    DWORD oldProt;
                    VirtualProtect(site, patchSize, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom   = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHashChain-Q7: JMP too far ({}) — NOT patching", dist);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        for (std::uint32_t i = 5; i < patchSize; i++) site[i] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, patchSize);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(returnNullAddr),
                            "Q7", site, patchSize);
                        logger::info("PatchHashChain-Q7: +0x1E0205 patched — bounded form-map find (max 65536)");
                    }
                }
            }

            // ── Patch Q8: Bounded find loop at +0x30AAB2 ─────────────────
            // BSTHashMap::find with sentinel at [rbx+0x118] (different
            // layout from Q1-Q7). Second-highest fault volume during
            // New Game — hundreds of VEH dispatches per load cycle.
            //
            // +0x30AAB2: cmp qword [rdx+rax*8+0x10], 0  ; bucket empty?
            // +0x30AAB8: lea rcx, [rdx+rax*8]            ; rcx = bucket
            // +0x30AABC: je +0x30AADC                    ; not found
            // +0x30AABE: nop (2 bytes)
            // +0x30AAC0: cmp [rcx], edi                  ; key match?
            // +0x30AAC2: je +0x30AAD3                    ; found
            // +0x30AAC4: mov rcx, [rcx+0x10]             ; next
            // +0x30AAC8: cmp rcx, [rbx+0x118]            ; sentinel
            // +0x30AACF: jne +0x30AAC0                   ; loop
            // +0x30AAD1: jmp +0x30AADC                   ; not found
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x30AAB2);
                static const std::uint8_t verify_Q8[] = { 0x48, 0x83, 0x7C, 0xC2, 0x10, 0x00 };
                constexpr std::uint32_t patchSize = 33;

                logger::info("PatchHashChain-Q8: bytes at +0x30AAB2: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                if (std::memcmp(site, verify_Q8, 6) != 0) {
                    logger::error("PatchHashChain-Q8: unexpected bytes — NOT patching");
                } else {
                    auto returnNullAddr = static_cast<std::uint64_t>(base + 0x30AADC);
                    auto foundAddr      = static_cast<std::uint64_t>(base + 0x30AAD3);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(128));
                    int off = 0;

                    // Pre-loop: cmp qword [rdx+rax*8+0x10], 0  (48 83 7C C2 10 00)
                    cave[off++] = 0x48; cave[off++] = 0x83;
                    cave[off++] = 0x7C; cave[off++] = 0xC2;
                    cave[off++] = 0x10; cave[off++] = 0x00;
                    // lea rcx, [rdx+rax*8]  (48 8D 0C C2)
                    cave[off++] = 0x48; cave[off++] = 0x8D;
                    cave[off++] = 0x0C; cave[off++] = 0xC2;
                    // je return_null
                    cave[off++] = 0x74;
                    int jeNullFixup1 = off;
                    cave[off++] = 0x00;

                    // mov r11d, 0x10000
                    cave[off++] = 0x41; cave[off++] = 0xBB;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x01; cave[off++] = 0x00;

                    int loopTop = off;
                    // cmp [rcx], edi  (39 39)
                    cave[off++] = 0x39; cave[off++] = 0x39;
                    // je found
                    cave[off++] = 0x74;
                    int jeFoundFixup = off;
                    cave[off++] = 0x00;
                    // mov rcx, [rcx+0x10]  (48 8B 49 10)
                    cave[off++] = 0x48; cave[off++] = 0x8B;
                    cave[off++] = 0x49; cave[off++] = 0x10;
                    // cmp rcx, [rbx+0x118]  (48 3B 8B 18 01 00 00)
                    cave[off++] = 0x48; cave[off++] = 0x3B;
                    cave[off++] = 0x8B; cave[off++] = 0x18;
                    cave[off++] = 0x01; cave[off++] = 0x00; cave[off++] = 0x00;
                    // je return_null
                    cave[off++] = 0x74;
                    int jeNullFixup2 = off;
                    cave[off++] = 0x00;
                    // dec r11d
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCB;
                    // jnz loop_top
                    cave[off++] = 0x75;
                    int loopBackOff = off;
                    cave[off++] = 0x00;
                    cave[loopBackOff] = static_cast<std::uint8_t>(
                        static_cast<std::int8_t>(loopTop - (loopBackOff + 1)));

                    int returnNullStart = off;
                    cave[jeNullFixup1] = static_cast<std::uint8_t>(returnNullStart - (jeNullFixup1 + 1));
                    cave[jeNullFixup2] = static_cast<std::uint8_t>(returnNullStart - (jeNullFixup2 + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &returnNullAddr, 8); off += 8;

                    int foundStart = off;
                    cave[jeFoundFixup] = static_cast<std::uint8_t>(foundStart - (jeFoundFixup + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &foundAddr, 8); off += 8;

                    logger::info("PatchHashChain-Q8: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    DWORD oldProt;
                    VirtualProtect(site, patchSize, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom   = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchHashChain-Q8: JMP too far ({}) — NOT patching", dist);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        for (std::uint32_t i = 5; i < patchSize; i++) site[i] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, patchSize);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(returnNullAddr),
                            "Q8", site, patchSize);
                        logger::info("PatchHashChain-Q8: +0x30AAB2 patched — bounded find (max 65536, sentinel [rbx+0x118])");
                    }
                }
            }

            // ── Patch R1: +0x2F9440 — canonical guard on form ptr deref ──
            // Small function loads form ptr from [rcx+0x40], null-checks it,
            // then dereferences [rax+0x1A].  Under Wine the form ptr can be
            // non-null garbage (redirected through g_zeroPage).  Replace the
            // entire 21-byte function with a cave that adds canonical
            // validation before the type-check dereference.
            //
            // +0x2F9440: mov rax, [rcx+0x40]
            // +0x2F9444: test rax, rax
            // +0x2F9447: je +0x2F9452
            // +0x2F9449: cmp byte [rax+0x1A], 0x28  ← FAULT
            // +0x2F944D: jne +0x2F9452
            // +0x2F944F: mov al, 1 / ret
            // +0x2F9452: xor al, al / ret
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2F9440);
                static const std::uint8_t verify_R1[] = { 0x48, 0x8B, 0x41, 0x40 };
                constexpr std::uint32_t patchSize = 21;

                logger::info("PatchR1: bytes at +0x2F9440: "
                    "{:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3]);

                if (std::memcmp(site, verify_R1, 4) != 0) {
                    logger::error("PatchR1: unexpected bytes — NOT patching");
                } else {
                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(64));
                    int off = 0;

                    // mov rax, [rcx+0x40]  (48 8B 41 40)
                    cave[off++] = 0x48; cave[off++] = 0x8B;
                    cave[off++] = 0x41; cave[off++] = 0x40;
                    // test rax, rax  (48 85 C0)
                    cave[off++] = 0x48; cave[off++] = 0x85; cave[off++] = 0xC0;
                    // je .false
                    cave[off++] = 0x74;
                    int jeFalse1 = off;
                    cave[off++] = 0x00;
                    // Canonical check: high dword in [1, 0x7F]
                    // mov r10, rax  (49 89 C2)
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xC2;
                    // shr r10, 32  (49 C1 EA 20)
                    cave[off++] = 0x49; cave[off++] = 0xC1;
                    cave[off++] = 0xEA; cave[off++] = 0x20;
                    // dec r10d  (41 FF CA)
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA;
                    // cmp r10d, 0x7F  (41 83 FA 7F)
                    cave[off++] = 0x41; cave[off++] = 0x83;
                    cave[off++] = 0xFA; cave[off++] = 0x7F;
                    // ja .false
                    cave[off++] = 0x77;
                    int jeFalse2 = off;
                    cave[off++] = 0x00;
                    // cmp byte [rax+0x1A], 0x28  (80 78 1A 28)
                    cave[off++] = 0x80; cave[off++] = 0x78;
                    cave[off++] = 0x1A; cave[off++] = 0x28;
                    // jne .false
                    cave[off++] = 0x75;
                    int jeFalse3 = off;
                    cave[off++] = 0x00;
                    // mov al, 1  (B0 01)
                    cave[off++] = 0xB0; cave[off++] = 0x01;
                    // ret  (C3)
                    cave[off++] = 0xC3;
                    // .false:
                    int falseStart = off;
                    cave[jeFalse1] = static_cast<std::uint8_t>(falseStart - (jeFalse1 + 1));
                    cave[jeFalse2] = static_cast<std::uint8_t>(falseStart - (jeFalse2 + 1));
                    cave[jeFalse3] = static_cast<std::uint8_t>(falseStart - (jeFalse3 + 1));
                    // xor al, al  (30 C0)
                    cave[off++] = 0x30; cave[off++] = 0xC0;
                    // ret  (C3)
                    cave[off++] = 0xC3;

                    logger::info("PatchR1: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    DWORD oldProt;
                    VirtualProtect(site, patchSize, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom   = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchR1: JMP too far ({}) — NOT patching", dist);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        for (std::uint32_t i = 5; i < patchSize; i++) site[i] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, patchSize);
                        std::uint64_t falseAddr = static_cast<std::uint64_t>(base + 0x2F9452);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(falseAddr),
                            "R1", site, patchSize);
                        logger::info("PatchR1: +0x2F9440 patched — canonical guard on form type check");
                    }
                }
            }

            // ── Patch R2: +0x44150C — null guard on object→form deref ────
            // +0x44150C: mov rcx, [rbx]        ; load object ptr
            // +0x44150F: mov rax, [rcx+0x40]   ; load form ptr ← FAULT
            // Cave validates rcx before the second load; if invalid, skip
            // to +0x4415B0 (the existing bail-out block).
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x44150C);
                static const std::uint8_t verify_R2[] = { 0x48, 0x8B, 0x0B };
                constexpr std::uint32_t patchSize = 7;

                logger::info("PatchR2: bytes at +0x44150C: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5], site[6]);

                if (std::memcmp(site, verify_R2, 3) != 0) {
                    logger::error("PatchR2: unexpected bytes — NOT patching");
                } else {
                    auto continueAddr = static_cast<std::uint64_t>(base + 0x441513);
                    auto skipAddr     = static_cast<std::uint64_t>(base + 0x4415B0);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(64));
                    int off = 0;

                    // mov rcx, [rbx]  (48 8B 0B)
                    cave[off++] = 0x48; cave[off++] = 0x8B; cave[off++] = 0x0B;
                    // test rcx, rcx  (48 85 C9)
                    cave[off++] = 0x48; cave[off++] = 0x85; cave[off++] = 0xC9;
                    // je .skip
                    cave[off++] = 0x74;
                    int jeSkip1 = off;
                    cave[off++] = 0x00;
                    // Canonical: mov r10, rcx / shr r10, 32 / dec r10d / cmp r10d, 0x7F / ja .skip
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xCA; // mov r10, rcx
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20; // shr r10, 32
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA; // dec r10d
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F; // cmp r10d, 0x7F
                    cave[off++] = 0x77; // ja .skip
                    int jaSkip2 = off;
                    cave[off++] = 0x00;
                    // mov rax, [rcx+0x40]  (48 8B 41 40)
                    cave[off++] = 0x48; cave[off++] = 0x8B;
                    cave[off++] = 0x41; cave[off++] = 0x40;
                    // jmp [rip+0] → continue
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    int skipStart = off;
                    cave[jeSkip1] = static_cast<std::uint8_t>(skipStart - (jeSkip1 + 1));
                    cave[jaSkip2] = static_cast<std::uint8_t>(skipStart - (jaSkip2 + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &skipAddr, 8); off += 8;

                    logger::info("PatchR2: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    DWORD oldProt;
                    VirtualProtect(site, patchSize, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom   = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchR2: JMP too far ({}) — NOT patching", dist);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        for (std::uint32_t i = 5; i < patchSize; i++) site[i] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, patchSize);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(skipAddr),
                            "R2", site, patchSize);
                        logger::info("PatchR2: +0x44150C patched — canonical guard on [rcx+0x40]");
                    }
                }
            }

            // ── Patch R3: +0x49CAED — canonical guard on refcount inc ────
            // +0x49CAED: test rax, rax
            // +0x49CAF0: je +0x49CAFA
            // +0x49CAF2: lock inc [rax+0x28]  ← FAULT (rax non-null garbage)
            // The existing null check passes but the pointer is invalid
            // (Wine/g_zeroPage redirect artifact). Add canonical validation.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x49CAED);
                static const std::uint8_t verify_R3[] = { 0x48, 0x85, 0xC0 };
                constexpr std::uint32_t patchSize = 9;

                logger::info("PatchR3: bytes at +0x49CAED: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4]);

                if (std::memcmp(site, verify_R3, 3) != 0) {
                    logger::error("PatchR3: unexpected bytes — NOT patching");
                } else {
                    auto continueAddr = static_cast<std::uint64_t>(base + 0x49CAF6);
                    auto skipAddr     = static_cast<std::uint64_t>(base + 0x49CAFA);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(64));
                    int off = 0;

                    // test rax, rax  (48 85 C0)
                    cave[off++] = 0x48; cave[off++] = 0x85; cave[off++] = 0xC0;
                    // je .skip
                    cave[off++] = 0x74;
                    int jeSkip1 = off;
                    cave[off++] = 0x00;
                    // Canonical: mov r10, rax / shr r10, 32 / dec r10d / cmp r10d, 0x7F / ja .skip
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xC2;
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20;
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA;
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F;
                    cave[off++] = 0x77;
                    int jaSkip2 = off;
                    cave[off++] = 0x00;
                    // lock inc dword [rax+0x28]  (F0 FF 40 28)
                    cave[off++] = 0xF0; cave[off++] = 0xFF;
                    cave[off++] = 0x40; cave[off++] = 0x28;
                    // jmp [rip+0] → continue (after lock inc)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    int skipStart = off;
                    cave[jeSkip1] = static_cast<std::uint8_t>(skipStart - (jeSkip1 + 1));
                    cave[jaSkip2] = static_cast<std::uint8_t>(skipStart - (jaSkip2 + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &skipAddr, 8); off += 8;

                    logger::info("PatchR3: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    DWORD oldProt;
                    VirtualProtect(site, patchSize, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom   = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchR3: JMP too far ({}) — NOT patching", dist);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        for (std::uint32_t i = 5; i < patchSize; i++) site[i] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, patchSize);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(skipAddr),
                            "R3", site, patchSize);
                        logger::info("PatchR3: +0x49CAED patched — canonical guard on refcount inc");
                    }
                }
            }

            // ── Patch R4: +0x49CB2B — canonical guard on refcount dec ────
            // +0x49CB2B: lea rcx, [rax+0x20]
            // +0x49CB2F: mov ebx, 0xFFFFFFFF
            // +0x49CB34: lock xadd [rcx+8], ebx  ← FAULT (rax garbage)
            // Skip entire refcount decrement if rax is invalid.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x49CB2B);
                static const std::uint8_t verify_R4[] = { 0x48, 0x8D, 0x48, 0x20 };
                constexpr std::uint32_t patchSize = 14;

                logger::info("PatchR4: bytes at +0x49CB2B: "
                    "{:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3]);

                if (std::memcmp(site, verify_R4, 4) != 0) {
                    logger::error("PatchR4: unexpected bytes — NOT patching");
                } else {
                    auto continueAddr = static_cast<std::uint64_t>(base + 0x49CB39);
                    auto skipAddr     = static_cast<std::uint64_t>(base + 0x49CB4A);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(80));
                    int off = 0;

                    // test rax, rax  (48 85 C0)
                    cave[off++] = 0x48; cave[off++] = 0x85; cave[off++] = 0xC0;
                    // je .skip
                    cave[off++] = 0x74;
                    int jeSkip1 = off;
                    cave[off++] = 0x00;
                    // Canonical check
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xC2; // mov r10, rax
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20; // shr r10, 32
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA; // dec r10d
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F; // cmp r10d, 0x7F
                    cave[off++] = 0x77; // ja .skip
                    int jaSkip2 = off;
                    cave[off++] = 0x00;
                    // lea rcx, [rax+0x20]  (48 8D 48 20)
                    cave[off++] = 0x48; cave[off++] = 0x8D;
                    cave[off++] = 0x48; cave[off++] = 0x20;
                    // mov ebx, 0xFFFFFFFF  (BB FF FF FF FF)
                    cave[off++] = 0xBB; cave[off++] = 0xFF;
                    cave[off++] = 0xFF; cave[off++] = 0xFF; cave[off++] = 0xFF;
                    // lock xadd [rcx+8], ebx  (F0 0F C1 59 08)
                    cave[off++] = 0xF0; cave[off++] = 0x0F;
                    cave[off++] = 0xC1; cave[off++] = 0x59; cave[off++] = 0x08;
                    // jmp [rip+0] → continue
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    int skipStart = off;
                    cave[jeSkip1] = static_cast<std::uint8_t>(skipStart - (jeSkip1 + 1));
                    cave[jaSkip2] = static_cast<std::uint8_t>(skipStart - (jaSkip2 + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &skipAddr, 8); off += 8;

                    logger::info("PatchR4: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    DWORD oldProt;
                    VirtualProtect(site, patchSize, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom   = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchR4: JMP too far ({}) — NOT patching", dist);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        for (std::uint32_t i = 5; i < patchSize; i++) site[i] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, patchSize);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(skipAddr),
                            "R4", site, patchSize);
                        logger::info("PatchR4: +0x49CB2B patched — canonical guard on refcount dec");
                    }
                }
            }

            // ── Patch R5: +0x2EFAC9 — canonical guard on flags deref ─────
            // +0x2EFAC9: test rcx, rcx
            // +0x2EFACC: je +0x2EFAE8
            // +0x2EFACE: test dword [rcx+0x28], 0x3FF  ← FAULT
            // Existing null check passes but rcx is non-null garbage.
            // Cave adds canonical validation, then executes the test dword
            // and jumps back to the jne at +0x2EFAD5 (flags preserved).
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2EFAC9);
                static const std::uint8_t verify_R5[] = { 0x48, 0x85, 0xC9 };
                constexpr std::uint32_t patchSize = 12;

                logger::info("PatchR5: bytes at +0x2EFAC9: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4]);

                if (std::memcmp(site, verify_R5, 3) != 0) {
                    logger::error("PatchR5: unexpected bytes — NOT patching");
                } else {
                    auto continueAddr = static_cast<std::uint64_t>(base + 0x2EFAD5);
                    auto skipAddr     = static_cast<std::uint64_t>(base + 0x2EFAE8);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(80));
                    int off = 0;

                    // test rcx, rcx  (48 85 C9)
                    cave[off++] = 0x48; cave[off++] = 0x85; cave[off++] = 0xC9;
                    // je .skip
                    cave[off++] = 0x74;
                    int jeSkip1 = off;
                    cave[off++] = 0x00;
                    // Canonical check on rcx
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xCA; // mov r10, rcx
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20; // shr r10, 32
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA; // dec r10d
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F; // cmp r10d, 0x7F
                    cave[off++] = 0x77; // ja .skip
                    int jaSkip2 = off;
                    cave[off++] = 0x00;
                    // test dword [rcx+0x28], 0x3FF  (F7 41 28 FF 03 00 00)
                    cave[off++] = 0xF7; cave[off++] = 0x41;
                    cave[off++] = 0x28; cave[off++] = 0xFF;
                    cave[off++] = 0x03; cave[off++] = 0x00; cave[off++] = 0x00;
                    // jmp [rip+0] → +0x2EFAD5 (jne uses FLAGS from test above)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    int skipStart = off;
                    cave[jeSkip1] = static_cast<std::uint8_t>(skipStart - (jeSkip1 + 1));
                    cave[jaSkip2] = static_cast<std::uint8_t>(skipStart - (jaSkip2 + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &skipAddr, 8); off += 8;

                    logger::info("PatchR5: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    DWORD oldProt;
                    VirtualProtect(site, patchSize, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom   = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchR5: JMP too far ({}) — NOT patching", dist);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        for (std::uint32_t i = 5; i < patchSize; i++) site[i] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, patchSize);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(skipAddr),
                            "R5", site, patchSize);
                        logger::info("PatchR5: +0x2EFAC9 patched — canonical guard on flags deref");
                    }
                }
            }

            // ── Patch R6: +0x2D0C33 — canonical guard on struct field sub ─
            // +0x2D0C33: mov eax, [rsi+0x94]
            // +0x2D0C39: sub eax, [rsi+0x98]  ← FAULT
            // RSI is a struct pointer that can be garbage under Wine.
            // Cave validates rsi, executes both loads, then jumps to the
            // cmp at +0x2D0C3F. On invalid rsi, skips to +0x2D0C53.
            {
                auto* site = reinterpret_cast<std::uint8_t*>(base + 0x2D0C33);
                static const std::uint8_t verify_R6[] = { 0x8B, 0x86, 0x94, 0x00, 0x00, 0x00 };
                constexpr std::uint32_t patchSize = 12;

                logger::info("PatchR6: bytes at +0x2D0C33: "
                    "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                    site[0], site[1], site[2], site[3], site[4], site[5]);

                if (std::memcmp(site, verify_R6, 6) != 0) {
                    logger::error("PatchR6: unexpected bytes — NOT patching");
                } else {
                    auto continueAddr = static_cast<std::uint64_t>(base + 0x2D0C3F);
                    auto skipAddr     = static_cast<std::uint64_t>(base + 0x2D0C53);

                    auto* cave = static_cast<std::uint8_t*>(trampoline.allocate(80));
                    int off = 0;

                    // test rsi, rsi  (48 85 F6)
                    cave[off++] = 0x48; cave[off++] = 0x85; cave[off++] = 0xF6;
                    // je .skip
                    cave[off++] = 0x74;
                    int jeSkip1 = off;
                    cave[off++] = 0x00;
                    // Canonical check on rsi
                    cave[off++] = 0x49; cave[off++] = 0x89; cave[off++] = 0xF2; // mov r10, rsi
                    cave[off++] = 0x49; cave[off++] = 0xC1; cave[off++] = 0xEA; cave[off++] = 0x20; // shr r10, 32
                    cave[off++] = 0x41; cave[off++] = 0xFF; cave[off++] = 0xCA; // dec r10d
                    cave[off++] = 0x41; cave[off++] = 0x83; cave[off++] = 0xFA; cave[off++] = 0x7F; // cmp r10d, 0x7F
                    cave[off++] = 0x77; // ja .skip
                    int jaSkip2 = off;
                    cave[off++] = 0x00;
                    // mov eax, [rsi+0x94]  (8B 86 94 00 00 00)
                    cave[off++] = 0x8B; cave[off++] = 0x86;
                    cave[off++] = 0x94; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    // sub eax, [rsi+0x98]  (2B 86 98 00 00 00)
                    cave[off++] = 0x2B; cave[off++] = 0x86;
                    cave[off++] = 0x98; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    // jmp [rip+0] → +0x2D0C3F (continue with cmp eax, 1)
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &continueAddr, 8); off += 8;

                    int skipStart = off;
                    cave[jeSkip1] = static_cast<std::uint8_t>(skipStart - (jeSkip1 + 1));
                    cave[jaSkip2] = static_cast<std::uint8_t>(skipStart - (jaSkip2 + 1));
                    cave[off++] = 0xFF; cave[off++] = 0x25;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    cave[off++] = 0x00; cave[off++] = 0x00;
                    std::memcpy(&cave[off], &skipAddr, 8); off += 8;

                    logger::info("PatchR6: cave {} bytes at 0x{:X}", off,
                        reinterpret_cast<std::uintptr_t>(cave));

                    DWORD oldProt;
                    VirtualProtect(site, patchSize, PAGE_EXECUTE_READWRITE, &oldProt);
                    auto jmpTarget = reinterpret_cast<std::intptr_t>(cave);
                    auto jmpFrom   = reinterpret_cast<std::intptr_t>(site + 5);
                    auto dist = jmpTarget - jmpFrom;
                    if (dist > INT32_MAX || dist < INT32_MIN) {
                        logger::error("PatchR6: JMP too far ({}) — NOT patching", dist);
                    } else {
                        auto rel32 = static_cast<std::int32_t>(dist);
                        site[0] = 0xE9;
                        std::memcpy(&site[1], &rel32, 4);
                        for (std::uint32_t i = 5; i < patchSize; i++) site[i] = 0x90;
                        FlushInstructionCache(GetCurrentProcess(), site, patchSize);
                        RegisterCavePatch(cave, off, static_cast<std::uintptr_t>(skipAddr),
                            "R6", site, patchSize);
                        logger::info("PatchR6: +0x2D0C33 patched — canonical guard on struct field sub");
                    }
                }
            }
#endif
        }

    } // namespace detail
} // namespace Patches::FormCaching
