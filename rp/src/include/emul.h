/**
 * File: emul.h
 * Author: Diego Parrilla Santamaría
 * Date: January 20205, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS SL
 * Description: Header for the ROM emulator core and setup features
 */

#ifndef EMUL_H
#define EMUL_H

/**
 * @brief
 *
 * Launches the ROM emulator application. Initializes terminal interfaces,
 * configures network and storage systems, and loads the ROM data from SD or
 * network sources. Manages the main loop which includes firmware bypass,
 * user interaction and potential system resets.
 */
void emul_start();

// App commands for `tools/dev/swd.py app NAME [WORDS...]`, debug builds only;
// the name after DEVHOOKS_APP_ is the one the tool takes.
//   heap_hold KB   hold KB more kilobytes of heap (0 releases everything);
//                  answers 0 when the allocation is refused, so repeated calls
//                  walk the heap down to a known remainder.
#define DEVHOOKS_APP_HEAP_HOLD 1

#endif  // EMUL_H
