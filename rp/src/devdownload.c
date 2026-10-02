/**
 * File: devdownload.c
 * Author: Diego Parrilla Santamaría
 * Date: September 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Debug-only test download, driven over SWD by
 *              tools/dev/download_harness.py. See devdownload.h.
 */

#include "devdownload.h"

#if defined(_DEBUG) && (_DEBUG != 0)

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "aconfig.h"
#include "debug.h"
#include "download.h"
#include "ff.h"
#include "lwip/stats.h"
#include "md5/md5.h"
#include "pico/time.h"

__attribute__((used)) DevdownloadState devdownloadState = {
    .magic = DEVDOWNLOAD_MAGIC};

static FIL hashFile;
static MD5Context md5Context;
// The file the URL names, as it was before this download: a failure must not
// create or change it.
static bool namedExisted;
static FSIZE_t namedSize;
static absolute_time_t startedAt;
static uint8_t hashChunk[4096];

static const char *devdownload_folder(void) {
  SettingsConfigEntry *entry =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  return (entry != NULL) ? entry->value : "";
}

static bool devdownload_statNamed(FILINFO *info) {
  char path[DOWNLOAD_BUFFLINE_SIZE];
  snprintf(path, sizeof(path), "%s/%s", devdownload_folder(),
           download_getFilename());
  return download_getFilename()[0] != '\0' && f_stat(path, info) == FR_OK;
}

// Which files the download left in the app folder.
static uint32_t devdownload_leftovers(void) {
  char path[DOWNLOAD_BUFFLINE_SIZE];
  FILINFO info;
  uint32_t left = 0;
  if (devdownload_statNamed(&info) &&
      (!namedExisted || info.fsize != namedSize)) {
    left |= DEVDOWNLOAD_LEFT_FILE;
  }
  snprintf(path, sizeof(path), "%s/tmp.download", devdownload_folder());
  if (f_stat(path, &info) == FR_OK) {
    left |= DEVDOWNLOAD_LEFT_TMP;
  }
  return left;
}

// lwIP's failed allocations since boot: its heap and every pool.
static uint32_t devdownload_lwipErrors(void) {
  uint32_t errors = 0;
#if MEMP_STATS
  for (int i = 0; i < MEMP_MAX; i++) {
    errors += lwip_stats.memp[i]->err;
  }
#endif
#if MEM_STATS
  errors += lwip_stats.mem.err;
#endif
  return errors;
}

static void devdownload_end(devdownload_state_t state, devdownload_step_t step,
                            uint32_t error) {
  devdownloadState.lwipErrors = devdownload_lwipErrors();
  devdownloadState.elapsedMs =
      (uint32_t)(absolute_time_diff_us(startedAt, get_absolute_time()) / 1000);
  devdownloadState.left = devdownload_leftovers();
  devdownloadState.step = step;
  devdownloadState.error = error;
  devdownloadState.state = state;
  DPRINTF("devdownload: %s, step %lu, error %lu, %lu bytes, %lu ms\n",
          (state == DEVDOWNLOAD_DONE) ? "done" : "failed", (unsigned long)step,
          (unsigned long)error, (unsigned long)devdownloadState.bytes,
          (unsigned long)devdownloadState.elapsedMs);
}

uint32_t devdownload_start(void) {
  if (devdownloadState.state == DEVDOWNLOAD_DOWNLOADING ||
      devdownloadState.state == DEVDOWNLOAD_HASHING) {
    return 0;
  }
  devdownloadState.url[DEVDOWNLOAD_URL_SIZE - 1] = '\0';
  devdownloadState.seq++;
  devdownloadState.step = DEVDOWNLOAD_STEP_NONE;
  devdownloadState.error = 0;
  devdownloadState.left = 0;
  devdownloadState.bytes = 0;
  devdownloadState.elapsedMs = 0;
  memset(devdownloadState.md5, 0, sizeof(devdownloadState.md5));
  startedAt = get_absolute_time();
  DPRINTF("devdownload: %s\n", devdownloadState.url);
  download_setFilepath(devdownloadState.url);
  devdownloadState.state = DEVDOWNLOAD_DOWNLOADING;
  download_err_t err = download_start();
  // download_start() has parsed the URL, and only download_confirm() writes
  // the file it names.
  FILINFO info;
  namedExisted = devdownload_statNamed(&info);
  namedSize = namedExisted ? info.fsize : 0;
  if (err != DOWNLOAD_OK) {
    devdownload_end(DEVDOWNLOAD_FAILED, DEVDOWNLOAD_STEP_START, err);
  }
  return 1;
}

void devdownload_poll(void) {
  if (devdownloadState.state == DEVDOWNLOAD_DOWNLOADING) {
    // The pattern an app follows (download.h): poll while started or in
    // progress, then finish.
    download_status_t status = download_getStatus();
    if (status == DOWNLOAD_STATUS_STARTED ||
        status == DOWNLOAD_STATUS_IN_PROGRESS) {
      download_poll();
      return;
    }
    download_err_t err = download_finish();
    if (err != DOWNLOAD_OK) {
      devdownload_end(DEVDOWNLOAD_FAILED, DEVDOWNLOAD_STEP_FINISH, err);
      return;
    }
    err = download_confirm();
    if (err != DOWNLOAD_OK) {
      devdownload_end(DEVDOWNLOAD_FAILED, DEVDOWNLOAD_STEP_CONFIRM, err);
      return;
    }
    char path[DOWNLOAD_BUFFLINE_SIZE];
    snprintf(path, sizeof(path), "%s/%s", devdownload_folder(),
             download_getFilename());
    FRESULT res = f_open(&hashFile, path, FA_READ);
    if (res != FR_OK) {
      devdownload_end(DEVDOWNLOAD_FAILED, DEVDOWNLOAD_STEP_OPEN, res);
      return;
    }
    md5Init(&md5Context);
    devdownloadState.state = DEVDOWNLOAD_HASHING;
    return;
  }
  if (devdownloadState.state == DEVDOWNLOAD_HASHING) {
    // One chunk per call: about 2 ms of SD reads at a time.
    UINT got = 0;
    FRESULT res = f_read(&hashFile, hashChunk, sizeof(hashChunk), &got);
    if (res != FR_OK) {
      f_close(&hashFile);
      devdownload_end(DEVDOWNLOAD_FAILED, DEVDOWNLOAD_STEP_READ, res);
      return;
    }
    md5Update(&md5Context, hashChunk, got);
    devdownloadState.bytes += got;
    if (got < sizeof(hashChunk)) {
      md5Finalize(&md5Context);
      memcpy(devdownloadState.md5, md5Context.digest,
             sizeof(devdownloadState.md5));
      f_close(&hashFile);
      devdownload_end(DEVDOWNLOAD_DONE, DEVDOWNLOAD_STEP_NONE, 0);
    }
  }
}

#endif  // _DEBUG
