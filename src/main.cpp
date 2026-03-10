// SSE Engine Fixes for Wine — Wine/Proton/CrossOver-compatible fork of SSE Engine Fixes
//
// All credit for the original reverse engineering and bug fixes goes to
// aers and contributors of SSE Engine Fixes:
//   https://github.com/aers/EngineFixesSkyrim64
//   https://www.nexusmods.com/skyrimspecialedition/mods/17230
//
// This fork removes the d3dx9_42.dll preloader requirement and installs
// all hooks during normal SKSE plugin load instead. The TBB dependency
// is replaced with standard C++ synchronization primitives, and the
// memory allocator uses HeapAlloc/HeapFree (Wine-compatible) instead of TBB.
//
// License: MIT (same as upstream SSE Engine Fixes)

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/spdlog.h>

#include "clean_cosaves.h"
#include "fixes/bslightingshader_parallax_bug.h"
#include "fixes/fixes.h"
#include "fixes/save_screenshots.h"
#include "fixes/tree_reflections.h"
#include "patches/editor_id_cache.h"
#include "patches/form_caching.h"
#include "patches/memory_manager.h"
#include "patches/patches.h"
#include "patches/save_added_sound_categories.h"
#include "settings.h"
#include "warnings/warnings.h"

std::chrono::high_resolution_clock::time_point start;

void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
    switch (a_msg->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        {
            // v1.22.0: Stop loading monitor and log final pipeline state
            Patches::EditorIdCache::LoadingMonitor::Stop();
            Patches::EditorIdCache::LoadingMonitor::LogFinalState();

            // Log memory allocator stats before other kDataLoaded work
            if (Settings::Memory::bReplaceAllocator.GetValue())
                Patches::WineMemoryManager::LogStats();

            // v1.22.22: Force form loading if the engine skipped it.
            // Under Wine with 600+ plugins, CompileFiles is skipped, which
            // causes the entire form loading pipeline to be skipped too.
            // ManuallyCompileFiles (CloseTES hook) populates compiledFileCollection,
            // but the engine's code path already branched past form loading.
            // We detect this by checking AddFormToDataHandler count: if zero,
            // we invoke the engine's form loading functions directly.
            {
                auto addFormCount = Patches::FormCaching::detail::g_addFormCalls.load(std::memory_order_relaxed);
                if (addFormCount == 0) {
                    logger::warn(">>> kDataLoaded: ZERO AddFormToDataHandler calls — form loading was SKIPPED <<<");
                    logger::warn(">>> Invoking ForceLoadAllForms to load forms from compiled files <<<");
                    if (Settings::Patches::bFormCaching.GetValue()) {
                        Patches::FormCaching::detail::ForceLoadAllForms();
                    }
                } else {
                    logger::info("kDataLoaded: {} AddFormToDataHandler calls — forms loaded normally", addFormCount);
                }
            }

            // Editor ID cache: populate NOW at kDataLoaded, BEFORE other plugins.
            // Our handler fires first (alphabetical: 0_ < C < p), so we MUST
            // populate before CoreImpactFramework (C) tries LookupByEditorID.
            // po3_Tweaks installed its "Load EditorIDs" hook at kPostLoad (before
            // ESM loading), so forms should have editor IDs by now.
            // We enumerate via TESDataHandler (BSTArray, no hashing — Wine-safe).
            if (Settings::Patches::bEditorIdCache.GetValue())
                Patches::EditorIdCache::OnDataLoaded();

            if (Settings::General::bCleanSKSECoSaves.GetValue())
                Util::CoSaves::Clean();
            if (Settings::Patches::bSaveAddedSoundCategories.GetValue())
                Patches::SaveAddedSoundCategories::LoadVolumes();
            // need ini settings
            if (Settings::Fixes::bSaveScreenshots.GetValue())
                Fixes::SaveScreenshots::Install();
            // need to make sure enb dll has loaded
            if (Settings::Fixes::bTreeReflections.GetValue())
                Fixes::TreeReflections::Install();
            // need to detect community shaders
            if (Settings::Fixes::bBSLightingShaderParallaxBug.GetValue())
                Fixes::BSLightingShaderParallaxBug::Install();
            if (Settings::Warnings::bRefHandleLimit.GetValue()) {
                Warnings::WarnActiveRefrHandleCount(Settings::Warnings::uRefrMainMenuLimit.GetValue());
            }

            auto timeElapsed = std::chrono::high_resolution_clock::now() - start;
            logger::info("time to main menu {}"sv, std::chrono::duration_cast<std::chrono::milliseconds>(timeElapsed).count());

            break;
        }
    case SKSE::MessagingInterface::kInputLoaded:
        {
            // kInputLoaded fires BEFORE kDataLoaded (confirmed by timestamps).
            // Cannot use it to defer work past kDataLoaded handlers.
            break;
        }
    case SKSE::MessagingInterface::kPostLoadGame:
        {
            logger::info(">>> kPostLoadGame fired <<<");
            // Fallback: retry editor ID cache if kDataLoaded found nothing.
            if (Settings::Patches::bEditorIdCache.GetValue())
                Patches::EditorIdCache::OnDataLoaded();

            if (Settings::Warnings::bRefHandleLimit.GetValue()) {
                Warnings::WarnActiveRefrHandleCount(Settings::Warnings::uRefrLoadedGameLimit.GetValue());
            }
            break;
        }
    case SKSE::MessagingInterface::kNewGame:
        {
            logger::info(">>> kNewGame fired — sentinel zpWritable={} zeroPageUse={} writeSkips={} catchAll={} formIdSkips={} <<<",
                Patches::FormCaching::detail::g_zpWritable.load(std::memory_order_relaxed),
                Patches::FormCaching::detail::g_zeroPageUseCount.load(std::memory_order_relaxed),
                Patches::FormCaching::detail::g_zeroPageWriteSkips.load(std::memory_order_relaxed),
                Patches::FormCaching::detail::g_catchAllCount.load(std::memory_order_relaxed),
                Patches::FormCaching::detail::g_formIdSkipCount.load(std::memory_order_relaxed));

            // Also write to crash log for guaranteed capture
            {
                FILE* f = nullptr;
                fopen_s(&f, Patches::FormCaching::detail::g_crashLogPath, "a");
                if (f) {
                    fprintf(f, "\n=== kNewGame (v1.22.80) === zpWritable=%d zpUse=%llu ws=%llu ca=%llu fi=%d cf=%llu er=%llu\n",
                        Patches::FormCaching::detail::g_zpWritable.load(std::memory_order_relaxed) ? 1 : 0,
                        Patches::FormCaching::detail::g_zeroPageUseCount.load(std::memory_order_relaxed),
                        Patches::FormCaching::detail::g_zeroPageWriteSkips.load(std::memory_order_relaxed),
                        Patches::FormCaching::detail::g_catchAllCount.load(std::memory_order_relaxed),
                        Patches::FormCaching::detail::g_formIdSkipCount.load(std::memory_order_relaxed),
                        (unsigned long long)Patches::FormCaching::detail::g_caveFaultCount.load(std::memory_order_relaxed),
                        (unsigned long long)Patches::FormCaching::detail::g_execRecoverCount.load(std::memory_order_relaxed));
                    fflush(f);
                    fclose(f);
                }
            }

            // v1.22.78: Install INT3 probes immediately at kNewGame.
            // The freeze happens within seconds of New Game — waiting for
            // watchdog freeze detection (30s) is too slow. By installing
            // probes now, the breakpoints are in place BEFORE the tight
            // loop starts, catching it on the very first iteration.
            if (!Patches::FormCaching::detail::g_probesInstalled.load(std::memory_order_acquire)) {
                auto imgBase = REL::Module::get().base();
                int installed = 0;
                for (int i = 0; i < Patches::FormCaching::detail::g_numProbes; ++i) {
                    auto* site = reinterpret_cast<std::uint8_t*>(
                        imgBase + Patches::FormCaching::detail::g_probes[i].offset);
                    DWORD oldProt;
                    if (VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
                        Patches::FormCaching::detail::g_probes[i].origByte = *site;
                        *site = 0xCC;  // INT3
                        FlushInstructionCache(GetCurrentProcess(), site, 1);
                        Patches::FormCaching::detail::g_probes[i].active.store(true, std::memory_order_release);
                        installed++;
                    }
                }
                Patches::FormCaching::detail::g_probesInstalled.store(true, std::memory_order_release);
                logger::info("kNewGame: installed {}/{} INT3 probes for freeze detection",
                    installed, Patches::FormCaching::detail::g_numProbes);
                {
                    FILE* f2 = nullptr;
                    fopen_s(&f2, Patches::FormCaching::detail::g_crashLogPath, "a");
                    if (f2) {
                        fprintf(f2, "PROBES: installed %d/%d INT3 probes at kNewGame\n",
                            installed, Patches::FormCaching::detail::g_numProbes);
                        fflush(f2);
                        fclose(f2);
                    }
                }
            }

            // Fallback: retry editor ID cache if kDataLoaded found nothing.
            if (Settings::Patches::bEditorIdCache.GetValue())
                Patches::EditorIdCache::OnDataLoaded();

            if (Settings::Warnings::bRefHandleLimit.GetValue()) {
                Warnings::WarnActiveRefrHandleCount(Settings::Warnings::uRefrLoadedGameLimit.GetValue());
            }
            break;
        }
    default:
        break;
    }
}

void OpenLog()
{
    auto path = SKSE::log::log_directory();

    if (!path)
        return;

    *path /= "SSEEngineFixesForWine.log";

    std::vector<spdlog::sink_ptr> sinks{
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true),
        std::make_shared<spdlog::sinks::msvc_sink_mt>()
    };

    auto logger = std::make_shared<spdlog::logger>("global", sinks.begin(), sinks.end());

#ifndef NDEBUG
    logger->set_level(spdlog::level::debug);
    logger->flush_on(spdlog::level::debug);
#else
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
#endif

    spdlog::set_default_logger(std::move(logger));
    spdlog::set_pattern("[%Y-%m-%d %T.%e][%-16s:%-4#][%L]: %v");
}

#ifdef SKYRIM_AE
extern "C" __declspec(dllexport) constinit auto SKSEPlugin_Version = []() {
    SKSE::PluginVersionData v;
    v.PluginVersion(Version::MAJOR);
    v.PluginName(Version::PROJECT);
    v.AuthorName("Corkscrew");
    v.UsesAddressLibrary();
    v.UsesUpdatedStructs();
    v.CompatibleVersions({ SKSE::RUNTIME_SSE_1_6_1170 });

    return v;
}();
#else
extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface*, SKSE::PluginInfo* a_info)
{
    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = Version::PROJECT.data();
    a_info->version = Version::MAJOR;

    return true;
}
#endif

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    start = std::chrono::high_resolution_clock::now();

    SKSE::Init(a_skse);
    OpenLog();

    logger::info("SSE Engine Fixes for Wine v{}.{}.{}"sv,
        Version::MAJOR, Version::MINOR, Version::PATCH);
    logger::info("Wine/Proton/CrossOver-compatible fork of SSE Engine Fixes by aers"sv);
    logger::info("Original: https://github.com/aers/EngineFixesSkyrim64"sv);

    const auto ver = REL::Module::get().version();
    if (ver < VAR_NUM(SKSE::RUNTIME_SSE_1_5_97, SKSE::RUNTIME_SSE_1_6_1170)) {
        logger::error("Unsupported runtime version {}"sv, ver);
        return false;
    }

    auto& trampoline = SKSE::GetTrampoline();
    trampoline.create(1 << 13);  // 8KB — needed for 13+ code caves + other patches

    Settings::Load();

    if (Settings::General::bVerboseLogging.GetValue()) {
        spdlog::set_level(spdlog::level::trace);
        spdlog::flush_on(spdlog::level::trace);
        logger::trace("enabled verbose logging"sv);
    }

    // Install Wine-compatible memory allocator FIRST — all subsequent allocations
    // (including from other patches and ESM loading) use our mimalloc-based allocator.
    // The original Engine Fixes uses Intel TBB which crashes under Wine; we replace
    // with mimalloc which uses per-thread caches and avoids global lock contention.
    Patches::WineMemoryManager::Install();

    // Install all patches and fixes during SKSE load (not preload).
    // This is the key difference from Engine Fixes — Wine/CrossOver/Proton
    // crash when hooks are installed during the d3dx9_42.dll preload phase.
    Patches::Install();
    Fixes::Install();

    // v1.22.3: Verify plugins.txt is readable from within the game process
    Patches::EditorIdCache::VerifyPluginsTxt();

    // v1.22.0: Start loading monitor — logs pipeline state every 2 seconds
    // (switches to 200ms after TDH appears) until kDataLoaded fires.
    Patches::EditorIdCache::LoadingMonitor::Start();

    const auto messaging = SKSE::GetMessagingInterface();
    if (!messaging->RegisterListener("SKSE", MessageHandler)) {
        logger::error("Failed to register messaging interface listener"sv);
        return false;
    }

    logger::info("SSE Engine Fixes for Wine loaded successfully"sv);

    return true;
}
