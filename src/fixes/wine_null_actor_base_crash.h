#pragma once

// Wine-specific fix: null parentCell crashes during actor initialization
//
// Under Wine/CrossOver/Proton, a race condition during game initialization
// causes the PlayerCharacter to be processed before being placed in a cell.
// Function 19507 processes actor data and makes many calls that dereference
// pointers derived from the actor. When PlayerCharacter has no parentCell,
// these derived pointers are invalid, causing cascading crashes:
//
//   - 14371+0x10: mov rax, [rcx+0x30]  (null TESActorBaseData, RCX=0x30)
//   - 38464+0x67: mov rax, [rcx]       (null ActorValueOwner, RCX=0xE8)
//   - Potentially many more within the ~0x600-byte function 19507
//
// Rather than patching each crash site individually (whack-a-mole), this
// fix hooks the CALL to 19507 from its caller (37791) and skips the entire
// call when PlayerCharacter's parentCell is null. This prevents all
// downstream crashes from this race condition.
//
// Call chain: 68445 → 36644 → 36553 → 36630 → 37791 → 19507 → crashes
// Return at 37791+0x25 (`mov rax, [rbx]`), CALL at 37791+0x20.
// 19507 returns void (caller doesn't test a return value).

namespace Fixes::WineNullActorBaseCrash
{
    namespace detail
    {
        // Hook at 37791+0x20: call to function 19507 (actor data processing).
        // Captures 4 register params to preserve any calling convention.
        // 19507 returns void — caller's next instruction is `mov rax, [rbx]`.
        static inline REL::Relocation<void (*)(void*, void*, void*, void*)> _original19507;

        inline void Hook19507(void* a1, void* a2, void* a3, void* a4)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && !player->GetParentCell()) {
                logger::warn("Wine null parentcell fix: skipping actor processing "
                    "(PlayerCharacter has no cell)");
                return;
            }
            _original19507(a1, a2, a3, a4);
        }

        // Safety net: also guard the specific call to 14371 within 19507,
        // in case other code paths reach it outside the 37791 chain.
        static inline REL::Relocation<std::uintptr_t (*)(void*)> _original14371;

        inline std::uintptr_t Hook14371(void* a_this)
        {
            if (reinterpret_cast<std::uintptr_t>(a_this) < 0x10000) {
                logger::warn("Wine null actor base fix: blocked crash at 14371 (this={:#x})",
                    reinterpret_cast<std::uintptr_t>(a_this));
                return 0;
            }
            return _original14371(a_this);
        }
    }

    inline void Install()
    {
#ifdef SKYRIM_AE
        // Primary fix: skip function 19507 entirely when PlayerCharacter
        // has no parentCell. This prevents ALL downstream null-pointer
        // crashes from the Wine cell-loading race condition.
        {
            REL::Relocation target{ REL::ID(37791), 0x20 };
            detail::_original19507 = target.write_call<5>(detail::Hook19507);
            logger::info("installed Wine null parentcell fix (skip 19507)"sv);
        }

        // Safety net: guard 14371 call site for other code paths
        {
            REL::Relocation target{ REL::ID(19507), 0x59B };
            detail::_original14371 = target.write_call<5>(detail::Hook14371);
            logger::info("installed Wine null actor base fix (guard 14371)"sv);
        }

        logger::info("installed Wine null actor base crash fix"sv);
#endif
    }
}
