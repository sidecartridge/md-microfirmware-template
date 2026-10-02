/**
 * File: chandler.c
 * Author: Diego Parrilla Santamaría
 * Date: November 2023-April 2025
 * Copyright: 2023 2025 - GOODDATA LABS SL
 * Description: Command Handler for the SidecarTridge protocol
 */

#include "chandler.h"

#include <stdlib.h>
#include <string.h>

#include "commemul.h"

static TransmissionProtocol pendingProtocol;
static bool protocolPending = false;
// The pending command came from chandler_injectProtocol(), not the ST.
static bool pendingInjected = false;

// The ST's CHANDLER_ST_HELLO: seen since this RP started, and not yet
// consumed by the app.
static bool stPresent = false;
static bool stBootPending = false;
// A CHANDLER_SET_SHARED_VAR not yet consumed by the app.
static bool sharedVarSetPending = false;

static uint32_t incrementalCmdCount = 0;

// Counters of the ST's synchronous handshake, plain globals so that
// tools/dev/swd.py can read them by symbol on a release build too:
//   chandlerHandled         commands answered
//   chandlerDropped         commands thrown away because one was pending
//   chandlerRepeated        commands with the previous one's token: the ST
//                           resending after its timeout
//   chandlerChecksumErrors  frames that failed their checksum
//   chandlerBusyUs          time from seeing a command to answering it
//   chandlerGapUs           time from answering one command to seeing the
//                           next: the ST's turnaround
//   chandlerQuietUs         of that gap, the time until the ST's next sample
//                           arrives at all
//   chandlerFramePolls      main-loop passes a frame spans, first sample to
//                           last: a slow sender against slow draining
//   chandlerPollUs          time spent draining the capture ring
//   chandlerInjected        commands the probe injected (debug builds),
//                           counted apart from all of the above: they run
//                           their callbacks but get no answer, since the ST
//                           never sent them
// with the largest single busy, gap and quiet times alongside.
uint32_t chandlerHandled = 0;
uint32_t chandlerDropped = 0;
uint32_t chandlerRepeated = 0;
uint32_t chandlerChecksumErrors = 0;
uint32_t chandlerBusyUs = 0;
uint32_t chandlerMaxBusyUs = 0;
uint32_t chandlerGapUs = 0;
uint32_t chandlerMaxGapUs = 0;
uint32_t chandlerQuietUs = 0;
uint32_t chandlerMaxQuietUs = 0;
uint32_t chandlerFramePolls = 0;
uint32_t chandlerPollUs = 0;
uint32_t chandlerInjected = 0;
static bool chandlerFrameInFlight = false;
static bool chandlerAwaitingFirstSample = false;
static uint32_t chandlerLastToken = 0;
static uint32_t chandlerAnsweredAtUs = 0;

// Address of the random-token reply slot (chandler_loop publishes the
// 64-bit { incrementalCmdCount | randomToken } value here so the m68k's
// send_sync poll wakes up).
static uint32_t memoryRandomTokenAddress = 0;

// Head of the callback list and current count (capped at
// CHANDLER_MAX_CALLBACKS).
static CommandCallbackNode *callbackListHead = NULL;
static unsigned int callbackListCount = 0;

static inline void __not_in_flash_func(chandler_clear_pending_protocol)(void) {
  pendingProtocol.command_id = 0;
  pendingProtocol.payload_size = 0;
  pendingProtocol.bytes_read = 0;
  pendingProtocol.final_checksum = 0;
  protocolPending = false;
  pendingInjected = false;
}

static inline bool __not_in_flash_func(chandler_protocol_matches_pending)(
    const TransmissionProtocol *protocol, uint16_t size) {
  if (!protocolPending) {
    return false;
  }

  if ((pendingProtocol.command_id != protocol->command_id) ||
      (pendingProtocol.payload_size != protocol->payload_size) ||
      (pendingProtocol.final_checksum != protocol->final_checksum)) {
    return false;
  }

  return memcmp(pendingProtocol.payload, protocol->payload, size) == 0;
}

void __not_in_flash_func(chandler_init)() {
  DPRINTF("Initializing Command Handler...\n");

  uint32_t shared_base = (unsigned int)&__rom_in_ram_start__;
  memoryRandomTokenAddress = shared_base + CHANDLER_RANDOM_TOKEN_OFFSET;

  // Seed the random-token slots with non-zero values before the first
  // m68k command runs. send_sync_command_to_sidecart reads
  // RANDOM_TOKEN_SEED_ADDR to derive its request token; on a cold boot
  // that slot would otherwise be whatever the RP RAM happened to contain.
  uint64_t boot_us = time_us_64();
  uint32_t seed = (uint32_t)(boot_us ^ (boot_us >> 32));
  if (seed == 0) {
    seed = 0xA1F0C0DEu;
  }
  TPROTO_SET_RANDOM_TOKEN(memoryRandomTokenAddress, seed);
  TPROTO_SET_RANDOM_TOKEN(shared_base + CHANDLER_RANDOM_TOKEN_SEED_OFFSET,
                          seed ^ 0xDEADBEEFu);

  // Zero the reserved 4-byte slot so apps can rely on a known initial
  // value if the framework later claims it for something concrete.
  *((volatile uint32_t *)(shared_base + CHANDLER_RESERVED_OFFSET)) = 0;

  chandler_clear_pending_protocol();
}

/**
 * @brief Register a callback
 *
 * Adds the callback to the linked list of callbacks.
 *
 * @param cb The callback function to register.
 */
void __not_in_flash_func(chandler_addCB)(CommandCallback cb) {
  if (!cb) return;
  if (callbackListCount >= CHANDLER_MAX_CALLBACKS) {
    DPRINTF("chandler_addCB: callback list full (cap=%u), refusing %p\n",
            (unsigned)CHANDLER_MAX_CALLBACKS, (void *)cb);
    return;
  }
  // Reject duplicates so accidental double-registration doesn't double-
  // dispatch every command (and doesn't waste a slot).
  for (CommandCallbackNode *cur = callbackListHead; cur; cur = cur->next) {
    if (cur->cb == cb) {
      DPRINTF("chandler_addCB: %p already registered, ignoring\n",
              (void *)cb);
      return;
    }
  }
  CommandCallbackNode *node = malloc(sizeof(*node));
  if (!node) return;
  node->cb = cb;
  node->next = NULL;
  if (!callbackListHead) {
    callbackListHead = node;
  } else {
    CommandCallbackNode *cur = callbackListHead;
    while (cur->next) cur = cur->next;
    cur->next = node;
  }
  callbackListCount++;
}

static inline void __not_in_flash_func(handle_protocol_command)(
    const TransmissionProtocol *protocol) {
  uint16_t size = tprotocol_clamp_payload_size(protocol->payload_size);

  if (protocolPending) {
    chandlerDropped++;
    if (!chandler_protocol_matches_pending(protocol, size)) {
      DPRINTF("Ignoring protocol %04x (%u bytes) while %04x is pending\n",
              protocol->command_id, protocol->payload_size,
              pendingProtocol.command_id);
    }
    return;
  }

  tprotocol_copy_safely(&pendingProtocol, protocol);
  protocolPending = true;
}

bool chandler_stPresent(void) { return stPresent; }

bool chandler_consumeStBoot(void) {
  bool booted = stBootPending;
  stBootPending = false;
  return booted;
}

bool chandler_consumeSharedVarSet(void) {
  bool set = sharedVarSetPending;
  sharedVarSetPending = false;
  return set;
}

#if defined(_DEBUG) && (_DEBUG != 0)
// Debug-only entry point for tools/dev/swd.py: queue a protocol command as if
// the ST had sent it, so the next chandler_loop() dispatches it normally.
bool chandler_injectProtocol(uint16_t commandId, const uint16_t *payload,
                             uint16_t payloadSize) {
  if (protocolPending) {
    return false;
  }
  uint16_t size = tprotocol_clamp_payload_size(payloadSize);
  pendingProtocol.command_id = commandId;
  pendingProtocol.payload_size = size;
  pendingProtocol.bytes_read = size;
  pendingProtocol.final_checksum = 0;
  memset(pendingProtocol.payload, 0, sizeof(pendingProtocol.payload));
  memcpy(pendingProtocol.payload, payload, (size + 1u) & ~1u);
  protocolPending = true;
  pendingInjected = true;
  DPRINTF("Injected command %04x (%u bytes)\n", commandId, (unsigned int)size);
  return true;
}
#endif

static inline void __not_in_flash_func(handle_protocol_checksum_error)(
    const TransmissionProtocol *protocol) {
  chandlerChecksumErrors++;
  DPRINTF(
      "Checksum error detected (CommandID=%x, Size=%x, Bytes Read=%x, "
      "Chksum=%x, RTOKEN=%x)\n",
      protocol->command_id, protocol->payload_size, protocol->bytes_read,
      protocol->final_checksum, TPROTO_GET_RANDOM_TOKEN(protocol->payload));
}

static inline void __not_in_flash_func(chandler_consume_rom3_sample)(
    uint16_t sample) {
  if (chandlerAwaitingFirstSample) {
    chandlerAwaitingFirstSample = false;
    chandlerFrameInFlight = true;
    uint32_t quiet = time_us_32() - chandlerAnsweredAtUs;
    chandlerQuietUs += quiet;
    if (quiet > chandlerMaxQuietUs) chandlerMaxQuietUs = quiet;
  }
  uint16_t addr_lsb = (uint16_t)(sample ^ CHANDLER_ADDRESS_HIGH_BIT);

  tprotocol_parse(addr_lsb, handle_protocol_command,
                  handle_protocol_checksum_error);
}

// Invoke this function to process the commands from the active loop in the
// main function
void __not_in_flash_func(chandler_loop)() {
  uint32_t pollStartUs = time_us_32();
  bool frameWasInFlight = chandlerFrameInFlight;
  commemul_poll(chandler_consume_rom3_sample);
  chandlerPollUs += time_us_32() - pollStartUs;
  if (frameWasInFlight || chandlerFrameInFlight) {
    chandlerFramePolls++;
  }

  if (!protocolPending) {
    // No command to process
    return;
  }
  uint32_t startedAtUs = time_us_32();

  // Shared by all commands
  // Read the random token from the command and increment the payload
  // pointer to the first parameter available in the payload
  uint32_t randomToken = TPROTO_GET_RANDOM_TOKEN(pendingProtocol.payload);
  uint16_t *payloadPtr = (uint16_t *)pendingProtocol.payload;
  uint16_t commandId = pendingProtocol.command_id;
  if ((commandId == 0) && (pendingProtocol.payload_size == 0) &&
      (pendingProtocol.final_checksum == 0)) {
    // Invalid command, clear the pending slot and return
    DPRINTF("Invalid command received. Ignoring.\n");
    chandler_clear_pending_protocol();
    return;
  }

  // Jump the random token
  TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);

  if (commandId == CHANDLER_ST_HELLO) {
    // The framework's own command: no callback sees it. The random token and
    // seed carry on across ST boots on purpose: the ST reads them back.
    DPRINTF("The ST has booted\n");
    stPresent = true;
    stBootPending = true;
  } else if (commandId == CHANDLER_SET_SHARED_VAR) {
    // The framework's own command: no callback sees it.
    uint32_t index = TPROTO_GET_PAYLOAD_PARAM32(payloadPtr);
    TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);
    uint32_t value = TPROTO_GET_PAYLOAD_PARAM32(payloadPtr);
    if (index < CHANDLER_SHARED_VARIABLES_SLOTS) {
      SET_SHARED_VAR(index, value, (uint32_t)&__rom_in_ram_start__,
                     CHANDLER_SHARED_VARIABLES_OFFSET);
      sharedVarSetPending = true;
    } else {
      DPRINTF("Shared variable %lu is past the %u slots; ignored\n",
              (unsigned long)index, (unsigned)CHANDLER_SHARED_VARIABLES_SLOTS);
    }
  } else {
    for (CommandCallbackNode *cur = callbackListHead; cur; cur = cur->next) {
      if (cur->cb) cur->cb(&pendingProtocol, payloadPtr);
    }
  }

  if (pendingInjected) {
    // The probe's, not the ST's: no answer, and none of the ST's counters.
    chandlerInjected++;
    chandler_clear_pending_protocol();
    return;
  }

  // The answer. The ST spins on it, so nothing slow goes between the callbacks
  // and this write: an LED update (on a Pico W a bus transaction to the Wi-Fi
  // chip), a trace, a flash write. Do such work after it.
  incrementalCmdCount++;
  // 64-bit write: low 32b -> RANDOM_TOKEN (echoes request token), high 32b
  // -> RANDOM_TOKEN_SEED (incrementalCmdCount). SEED MUST differ from the
  // request token: the m68k waiter uses "SEED advanced" to distinguish a
  // real reply from open-bus reads when no firmware is responding.
  TPROTO_SET_RANDOM_TOKEN64(
      memoryRandomTokenAddress,
      (((uint64_t)incrementalCmdCount) << 32) | randomToken);

  // The counters, after the answer.
  uint32_t answeredAtUs = time_us_32();
  uint32_t busy = answeredAtUs - startedAtUs;
  chandlerBusyUs += busy;
  if (busy > chandlerMaxBusyUs) chandlerMaxBusyUs = busy;
  if (chandlerAnsweredAtUs != 0) {
    uint32_t gap = startedAtUs - chandlerAnsweredAtUs;
    chandlerGapUs += gap;
    if (gap > chandlerMaxGapUs) chandlerMaxGapUs = gap;
  }
  chandlerAnsweredAtUs = answeredAtUs;
  chandlerAwaitingFirstSample = true;
  chandlerFrameInFlight = false;
  chandlerHandled++;
  if (randomToken == chandlerLastToken) {
    chandlerRepeated++;
  }
  chandlerLastToken = randomToken;

  chandler_clear_pending_protocol();
}
