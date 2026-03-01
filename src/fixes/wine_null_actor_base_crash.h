#pragma once

// Wine-specific fix: null TESActorBaseData crash during actor initialization
//
// Under Wine/CrossOver/Proton, a race condition during game initialization
// causes the PlayerCharacter's actor base data to be accessed before the
// character is fully placed in a cell. Function 14371 (a TESActorBaseData
// method) receives an invalid this pointer (0x30) — the result of casting
// a null TESNPC* to TESActorBaseData* (which sits at offset 0x30 in the
// class hierarchy). The function then tries to read baseTemplateForm at
// [this+0x30], accessing address 0x60 and crashing.
//
// Crash signature: SkyrimSE.exe+01D74A0 (14371+0x10) mov rax, [rcx+0x30]
// RCX=0x30, reading 0x60, PlayerCharacter ParentCell: None
//
// Call chain: 19507+0x59B calls 14371, return at 19507+0x5A0.
// The caller checks return with `test al, al` — returning 0 is safe.
//
// Strategy: patch the CALL site in function 19507 rather than hooking
// function 14371's entry (SafetyHookInline may not work under Wine).

namespace Fixes::WineNullActorBaseCrash
{
    namespace detail
    {
        // Original function 14371 — stored by write_call<5>
        static inline REL::Relocation<std::uintptr_t (*)(void*)> _original;

        inline std::uintptr_t HookedFunc(void* a_this)
        {
            if (reinterpret_cast<std::uintptr_t>(a_this) < 0x10000) {
                logger::warn("Wine null actor base fix: blocked crash (this={:#x})",
                    reinterpret_cast<std::uintptr_t>(a_this));
                return 0;
            }
            return _original(a_this);
        }
    }

    inline void Install()
    {
#ifdef SKYRIM_AE
        // Hook the CALL to function 14371 inside function 19507.
        // Return address is 19507+0x5A0, so the 5-byte CALL is at +0x59B.
        REL::Relocation target{ REL::ID(19507), 0x59B };
        detail::_original = target.write_call<5>(detail::HookedFunc);
        logger::info("installed Wine null actor base crash fix"sv);
#endif
    }
}
