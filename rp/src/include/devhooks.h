/**
 * File: devhooks.h
 * Author: Diego Parrilla Santamaría
 * Date: September 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Debug-only mailbox that host tools write over SWD, so they can
 *              drive the firmware without its own services. tools/dev/swd.py
 *              is the host side.
 */

#ifndef DEVHOOKS_H
#define DEVHOOKS_H

// The mailbox is a struct in RAM found by its symbol, devhooksMailbox. The
// host writes kind, command_id, payload_size and payload, then sets seq to
// ack + 1. devhooks_poll(), called from the main loop, runs the request,
// stores the outcome in result and copies seq to ack.
//
//   DEVHOOKS_KIND_PROTOCOL  Inject a transmission-protocol command as if the
//                           ST had sent it: command_id, payload_size bytes of
//                           payload words (random token first). result is 1
//                           when it was queued, 0 when another command was
//                           pending (retry).
//   DEVHOOKS_KIND_APP       App-defined command: command_id and payload go to
//                           the handler set with devhooks_setAppHandler().
//                           result is the handler's return value, 0 when no
//                           handler is set.
//
// Include this header in one file only (it holds the definitions, as weak
// symbols) and call devhooks_poll() from the main loop. In release builds the
// calls compile to nothing.

#include <stdbool.h>
#include <stdint.h>

#define DEVHOOKS_MAGIC 0x444B4831u  // "DKH1"
#define DEVHOOKS_KIND_PROTOCOL 1u
#define DEVHOOKS_KIND_APP 2u
#define DEVHOOKS_PAYLOAD_WORDS 16u

typedef uint32_t (*devhooks_app_handler_t)(uint16_t commandId,
                                           const uint16_t *payload,
                                           uint16_t payloadSize);

#if defined(_DEBUG) && (_DEBUG != 0)

#include "chandler.h"

typedef struct {
  uint32_t magic;
  uint32_t seq;
  uint32_t ack;
  uint32_t kind;
  uint32_t result;
  uint16_t command_id;
  uint16_t payload_size;  // bytes
  uint16_t payload[DEVHOOKS_PAYLOAD_WORDS];
} DevhooksMailbox;

__attribute__((weak, used)) DevhooksMailbox devhooksMailbox = {
    .magic = DEVHOOKS_MAGIC};
__attribute__((weak)) devhooks_app_handler_t devhooksAppHandler = NULL;

__attribute__((weak, noinline)) void devhooks_setAppHandler(
    devhooks_app_handler_t handler) {
  devhooksAppHandler = handler;
}

__attribute__((weak, noinline)) void devhooks_poll(void) {
  DevhooksMailbox *m = &devhooksMailbox;
  uint32_t seq = m->seq;
  if (seq == m->ack) {
    return;
  }
  uint16_t size = m->payload_size;
  if (size > DEVHOOKS_PAYLOAD_WORDS * 2u) {
    size = DEVHOOKS_PAYLOAD_WORDS * 2u;
  }
  uint32_t result = 0;
  if (m->kind == DEVHOOKS_KIND_PROTOCOL) {
    result = chandler_injectProtocol(m->command_id, m->payload, size) ? 1u : 0u;
  } else if (m->kind == DEVHOOKS_KIND_APP && devhooksAppHandler != NULL) {
    result = devhooksAppHandler(m->command_id, m->payload, size);
  }
  m->result = result;
  m->ack = seq;
}

#else

// The handler token is swallowed, not evaluated: in release builds the
// handler function itself is compiled out.
#define devhooks_setAppHandler(handler) ((void)0)
#define devhooks_poll() ((void)0)

#endif  // _DEBUG

#endif  // DEVHOOKS_H
