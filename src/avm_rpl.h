#ifndef AVM_RPL_H
#define AVM_RPL_H

#include <stdint.h>

#define AVM_CONFIG_OFFSET_RGB_RANGE 0x0C
#define AVM_TEXT_OFFSET_WRITE_FLAG  0x0200165C

typedef enum {
    AVM_RGB_RANGE_FULL    = 1, // 0-255
    AVM_RGB_RANGE_LIMITED = 2  // 16-235
} AVM_RGBRange;

// Structure simplifiée du bloc AVM (Taille unitaire : 0x14 octets)
typedef struct __attribute__((packed)) {
    uint32_t field0_0x0;       // 0x00
    uint32_t field1_0x4;       // 0x04
    uint32_t field2_0x8;       // 0x08
    uint32_t color_range_flag; // 0x0C : 1 = Full RGB, 2 = Limited
    uint8_t reserved[4];       // 0x10 - 0x13
} AVMVideoConfig;

#endif // AVM_RPL_H