#pragma once

namespace Patches::MaxStdIO
{
    // Saved function pointers for querying from other modules
    inline decltype(&_getmaxstdio) g_getmaxstdio = nullptr;
    inline decltype(&_setmaxstdio) g_setmaxstdio = nullptr;

    // Hook on _setmaxstdio to detect any attempt to lower the limit
    inline SafetyHookInline g_hk_setmaxstdio;
    inline int Hooked_setmaxstdio(int newmax)
    {
        auto oldmax = g_getmaxstdio ? g_getmaxstdio() : -1;
        if (newmax < 8192) {
            logger::warn("_setmaxstdio({}) intercepted (current={}), forcing 8192"sv, newmax, oldmax);
            return g_hk_setmaxstdio.call<int>(8192);
        }
        logger::info("_setmaxstdio({}) pass-through (current={})"sv, newmax, oldmax);
        return g_hk_setmaxstdio.call<int>(newmax);
    }

    inline void Install()
    {
        const auto handle = REX::W32::GetModuleHandleW(L"API-MS-WIN-CRT-STDIO-L1-1-0.DLL");
        const auto proc =
            handle ?
                reinterpret_cast<decltype(&_setmaxstdio)>(REX::W32::GetProcAddress(handle, "_setmaxstdio")) :
                nullptr;
        if (proc != nullptr) {
            g_setmaxstdio = proc;
            g_getmaxstdio = reinterpret_cast<decltype(&_getmaxstdio)>(REX::W32::GetProcAddress(handle, "_getmaxstdio"));
            const auto old = g_getmaxstdio ? g_getmaxstdio() : -1;
            auto       result = proc(8192);
            if (result != -1) {
                logger::info("set max stdio to {} from {}"sv, result, old);
            } else {
                result = proc(2048);
                if (result != -1) {
                    logger::info("set max stdio to {} from {} (fallback to 2048)"sv, result, old);
                }
            }

            // Hook _setmaxstdio to prevent the game from lowering the limit later
            auto procAddr = reinterpret_cast<void*>(proc);
            g_hk_setmaxstdio = safetyhook::create_inline(procAddr, Hooked_setmaxstdio);
            if (g_hk_setmaxstdio) {
                logger::info("installed _setmaxstdio hook to prevent limit reduction"sv);
            } else {
                logger::warn("failed to hook _setmaxstdio — game may reset limit to 512"sv);
            }
        } else {
            logger::error("failed to install MaxStdIO patch"sv);
        }
    }
}