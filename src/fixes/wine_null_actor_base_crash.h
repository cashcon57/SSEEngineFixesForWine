#pragma once

// Wine-specific fix: null parentCell crashes during actor initialization
//
// Under Wine/CrossOver/Proton, a race condition during game initialization
// causes the PlayerCharacter to be processed before being placed in a cell.
// The actor processing chain (68445→36644→36553→36630→37791→19507→...)
// dereferences many pointers derived from the actor. When PlayerCharacter
// has no parentCell, these cascade into null-pointer crashes at multiple
// points within function 37791 and its callees.
//
// Observed crash sites (all same root cause):
//   37791→19507: 14371+0x10 mov rax, [rcx+0x30]  (null TESActorBaseData)
//   37791→19507: 38464+0x67 mov rax, [rcx]        (null ActorValueOwner)
//   37791+0x8D:  mov ecx, [rax+0x38]              (null result in 37791)
//
// Fix: hook the CALL to 37791 from 36630, skip when PlayerCharacter has
// no parentCell. Also keep lower-level guards as safety nets.
//
// 36630+0xD7 is return from 37791, CALL at 36630+0xD2.
// 37791 returns void (caller reads a global after the return).

namespace Fixes::WineNullActorBaseCrash
{
    namespace detail
    {
        // Primary fix: hook call to 37791 from 36630.
        // Skip the entire actor processing function when PlayerCharacter
        // has no parentCell — prevents ALL crashes in 37791 and its callees.
        static inline REL::Relocation<void (*)(void*, void*, void*, void*)> _original37791;

        inline void Hook37791(void* a1, void* a2, void* a3, void* a4)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && !player->GetParentCell()) {
                logger::warn("Wine null parentcell fix: skipping 37791 "
                    "(PlayerCharacter has no cell)");
                return;
            }
            _original37791(a1, a2, a3, a4);
        }

        // Safety net #1: guard 19507 call within 37791
        static inline REL::Relocation<void (*)(void*, void*, void*, void*)> _original19507;

        inline void Hook19507(void* a1, void* a2, void* a3, void* a4)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && !player->GetParentCell()) {
                return;
            }
            _original19507(a1, a2, a3, a4);
        }

        // Safety net #2: guard 14371 call within 19507
        static inline REL::Relocation<std::uintptr_t (*)(void*)> _original14371;

        inline std::uintptr_t Hook14371(void* a_this)
        {
            if (reinterpret_cast<std::uintptr_t>(a_this) < 0x10000) {
                return 0;
            }
            return _original14371(a_this);
        }
    }

    inline void Install()
    {
#ifdef SKYRIM_AE
        // Primary: skip entire function 37791 when player has no cell
        // CALL at 36630+0xD2 (return at 36630+0xD7)
        {
            REL::Relocation target{ REL::ID(36630), 0xD2 };
            detail::_original37791 = target.write_call<5>(detail::Hook37791);
            logger::info("installed Wine null parentcell fix (skip 37791)"sv);
        }

        // Safety net: skip 19507 within 37791
        // CALL at 37791+0x20 (return at 37791+0x25)
        {
            REL::Relocation target{ REL::ID(37791), 0x20 };
            detail::_original19507 = target.write_call<5>(detail::Hook19507);
        }

        // Safety net: guard 14371 within 19507
        // CALL at 19507+0x59B (return at 19507+0x5A0)
        {
            REL::Relocation target{ REL::ID(19507), 0x59B };
            detail::_original14371 = target.write_call<5>(detail::Hook14371);
        }

        logger::info("installed Wine null actor base crash fix"sv);
#endif
    }
}
