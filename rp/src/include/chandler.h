/**
 * File: chandler.h
 * Author: Diego Parrilla Santamaría
 * Date: November 2023 - April 2025
 * Copyright: 2023 2025 - GOODDATA LABS SL
 * Description: Header file for the Command Handler C program.
 */

#ifndef CHANDLER_H
#define CHANDLER_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "constants.h"
#include "debug.h"
#include "memfunc.h"
#include "pico/stdlib.h"
#include "tprotocol.h"

#define CHANDLER_ADDRESS_HIGH_BIT 0x8000  // High bit of the address
#define CHANDLER_PARAMETERS_MAX_SIZE 20  // Max size of the parameters for debug

#define CHANDLER_RANDOM_TOKEN_OFFSET (0x08200)  // Offset from 0x8200
#define CHANDLER_RANDOM_TOKEN_SEED_OFFSET (CHANDLER_RANDOM_TOKEN_OFFSET + 4)
#define CHANDLER_SHARED_VARIABLES_OFFSET (CHANDLER_RANDOM_TOKEN_SEED_OFFSET + 4)

// Index for the common shared variables
#define CHANDLER_HARDWARE_TYPE 0
#define CHANDLER_SVERSION 1
#define CHANDLER_BUFFER_TYPE 2

// 0x8200 ┌────────────────────────────────────────────┐
//        │ CHANDLER_RANDOM_TOKEN_OFFSET               │
//        │   offset 0x0000, size 4 bytes              │
// 0x8204 ├────────────────────────────────────────────┤
//        │ CHANDLER_RANDOM_TOKEN_SEED_OFFSET          │
//        │   offset 0x0004, size 4 bytes              │
// 0x8208 ├────────────────────────────────────────────┤
//        │ CHANDLER_SHARED_VARIABLES_OFFSET Start     │
//        │   offset 0x0008                            |
//        │  CHANDLER_HARDWARE_TYPE, size 4 bytes      │
// 0x820C ├────────────────────────────────────────────┤
//        │ CHANDLER_SVERSION, size 4 bytes            │
//        │   offset 0x000C                            │
// 0x8210 ├────────────────────────────────────────────┤
//        │ CHANDLER_BUFFER_TYPE, size 4 bytes         │
//        │   offset 0x0010                            │
// 0x8214 ├────────────────────────────────────────────┤

// Callback function type
typedef void (*CommandCallback)(TransmissionProtocol *protocol,
                                uint16_t *payloadPtr);

// Node for linked list of callbacks
typedef struct CommandCallbackNode {
  CommandCallback cb;
  struct CommandCallbackNode *next;
} CommandCallbackNode;

// Function Prototypes
void chandler_init();
void __not_in_flash_func(chandler_loop)();

void __not_in_flash_func(chandler_addCB)(CommandCallback cb);

#endif  // CHANDLER_H
