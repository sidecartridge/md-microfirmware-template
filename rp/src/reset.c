#include "reset.h"

void reset_device() {
  DPRINTF("Resetting the device\n");

  save_and_disable_interrupts();
  // watchdog_enable(RESET_WATCHDOG_TIMEOUT, 0);
  watchdog_reboot(0, 0, RESET_WATCHDOG_TIMEOUT);
  // 20 ms timeout, for example, then the chip will reset. Said once: in the
  // loop it filled the debug console with about 60 lines per reset.
  DPRINTF("Waiting for the device to reset\n");
  while (1) {
    tight_loop_contents();
  }
}

void reset_deviceAndEraseFlash() {
  // Erase the settings
  DPRINTF("Erasing the flash memory\n");
  settings_erase(gconfig_getContext());
  DPRINTF("Erasing the app lookup table\n");
  sleep_ms(SEC_TO_MS);

  // Reset the device
  DPRINTF("Resetting the device\n");
  reset_device();
}