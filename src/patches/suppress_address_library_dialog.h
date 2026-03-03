#pragma once

#include <cstring>
#include <cwchar>

namespace Patches::SuppressAddressLibraryDialog
{
    // Saved hook objects — safetyhook manages trampoline and original pointer
    inline SafetyHookInline g_hk_MessageBoxA;
    inline SafetyHookInline g_hk_MessageBoxW;

    // Hook for MessageBoxA — intercepts the SKSE Address Library warning
    inline int WINAPI Hooked_MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType)
    {
        // The SKSE plugin loader shows a MessageBoxA/W with caption "SKSE Plugin Loader"
        // and body mentioning "Address Library" when plugins fail the Address Library
        // version check.  Under Wine/CrossOver on macOS the dialog appears behind the
        // fullscreen game window and is invisible; the game then waits forever for a
        // button click that can never come, permanently blocking the loading screen.
        // Auto-dismiss with IDNO (equivalent to "No" — don't open the browser).
        if (lpCaption && std::strstr(lpCaption, "SKSE") != nullptr &&
            lpText && std::strstr(lpText, "Address Library") != nullptr) {
            logger::info("Address Library warning dialog suppressed (auto-dismissed as No)"sv);
            return IDNO;
        }
        return g_hk_MessageBoxA.call<int>(hWnd, lpText, lpCaption, uType);
    }

    // Hook for MessageBoxW — same logic, wide-string variant
    inline int WINAPI Hooked_MessageBoxW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType)
    {
        if (lpCaption && std::wcsstr(lpCaption, L"SKSE") != nullptr &&
            lpText && std::wcsstr(lpText, L"Address Library") != nullptr) {
            logger::info("Address Library warning dialog suppressed (auto-dismissed as No, wide)"sv);
            return IDNO;
        }
        return g_hk_MessageBoxW.call<int>(hWnd, lpText, lpCaption, uType);
    }

    inline void Install()
    {
        // Hook MessageBoxA and MessageBoxW in user32.dll to suppress the SKSE
        // Address Library version warning.  This is a Wine/CrossOver-specific fix:
        // on native Windows the dialog is visible and clickable; under CrossOver it
        // hides behind the fullscreen window and cannot be dismissed.
        auto user32 = REX::W32::GetModuleHandleW(L"user32.dll");
        if (!user32) {
            logger::warn("user32.dll not found — Address Library dialog suppressor not installed"sv);
            return;
        }

        const auto mbA = reinterpret_cast<void*>(REX::W32::GetProcAddress(user32, "MessageBoxA"));
        if (mbA) {
            g_hk_MessageBoxA = safetyhook::create_inline(mbA, reinterpret_cast<void*>(Hooked_MessageBoxA));
            if (g_hk_MessageBoxA) {
                logger::info("installed MessageBoxA hook (Address Library dialog suppressor)"sv);
            } else {
                logger::warn("failed to hook MessageBoxA — Address Library dialog will still appear"sv);
            }
        }

        const auto mbW = reinterpret_cast<void*>(REX::W32::GetProcAddress(user32, "MessageBoxW"));
        if (mbW) {
            g_hk_MessageBoxW = safetyhook::create_inline(mbW, reinterpret_cast<void*>(Hooked_MessageBoxW));
            if (g_hk_MessageBoxW) {
                logger::info("installed MessageBoxW hook (Address Library dialog suppressor)"sv);
            } else {
                logger::warn("failed to hook MessageBoxW — Address Library dialog will still appear"sv);
            }
        }
    }
}
