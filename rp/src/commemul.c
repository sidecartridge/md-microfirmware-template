/**
 * File: commemul.c
 * Author: Diego Parrilla Santamaría
 * Date: March 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: ROM3 communication emulator backed by a DMA ring buffer.
 */

#include "commemul.h"

#include "commemul.pio.h"
#include "constants.h"
#include "debug.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

// The ring has to hold what the ST can send while this side is not draining
// it. The ST's sends are synchronous: it sends one frame and waits for the
// answer, then resends it with a fresh token, CMD_RETRIES_COUNT (3) times,
// before it gives up. One sample per 16-bit word, so the largest frame is the
// header, command, size, token and checksum words plus
// MAX_PROTOCOL_PAYLOAD_SIZE (2,112 bytes) of payload: about 1,060 samples, and
// a whole send with its retries is 4 x 1,060 = 4,240. The longest wait without
// a drain is bringing Wi-Fi up (about 0.9 s), which that covers. 2^13 words
// (4,096) would be just short, so 2^14 (8,192 samples, 16 KB): about two whole
// sends. A lap beyond that is counted in commOverruns and its samples dropped.
// The ring must be aligned to its size for the DMA's address wrap, and a
// smaller ring also wastes less RAM on that alignment.
#define COMM_RING_BITS 14u
#define COMM_RING_SIZE_BYTES (1ul << COMM_RING_BITS)
#define COMM_RING_WORDS (COMM_RING_SIZE_BYTES / sizeof(uint16_t))
#define COMM_RING_MASK (COMM_RING_WORDS - 1u)
#define COMM_DMA_TRANSFER_COUNT (0xFFFFFFFFu)
// Re-arm the DMA channel once this many samples have been captured, long before
// the transfer count runs out and capture stops.
#define COMM_DMA_REARM_THRESHOLD (0x80000000u)

// In its own section at the start of RAM (memmap_rp.ld), where the alignment
// leaves no gap. Not zeroed at boot: only what the DMA has written is read.
static uint16_t commRing[COMM_RING_WORDS]
    __attribute__((aligned(COMM_RING_SIZE_BYTES), section(".comm_ring")));
static uint32_t commReadIdx = 0;
// Samples captured since the channel was last armed, as of the last poll.
static uint32_t commLastWritten = 0;
// Ring index the channel was armed at.
static uint32_t commArmIdx = 0;
// Laps of the ring: times the reader fell a whole ring behind. Read over SWD
// by symbol, and through commemul_getOverruns().
uint32_t commOverruns = 0;
static int commDmaChannel = -1;
static int commSm = -1;
static bool commInitialized = false;
static PIO commPio = pio0;

int commemul_init(void) {
  if (commInitialized) {
    return 0;
  }

  for (int i = 0; i < READ_ADDR_PIN_COUNT; i++) {
    pio_gpio_init(commPio, READ_ADDR_GPIO_BASE + i);
  }

  pio_gpio_init(commPio, READ_SIGNAL_GPIO_BASE);
  pio_gpio_init(commPio, WRITE_SIGNAL_GPIO_BASE);
  pio_gpio_init(commPio, ROM3_GPIO);

  gpio_set_dir(ROM3_GPIO, GPIO_IN);
  gpio_set_pulls(ROM3_GPIO, true, false);
  gpio_pull_up(ROM3_GPIO);

  int offset = pio_add_program(commPio, &commemul_read_program);
  if (offset < 0) {
    DPRINTF("commemul_init: pio_add_program failed (%d)\n", offset);
    return -1;
  }
  commSm = pio_claim_unused_sm(commPio, true);
  commemul_read_program_init(commPio, commSm, (uint)offset, READ_ADDR_GPIO_BASE,
                             READ_ADDR_PIN_COUNT, READ_SIGNAL_GPIO_BASE,
                             SAMPLE_DIV_FREQ);

  pio_sm_set_enabled(commPio, commSm, false);
  pio_sm_clear_fifos(commPio, commSm);
  pio_sm_restart(commPio, commSm);

  commDmaChannel = dma_claim_unused_channel(true);
  dma_channel_config c = dma_channel_get_default_config(commDmaChannel);
  channel_config_set_read_increment(&c, false);
  channel_config_set_write_increment(&c, true);
  channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
  channel_config_set_ring(&c, true, COMM_RING_BITS);
  channel_config_set_dreq(&c, pio_get_dreq(commPio, commSm, false));

  dma_channel_configure(
      commDmaChannel, &c, commRing,
      (uint8_t *)(&commPio->rxf[commSm]) + 2,  // upper halfword of FIFO word
      COMM_DMA_TRANSFER_COUNT, true);

  pio_sm_set_enabled(commPio, commSm, true);
  commReadIdx = 0;
  commInitialized = true;

  DPRINTF(
      "ROM3 comm ring initialized on pio0/sm%d dma%d (%u words / %u "
      "bytes)\n",
      commSm, commDmaChannel, (unsigned int)COMM_RING_WORDS,
      (unsigned int)COMM_RING_SIZE_BYTES);
  return 0;
}

void __not_in_flash_func(commemul_poll)(CommEmulSampleCallback callback) {
  if ((!commInitialized) || (callback == NULL)) {
    return;
  }

  uint32_t transfersWritten =
      COMM_DMA_TRANSFER_COUNT - dma_hw->ch[commDmaChannel].transfer_count;
  uint32_t writeIdx = (commArmIdx + transfersWritten) & COMM_RING_MASK;

  // When a whole ring or more arrived since the last poll (exactly a ring
  // reads as an empty one), the DMA lapped the reader and the unread samples
  // are a mix of old and new ones. Count it and drop them: the parser then
  // resynchronises on the next header.
  uint32_t unread = transfersWritten - commLastWritten;
  commLastWritten = transfersWritten;
  if (unread >= COMM_RING_WORDS) {
    commOverruns++;
    commReadIdx = writeIdx;
    return;
  }

  while (commReadIdx != writeIdx) {
    callback(commRing[commReadIdx]);
    commReadIdx = (commReadIdx + 1u) & COMM_RING_MASK;
  }

  if (transfersWritten >= COMM_DMA_REARM_THRESHOLD) {
    // Restart the channel where it is writing now, so the ring carries on.
    // The transfer count only means something while the channel runs, so the
    // position comes from its live write address. Samples written since the
    // drain above stay unread from commReadIdx, and samples arriving during
    // the restart wait in the PIO FIFO.
    dma_channel_abort(commDmaChannel);
    uint32_t idx =
        ((dma_hw->ch[commDmaChannel].write_addr - (uint32_t)commRing) /
         sizeof(uint16_t)) &
        COMM_RING_MASK;
    commArmIdx = idx;
    commLastWritten = 0;
    dma_channel_set_write_addr(commDmaChannel, &commRing[idx], false);
    dma_channel_set_trans_count(commDmaChannel, COMM_DMA_TRANSFER_COUNT, false);
    dma_channel_start(commDmaChannel);
    DPRINTF("commemul: DMA re-armed at ring index %lu\n", (unsigned long)idx);
  }
}

uint32_t commemul_getOverruns(void) { return commOverruns; }
