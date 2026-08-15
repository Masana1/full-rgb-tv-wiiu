#include <coreinit/cache.h>
#include <coreinit/dynload.h>
#include <coreinit/memory.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/title.h>
#include <cstdint>
#include <cstring>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <wups.h>
#include <wups/config/WUPSConfigCategory.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/config_api.h>

WUPS_PLUGIN_NAME("Activated Full RGB TV");
WUPS_PLUGIN_DESCRIPTION("Full RGB 11 Patches");
WUPS_PLUGIN_VERSION("v1.0.0-beta.1");
WUPS_PLUGIN_AUTHOR("Masana");
WUPS_PLUGIN_LICENSE("GPLv3");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("full_rgb_TV");

#define LOG(fmt, ...)        WHBLogPrintf("[FULL_RGB_TV] " fmt "\n", ##__VA_ARGS__)

#define ADDR_DC_FUN_0200210C 0x0110F710

extern "C" uint32_t OSEffectiveToPhysical(uint32_t addr);

// Structure de patch mémoire
typedef struct {
    uint32_t addr;
    uint32_t patchVal;
    uint32_t standardVal; // Valeur d'origine (Standard RGB / Limited) pour le mode False
    const char *desc;
} MemoryPatch;

// ============================================================================
// TABLE DES 11 PATCHS MÂTRES AVEC VALEURS D'ORIGINE POUR LE FALSE
// ============================================================================
static const MemoryPatch g_MasterPatches[] = {
        // --- 1. AVM.RPL ---
        {0x00FB6684, 0x38E00001, 0x38E00000, "AVM Color Config Force Full #1"},
        {0x00FE5F2C, 0x38E00001, 0x38E00000, "AVM Color Config Force Full #2"},

        // --- 2. TVE.RPL ---
        {0x010AFC64, 0x38000001, 0x38000000, "TVE Handle 10 Write Force RGB"},
        {0x0115248C, 0x60000000, 0x41820008, "TVE Bypass 1080p Skip (nop vs branch)"},
        {0x01152490, 0x3980002A, 0x38800010, "TVE HDMI AVI InfoFrame PB3 Full Range"},
        {0x0101AD88, 0x39800002, 0x39800001, "TVE Quantization Mode Override #1"},
        {0x01022F80, 0x39800002, 0x39800001, "TVE Quantization Mode Override #2"},

        // --- 3. DC.RPL (GPU LATTE CSC BYPASS) ---
        {0x01112CEC, 0x38A00000, 0x7CA52278, "DC GPU CSC Control Reg 0x18EC Bypass #1"},
        {0x01112CFC, 0x38A00000, 0x7CA52278, "DC GPU CSC Matrix Reg 0x18E0 Bypass #1"},
        {0x01112F38, 0x38A00000, 0x7CA52278, "DC GPU CSC Reg 0x18EC Bypass #2"},
        {0x01112F48, 0x38A00000, 0x7CA52278, "DC GPU CSC Matrix Reg 0x18E0 Bypass #2"}};

#define TOTAL_PATCHES (sizeof(g_MasterPatches) / sizeof(MemoryPatch))

typedef int32_t (*AVMSetTVScanMode_t)(uint32_t mode);
static AVMSetTVScanMode_t pAVMSetTVScanMode = nullptr;

typedef int32_t (*TVESetAll_t)(uint32_t handle, void *config);
static TVESetAll_t real_TVESetAll = nullptr;

static volatile bool s_ThreadRegistered = false;

enum ResolutionOptions {
    RES_1080P = 0,
    RES_720P  = 1,
    RES_1080I = 2,
    RES_480P  = 3,
};

#define RES_STORAGE_KEY   "selected_resolution"
#define RGB_STORAGE_KEY   "full_rgb_enabled"
#define DEFAULT_RES_IDX   RES_1080P
#define DEFAULT_RGB_STATE true

static ResolutionOptions s_SelectedResIdx = DEFAULT_RES_IDX;
static bool s_FullRGBEnabled              = DEFAULT_RGB_STATE;
static const uint32_t s_ScanModes[]       = {7, 4, 6, 3}; // 7=1080p, 4=720p, 6=1080i, 3=480p

// Prototypes
static bool Apply11PatchesInRAM(bool enable);
static void TriggerAVMNativeReInit(uint32_t scanMode);
int32_t Hook_TVESetAll(uint32_t handle, void *config);

// ============================================================================
// ACCÈS MÉMOIRE UNCACHED PHYSIQUE
// ============================================================================
static bool Write32Uncached(uint32_t addr, uint32_t val) {
    uint32_t phys = OSEffectiveToPhysical(addr);
    if (!phys) return false;
    volatile uint32_t *uncachedPtr = (volatile uint32_t *) (0x30000000 | phys);
    *uncachedPtr                   = val;
    DCFlushRange((void *) addr, 4);
    ICInvalidateRange((void *) addr, 4);
    return true;
}

static uint32_t Read32Uncached(uint32_t addr) {
    uint32_t phys = OSEffectiveToPhysical(addr);
    if (!phys) return 0;
    volatile uint32_t *uncachedPtr = (volatile uint32_t *) (0x30000000 | phys);
    return *uncachedPtr;
}

// ============================================================================
// DYNLOAD AVM NATIVE RE-INIT (CHANGEMENT DE RÉSOLUTION À LA VOLÉE)
// ============================================================================
static void TriggerAVMNativeReInit(uint32_t scanMode) {
    OSDynLoad_Module handleAVM = 0;
    if (OSDynLoad_Acquire("avm.rpl", &handleAVM) == 0) {
        if (OSDynLoad_FindExport(handleAVM, OS_DYNLOAD_EXPORT_FUNC, "AVMSetTVScanMode", (void **) &pAVMSetTVScanMode) == 0 && pAVMSetTVScanMode) {

            uint32_t *fnPtr = (uint32_t *) pAVMSetTVScanMode;
            for (int i = 0; i < 20; i++) {
                uint32_t inst = Read32Uncached((uint32_t) &fnPtr[i]);
                if ((inst & 0xFC000000) == 0x40000000) {
                    Write32Uncached((uint32_t) &fnPtr[i], 0x60000000); // NOP
                }
            }
            LOG("⚡ [AVM ON-THE-FLY] Application de AVMSetTVScanMode(%u)...", scanMode);
            pAVMSetTVScanMode(scanMode);
        }
    }
}

// ============================================================================
// HOOK NATIVE DE TVESetAll
// ============================================================================
int32_t Hook_TVESetAll(uint32_t handle, void *config) {
    Apply11PatchesInRAM(s_FullRGBEnabled);
    if (real_TVESetAll) {
        return real_TVESetAll(handle, config);
    }
    return 0;
}

// ============================================================================
// APPLICATION OU RESTAURATION DES PATCHS EN RAM (TRUE / FALSE)
// ============================================================================
static bool Apply11PatchesInRAM(bool enable) {
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
// WATCHDOG THREAD (MAINTIEN CONTINU)
// ============================================================================
static OSThread s_WatchdogThread;
static uint8_t s_WatchdogThreadStack[0x4000] __attribute__((aligned(16)));

int WatchdogThreadFunc(int argc, const char **argv) {
    while (s_ThreadRegistered) {
        OSSleepTicks(OSMillisecondsToTicks(250));
        if (s_FullRGBEnabled) {
            if (Read32Uncached(0x00FB6684) != 0x38E00001 || Read32Uncached(0x010AFC64) != 0x38000001) {
                Apply11PatchesInRAM(true);
            }
        }
    }
    return 0;
}

// ============================================================================
// CALLBACKS DU MENU DE CONFIGURATION AROMA
// ============================================================================
void multipleValueItemChanged(ConfigItemMultipleValues *item, uint32_t newValue) {
    s_SelectedResIdx = (ResolutionOptions) newValue;
    LOG("--> [AROMA MENU] Résolution sélectionnée temporairement : index %u", newValue);
}

void boolItemChanged(ConfigItemBoolean *item, bool newValue) {
    s_FullRGBEnabled = newValue;
    LOG("--> [AROMA MENU] Full RGB basculé à temporairement : %d", newValue);
}

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle) {
    WUPSConfigCategory root = WUPSConfigCategory(rootHandle);
    try {
        constexpr WUPSConfigItemMultipleValues::ValuePair possibleValues[] = {
                {RES_1080P, "1080p (Progressif 60Hz)"},
                {RES_720P, "720p (Progressif 60Hz)"},
                {RES_1080I, "1080i (Entrelacé 60Hz)"},
                {RES_480P, "480p"},
        };
        // Chargement des valeurs actuelles depuis le stockage
        WUPSStorageAPI::GetOrStoreDefault(RES_STORAGE_KEY, s_SelectedResIdx, DEFAULT_RES_IDX);
        WUPSStorageAPI::GetOrStoreDefault(RGB_STORAGE_KEY, s_FullRGBEnabled, DEFAULT_RGB_STATE);

        // 1. Bouton True / False pour activer/désactiver le Full RGB
        root.add(WUPSConfigItemBoolean::Create(
                RGB_STORAGE_KEY, "Activer Full RGB",
                DEFAULT_RGB_STATE, s_FullRGBEnabled,
                boolItemChanged));
        // 2. Sélecteur de résolution
        root.add(WUPSConfigItemMultipleValues::CreateFromValue(
                RES_STORAGE_KEY, "Résolution Cible",
                DEFAULT_RES_IDX, s_SelectedResIdx,
                possibleValues,
                multipleValueItemChanged));
    } catch (std::exception &e) {
        LOG("Erreur création menu config: %s", e.what());
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }
    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosedCallback() {
    LOG("--> [AROMA MENU] Menu fermé. Sauvegarde et application instantanée...");

    // Sauvegarde persistante des choix
    WUPSStorageAPI::Store(RES_STORAGE_KEY, s_SelectedResIdx);
    WUPSStorageAPI::Store(RGB_STORAGE_KEY, s_FullRGBEnabled);
    WUPSStorageAPI::SaveStorage();

    // Applique l'état (True active les patchs, False restaure les valeurs d'origine)
    Apply11PatchesInRAM(s_FullRGBEnabled);
    if (s_FullRGBEnabled) {
        uint32_t targetMode = s_ScanModes[s_SelectedResIdx];
        TriggerAVMNativeReInit(targetMode); // Applique le changement de résolution à la volée
    }
}

// ============================================================================
// ÉVÉNEMENTS PLUGIN AROMA
// ============================================================================
INITIALIZE_PLUGIN() {
    WHBLogUdpInit();
    LOG("Plugin Activated Full RGB v4.5 Initialisé (Passif au démarrage).");
    WUPSStorageAPI::GetOrStoreDefault(RES_STORAGE_KEY, s_SelectedResIdx, DEFAULT_RES_IDX);
    WUPSStorageAPI::GetOrStoreDefault(RGB_STORAGE_KEY, s_FullRGBEnabled, DEFAULT_RGB_STATE);
    WUPSStorageAPI::SaveStorage();

    WUPSConfigAPIOptionsV1 configOptions = {.name = "Full RGB TV"};
    if (WUPSConfigAPI_Init(configOptions, ConfigMenuOpenedCallback, ConfigMenuClosedCallback) != WUPSCONFIG_API_RESULT_SUCCESS) {
        LOG("Erreur lors de l'initialisation de WUPSConfigAPI !");
    }
}

DEINITIALIZE_PLUGIN() {
    WUPSStorageAPI::SaveStorage();
    WHBLogUdpDeinit();
}

ON_APPLICATION_START() {
    // Applique l'état stocké au lancement d'application
    Apply11PatchesInRAM(s_FullRGBEnabled);
    s_ThreadRegistered = true;
    OSCreateThread(&s_WatchdogThread, WatchdogThreadFunc, 0, nullptr,
                   s_WatchdogThreadStack + sizeof(s_WatchdogThreadStack),
                   sizeof(s_WatchdogThreadStack), 16, OS_THREAD_ATTRIB_AFFINITY_ANY);
    OSResumeThread(&s_WatchdogThread);
}

ON_APPLICATION_ENDS() {
    s_ThreadRegistered = false;
    OSJoinThread(&s_WatchdogThread, nullptr);
}

ON_APPLICATION_REQUESTS_EXIT() {}