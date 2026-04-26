/**
 * File: commemul.h
 * Author: Diego Parrilla Santamaría
 * Date: March 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: ROM3 communication emulator backed by a DMA ring buffer.
 */

#ifndef COMMEMUL_H
#define COMMEMUL_H

#include <inttypes.h>
#include <stdbool.h>

#include "pico/stdlib.h"

typedef void (*CommEmulSampleCallback)(uint16_t sample);

void commemul_init(void);
void __not_in_flash_func(commemul_poll)(CommEmulSampleCallback callback);

#endif  // COMMEMUL_H
