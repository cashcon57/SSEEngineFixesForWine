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
// memory manager overrides are removed entirely (Wine-incompatible).
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
            // Editor ID cache: DON'T populate at kDataLoaded.
            // On AE, the editor ID map is empty at this point — po3_Tweaks
            // populates it during its own kDataLoaded handler (which fires after
            // ours due to alphabetical DLL ordering: 0_ < p). We populate our
            // cache at kPostLoadGame/kNewGame instead (fires after ALL data is loaded).
            logger::info("editor ID cache: deferring to kPostLoadGame (editor ID map empty at kDataLoaded on AE)"sv);

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
    case SKSE::MessagingInterface::kNewGame:
        {
            // Primary: populate editor ID cache AFTER all kDataLoaded handlers
            // (including po3_Tweaks which loads editor IDs onto forms).
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
    trampoline.create(1 << 11);

    Settings::Load();

    if (Settings::General::bVerboseLogging.GetValue()) {
        spdlog::set_level(spdlog::level::trace);
        spdlog::flush_on(spdlog::level::trace);
        logger::trace("enabled verbose logging"sv);
    }

    // Install all patches and fixes during SKSE load (not preload).
    // This is the key difference from Engine Fixes — Wine/CrossOver/Proton
    // crash when hooks are installed during the d3dx9_42.dll preload phase.
    // Memory manager overrides are intentionally omitted (Wine-incompatible).
    Patches::Install();
    Fixes::Install();

    const auto messaging = SKSE::GetMessagingInterface();
    if (!messaging->RegisterListener("SKSE", MessageHandler)) {
        logger::error("Failed to register messaging interface listener"sv);
        return false;
    }

    logger::info("SSE Engine Fixes for Wine loaded successfully"sv);

    return true;
}
