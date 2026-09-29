/**
 * File: download.h
 * Author: Diego Parrilla Santamaría
 * Date: January 20205, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS SL
 * Description: Header for download wrapper
 */

#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aconfig.h"
#include "constants.h"
#include "debug.h"
#include "ff.h"
#include "httpc/httpc.h"
#include "memfunc.h"
#include "network.h"

#define DOWNLOAD_BUFFLINE_SIZE 256
#define DOWNLOAD_FILENAME_SIZE 64
#define DOWNLOAD_HOSTNAME_SIZE 128
#define DOWNLOAD_PROTOCOL_SIZE 16
// The longest URL a download takes, redirect targets included: longer ones
// are refused, never cut. An HTTPS build takes release-asset URLs, whose
// signed query strings run to about 1,000 characters after a redirect; an
// HTTP-only build keeps 256 bytes.
#if APP_DOWNLOAD_HTTPS == 1
#define DOWNLOAD_URL_SIZE 1536
#else
#define DOWNLOAD_URL_SIZE DOWNLOAD_BUFFLINE_SIZE
#endif
// Redirects followed per download, and retries of a hop that failed in
// transit (a timeout, a refused or dropped connection), each started from
// download_poll() once DOWNLOAD_SETTLE_MS have passed since the last
// connection closed: a connection opened within a few milliseconds of the
// previous one received nothing (measured by Booster).
#define DOWNLOAD_MAX_REDIRECTS 5
#define DOWNLOAD_MAX_HOP_RETRIES 2
#define DOWNLOAD_SETTLE_MS 500

// A download goes STARTED -> IN_PROGRESS (held there through redirects and
// retries) -> COMPLETED, set by download_poll() once the whole body is on the
// card, or FAILED at any point. Poll while it is STARTED or IN_PROGRESS.
typedef enum {
  DOWNLOAD_STATUS_IDLE,
  DOWNLOAD_STATUS_REQUESTED,
  DOWNLOAD_STATUS_NOT_STARTED,
  DOWNLOAD_STATUS_STARTED,
  DOWNLOAD_STATUS_IN_PROGRESS,
  DOWNLOAD_STATUS_COMPLETED,
  DOWNLOAD_STATUS_FAILED
} download_status_t;

typedef enum {
  DOWNLOAD_POLL_CONTINUE,
  DOWNLOAD_POLL_ERROR,
  DOWNLOAD_POLL_COMPLETED
} download_poll_t;

typedef enum {
  DOWNLOAD_OK,
  DOWNLOAD_BASE64_ERROR,
  DOWNLOAD_PARSEJSON_ERROR,
  DOWNLOAD_PARSEMD5_ERROR,
  DOWNLOAD_CANNOTOPENFILE_ERROR,
  DOWNLOAD_CANNOTCLOSEFILE_ERROR,
  DOWNLOAD_FORCEDABORT_ERROR,
  DOWNLOAD_CANNOTSTARTDOWNLOAD_ERROR,
  DOWNLOAD_CANNOTREADFILE_ERROR,
  DOWNLOAD_CANNOTPARSEURL_ERROR,
  DOWNLOAD_MD5MISMATCH_ERROR,
  DOWNLOAD_CANNOTRENAMEFILE_ERROR,
  DOWNLOAD_CANNOTCREATE_CONFIG,
  DOWNLOAD_CANNOTDELETECONFIGSECTOR_ERROR,
  DOWNLOAD_UNSUPPORTEDSCHEME_ERROR,  // neither http:// nor https://
  DOWNLOAD_HTTPSNOTBUILT_ERROR,      // https:// in a build without
                                     // APP_DOWNLOAD_HTTPS
  DOWNLOAD_URLTOOLONG_ERROR,  // longer than DOWNLOAD_URL_SIZE or its parts
  DOWNLOAD_HTTPSTATUS_ERROR,  // not 2xx: download_getHttpStatus() says which
  DOWNLOAD_TOOMANYREDIRECTS_ERROR,  // past DOWNLOAD_MAX_REDIRECTS
  DOWNLOAD_TIMEOUT_ERROR,           // nothing received for lwIP's 15 s
  DOWNLOAD_TRANSFER_ERROR  // DNS, connection, length mismatch or write failure
} download_err_t;

typedef struct {
  char protocol[DOWNLOAD_PROTOCOL_SIZE];
  char host[DOWNLOAD_HOSTNAME_SIZE];
  uint16_t port;  // 0: the scheme's default
  char uri[DOWNLOAD_URL_SIZE];
} download_url_components_t;

typedef struct {
  char filename[DOWNLOAD_FILENAME_SIZE];
} download_file_t;

/**
 * @brief Initiates the download by parsing the current URL, opening a temporary
 * file, and starting the HTTP client request for the file. Checks and prepares
 * the file system environment (e.g., clearing read-only attributes, handling
 * locked files) before initiating the asynchronous download. The scheme picks
 * the transport: http:// is plain TCP, https:// is TLS in a build with
 * APP_DOWNLOAD_HTTPS=1 and refused otherwise. A URL longer than
 * DOWNLOAD_URL_SIZE is refused. The server's certificate is not verified.
 *
 * @return A download_err_t code indicating a successful start or a specific
 * error.
 */
download_err_t download_start(void);

/**
 * @brief Polls the download process by invoking the asynchronous context
 * routines. Processes incoming data packets and HTTP events, and starts the
 * next request of a redirect or a retry. Never waits: call it from a loop that
 * keeps serving the ST, until it reports completion.
 *
 * Only a 2xx response is written to the file. Anything else, a timeout or a
 * transfer error fails the download, and the temporary file is deleted.
 *
 * @return DOWNLOAD_POLL_CONTINUE while in progress, DOWNLOAD_POLL_COMPLETED
 * when the file arrived whole, DOWNLOAD_POLL_ERROR when it failed
 * (download_getError() says why).
 */
download_poll_t download_poll(void);

/**
 * @brief Finalizes the download process by closing the temporary file.
 *
 * @return DOWNLOAD_OK when the file arrived whole, otherwise the reason the
 * download failed (as download_getError()).
 */
download_err_t download_finish(void);

/**
 * @brief Renames the temporary download file to its final filename.
 *
 * Generates the final file path based on application configuration and deletes
 * any pre-existing file. Ensures integrity by checking the result of the rename
 * operation.
 *
 * @return A download_err_t code indicating a successful rename or an error code
 * if renaming fails.
 */
download_err_t download_confirm(void);

/**
 * @brief Retrieves the current status of the download process.
 *
 * @return The current download_status_t representing the state.
 */
download_status_t download_getStatus(void);

/**
 * @brief Updates the status of the download process.
 *
 * @param status New download_status_t value to set.
 */
void download_setStatus(download_status_t status);

/**
 * @brief Retrieves the current file path used in the download process.
 *
 * This path may represent the temporary file or a user-defined URL for
 * downloading.
 *
 * @return A pointer to a null-terminated string with the current file path.
 */
const char *download_getFilepath(void);

/**
 * @brief Sets the file path for the download process.
 *
 * Copies the supplied path into internal storage ensuring proper
 * null-termination.
 *
 * @param path A null-terminated string containing the new file path.
 */
void download_setFilepath(const char *path);

/**
 * @brief Provides access to the parsed components of the download URL.
 *
 * This includes protocol, hostname, and URI as extracted from the user-supplied
 * URL.
 *
 * @return A pointer to a download_url_components_t structure.
 */
const download_url_components_t *download_getUrlComponents(void);

/**
 * @brief The name the download is saved under in the app folder: the last
 * segment of the URL's path, or "default.bin" when it has none.
 *
 * @return A null-terminated string, empty before the first download_start().
 */
const char *download_getFilename(void);

/**
 * @brief Why the last download failed, DOWNLOAD_OK while none has.
 */
download_err_t download_getError(void);

/**
 * @brief The HTTP status of the last response, 0 before one arrived.
 */
int download_getHttpStatus(void);

#endif  // DOWNLOAD_H