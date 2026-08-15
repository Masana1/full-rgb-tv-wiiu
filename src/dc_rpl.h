#ifndef DC_RPL_H
#define DC_RPL_H

#include <stdint.h>

#define DC_TOTAL_VIDEO_MODES     26
#define DC_MODE_STRUCT_SIZE      0x3C
#define DC_MODE_OFFSET_RGB_RANGE 0x1C

// Structure d'une entrée de mode vidéo (60 octets / 0x3C)
typedef struct __attribute__((packed)) {
    uint16_t h_active;          // 0x00
    uint16_t v_active;          // 0x02
    uint32_t refresh_rate;      // 0x04
    uint32_t interlaced;        // 0x08
    uint32_t aspect_ratio;      // 0x0C
    uint32_t vic_code;          // 0x10
    uint32_t pixel_clock;       // 0x14
    uint32_t reserved_18;       // 0x18
    uint32_t rgb_range;         // 0x1C : 1 = Full (0-255), 2 = Limited (16-235)
    uint32_t csc_mode;          // 0x20
    uint32_t avi_infoframe;     // 0x24
    uint8_t hdmi_registers[20]; // 0x28 - 0x3B
} DCVideoMode;

// Pointeur de fonction vers FUN_02007078 (DCApplyHDMIConfig)
typedef int (*DCApplyHDMIConfig_t)(void *crtcContext, int updateFlags);

#endif // DC_RPL_H