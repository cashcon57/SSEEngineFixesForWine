#include "patches.h"

#include <cmath>

#include "disable_chargen_precache.h"
#include "suppress_address_library_dialog.h"
#include "disable_snow_flag.h"
#include "editor_id_cache.h"
#include "enable_achievements.h"
#include "form_caching.h"
#include "ini_setting_collection.h"
#include "max_stdio.h"
#include "regular_quicksaves.h"
#include "safe_exit.h"
#include "save_added_sound_categories.h"
#include "save_game_max_size.h"
#include "scrolling_doesnt_switch_pov.h"
#include "sleep_wait_time.h"
#include "tree_lod_reference_caching.h"
#include "waterflow_animation.h"

namespace Patches
{
    void Install()
    {
        // Always-on: suppress the SKSE Address Library warning dialog.
        // Under Wine/CrossOver the dialog appears behind the fullscreen game window
        // and permanently blocks the loading screen.  Auto-dismiss it with IDNO.
        SuppressAddressLibraryDialog::Install();

        if (Settings::Patches::bDisableChargenPrecache.GetValue())
            DisableChargenPrecache::Install();

        if (Settings::Patches::bDisableSnowFlag.GetValue())
            DisableSnowFlag::Install();

        if (Settings::Patches::bEnableAchievementsWithMods.GetValue())
            EnableAchievementsWithMods::Install();

        if (Settings::Patches::bEditorIdCache.GetValue())
            EditorIdCache::Install();

        if (Settings::Patches::bFormCaching.GetValue())
            FormCaching::Install();

        if (Settings::Patches::bINISettingCollection.GetValue())
            INISettingCollection::Install();

        if (Settings::Patches::bMaxStdIO.GetValue())
            MaxStdIO::Install();

        if (Settings::Patches::bRegularQuicksaves.GetValue())
            RegularQuicksaves::Install();

        if (Settings::Patches::bSafeExit.GetValue())
            SafeExit::Install();

        if (Settings::Patches::bSaveAddedSoundCategories.GetValue())
            SaveAddedSoundCategories::Install();

        if (Settings::Patches::iSaveGameMaxSize.GetValue() != 128)
            SaveGameMaxSize::Install();

        if (Settings::Patches::bScrollingDoesntSwitchPOV.GetValue())
            ScrollingDoesntSwitchPOV::Install();

        if (std::abs(Settings::Patches::fSleepWaitTimeModifier.GetValue() - 1.0f) > 0.001f)
            SleepWaitTime::Install();

        if (Settings::Patches::bFormCaching.GetValue() && Settings::Patches::bTreeLodReferenceCaching.GetValue())
            TreeLodReferenceCaching::Install();

        if (Settings::Patches::bWaterflowAnimation.GetValue())
            WaterflowAnimation::Install();
    }
}