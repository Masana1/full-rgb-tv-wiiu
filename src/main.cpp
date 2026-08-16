#include <coreinit/cache.h>
#include <coreinit/dynload.h>
#include <coreinit/memory.h>
#include <cstdint>
#include <cstring>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <wups.h>
#include <wups/config/WUPSConfigCategory.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/config_api.h>

WUPS_PLUGIN_NAME("Full RGB TV");
WUPS_PLUGIN_DESCRIPTION("Clean Full RGB & Video Switcher");
WUPS_PLUGIN_VERSION("v1.0.0-beta.2");
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
// TABLE DES 4 PATCHS HDMI CIBLÉS (AVM + INFOFRAME SANS CONFLIT GAMEPAD)
// ============================================================================
static const MemoryPatch g_MasterPatches[] = {
        // --- 1. AVM.RPL (Espace colorimétrique) ---
        {0x00FB6684, 0x38E00001, 0x38E00000, "AVM Color Config Force Full #1"},
        {0x00FE5F2C, 0x38E00001, 0x38E00000, "AVM Color Config Force Full #2"},

        // --- 2. TVE.RPL (HDMI AVI InfoFrame PB3 Full Range) ---
        {0x0115248C, 0x60000000, 0x41820008, "TVE Bypass 1080p Skip (nop vs branch)"},
        {0x01152490, 0x3980002A, 0x38800010, "TVE HDMI AVI InfoFrame PB3 Full Range"}};

#define TOTAL_PATCHES (sizeof(g_MasterPatches) / sizeof(MemoryPatch))

typedef int32_t (*AVMSetTVScanMode_t)(uint32_t mode);
static AVMSetTVScanMode_t pAVMSetTVScanMode = nullptr;

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

#define RES_STORAGE_KEY   "selected_resolution"
#define RGB_STORAGE_KEY   "full_rgb_enabled"
#define DEFAULT_RES_IDX   RES_HDMI_1080P_60
#define DEFAULT_RGB_STATE true

static uint32_t s_SelectedResIdx = DEFAULT_RES_IDX;
static bool s_FullRGBEnabled     = DEFAULT_RGB_STATE;
static uint32_t s_AppliedResIdx  = DEFAULT_RES_IDX;
static bool s_AppliedRGBEnabled  = DEFAULT_RGB_STATE;

static const uint32_t s_ScanModes[] = {
        2, 1, 8, 3, 9, 10, 4, 5, 11, 6, 12, 7};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_PossibleResolutions[] = {
        {RES_ANALOG_480I, "[Composite] 480i (NTSC 60Hz)"},
        {RES_ANALOG_576I, "[Composite] 576i (PAL 50Hz)"},
        {RES_ANALOG_480I_PAL60, "[SCART] 480i PAL60 (60Hz)"},
        {RES_HDMI_480P_60, "[HDMI] 480p 60Hz (Progressif)"},
        {RES_HDMI_576P_50, "[HDMI] 576p 50Hz (PAL)"},
        {RES_HDMI_720P_50, "[HDMI] 720p 50Hz (GamePad saccadé)"},
        {RES_HDMI_720P_60, "[HDMI] 720p 60Hz (Progressif)"},
        {RES_HDMI_720P_3D, "[HDMI] 720p 3D (Frame Packing)"},
        {RES_HDMI_1080I_50, "[HDMI] 1080i 50Hz (GamePad saccadé)"},
        {RES_HDMI_1080I_60, "[HDMI] 1080i 60Hz (Entrelacé)"},
        {RES_HDMI_1080P_50, "[HDMI] 1080p 50Hz (GamePad saccadé)"},
        {RES_HDMI_1080P_60, "[HDMI] 1080p 60Hz (Progressif)"},
};

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

static void TriggerAVMNativeReInit(uint32_t scanMode) {
    OSDynLoad_Module handleAVM = 0;
    if (OSDynLoad_Acquire("avm.rpl", &handleAVM) == 0) {
        if (OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVScanMode", (void **) &pAVMSetTVScanMode) == 0 && pAVMSetTVScanMode) {
            LOG("⚡ [AVM NATIVE] Application du mode %u...", scanMode);
            pAVMSetTVScanMode(scanMode);
            ApplyPatchesInRAM(s_FullRGBEnabled);
        }
    }
}

void multipleValueItemChanged(ConfigItemMultipleValues *item, uint32_t newValue) {
    s_SelectedResIdx = newValue;
}

void boolItemChanged(ConfigItemBoolean *item, bool newValue) {
    s_FullRGBEnabled = newValue;
}

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle) {
    WUPSConfigCategory root = WUPSConfigCategory(rootHandle);
    try {
        WUPSStorageAPI::GetOrStoreDefault(RES_STORAGE_KEY, s_SelectedResIdx, (uint32_t) DEFAULT_RES_IDX);
        WUPSStorageAPI::GetOrStoreDefault(RGB_STORAGE_KEY, s_FullRGBEnabled, DEFAULT_RGB_STATE);

        root.add(WUPSConfigItemBoolean::Create(
                RGB_STORAGE_KEY, "Activer Full RGB",
                DEFAULT_RGB_STATE, s_FullRGBEnabled,
                boolItemChanged));

        root.add(WUPSConfigItemMultipleValues::CreateFromValue(
                RES_STORAGE_KEY, "Mode Vidéo / Résolution",
                DEFAULT_RES_IDX, s_SelectedResIdx,
                s_PossibleResolutions,
                multipleValueItemChanged));
    } catch (std::exception &e) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }
    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosedCallback() {
    bool resChanged = (s_SelectedResIdx != s_AppliedResIdx);
    bool rgbChanged = (s_FullRGBEnabled != s_AppliedRGBEnabled);

    WUPSStorageAPI::Store(RES_STORAGE_KEY, s_SelectedResIdx);
    WUPSStorageAPI::Store(RGB_STORAGE_KEY, s_FullRGBEnabled);
    WUPSStorageAPI::SaveStorage();

    if (resChanged || rgbChanged) {
        ApplyPatchesInRAM(s_FullRGBEnabled);
        uint32_t targetMode = s_ScanModes[s_SelectedResIdx];
        TriggerAVMNativeReInit(targetMode);

        s_AppliedResIdx     = s_SelectedResIdx;
        s_AppliedRGBEnabled = s_FullRGBEnabled;
    }
}

INITIALIZE_PLUGIN() {
    WHBLogUdpInit();
    WUPSStorageAPI::GetOrStoreDefault(RES_STORAGE_KEY, s_SelectedResIdx, (uint32_t) DEFAULT_RES_IDX);
    WUPSStorageAPI::GetOrStoreDefault(RGB_STORAGE_KEY, s_FullRGBEnabled, DEFAULT_RGB_STATE);
    WUPSStorageAPI::SaveStorage();

    s_AppliedResIdx     = s_SelectedResIdx;
    s_AppliedRGBEnabled = s_FullRGBEnabled;

    WUPSConfigAPIOptionsV1 configOptions = {};
    configOptions.name                   = "Full RGB TV";
    WUPSConfigAPI_Init(configOptions, ConfigMenuOpenedCallback, ConfigMenuClosedCallback);
}

DEINITIALIZE_PLUGIN() {
    WUPSStorageAPI::SaveStorage();
    WHBLogUdpDeinit();
}

ON_APPLICATION_START() {}
ON_APPLICATION_ENDS() {}
ON_APPLICATION_REQUESTS_EXIT() {}