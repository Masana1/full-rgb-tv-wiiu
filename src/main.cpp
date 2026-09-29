#include <avm/tv.h>
#include <coreinit/cache.h>
#include <coreinit/dynload.h>
#include <coreinit/memory.h>
#include <coreinit/memorymap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/title.h>
#include <cstdint>
#include <cstring>
#include <exception>
#include <utility>
#include <vpad/input.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <wups.h>
#include <wups/config/WUPSConfigCategory.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/config_api.h>
#include <wups/storage.h>

WUPS_PLUGIN_NAME("Full RGB TV & WiiU Video Mode");
WUPS_PLUGIN_DESCRIPTION("Full RGB TV (0-255) & Video Mode Changer for Wii U");
WUPS_PLUGIN_VERSION("v1.0.0-beta.5");
WUPS_PLUGIN_AUTHOR("Masana, Lynx64, FIX94");
WUPS_PLUGIN_LICENSE("GPLv3");

WUPS_USE_STORAGE("Full_RGB_TV");

#ifndef WUPS_STORAGE_ROOT_ITEM
#define WUPS_STORAGE_ROOT_ITEM ((wups_storage_item) 0)
#endif

#define LOG_I(fmt, ...) \
    do { \
        OSTime _now  = OSGetTime(); \
        uint32_t _ms = (uint32_t) OSTicksToMilliseconds(_now); \
        WHBLogPrintf("[FullRGB][%ums] " fmt "\n", _ms, ##__VA_ARGS__); \
    } while (0)

typedef int32_t (*AVMGetTVScanMode_t)(uint32_t *mode);
typedef int32_t (*AVMSetTVScanMode_t)(uint32_t mode);
typedef int32_t (*AVMSetTVTileMode_t)(uint8_t mode);
typedef int32_t (*AVMSetTVVideoRegion_t)(AVMTvVideoRegion region, TVEPort port, AVMTvResolution res);
typedef int32_t (*AVMSetTVOutPort_t)(TVEPort port, AVMTvResolution res);
typedef int32_t (*AVMSetTVScanResolution_t)(AVMTvResolution res);
typedef int32_t (*AVMSetTVAspectRatio_t)(AVMTvAspectRatio ratio);
typedef int32_t (*AVMIsAVOutReady_t)(void);
typedef int32_t (*TVESetForceRGBMode_t)(uint32_t mode);

static inline uint32_t DirectRead32(uint32_t addr) {
    return *reinterpret_cast<const volatile uint32_t *>(addr);
}

static bool WriteCodeSafe32(uint32_t addr, uint32_t value) {
    if (!OSIsAddressValid(addr)) {
        LOG_I("Adresse virtuelle invalide : 0x%08X", addr);
        return false;
    }
    uint32_t phys = OSEffectiveToPhysical(addr);
    if (!phys) {
        LOG_I("Conversion effective -> physique echouee a 0x%08X", addr);
        return false;
    }

    volatile uint32_t *ptr = reinterpret_cast<volatile uint32_t *>(0x30000000 | phys);
    *ptr = value;

    void *aligned = reinterpret_cast<void *>(addr & ~0x1F);
    DCFlushRange(aligned, 0x20);
    asm volatile("sync; isync;");
    ICInvalidateRange(aligned, 0x20);
    asm volatile("sync; isync;");

    uint32_t verify = DirectRead32(addr);
    if (verify != value) {
        LOG_I("Echec verification patch a 0x%08X : lu 0x%08X, attendu 0x%08X", addr, verify, value);
        return false;
    }

    return true;
}

static bool LooksLikePlausibleFollowUp(uint32_t addr) {
    uint32_t op = DirectRead32(addr);
    uint32_t primaryOpcode = op >> 26;
    switch (primaryOpcode) {
        case 36: // stw
        case 37: // stwu
        case 24: // ori
        case 31: // extended (or, mr, cmp)
        case 18: // b / bl
        case 16: // bc
            return true;
        default:
            return false;
    }
}

static uint32_t s_TargetPatchAddr = 0;
static uint32_t s_OriginalOpcode   = 0x38000002;
static bool     s_OpcodeRecorded   = false;

static bool ApplyAVMFullRGBPatch(bool enable) {
    if (s_TargetPatchAddr == 0) {
        OSDynLoad_Module avmHandle = 0;
        if (OSDynLoad_Acquire("avm.rpl", &avmHandle) != 0 || avmHandle == 0) {
            LOG_I("Echec acquisition avm.rpl");
            return false;
        }

        void *pFunc = nullptr;
        if (OSDynLoad_FindExport(avmHandle, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVBufferAttr", &pFunc) != 0 || !pFunc) {
            LOG_I("Symbole AVMSetTVBufferAttr introuvable");
            OSDynLoad_Release(avmHandle);
            return false;
        }

        uint32_t baseFunc = reinterpret_cast<uint32_t>(pFunc);
        uint32_t candidateAddr = 0;
        int candidateCount = 0;

        for (uint32_t addr = baseFunc + 4; addr < baseFunc + 0x2000; addr += 4) {
            uint32_t val = DirectRead32(addr);
            if (val == 0x38000002 || val == 0x38000001) {
                uint32_t prev = DirectRead32(addr - 4);
                if (prev != 0x38800007) {
                    continue;
                }
                if (!LooksLikePlausibleFollowUp(addr + 4) || !LooksLikePlausibleFollowUp(addr + 8)) {
                    continue;
                }
                candidateAddr = addr;
                candidateCount++;
            }
        }

        OSDynLoad_Release(avmHandle);

        if (candidateCount == 0) {
            LOG_I("Signature Full RGB introuvable dans AVMSetTVBufferAttr");
            return false;
        }
        s_TargetPatchAddr = candidateAddr;
    }

    uint32_t currentOpcode = DirectRead32(s_TargetPatchAddr);
    if (currentOpcode != 0x38000001 && currentOpcode != 0x38000002) {
        LOG_I("Opcode critique inattendu a 0x%08X : 0x%08X", s_TargetPatchAddr, currentOpcode);
        return false;
    }

    if (!s_OpcodeRecorded) {
        s_OriginalOpcode = (currentOpcode == 0x38000001) ? 0x38000002 : currentOpcode;
        s_OpcodeRecorded = true;
    }

    uint32_t targetOpcode = enable ? 0x38000001 : s_OriginalOpcode;
    if (currentOpcode != targetOpcode) {
        return WriteCodeSafe32(s_TargetPatchAddr, targetOpcode);
    }
    return true;
}

// Neutralisation de l'écrasement Full RGB dans l'unique point d'écriture de tve.rpl (0x02016600)
static bool PatchTVEForceRGB(bool enable) {
    OSDynLoad_Module handleTVE = 0;
    if (OSDynLoad_Acquire("tve.rpl", &handleTVE) != 0 || handleTVE == 0) {
        LOG_I("Echec acquisition tve.rpl");
        return false;
    }

    void *pFunc = nullptr;
    if (OSDynLoad_FindExport(handleTVE, OS_DYNLOAD_EXPORT_FUNC, "TVESetForceRGBMode", &pFunc) != 0 || !pFunc) {
        LOG_I("Export TVESetForceRGBMode introuvable");
        OSDynLoad_Release(handleTVE);
        return false;
    }

    uint32_t exportAddr = reinterpret_cast<uint32_t>(pFunc);
    uint32_t op = DirectRead32(exportAddr);
    uint32_t targetAddr = exportAddr;

    // Décodage du saut vers le corps FUN_020165FC
    if ((op >> 26) == 18) {
        int32_t rel = static_cast<int32_t>((op & 0x03FFFFFC) << 6) >> 6;
        targetAddr = exportAddr + rel;
    }

    // Offset +4 : stw r3, 0x3b28(r12) -> stw r12, 0x3b28(r12)
    uint32_t writeOp = DirectRead32(targetAddr + 4);
    if ((writeOp & 0xFFFF0000) == 0x906C0000 || (writeOp & 0xFFFF0000) == 0x918C0000) {
        uint16_t offset = writeOp & 0xFFFF;
        uint32_t newOp = enable ? (0x918C0000 | offset) : (0x906C0000 | offset);
        if (writeOp != newOp) {
            WriteCodeSafe32(targetAddr + 4, newOp);
            LOG_I("tve.rpl verouille a 0x%08X (Opcode: 0x%08X)", targetAddr + 4, newOp);
        }
    }

    TVESetForceRGBMode_t pTVESetForceRGBMode = reinterpret_cast<TVESetForceRGBMode_t>(pFunc);
    pTVESetForceRGBMode(enable ? 1 : 0);

    OSDynLoad_Release(handleTVE);
    return true;
}

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
    {"1080p (60Hz)", AVM_TV_RESOLUTION_1080P}
};

static constexpr size_t g_AdvResolutionsCount = sizeof(g_AdvResolutions) / sizeof(g_AdvResolutions[0]);
static WUPSConfigItemMultipleValues::ValuePair s_AdvResValuePairs[g_AdvResolutionsCount];

static void BuildAdvResValuePairs() {
    for (size_t i = 0; i < g_AdvResolutionsCount; ++i) {
        s_AdvResValuePairs[i].value = static_cast<uint32_t>(i);
        s_AdvResValuePairs[i].name  = g_AdvResolutions[i].name;
    }
}

static constexpr TVEPort g_PortHardwareMap[] = {
    (TVEPort) 2, // Composite
    (TVEPort) 3, // SCART
    (TVEPort) 1, // Component
    (TVEPort) 0  // HDMI
};

static uint32_t s_SelectedStdRes          = STD_RES_1080P;
static uint32_t s_SelectedRGB             = RGB_STARTUP_ENABLED;
static uint32_t s_SelectedConfigMode      = MODE_STANDARD;
static uint32_t s_SelectedAutobootRegInit = AUTOBOOT_REGINIT_ENABLED;

static uint32_t s_SelectedAdvRegion = 1;
static uint32_t s_SelectedAdvPort   = 3;
static uint32_t s_SelectedAdvResIdx = 11;
static uint32_t s_SelectedAdvAspect = 1;
static uint32_t s_SelectedTileMode  = 1;

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
    {STD_RES_480P,  "480p"},
    {STD_RES_720P,  "720p"},
    {STD_RES_1080I, "1080i"},
    {STD_RES_1080P, "1080p"},
};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_RGBStartupList[] = {
    {RGB_STARTUP_DISABLED, "Disabled (Limited 16-235)"},
    {RGB_STARTUP_ENABLED,  "Enabled (Full RGB 0-255)"},
};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_ConfigModeList[] = {
    {MODE_STANDARD,           "Standard"},
    {MODE_VIDEO_MODE_CHANGER, "Video Mode Changer"},
};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_AutobootRegInitList[] = {
    {AUTOBOOT_REGINIT_DISABLED, "Disabled"},
    {AUTOBOOT_REGINIT_ENABLED,  "Enabled"},
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

static constexpr WUPSConfigItemMultipleValues::ValuePair s_AdvAspectList[] = {
    {0, "4:3"},
    {1, "16:9"},
};

static constexpr WUPSConfigItemMultipleValues::ValuePair s_AdvTileList[] = {
    {0, "Disabled"},
    {1, "Enabled (Fix Illegible Screen)"},
};

static void LoadConfig() {
    int32_t val = 0;
    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "autoboot_reg_init", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedAutobootRegInit = (val == 0) ? AUTOBOOT_REGINIT_DISABLED : AUTOBOOT_REGINIT_ENABLED;
    }
    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "std_res", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedStdRes = (val >= 0 && val <= 3) ? static_cast<uint32_t>(val) : STD_RES_1080P;
    }
    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "rgb_mode", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedRGB = (val == 0) ? RGB_STARTUP_DISABLED : RGB_STARTUP_ENABLED;
    }
    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "config_mode", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedConfigMode = (val == 1) ? MODE_VIDEO_MODE_CHANGER : MODE_STANDARD;
    }
    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "vmc_region", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedAdvRegion = (val == 0) ? 0 : 1;
    }
    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "vmc_port", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedAdvPort = (val >= 0 && val <= 3) ? static_cast<uint32_t>(val) : 3;
    }
    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "vmc_res", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        uint32_t maxIdx = static_cast<uint32_t>(g_AdvResolutionsCount) - 1;
        s_SelectedAdvResIdx = (val >= 0 && static_cast<uint32_t>(val) <= maxIdx) ? static_cast<uint32_t>(val) : maxIdx;
    }
    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "vmc_aspect", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedAdvAspect = (val == 0) ? 0 : 1;
    }
    if (WUPSStorageAPI_GetInt(WUPS_STORAGE_ROOT_ITEM, "vmc_tile", &val) == WUPS_STORAGE_ERROR_SUCCESS) {
        s_SelectedTileMode = (val == 0) ? 0 : 1;
    }
    LOG_I("Configuration chargee.");
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
    LOG_I("Configuration sauvegardee.");
}

static uint32_t CalculateTargetScanMode() {
    switch (s_SelectedStdRes) {
        case STD_RES_480P:  return 3;
        case STD_RES_720P:  return 4;
        case STD_RES_1080I: return 6;
        case STD_RES_1080P:
        default:            return 7;
    }
}

static void ApplyAdvancedVideoMode(OSDynLoad_Module handleAVM) {
    AVMSetTVTileMode_t pTileMode       = nullptr;
    AVMSetTVVideoRegion_t pVideoReg    = nullptr;
    AVMSetTVOutPort_t pOutPort         = nullptr;
    AVMSetTVScanResolution_t pScanRes  = nullptr;
    AVMSetTVAspectRatio_t pAspect      = nullptr;

    OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVTileMode", reinterpret_cast<void **>(&pTileMode));
    OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVVideoRegion", reinterpret_cast<void **>(&pVideoReg));
    OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVOutPort", reinterpret_cast<void **>(&pOutPort));
    OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVScanResolution", reinterpret_cast<void **>(&pScanRes));
    OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVAspectRatio", reinterpret_cast<void **>(&pAspect));

    AVMTvResolution targetRes  = g_AdvResolutions[s_SelectedAdvResIdx].value;
    TVEPort targetPort         = g_PortHardwareMap[s_SelectedAdvPort];
    AVMTvVideoRegion targetReg = (s_SelectedAdvRegion == 1) ? AVM_TV_VIDEO_REGION_NTSC : AVM_TV_VIDEO_REGION_PAL;

    if (pVideoReg) pVideoReg(targetReg, targetPort, targetRes);
    else if (pOutPort) pOutPort(targetPort, targetRes);
    else if (pScanRes) pScanRes(targetRes);

    if (pAspect) pAspect(static_cast<AVMTvAspectRatio>(s_SelectedAdvAspect));
    if (pTileMode) pTileMode(static_cast<uint8_t>(s_SelectedTileMode));
}

static void ApplyVideoSettingsDirect(bool forceReginit = false) {
    bool enableRGB = (s_SelectedRGB == RGB_STARTUP_ENABLED);
    ApplyAVMFullRGBPatch(enableRGB);
    PatchTVEForceRGB(enableRGB);

    OSDynLoad_Module handleAVM = 0;
    if (OSDynLoad_Acquire("avm.rpl", &handleAVM) != 0 || handleAVM == 0) {
        return;
    }

    if (s_SelectedConfigMode == MODE_STANDARD) {
        uint32_t targetMode = CalculateTargetScanMode();
        AVMGetTVScanMode_t pAVMGetTVScanMode = nullptr;
        AVMSetTVScanMode_t pAVMSetTVScanMode = nullptr;

        OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMGetTVScanMode", reinterpret_cast<void **>(&pAVMGetTVScanMode));
        OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVScanMode", reinterpret_cast<void **>(&pAVMSetTVScanMode));

        if (pAVMSetTVScanMode) {
            uint32_t currentMode = 0xFFFFFFFF;
            bool needSwitch = forceReginit;
            if (!needSwitch && pAVMGetTVScanMode && pAVMGetTVScanMode(&currentMode) == 0) {
                needSwitch = (currentMode != targetMode);
            }

            if (needSwitch) {
                pAVMSetTVScanMode(targetMode);
                LOG_I("AVMSetTVScanMode(%u) execute", targetMode);
            }
        }
    } else {
        ApplyAdvancedVideoMode(handleAVM);
    }

    OSDynLoad_Release(handleAVM);
}

static inline bool IsWiiUMenuTitle() {
    uint64_t tid = OSGetTitleID();
    return (tid == 0x0005001010040200ull ||
            tid == 0x0005001010040100ull ||
            tid == 0x0005001010040000ull);
}

static OSThread s_AutobootThread;
static uint8_t  s_AutobootStack[0x8000] __attribute__((aligned(32)));
static volatile bool s_AutobootCancelled     = false;
static volatile bool s_AutobootThreadCreated = false;
static bool          s_ColdBootExecuted      = false;

static int AutobootThreadEntry(int argc, const char **argv) {
    OSSleepTicks(OSMillisecondsToTicks(4500));
    if (s_AutobootCancelled || !IsWiiUMenuTitle() || s_ColdBootExecuted) {
        return 0;
    }

    OSDynLoad_Module handleAVM = 0;
    if (OSDynLoad_Acquire("avm.rpl", &handleAVM) == 0 && handleAVM != 0) {
        AVMIsAVOutReady_t pIsReady = nullptr;
        OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMIsAVOutReady", reinterpret_cast<void **>(&pIsReady));
        int retries = 0;
        while (pIsReady && !pIsReady() && retries < 10 && !s_AutobootCancelled) {
            OSSleepTicks(OSMillisecondsToTicks(500));
            retries++;
        }
        OSDynLoad_Release(handleAVM);
    }

    if (s_AutobootCancelled || !IsWiiUMenuTitle() || s_ColdBootExecuted) {
        return 0;
    }

    if (s_SelectedAutobootRegInit == AUTOBOOT_REGINIT_ENABLED) {
        s_ColdBootExecuted = true;
        ApplyVideoSettingsDirect(true);
        LOG_I("Autoboot initial execute avec succes.");
    }
    return 0;
}

static void CleanupAutobootThread() {
    if (s_AutobootThreadCreated) {
        s_AutobootCancelled = true;
        OSDetachThread(&s_AutobootThread);
        s_AutobootThreadCreated = false;
    }
}

void stdResChanged(ConfigItemMultipleValues *, uint32_t val) {
    s_SelectedStdRes     = val;
    s_SelectedConfigMode = MODE_STANDARD;
}
void rgbStartupChanged(ConfigItemMultipleValues *, uint32_t val) { s_SelectedRGB = val; }
void configModeChanged(ConfigItemMultipleValues *, uint32_t val) { s_SelectedConfigMode = val; }
void autobootRegInitChanged(ConfigItemMultipleValues *, uint32_t val) { s_SelectedAutobootRegInit = val; }

void advRegionChanged(ConfigItemMultipleValues *, uint32_t val) {
    s_SelectedAdvRegion  = val;
    s_SelectedConfigMode = MODE_VIDEO_MODE_CHANGER;
}
void advPortChanged(ConfigItemMultipleValues *, uint32_t val) {
    s_SelectedAdvPort    = val;
    s_SelectedConfigMode = MODE_VIDEO_MODE_CHANGER;
}
void advResChanged(ConfigItemMultipleValues *, uint32_t val) {
    s_SelectedAdvResIdx  = val;
    s_SelectedConfigMode = MODE_VIDEO_MODE_CHANGER;
}
void advAspectChanged(ConfigItemMultipleValues *, uint32_t val) {
    s_SelectedAdvAspect  = val;
    s_SelectedConfigMode = MODE_VIDEO_MODE_CHANGER;
}
void advTileChanged(ConfigItemMultipleValues *, uint32_t val) {
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
        root.add(WUPSConfigItemMultipleValues::CreateFromValue("std_res", "Standard Resolution", (uint32_t) STD_RES_1080P, s_SelectedStdRes, s_StdResolutionsList, stdResChanged));
        root.add(WUPSConfigItemMultipleValues::CreateFromValue("rgb_mode", "Color Range Mode", (uint32_t) RGB_STARTUP_ENABLED, s_SelectedRGB, s_RGBStartupList, rgbStartupChanged));
        root.add(WUPSConfigItemMultipleValues::CreateFromValue("config_mode", "Mode Selector", (uint32_t) MODE_STANDARD, s_SelectedConfigMode, s_ConfigModeList, configModeChanged));
        root.add(WUPSConfigItemMultipleValues::CreateFromValue("autoboot_reg_init", "Autoboot", (uint32_t) AUTOBOOT_REGINIT_ENABLED, s_SelectedAutobootRegInit, s_AutobootRegInitList, autobootRegInitChanged));

        WUPSConfigCategory vmcCategory = WUPSConfigCategory::Create("Video Mode Changer");
        vmcCategory.add(WUPSConfigItemMultipleValues::CreateFromValue("vmc_region", "Video Region", (uint32_t) 1, s_SelectedAdvRegion, s_AdvRegionList, advRegionChanged));
        vmcCategory.add(WUPSConfigItemMultipleValues::CreateFromValue("vmc_port", "Output Port", (uint32_t) 3, s_SelectedAdvPort, s_AdvPortList, advPortChanged));
        vmcCategory.add(WUPSConfigItemMultipleValues::CreateFromValue("vmc_res", "Resolution & Refresh Rate", (uint32_t) 11, s_SelectedAdvResIdx, s_AdvResValuePairs, advResChanged));
        vmcCategory.add(WUPSConfigItemMultipleValues::CreateFromValue("vmc_aspect", "Aspect Ratio", (uint32_t) 1, s_SelectedAdvAspect, s_AdvAspectList, advAspectChanged));
        vmcCategory.add(WUPSConfigItemMultipleValues::CreateFromValue("vmc_tile", "Tile Mode Fix", (uint32_t) 1, s_SelectedTileMode, s_AdvTileList, advTileChanged));
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
    if (!configChanged) return;

    SaveConfig();
    if (videoChanged) {
        ApplyVideoSettingsDirect(true);
    }
}

INITIALIZE_PLUGIN() {
    WHBLogUdpInit();
    LOG_I("Plugin Full RGB v2.0.0 initialise.");
    BuildAdvResValuePairs();
    LoadConfig();

    bool enableRGB = (s_SelectedRGB == RGB_STARTUP_ENABLED);
    ApplyAVMFullRGBPatch(enableRGB);
    PatchTVEForceRGB(enableRGB);

    WUPSConfigAPIOptionsV1 configOptions = {};
    configOptions.name                   = "Full RGB TV";
    WUPSConfigAPI_Init(configOptions, ConfigMenuOpenedCallback, ConfigMenuClosedCallback);
}

DEINITIALIZE_PLUGIN() {
    CleanupAutobootThread();
    WHBLogUdpDeinit();
}

ON_APPLICATION_START() {
    // Réinitialise le cache pour trouver dynamiquement AVMSetTVBufferAttr dans la nouvelle application
    s_TargetPatchAddr = 0;
    s_OpcodeRecorded  = false;

    bool enableRGB = (s_SelectedRGB == RGB_STARTUP_ENABLED);
    ApplyAVMFullRGBPatch(enableRGB);
    PatchTVEForceRGB(enableRGB);

    // L'autoboot reginit ne tourne qu'une seule fois au premier démarrage de la console
    if (IsWiiUMenuTitle() && !s_ColdBootExecuted) {
        s_AutobootCancelled = false;
        if (s_SelectedAutobootRegInit == AUTOBOOT_REGINIT_ENABLED) {
            BOOL created = OSCreateThread(&s_AutobootThread, AutobootThreadEntry, 0, nullptr,
                                          reinterpret_cast<uint8_t *>(s_AutobootStack) + sizeof(s_AutobootStack),
                                          sizeof(s_AutobootStack), 16, OS_THREAD_ATTRIB_AFFINITY_CPU2);
            if (created) {
                s_AutobootThreadCreated = true;
                OSResumeThread(&s_AutobootThread);
            }
        }
    }
}

ON_APPLICATION_ENDS() {
    s_AutobootCancelled = true;
    CleanupAutobootThread();
}

ON_APPLICATION_REQUESTS_EXIT() {
    s_AutobootCancelled = true;
    CleanupAutobootThread();
}