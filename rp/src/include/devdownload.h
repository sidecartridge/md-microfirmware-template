/**
 * File: devdownload.h
 * Author: Diego Parrilla Santamaría
 * Date: September 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Debug-only test download, driven over SWD by
 *              tools/dev/download_harness.py.
 */

#ifndef DEVDOWNLOAD_H
#define DEVDOWNLOAD_H

#include <stdint.h>

// The host writes a URL into devdownloadState.url and sends the app command
// DEVHOOKS_APP_DOWNLOAD (emul.h). devdownload_poll(), called from the main
// loop, drives the download through download.h as an app would, then hashes
// the saved file a chunk per call, so the loop keeps serving the ST. The host
// reads the outcome from devdownloadState, found by its symbol. In release
// builds both calls compile to nothing.

#define DEVDOWNLOAD_MAGIC 0x4C445644u  // "DVDL"
#define DEVDOWNLOAD_URL_SIZE 2048u
#define DEVDOWNLOAD_MD5_SIZE 16u

typedef enum {
  DEVDOWNLOAD_IDLE = 0,
  DEVDOWNLOAD_DOWNLOADING,
  DEVDOWNLOAD_HASHING,
  DEVDOWNLOAD_DONE,
  DEVDOWNLOAD_FAILED
} devdownload_state_t;

// The step a failure came from; error holds its download_err_t or FRESULT.
typedef enum {
  DEVDOWNLOAD_STEP_NONE = 0,
  DEVDOWNLOAD_STEP_START,    // download_start(): download_err_t
  DEVDOWNLOAD_STEP_FINISH,   // download_finish(): download_err_t
  DEVDOWNLOAD_STEP_CONFIRM,  // download_confirm(): download_err_t
  DEVDOWNLOAD_STEP_OPEN,     // opening the saved file: FRESULT
  DEVDOWNLOAD_STEP_READ      // reading it to hash it: FRESULT
} devdownload_step_t;

// Files in the app folder that the download left when it ended.
#define DEVDOWNLOAD_LEFT_FILE 1u  // the file the URL names, created or resized
#define DEVDOWNLOAD_LEFT_TMP 2u   // download.c's temporary file

typedef struct {
  uint32_t magic;
  uint32_t seq;         // incremented by every accepted start
  uint32_t state;       // devdownload_state_t
  uint32_t step;        // devdownload_step_t of a failure
  uint32_t error;       // the failing step's result
  uint32_t left;        // DEVDOWNLOAD_LEFT_* when it ended
  uint32_t bytes;       // size of the saved file
  uint32_t elapsedMs;   // from the start to the end of the hash
  uint32_t lwipErrors;  // lwIP's failed allocations since boot, at the end
  uint8_t md5[DEVDOWNLOAD_MD5_SIZE];
  char url[DEVDOWNLOAD_URL_SIZE];  // written by the host
} DevdownloadState;

#if defined(_DEBUG) && (_DEBUG != 0)
// Starts downloading devdownloadState.url. Returns 1 when accepted (the
// outcome follows in devdownloadState), 0 while a download is running.
uint32_t devdownload_start(void);
void devdownload_poll(void);
#else
static inline uint32_t devdownload_start(void) { return 0; }
static inline void devdownload_poll(void) {}
#endif

#endif  // DEVDOWNLOAD_H
