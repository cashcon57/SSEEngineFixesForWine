#pragma once

// Wine-compatible form caching — replaces tbb::concurrent_hash_map with
// std::shared_mutex + std::unordered_map (256 shards by master index)
//
// Split into focused modules (v1.22.97):
//   form_caching_globals.h  — All shared state, helpers, VehLog, IsReadableMemory
//   form_caching_veh.h      — VEH handlers (ForceLoadVEH, CrashLoggerVEH, etc.)
//   form_caching_hooks.h    — SafetyHook wrappers (SetAt, grow, GetForm, etc.)
//   form_caching_patches.h  — Code cave patches (A-L, N-P, Q1-Q8, R1-R6)
//   form_caching_loading.h  — ForceLoadAllForms + watchdog thread
//
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

#include "form_caching_globals.h"
#include "form_caching_veh.h"
#include "form_caching_hooks.h"
#include "form_caching_patches.h"
#include "form_caching_loading.h"

namespace Patches::FormCaching
{
    inline void Install()
    {
        detail::InitPaths();
        detail::ReplaceFormMapFunctions();

        // v1.22.90: Hook grow_A and grow_B — passthrough + sentinel capture.
        // grow() steals entries (sets next = nullptr), but with NOP'd deallocate
        // the old array stays alive. The null page has sentinel at +0x10/+0x28,
        // so concurrent find() following a stolen null chain reads sentinel →
        // loop exits cleanly. The VEH also has chain-follow detection as backup.
        {
            auto imgBase = REL::Module::get().base();
            auto* growA = reinterpret_cast<void*>(imgBase + 0x198390);
            auto* growB = reinterpret_cast<void*>(imgBase + 0x198610);

            detail::g_hk_growA = safetyhook::create_inline(growA, detail::BSTHashMap_grow_A);
            detail::g_hk_growB = safetyhook::create_inline(growB, detail::BSTHashMap_grow_B);

            // NOP the deallocate (free) CALL instructions inside grow_A and grow_B.
            // This prevents the old bucket array from being freed while concurrent
            // find() threads may still be traversing its chains. The old entries
            // have next = nullptr (from steal), and the null page returns sentinel,
            // so find loops exit cleanly.
            //
            // grow_A: CALL at +0x1985F8 (offset 0x268 into function)
            // grow_B: CALL at +0x198878 (offset 0x268 into function)
            // Both call the same deallocate function (ScrapHeap::Free or CRT free).
            auto nopCall = [&](std::uintptr_t callRva, const char* name) {
                auto* site = reinterpret_cast<std::uint8_t*>(imgBase + callRva);
                if (*site == 0xE8) {  // Verify it's a CALL rel32
                    DWORD oldProt = 0;
                    if (VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
                        memset(site, 0x90, 5);  // NOP × 5
                        FlushInstructionCache(GetCurrentProcess(), site, 5);
                        logger::info("v1.22.90: NOP'd {} deallocate CALL at +0x{:X}", name, callRva);
                    }
                } else {
                    logger::warn("v1.22.90: {} at +0x{:X} is 0x{:02X}, not CALL (0xE8) — skipping NOP",
                        name, callRva, *site);
                }
            };
            nopCall(0x1985F8, "grow_A");
            nopCall(0x198878, "grow_B");

            logger::info("v1.22.90: BSTHashMap::grow hooks installed — "
                "passthrough + NOP-free + sentinel capture");
        }

        logger::info("installed form caching patch"sv);
    }
}
