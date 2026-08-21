#include <avm/tv.h>
#include <coreinit/cache.h>
#include <coreinit/dynload.h>
#include <coreinit/memory.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/title.h>
#include <cstdint>
#include <cstring>
#include <exception>
#include <vpad/input.h>
#include <wups.h>
#include <wups/config/WUPSConfigCategory.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/config_api.h>
#include <wups/storage.h>

WUPS_PLUGIN_NAME("Full RGB TV");
WUPS_PLUGIN_DESCRIPTION("Full RGB TV & Video Mode Changer");
WUPS_PLUGIN_VERSION("v1.0.0-beta.4");
WUPS_PLUGIN_AUTHOR("Masana");
WUPS_PLUGIN_LICENSE("GPLv3");

WUPS_USE_STORAGE("Full_RGB_TV");

#ifndef WUPS_STORAGE_ROOT_ITEM
#define WUPS_STORAGE_ROOT_ITEM ((wups_storage_item) 0)
#endif

// ============================================================================
// WII U MENU DETECTION (WARAWARA PLAZA)
// ============================================================================
static inline bool IsWiiUMenu() {
    uint64_t tid = OSGetTitleID();
    return (tid == 0x0005001010040200ull || // Europe
            tid == 0x0005001010040100ull || // USA
            tid == 0x0005001010040000ull);  // Japan
}

// ============================================================================
// AVM PROTOTYPES & DYNAMIC LOADING
// ============================================================================
typedef int32_t (*AVMSetTVScanMode_t)(uint32_t mode);
typedef int32_t (*AVMSetTVTileMode_t)(uint8_t mode);
typedef int32_t (*AVMSetTVVideoRegion_t)(AVMTvVideoRegion region, TVEPort port, AVMTvResolution res);
typedef int32_t (*AVMSetTVOutPort_t)(TVEPort port, AVMTvResolution res);
typedef int32_t (*AVMSetTVScanResolution_t)(AVMTvResolution res);
typedef int32_t (*AVMSetTVAspectRatio_t)(AVMTvAspectRatio ratio);

// ============================================================================
// SAFE MEMORY ACCESS & CACHE FLUSH
// ============================================================================
static bool IsValidAddress(uint32_t addr) {
    bool inMEM1 = (addr >= 0x00800000 && addr < 0x02000000);
    bool inMEM2 = (addr >= 0x10000000 && addr < 0x50000000);
    return (inMEM1 || inMEM2);
}

static uint32_t ReadCode32Safe(uint32_t addr) {
    if (!IsValidAddress(addr)) {
        return 0;
    }
    volatile uint32_t *ptr = reinterpret_cast<volatile uint32_t *>(addr);
    return *ptr;
}

static bool WriteCode32Safe(uint32_t addr, uint32_t val) {
    if (!IsValidAddress(addr)) {
        return false;
    }

    volatile uint32_t *ptr = reinterpret_cast<volatile uint32_t *>(addr);
    *ptr                   = val;

    void *aligned = reinterpret_cast<void *>(addr & ~0x1F);
    DCFlushRange(aligned, 0x20);
    asm volatile("sync; isync;");
    ICInvalidateRange(aligned, 0x20);
    asm volatile("sync; isync;");
    return true;
}

// ============================================================================
// DYNAMIC UNIVERSAL AVM SCANNER (FULL RGB)
// ============================================================================
struct SignaturePattern {
    uint32_t searchStart;
    uint32_t searchSize;
    uint32_t patchedValue;  // 0x38E00001 (li r7, 1) -> Full RGB
    uint32_t originalValue; // 0x38E00030 (li r7, 48) -> Limited RGB
};

static const SignaturePattern g_Patterns[] = {
        {0x00FA0000, 0x30000, 0x38E00001, 0x38E00030},
        {0x00FD0000, 0x30000, 0x38E00001, 0x38E00030}};
static constexpr size_t g_NumPatterns              = sizeof(g_Patterns) / sizeof(g_Patterns[0]);
static uint32_t s_ResolvedAddresses[g_NumPatterns] = {0};

static uint32_t ScanMemoryForPattern(const SignaturePattern &sig, bool targetPatched) {
    uint32_t targetVal = targetPatched ? sig.patchedValue : sig.originalValue;
    for (uint32_t addr = sig.searchStart; addr < (sig.searchStart + sig.searchSize); addr += 4) {
        if (!IsValidAddress(addr)) {
            continue;
        }
        if (ReadCode32Safe(addr) == targetVal) {
            return addr;
        }
    }
    return 0;
}

static void ApplyDynamicAvmPatches(bool enable) {
    for (size_t i = 0; i < g_NumPatterns; ++i) {
        const auto &sig = g_Patterns[i];
        uint32_t addr   = s_ResolvedAddresses[i];

        if (addr == 0) {
            addr = ScanMemoryForPattern(sig, false);
            if (addr == 0) {
                addr = ScanMemoryForPattern(sig, true);
            }

            if (addr != 0) {
                s_ResolvedAddresses[i] = addr;
            } else {
                continue;
            }
        }

        uint32_t currentVal = ReadCode32Safe(addr);
        uint32_t valToApply = enable ? sig.patchedValue : sig.originalValue;

        if (currentVal == valToApply) {
            continue;
        }

        WriteCode32Safe(addr, valToApply);
    }
}

// ============================================================================
// CONFIGURATION ENUMS & STRUCTURES
// ============================================================================
enum ConfigOperatingMode : uint32_t {
    MODE_STANDARD           = 0,
    MODE_VIDEO_MODE_CHANGER = 1,
};

enum RGBStartupOptions : uint32_t {
    RGB_STARTUP_DISABLED = 0,
    RGB_STARTUP_ENABLED  = 1,
};

enum AutobootRegInitOptions : uint32_t {
    AUTOBOOT_REGINIT_DISABLED = 0,
    AUTOBOOT_REGINIT_ENABLED  = 1,
};

enum StandardResList : uint32_t {
    STD_RES_480P  = 0,
    STD_RES_720P  = 1,
    STD_RES_1080I = 2,
    STD_RES_1080P = 3,
};

struct ResolutionEntry {
    const char *name;
    AVMTvResolution value;
};

static constexpr ResolutionEntry g_AdvResolutions[] = {
        {"480i (60Hz)", AVM_TV_RESOLUTION_480I},
        {"480i PAL60 (60Hz)", AVM_TV_RESOLUTION_480I_PAL60},
        {"480p (60Hz)", AVM_TV_RESOLUTION_480P},
        {"576i (50Hz)", AVM_TV_RESOLUTION_576I},
        {"576p (50Hz)", AVM_TV_RESOLUTION_576P},
        {"720p 3D (60Hz)", AVM_TV_RESOLUTION_720P_3D},
        {"720p (50Hz - glitchy GamePad)", AVM_TV_RESOLUTION_720P_50HZ},
        {"720p (60Hz)", AVM_TV_RESOLUTION_720P},
        {"1080i (50Hz - glitchy GamePad)", AVM_TV_RESOLUTION_1080I_50HZ},
        {"1080i (60Hz)", AVM_TV_RESOLUTION_1080I},
        {"1080p (50Hz - glitchy GamePad)", AVM_TV_RESOLUTION_1080P_50HZ},
        {"1080p (60Hz)", AVM_TV_RESOLUTION_1080P}};
static constexpr size_t g_NumAdvResolutions = sizeof(g_AdvResolutions) / sizeof(g_AdvResolutions[0]);

static constexpr TVEPort g_PortHardwareMap[] = {
        (TVEPort) 2, // Composite
        (TVEPort) 3, // SCART
        (TVEPort) 1, // Component
        (TVEPort) 0  // HDMI
};

// ============================================================================
// STATE VARIABLES
// ============================================================================
static uint32_t s_SelectedStdRes          = STD_RES_1080P;
static uint32_t s_SelectedRGB             = RGB_STARTUP_ENABLED;
static uint32_t s_SelectedConfigMode      = MODE_STANDARD;
static uint32_t s_SelectedAutobootRegInit = AUTOBOOT_REGINIT_ENABLED;

static uint32_t s_SelectedAdvRegion = 1;  // 0 = PAL, 1 = NTSC
static uint32_t s_SelectedAdvPort   = 3;  // 3 = HDMI default
static uint32_t s_SelectedAdvResIdx = 11; // 11 = 1080p (60Hz) default
static uint32_t s_SelectedAdvAspect = 1;  // 0 = 4:3, 1 = 16:9
static uint32_t s_SelectedTileMode  = 1;  // 1 = Fix illegible TV screen

static uint32_t s_SnapshotStdRes          = STD_RES_1080P;
static uint32_t s_SnapshotRGB             = RGB_STARTUP_ENABLED;
static uint32_t s_SnapshotConfigMode      = MODE_STANDARD;
static uint32_t s_SnapshotAutobootRegInit = AUTOBOOT_REGINIT_ENABLED;
static uint32_t s_SnapshotAdvRegion       = 1;
static uint32_t s_SnapshotAdvPort         = 3;
static uint32_t s_SnapshotAdvResIdx       = 11;
static uint32_t s_SnapshotAdvAspect       = 1;
static uint32_t s_SnapshotTileMode        = 1;

static constexpr WUPSConfigItemMultipleValues::ValuePair s_StdResolutionsList[] = {
        {STD_RES_480P, "480p"},
        {STD_RES_720P, "720p"},
        {STD_RES_1080I, "1080i"},
        {STD_RES_1080P, "1080p"},
};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_RGBStartupList[] = {
        {RGB_STARTUP_DISABLED, "Disabled (Limited 16-235)"},
        {RGB_STARTUP_ENABLED, "Enabled (Full RGB 0-255)"},
};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_ConfigModeList[] = {
        {MODE_STANDARD, "Standard"},
        {MODE_VIDEO_MODE_CHANGER, "Video Mode Changer"},
};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_AutobootRegInitList[] = {
        {AUTOBOOT_REGINIT_DISABLED, "Disabled"},
        {AUTOBOOT_REGINIT_ENABLED, "Enabled"},
};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_AdvRegionList[] = {
        {0, "PAL"},
        {1, "NTSC"},
};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_AdvPortList[] = {
        {0, "Composite"},
        {1, "SCART"},
        {2, "Component"},
        {3, "HDMI"},
};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_AdvResValuePairs[] = {
        {0, "480i (60Hz)"},
        {1, "480i PAL60 (60Hz)"},
        {2, "480p (60Hz)"},
        {3, "576i (50Hz)"},
        {4, "576p (50Hz)"},
        {5, "720p 3D (60Hz)"},
        {6, "720p (50Hz - glitchy GamePad)"},
        {7, "720p (60Hz)"},
        {8, "1080i (50Hz - glitchy GamePad)"},
        {9, "1080i (60Hz)"},
        {10, "1080p (50Hz - glitchy GamePad)"},
        {11, "1080p (60Hz)"},
};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_AdvAspectList[] = {
        {0, "4:3"},
        {1, "16:9"},
};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_AdvTileList[] = {
        {0, "Disabled"},
        {1, "Enabled (Fix Illegible Screen)"},
};

// ============================================================================
// WUPS PERSISTENT STORAGE MANAGEMENT
// ============================================================================
static void LoadConfig() {
    int32_t val = 0;
    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "autoboot_reg_init", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedAutobootRegInit = static_cast<uint32_t>(val);
    } else {
        s_SelectedAutobootRegInit = AUTOBOOT_REGINIT_ENABLED;
    }

    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "std_res", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedStdRes = static_cast<uint32_t>(val);
    } else {
        s_SelectedStdRes = STD_RES_1080P;
    }

    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "rgb_mode", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedRGB = static_cast<uint32_t>(val);
    } else {
        s_SelectedRGB = RGB_STARTUP_ENABLED;
    }

    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "config_mode", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedConfigMode = static_cast<uint32_t>(val);
    } else {
        s_SelectedConfigMode = MODE_STANDARD;
    }

    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "vmc_region", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedAdvRegion = static_cast<uint32_t>(val);
    } else {
        s_SelectedAdvRegion = 1;
    }

    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "vmc_port", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedAdvPort = static_cast<uint32_t>(val);
    } else {
        s_SelectedAdvPort = 3;
    }

    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "vmc_res", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedAdvResIdx = static_cast<uint32_t>(val);
    } else {
        s_SelectedAdvResIdx = 11;
    }

    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "vmc_aspect", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedAdvAspect = static_cast<uint32_t>(val);
    } else {
        s_SelectedAdvAspect = 1;
    }

    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "vmc_tile", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedTileMode = static_cast<uint32_t>(val);
    } else {
        s_SelectedTileMode = 1;
    }
}

static void SaveConfig() {
    WUPSStorageAPI_StoreInt(WUPS_STORAGE_ROOT_ITEM, "autoboot_reg_init", static_cast<int32_t>(s_SelectedAutobootRegInit));
    WUPSStorageAPI_StoreInt(WUPS_STORAGE_ROOT_ITEM, "std_res", static_cast<int32_t>(s_SelectedStdRes));
    WUPSStorageAPI_StoreInt(WUPS_STORAGE_ROOT_ITEM, "rgb_mode", static_cast<int32_t>(s_SelectedRGB));
    WUPSStorageAPI_StoreInt(WUPS_STORAGE_ROOT_ITEM, "config_mode", static_cast<int32_t>(s_SelectedConfigMode));
    WUPSStorageAPI_StoreInt(WUPS_STORAGE_ROOT_ITEM, "vmc_region", static_cast<int32_t>(s_SelectedAdvRegion));
    WUPSStorageAPI_StoreInt(WUPS_STORAGE_ROOT_ITEM, "vmc_port", static_cast<int32_t>(s_SelectedAdvPort));
    WUPSStorageAPI_StoreInt(WUPS_STORAGE_ROOT_ITEM, "vmc_res", static_cast<int32_t>(s_SelectedAdvResIdx));
    WUPSStorageAPI_StoreInt(WUPS_STORAGE_ROOT_ITEM, "vmc_aspect", static_cast<int32_t>(s_SelectedAdvAspect));
    WUPSStorageAPI_StoreInt(WUPS_STORAGE_ROOT_ITEM, "vmc_tile", static_cast<int32_t>(s_SelectedTileMode));
    WUPSStorageAPI_SaveStorage(WUPS_STORAGE_ROOT_ITEM);
}

static uint32_t CalculateTargetScanMode() {
    switch (s_SelectedStdRes) {
        case STD_RES_480P:
            return 3;
        case STD_RES_720P:
            return 4;
        case STD_RES_1080I:
            return 6;
        case STD_RES_1080P:
        default:
            return 7;
    }
}

// ============================================================================
// NATIVE AVM VIDEO SWITCHING
// ============================================================================
static void ApplyAdvancedVideoMode() {
    OSDynLoad_Module handleAVM = 0;
    if (OSDynLoad_Acquire("avm.rpl", &handleAVM) == 0 && handleAVM != 0) {
        AVMSetTVTileMode_t pTileMode      = nullptr;
        AVMSetTVVideoRegion_t pVideoReg   = nullptr;
        AVMSetTVOutPort_t pOutPort        = nullptr;
        AVMSetTVScanResolution_t pScanRes = nullptr;
        AVMSetTVAspectRatio_t pAspect     = nullptr;

        OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVTileMode", reinterpret_cast<void **>(&pTileMode));
        OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVVideoRegion", reinterpret_cast<void **>(&pVideoReg));
        OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVOutPort", reinterpret_cast<void **>(&pOutPort));
        OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVScanResolution", reinterpret_cast<void **>(&pScanRes));
        OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVAspectRatio", reinterpret_cast<void **>(&pAspect));

        AVMTvResolution targetRes  = g_AdvResolutions[s_SelectedAdvResIdx].value;
        TVEPort targetPort         = g_PortHardwareMap[s_SelectedAdvPort];
        AVMTvVideoRegion targetReg = (s_SelectedAdvRegion == 1) ? AVM_TV_VIDEO_REGION_NTSC : AVM_TV_VIDEO_REGION_PAL;

        if (pVideoReg) {
            pVideoReg(targetReg, targetPort, targetRes);
        } else if (pOutPort) {
            pOutPort(targetPort, targetRes);
        } else if (pScanRes) {
            pScanRes(targetRes);
        }

        if (pAspect) {
            pAspect(static_cast<AVMTvAspectRatio>(s_SelectedAdvAspect));
        }

        if (pTileMode && s_SelectedTileMode) {
            pTileMode(static_cast<uint8_t>(s_SelectedTileMode));
        }

        OSDynLoad_Release(handleAVM);
    }
}

static void ApplyVideoSettingsDirect() {
    ApplyDynamicAvmPatches(s_SelectedRGB == RGB_STARTUP_ENABLED);

    if (s_SelectedConfigMode == MODE_STANDARD) {
        uint32_t targetMode        = CalculateTargetScanMode();
        OSDynLoad_Module handleAVM = 0;
        if (OSDynLoad_Acquire("avm.rpl", &handleAVM) == 0 && handleAVM != 0) {
            AVMSetTVScanMode_t pAVMSetTVScanMode = nullptr;
            if (OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVScanMode", reinterpret_cast<void **>(&pAVMSetTVScanMode)) == 0 && pAVMSetTVScanMode) {
                pAVMSetTVScanMode(targetMode);
            }
            OSDynLoad_Release(handleAVM);
        }
    } else {
        ApplyAdvancedVideoMode();
    }
}

// ============================================================================
// INITIAL AUTOBOOT THREAD (STARTUP ONLY)
// ============================================================================
static OSThread s_AutoBootThread;
static uint8_t s_AutoBootThreadStack[0x4000] __attribute__((aligned(32)));
static volatile bool s_AutobootDone = false;
static uint32_t s_VPADFrameCounter  = 0;

static int AutoBootThreadMain(int argc, const char **argv) {
    ApplyDynamicAvmPatches(s_SelectedRGB == RGB_STARTUP_ENABLED);

    uint32_t targetMode = CalculateTargetScanMode();
    uint32_t tempMode   = (targetMode == 7) ? 4 : 7;

    OSDynLoad_Module handleAVM = 0;
    if (OSDynLoad_Acquire("avm.rpl", &handleAVM) == 0 && handleAVM != 0) {
        AVMSetTVScanMode_t pAVMSetTVScanMode = nullptr;
        if (OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVScanMode", reinterpret_cast<void **>(&pAVMSetTVScanMode)) == 0 && pAVMSetTVScanMode) {
            // HDMI handshake: 720p -> 1080p switch forces TV/scaler into RGB mode
            pAVMSetTVScanMode(tempMode);
            OSSleepTicks(OSMillisecondsToTicks(1500));
            pAVMSetTVScanMode(targetMode);
        }
        OSDynLoad_Release(handleAVM);
    }

    return 0;
}

// ============================================================================
// VPADRead HOOK
// ============================================================================
DECL_FUNCTION(int32_t, VPADRead, VPADChan chan, VPADStatus *buffers, uint32_t count, VPADReadError *error) {
    int32_t result = real_VPADRead(chan, buffers, count, error);

    if (s_SelectedAutobootRegInit == AUTOBOOT_REGINIT_ENABLED && !s_AutobootDone && chan == VPAD_CHAN_0) {
        if (IsWiiUMenu()) {
            s_VPADFrameCounter++;

            if (s_VPADFrameCounter >= 240) { // ~4 seconds after WaraWara Plaza loads
                s_AutobootDone = true;

                bool threadOk = OSCreateThread(&s_AutoBootThread,
                                               AutoBootThreadMain,
                                               0, nullptr,
                                               s_AutoBootThreadStack + sizeof(s_AutoBootThreadStack),
                                               sizeof(s_AutoBootThreadStack),
                                               20,
                                               OS_THREAD_ATTRIB_AFFINITY_CPU1);

                if (threadOk) {
                    OSResumeThread(&s_AutoBootThread);
                }
            }
        }
    }

    return result;
}

WUPS_MUST_REPLACE(VPADRead, WUPS_LOADER_LIBRARY_VPAD, VPADRead);

// ============================================================================
// AROMA CONFIGURATION MENU CALLBACKS
// ============================================================================
void stdResChanged(ConfigItemMultipleValues *item, uint32_t val) {
    s_SelectedStdRes = val;
    // Automatically switch to MODE_STANDARD when standard resolution is modified
    s_SelectedConfigMode = MODE_STANDARD;
}

void rgbStartupChanged(ConfigItemMultipleValues *item, uint32_t val) {
    s_SelectedRGB = val;
}

void configModeChanged(ConfigItemMultipleValues *item, uint32_t val) {
    s_SelectedConfigMode = val;
}

void autobootRegInitChanged(ConfigItemMultipleValues *item, uint32_t val) {
    s_SelectedAutobootRegInit = val;
}

void advRegionChanged(ConfigItemMultipleValues *item, uint32_t val) {
    s_SelectedAdvRegion  = val;
    s_SelectedConfigMode = MODE_VIDEO_MODE_CHANGER;
}

void advPortChanged(ConfigItemMultipleValues *item, uint32_t val) {
    s_SelectedAdvPort    = val;
    s_SelectedConfigMode = MODE_VIDEO_MODE_CHANGER;
}

void advResChanged(ConfigItemMultipleValues *item, uint32_t val) {
    s_SelectedAdvResIdx  = val;
    s_SelectedConfigMode = MODE_VIDEO_MODE_CHANGER;
}

void advAspectChanged(ConfigItemMultipleValues *item, uint32_t val) {
    s_SelectedAdvAspect  = val;
    s_SelectedConfigMode = MODE_VIDEO_MODE_CHANGER;
}

void advTileChanged(ConfigItemMultipleValues *item, uint32_t val) {
    s_SelectedTileMode   = val;
    s_SelectedConfigMode = MODE_VIDEO_MODE_CHANGER;
}

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle) {
    s_SnapshotStdRes          = s_SelectedStdRes;
    s_SnapshotRGB             = s_SelectedRGB;
    s_SnapshotConfigMode      = s_SelectedConfigMode;
    s_SnapshotAutobootRegInit = s_SelectedAutobootRegInit;
    s_SnapshotAdvRegion       = s_SelectedAdvRegion;
    s_SnapshotAdvPort         = s_SelectedAdvPort;
    s_SnapshotAdvResIdx       = s_SelectedAdvResIdx;
    s_SnapshotAdvAspect       = s_SelectedAdvAspect;
    s_SnapshotTileMode        = s_SelectedTileMode;

    WUPSConfigCategory root = WUPSConfigCategory(rootHandle);
    try {
        root.add(WUPSConfigItemMultipleValues::CreateFromValue(
                "std_res", "Standard Resolution",
                (uint32_t) STD_RES_1080P, s_SelectedStdRes,
                s_StdResolutionsList, stdResChanged));

        root.add(WUPSConfigItemMultipleValues::CreateFromValue(
                "rgb_mode", "Color Range Mode",
                (uint32_t) RGB_STARTUP_ENABLED, s_SelectedRGB,
                s_RGBStartupList, rgbStartupChanged));

        root.add(WUPSConfigItemMultipleValues::CreateFromValue(
                "config_mode", "Mode Selector",
                (uint32_t) MODE_STANDARD, s_SelectedConfigMode,
                s_ConfigModeList, configModeChanged));

        root.add(WUPSConfigItemMultipleValues::CreateFromValue(
                "autoboot_reg_init", "Autoboot",
                (uint32_t) AUTOBOOT_REGINIT_ENABLED, s_SelectedAutobootRegInit,
                s_AutobootRegInitList, autobootRegInitChanged));

        WUPSConfigCategory vmcCategory = WUPSConfigCategory::Create("Video Mode Changer");

        vmcCategory.add(WUPSConfigItemMultipleValues::CreateFromValue(
                "vmc_region", "Video Region",
                (uint32_t) 1, s_SelectedAdvRegion,
                s_AdvRegionList, advRegionChanged));

        vmcCategory.add(WUPSConfigItemMultipleValues::CreateFromValue(
                "vmc_port", "Output Port",
                (uint32_t) 3, s_SelectedAdvPort,
                s_AdvPortList, advPortChanged));

        vmcCategory.add(WUPSConfigItemMultipleValues::CreateFromValue(
                "vmc_res", "Resolution & Refresh Rate",
                (uint32_t) 11, s_SelectedAdvResIdx,
                s_AdvResValuePairs, advResChanged));

        vmcCategory.add(WUPSConfigItemMultipleValues::CreateFromValue(
                "vmc_aspect", "Aspect Ratio",
                (uint32_t) 1, s_SelectedAdvAspect,
                s_AdvAspectList, advAspectChanged));

        vmcCategory.add(WUPSConfigItemMultipleValues::CreateFromValue(
                "vmc_tile", "Tile Mode Fix",
                (uint32_t) 1, s_SelectedTileMode,
                s_AdvTileList, advTileChanged));

        root.add(std::move(vmcCategory));

    } catch (std::exception &e) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }
    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosedCallback() {
    bool videoChanged = (s_SelectedStdRes != s_SnapshotStdRes) ||
                        (s_SelectedRGB != s_SnapshotRGB) ||
                        (s_SelectedConfigMode != s_SnapshotConfigMode) ||
                        (s_SelectedAdvRegion != s_SnapshotAdvRegion) ||
                        (s_SelectedAdvPort != s_SnapshotAdvPort) ||
                        (s_SelectedAdvResIdx != s_SnapshotAdvResIdx) ||
                        (s_SelectedAdvAspect != s_SnapshotAdvAspect) ||
                        (s_SelectedTileMode != s_SnapshotTileMode);

    bool configChanged = (s_SelectedAutobootRegInit != s_SnapshotAutobootRegInit) || videoChanged;

    if (!configChanged) {
        return;
    }

    // Persist configuration to WUPS storage
    SaveConfig();

    // Trigger hardware video re-init only if display parameters changed
    if (videoChanged) {
        ApplyVideoSettingsDirect();
    }
}

// ============================================================================
// PLUGIN LIFECYCLE
// ============================================================================
INITIALIZE_PLUGIN() {
    LoadConfig();

    WUPSConfigAPIOptionsV1 configOptions = {};
    configOptions.name                   = "Full RGB TV";
    WUPSConfigAPI_Init(configOptions, ConfigMenuOpenedCallback, ConfigMenuClosedCallback);
}

DEINITIALIZE_PLUGIN() {}
ON_APPLICATION_START() {}
ON_APPLICATION_ENDS() {}
ON_APPLICATION_REQUESTS_EXIT() {}