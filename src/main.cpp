#include <coreinit/cache.h>
#include <coreinit/dynload.h>
#include <coreinit/memory.h>
#include <cstdint>
#include <cstring>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <wups.h>
#include <wups/config/WUPSConfigCategory.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/config_api.h>

WUPS_PLUGIN_NAME("Full RGB TV");
WUPS_PLUGIN_DESCRIPTION("Full RGB TV & Unified Video Switcher");
WUPS_PLUGIN_VERSION("v1.0.0-beta.3");
WUPS_PLUGIN_AUTHOR("Masana");
WUPS_PLUGIN_LICENSE("GPLv3");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("full_rgb_TV");

#define LOG(fmt, ...) WHBLogPrintf("[FULL_RGB_TV] " fmt "\n", ##__VA_ARGS__)

extern "C" uint32_t OSEffectiveToPhysical(uint32_t addr);

typedef struct {
    uint32_t addr;
    uint32_t patchVal;
    uint32_t standardVal;
    const char *desc;
} MemoryPatch;

// ============================================================================
// TARGETED HDMI MASTER PATCHES
// ============================================================================
static const MemoryPatch g_MasterPatches[] = {
        // --- 1. AVM.RPL ---
        {0x00FB6684, 0x38E00001, 0x38E00000, "AVM Color Config Force Full #1"},
        {0x00FE5F2C, 0x38E00001, 0x38E00000, "AVM Color Config Force Full #2"},

        // --- 2. TVE.RPL ---
        {0x0115248C, 0x60000000, 0x60000000, "TVE Bypass 1080p Skip (always nop)"},
        {0x01152490, 0x3880002A, 0x38800010, "TVE HDMI AVI InfoFrame PB3 Full/Limited"}};

#define TOTAL_PATCHES (sizeof(g_MasterPatches) / sizeof(MemoryPatch))

typedef int32_t (*AVMSetTVScanMode_t)(uint32_t mode);
typedef int32_t (*TVESetTVScanMode_t)(uint32_t mode);

// ============================================================================
// ENUMERATIONS & MENU OPTIONS
// ============================================================================
enum RGBModeOptions : uint32_t {
    RGB_DISABLED      = 0,
    RGB_ENABLED       = 1,
    RGB_APPLY_ON_BOOT = 2,
};

enum ResolutionOptions : uint32_t {
    RES_ANALOG_480I       = 0,
    RES_ANALOG_576I       = 1,
    RES_ANALOG_480I_PAL60 = 2,
    RES_HDMI_480P_60      = 3,
    RES_HDMI_576P_50      = 4,
    RES_HDMI_720P_50      = 5,
    RES_HDMI_720P_60      = 6,
    RES_HDMI_720P_3D      = 7,
    RES_HDMI_1080I_50     = 8,
    RES_HDMI_1080I_60     = 9,
    RES_HDMI_1080P_50     = 10,
    RES_HDMI_1080P_60     = 11,
};

#define RES_STORAGE_KEY      "selected_resolution"
#define RGB_MODE_STORAGE_KEY "rgb_mode_selection"

#define DEFAULT_RES_IDX      RES_HDMI_1080P_60
#define DEFAULT_RGB_MODE     RGB_DISABLED

static uint32_t s_SelectedResIdx  = DEFAULT_RES_IDX;
static uint32_t s_SelectedRGBMode = DEFAULT_RGB_MODE;

static uint32_t s_AppliedResIdx  = DEFAULT_RES_IDX;
static uint32_t s_AppliedRGBMode = DEFAULT_RGB_MODE;
static bool s_ColdbootDone       = false;

static const uint32_t s_ScanModes[] = {
        2, 1, 8, 3, 9, 10, 4, 5, 11, 6, 12, 7};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_PossibleRGBModes[] = {
        {RGB_DISABLED, "Disabled"},
        {RGB_ENABLED, "Enabled (Session only)"},
        {RGB_APPLY_ON_BOOT, "Enabled (Apply on Boot)"},
};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_PossibleResolutions[] = {
        {RES_ANALOG_480I, "[Composite] 480i (NTSC 60Hz)"},
        {RES_ANALOG_576I, "[Composite] 576i (PAL 50Hz)"},
        {RES_ANALOG_480I_PAL60, "[SCART] 480i PAL60 (60Hz)"},
        {RES_HDMI_480P_60, "[HDMI] 480p 60Hz (Progressive)"},
        {RES_HDMI_576P_50, "[HDMI] 576p 50Hz (PAL)"},
        {RES_HDMI_720P_50, "[HDMI] 720p 50Hz (GamePad stutter)"},
        {RES_HDMI_720P_60, "[HDMI] 720p 60Hz (Progressive)"},
        {RES_HDMI_720P_3D, "[HDMI] 720p 3D (Frame Packing)"},
        {RES_HDMI_1080I_50, "[HDMI] 1080i 50Hz (GamePad stutter)"},
        {RES_HDMI_1080I_60, "[HDMI] 1080i 60Hz (Interlaced)"},
        {RES_HDMI_1080P_50, "[HDMI] 1080p 50Hz (GamePad stutter)"},
        {RES_HDMI_1080P_60, "[HDMI] 1080p 60Hz (Progressive)"},
};

// ============================================================================
// MEMORY WRITE WITH POWERPC BARRIERS
// ============================================================================
static bool Write32Uncached(uint32_t addr, uint32_t val) {
    uint32_t phys = OSEffectiveToPhysical(addr);
    if (!phys || phys < 0x00800000) return false;

    volatile uint32_t *uncachedPtr = (volatile uint32_t *) (0x30000000 | phys);
    *uncachedPtr                   = val;

    asm volatile("sync; isync;");
    DCFlushRange((void *) addr, 4);
    ICInvalidateRange((void *) addr, 4);
    asm volatile("sync; isync;");
    return true;
}

static bool ApplyPatchesInRAM(bool enable) {
    uint32_t successCount = 0;
    for (size_t i = 0; i < TOTAL_PATCHES; i++) {
        uint32_t valToApply = enable ? g_MasterPatches[i].patchVal : g_MasterPatches[i].standardVal;
        if (Write32Uncached(g_MasterPatches[i].addr, valToApply)) {
            successCount++;
        }
    }
    return (successCount == TOTAL_PATCHES);
}

// ============================================================================
// DIRECT VIDEO DRIVER FORCE (TVE + AVM)
// ============================================================================
static void TriggerAVMNativeReInit(uint32_t scanMode, bool fullRGB) {
    LOG("⚡ [VIDEO] Direct re-init to scanMode %u (Full RGB: %d)...", scanMode, fullRGB);

    // 1. Apply memory patches (0x2A for Full RGB, 0x10 for Limited RGB)
    ApplyPatchesInRAM(fullRGB);

    // 2. Direct call to low-level TVE driver to force AVI InfoFrame refresh
    OSDynLoad_Module handleTVE = 0;
    if (OSDynLoad_Acquire("tve.rpl", &handleTVE) == 0) {
        TVESetTVScanMode_t pTVESetTVScanMode = nullptr;
        if (OSDynLoad_FindExport(handleTVE, OS_DYNLOAD_EXPORT_FUNC, "TVESetTVScanMode", (void **) &pTVESetTVScanMode) == 0 && pTVESetTVScanMode) {
            pTVESetTVScanMode(scanMode);
        }
        OSDynLoad_Release(handleTVE);
    }

    // 3. Notify AVM to synchronize global system video state
    OSDynLoad_Module handleAVM = 0;
    if (OSDynLoad_Acquire("avm.rpl", &handleAVM) == 0) {
        AVMSetTVScanMode_t pAVMSetTVScanMode = nullptr;
        if (OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVScanMode", (void **) &pAVMSetTVScanMode) == 0 && pAVMSetTVScanMode) {
            pAVMSetTVScanMode(scanMode);
        }
        OSDynLoad_Release(handleAVM);
    }

    // 4. Final in-RAM patch lock
    ApplyPatchesInRAM(fullRGB);
}

// ============================================================================
// CONFIG MENU CALLBACKS
// ============================================================================
void rgbModeChanged(ConfigItemMultipleValues *item, uint32_t newValue) {
    s_SelectedRGBMode = newValue;
}

void resModeChanged(ConfigItemMultipleValues *item, uint32_t newValue) {
    s_SelectedResIdx = newValue;
}

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle) {
    WUPSConfigCategory root = WUPSConfigCategory(rootHandle);
    try {
        WUPSStorageAPI::GetOrStoreDefault(RES_STORAGE_KEY, s_SelectedResIdx, (uint32_t) DEFAULT_RES_IDX);
        WUPSStorageAPI::GetOrStoreDefault(RGB_MODE_STORAGE_KEY, s_SelectedRGBMode, (uint32_t) DEFAULT_RGB_MODE);

        // 1. Full RGB Mode (Disabled / Session only / Apply on Boot)
        root.add(WUPSConfigItemMultipleValues::CreateFromValue(
                RGB_MODE_STORAGE_KEY, "Full RGB Mode",
                (uint32_t) DEFAULT_RGB_MODE, s_SelectedRGBMode,
                s_PossibleRGBModes,
                rgbModeChanged));

        // 2. Video Mode / Resolution
        root.add(WUPSConfigItemMultipleValues::CreateFromValue(
                RES_STORAGE_KEY, "Video Mode / Resolution",
                (uint32_t) DEFAULT_RES_IDX, s_SelectedResIdx,
                s_PossibleResolutions,
                resModeChanged));
    } catch (std::exception &e) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }
    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosedCallback() {
    bool resChanged = (s_SelectedResIdx != s_AppliedResIdx);
    bool rgbChanged = (s_SelectedRGBMode != s_AppliedRGBMode);

    WUPSStorageAPI::Store(RES_STORAGE_KEY, s_SelectedResIdx);
    WUPSStorageAPI::Store(RGB_MODE_STORAGE_KEY, s_SelectedRGBMode);
    WUPSStorageAPI::SaveStorage();

    if (resChanged || rgbChanged) {
        bool enableRGB      = (s_SelectedRGBMode != RGB_DISABLED);
        uint32_t targetMode = s_ScanModes[s_SelectedResIdx];

        LOG("⚡ [CONFIG] Change applied (Mode: %u, RGB: %d)...", targetMode, enableRGB);
        TriggerAVMNativeReInit(targetMode, enableRGB);

        s_AppliedResIdx  = s_SelectedResIdx;
        s_AppliedRGBMode = s_SelectedRGBMode;
    }
}

// ============================================================================
// PLUGIN LIFECYCLE
// ============================================================================
INITIALIZE_PLUGIN() {
    WHBLogUdpInit();

    WUPSStorageAPI::GetOrStoreDefault(RES_STORAGE_KEY, s_SelectedResIdx, (uint32_t) DEFAULT_RES_IDX);
    WUPSStorageAPI::GetOrStoreDefault(RGB_MODE_STORAGE_KEY, s_SelectedRGBMode, (uint32_t) DEFAULT_RGB_MODE);
    WUPSStorageAPI::SaveStorage();

    s_AppliedResIdx  = s_SelectedResIdx;
    s_AppliedRGBMode = s_SelectedRGBMode;

    WUPSConfigAPIOptionsV1 configOptions = {};
    configOptions.name                   = "Full RGB TV";
    WUPSConfigAPI_Init(configOptions, ConfigMenuOpenedCallback, ConfigMenuClosedCallback);
}

DEINITIALIZE_PLUGIN() {
    WUPSStorageAPI::SaveStorage();
    WHBLogUdpDeinit();
}

ON_APPLICATION_START() {
    // If 'Apply on Boot' is selected, initialize video mode on coldboot
    if (!s_ColdbootDone) {
        s_ColdbootDone = true;

        if (s_SelectedRGBMode == RGB_APPLY_ON_BOOT) {
            LOG("⚡ [COLDBOOT] Initializing Video Mode %u with Full RGB...", s_SelectedResIdx);
            uint32_t targetMode = s_ScanModes[s_SelectedResIdx];
            TriggerAVMNativeReInit(targetMode, true);

            s_AppliedResIdx  = s_SelectedResIdx;
            s_AppliedRGBMode = RGB_APPLY_ON_BOOT;
        }
    }
}

ON_APPLICATION_ENDS() {}
ON_APPLICATION_REQUESTS_EXIT() {}