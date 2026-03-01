#pragma once

// Wine-specific fix: null parentCell crashes during actor initialization
//
// Under Wine/CrossOver/Proton, a race condition during game initialization
// causes the PlayerCharacter to be processed before being placed in a cell.
// The actor processing chain (68445→36644→36553→36630→37791→19507→...)
// dereferences many pointers derived from the actor. When PlayerCharacter
// has no parentCell, these cascade into null-pointer crashes at multiple
// points within function 36630 and its callees (37791, 19507, 14371, etc.).
//
// Observed crash sites (all same root cause):
//   36630→41572→38464+0x67:  mov rax, [rcx]        (null ActorValueOwner)
//   36630→37791→19507: 14371+0x10 mov rax, [rcx+0x30]  (null TESActorBaseData)
//   36630→37791→19507: 38464+0x67 mov rax, [rcx]        (null ActorValueOwner)
//   36630→37791+0x8D:  mov ecx, [rax+0x38]              (null result in 37791)
//
// Fix: hook the CALL to 36630 from 36553, skip when PlayerCharacter has
// no parentCell. This prevents ALL crashes in 36630 and all its callees.
// Lower-level hooks are kept as safety nets.
//
// 36553+0x3F0 is return from 36630, CALL at 36553+0x3EB (5-byte E8).
// 36630+0xD7 is return from 37791, FF 15 indirect CALL at 36630+0xD1 (6 bytes).

namespace Fixes::WineNullActorBaseCrash
{
    namespace detail
    {
        // Primary fix: hook call to 36630 from 36553.
        // Skip the entire actor processing function when PlayerCharacter
        // has no parentCell — prevents ALL crashes in 36630 and its callees.
        static inline REL::Relocation<void (*)(void*, void*, void*, void*)> _original36630;

        inline void Hook36630(void* a1, void* a2, void* a3, void* a4)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && !player->GetParentCell()) {
                logger::warn("Wine null parentcell fix: skipping 36630 "
                    "(PlayerCharacter has no cell)");
                return;
            }
            _original36630(a1, a2, a3, a4);
        }

        // Safety net #1: hook call to 37791 from 36630.
        static inline REL::Relocation<void (*)(void*, void*, void*, void*)> _original37791;

        inline void Hook37791(void* a1, void* a2, void* a3, void* a4)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && !player->GetParentCell()) {
                return;
            }
            _original37791(a1, a2, a3, a4);
        }

        // Safety net #2: guard 19507 call within 37791
        static inline REL::Relocation<void (*)(void*, void*, void*, void*)> _original19507;

        inline void Hook19507(void* a1, void* a2, void* a3, void* a4)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && !player->GetParentCell()) {
                return;
            }
            _original19507(a1, a2, a3, a4);
        }

        // Safety net #3: guard 14371 call within 19507
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
        // Primary: skip entire function 36630 when player has no cell
        // E8 CALL at 36553+0x3EB (return at 36553+0x3F0)
        {
            REL::Relocation target{ REL::ID(36553), 0x3EB };
            detail::_original36630 = target.write_call<5>(detail::Hook36630);
            logger::info("installed Wine null parentcell fix (skip 36630)"sv);
        }

        // Safety: skip 37791 within 36630
        // FF 15 indirect CALL at 36630+0xD1 (6 bytes)
        {
            REL::Relocation target{ REL::ID(36630), 0xD1 };
            detail::_original37791 = target.write_call<6>(detail::Hook37791);
        }

        // Safety: skip 19507 within 37791
        // CALL at 37791+0x20 (return at 37791+0x25)
        {
            REL::Relocation target{ REL::ID(37791), 0x20 };
            detail::_original19507 = target.write_call<5>(detail::Hook19507);
        }

        // Safety: guard 14371 within 19507
        // CALL at 19507+0x59B (return at 19507+0x5A0)
        {
            REL::Relocation target{ REL::ID(19507), 0x59B };
            detail::_original14371 = target.write_call<5>(detail::Hook14371);
        }

        logger::info("installed Wine null actor base crash fix"sv);
#endif
    }
}
