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
// This fix hooks function 14371 and returns nullptr when the this pointer
// is invalid, preventing the crash. The caller (19507+0x5A0) checks the
// return value with `test al, al` and handles null gracefully.

namespace Fixes::WineNullActorBaseCrash
{
    namespace detail
    {
        inline SafetyHookInline g_hk{};

        // Hook for function 14371 (TESActorBaseData member function).
        // Captures all 4 register-passed params to preserve calling convention.
        inline std::uintptr_t HookedFunc(void* a_this, void* a2, void* a3, void* a4)
        {
            if (reinterpret_cast<std::uintptr_t>(a_this) < 0x10000) {
                logger::warn("Wine null actor base fix: blocked crash (this={:#x})",
                    reinterpret_cast<std::uintptr_t>(a_this));
                return 0;
            }
            return g_hk.call<std::uintptr_t>(a_this, a2, a3, a4);
        }
    }

    inline void Install()
    {
#ifdef SKYRIM_AE
        const REL::Relocation target{ REL::ID(14371) };
        detail::g_hk = safetyhook::create_inline(target.address(), detail::HookedFunc);
        logger::info("installed Wine null actor base crash fix"sv);
#endif
    }
}
