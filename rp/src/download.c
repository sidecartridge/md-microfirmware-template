/**
 * File: download.c
 * Author: Diego Parrilla Santamaría
 * Date: February 2025, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS
 * Description: Download files. Wrapper for HTTP and SD card.
 */

#include "download.h"

#include <strings.h>

// Download
static FIL file;
static download_status_t downloadStatus = DOWNLOAD_STATUS_IDLE;
static download_err_t downloadError = DOWNLOAD_OK;
static HTTPC_REQUEST_T request = {0};
static char filepath[DOWNLOAD_URL_SIZE] = {0};
static bool filepathTooLong = false;
static download_url_components_t components;
static download_file_t fileUrl;
static int httpStatus = 0;

// A redirect or a retry is started from download_poll(), never from inside
// an lwIP callback, which would reenter the stack.
static bool redirectPending = false;
static bool retryPending = false;
static bool settling = false;
// The response arrived whole; download_poll() declares the download
// completed once the queued body is on the card, never before.
static bool transferDone = false;
static int redirectHops = 0;
static int hopRetries = 0;
static char nextUrl[DOWNLOAD_URL_SIZE] = {0};     // the redirect target
static char currentUrl[DOWNLOAD_URL_SIZE] = {0};  // the request in flight
static char location[DOWNLOAD_URL_SIZE] = {0};    // off the 4 KB stack
static absolute_time_t reissueAt;

// The body is queued by the receive callback and written by download_poll(),
// one chunk per call: a pass of the caller's loop then holds at most one card
// write. The data not yet written holds the TCP window shut, so the server
// sends no faster than the card takes it. Writing each packet from the
// callback as it arrived, several per pass, held the ST's 1 KB commands for
// 41 ms each during a download and failed 3 of 3,000. Sector-aligned 4 KB
// chunks also cost the card fewer operations than 1,460-byte packets.
#define DOWNLOAD_WRITE_CHUNK 4096
static struct pbuf *pending = NULL;
static struct altcp_pcb *pendingConn = NULL;
static uint8_t *writeChunk = NULL;  // on the heap only while a download runs

// Generates a temporary file path for downloads.
static void getTmpFilenamePath(char filename[DOWNLOAD_BUFFLINE_SIZE]) {
  snprintf(
      filename, DOWNLOAD_BUFFLINE_SIZE, "%s/tmp.download",
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER)->value);
}

// Close and delete the temporary file, so a failed or redirected transfer
// never leaves a partial body, an error page or a redirect page behind.
static void deleteTmpFile(void) {
  char filename[DOWNLOAD_BUFFLINE_SIZE] = {0};
  f_close(&file);
  getTmpFilenamePath(filename);
  f_unlink(filename);
}

static void dropPending(void) {
  if (pending != NULL) {
    pbuf_free(pending);
    pending = NULL;
  }
}

static void fail(download_err_t err) {
  downloadError = err;
  downloadStatus = DOWNLOAD_STATUS_FAILED;
  dropPending();
  deleteTmpFile();
}

// Write one chunk of the queued body, and open the TCP window by as much
// while the connection is still up (httpc frees it when the request ends).
static void writePendingChunk(void) {
  u16_t len = (pending->tot_len < DOWNLOAD_WRITE_CHUNK) ? pending->tot_len
                                                        : DOWNLOAD_WRITE_CHUNK;
  pbuf_copy_partial(pending, writeChunk, len, 0);
  UINT written = 0;
  FRESULT res = f_write(&file, writeChunk, len, &written);
  pending = pbuf_free_header(pending, len);
  if (res != FR_OK || written != len) {
    DPRINTF("Error writing to file: %i\n", res);
    if (request.complete) {
      fail(DOWNLOAD_TRANSFER_ERROR);
    } else {
      // The result callback, called from the abort, fails the download.
      downloadError = DOWNLOAD_TRANSFER_ERROR;
      altcp_abort(pendingConn);
    }
    return;
  }
  if (!request.complete) {
    altcp_recved(pendingConn, len);
  }
}

// Parses a URL into its components. Returns DOWNLOAD_OK, or the reason it
// cannot be used: a part that does not fit is refused, never cut short.
static download_err_t parseUrl(const char *url,
                               download_url_components_t *components) {
  memset(components, 0, sizeof(download_url_components_t));
  if (strlen(url) >= DOWNLOAD_URL_SIZE) {
    return DOWNLOAD_URLTOOLONG_ERROR;
  }

  // Find the protocol separator "://"
  const char *protocolEnd = strstr(url, "://");
  if (!protocolEnd) {
    return DOWNLOAD_CANNOTPARSEURL_ERROR;
  }

  size_t protocolLen = protocolEnd - url;
  if (protocolLen >= sizeof(components->protocol)) {
    return DOWNLOAD_CANNOTPARSEURL_ERROR;
  }
  memcpy(components->protocol, url, protocolLen);

  // The host, with an optional ":port", runs from "://" to the first slash,
  // which starts the URI.
  const char *hostStart = protocolEnd + 3;
  const char *uriStart = strchr(hostStart, '/');
  size_t hostLen =
      uriStart ? (size_t)(uriStart - hostStart) : strlen(hostStart);
  if (hostLen == 0) {
    return DOWNLOAD_CANNOTPARSEURL_ERROR;
  }
  if (hostLen >= sizeof(components->host)) {
    return DOWNLOAD_URLTOOLONG_ERROR;
  }
  memcpy(components->host, hostStart, hostLen);
  snprintf(components->uri, sizeof(components->uri), "%s",
           uriStart ? uriStart : "/");

  char *portSep = strchr(components->host, ':');
  if (portSep != NULL) {
    char *end = NULL;
    unsigned long port = strtoul(portSep + 1, &end, DEC_BASE);
    if (end == portSep + 1 || *end != '\0' || port == 0 || port > UINT16_MAX) {
      return DOWNLOAD_CANNOTPARSEURL_ERROR;
    }
    components->port = (uint16_t)port;
    *portSep = '\0';
  }
  return DOWNLOAD_OK;
}

// The name the file is saved under: the last segment of the URL's path,
// without a query string or fragment ('?' is not a legal FAT character).
static void filenameFromUri(const char *uri, download_file_t *file) {
  memset(file, 0, sizeof(download_file_t));
  const char *lastSlash = strrchr(uri, '/');
  const char *filenameStart = lastSlash ? lastSlash + 1 : uri;
  size_t len = strcspn(filenameStart, "?#");
  if (len >= sizeof(file->filename)) {
    len = sizeof(file->filename) - 1;
  }
  memcpy(file->filename, filenameStart, len);
  if (file->filename[0] == '\0') {
    snprintf(file->filename, sizeof(file->filename), "default.bin");
  }
}

// Find a header (case-insensitive name at the start of a line) in a block of
// response headers and copy its value into out. False if absent or empty;
// *tooLong is set when it is present but longer than out.
static bool findHeaderValue(const char *headers, const char *name, char *out,
                            size_t outLen, bool *tooLong) {
  size_t nameLen = strlen(name);
  const char *line = headers;
  *tooLong = false;
  while (line != NULL && *line != '\0') {
    if (strncasecmp(line, name, nameLen) == 0) {
      const char *value = line + nameLen;
      while (*value == ' ' || *value == '\t') {
        value++;
      }
      size_t len = strcspn(value, "\r\n");
      if (len >= outLen) {
        *tooLong = true;
        return false;
      }
      memcpy(out, value, len);
      out[len] = '\0';
      return len > 0;
    }
    line = strstr(line, "\r\n");
    if (line != NULL) {
      line += 2;
    }
  }
  return false;
}

// Resolve a Location value against the request that produced it: an absolute
// URL passes through; a host-relative ("/path") or path-relative target is
// rebuilt from the current scheme, host, port and URI. False if the result
// does not fit.
static bool resolveRedirectUrl(const char *target, char *out, size_t outLen) {
  char hostPort[DOWNLOAD_HOSTNAME_SIZE + 8];
  if (components.port != 0) {
    snprintf(hostPort, sizeof(hostPort), "%s:%u", components.host,
             components.port);
  } else {
    snprintf(hostPort, sizeof(hostPort), "%s", components.host);
  }
  int len;
  if (strstr(target, "://") != NULL) {
    len = snprintf(out, outLen, "%s", target);
  } else if (target[0] == '/') {
    len = snprintf(out, outLen, "%s://%s%s", components.protocol, hostPort,
                   target);
  } else {
    const char *lastSlash = strrchr(components.uri, '/');
    int baseLen = lastSlash ? (int)(lastSlash - components.uri) + 1 : 0;
    len = snprintf(out, outLen, "%s://%s%.*s%s", components.protocol, hostPort,
                   baseLen, components.uri, target);
  }
  return len > 0 && (size_t)len < outLen;
}

// Queue the body for download_poll() to write. The body of a redirect is read
// and dropped.
static err_t httpClientReceiveFileFn(__unused void *arg, struct altcp_pcb *conn,
                                     struct pbuf *ptr, err_t err) {
  // Check for null input or errors
  if (ptr == NULL) {
    DPRINTF("End of data or connection closed by the server.\n");
    return ERR_OK;  // Signal the connection closure
  }

  if (err != ERR_OK) {
    DPRINTF("Error receiving file: %i\n", err);
    pbuf_free(ptr);
    return ERR_VAL;  // Invalid input or error occurred
  }

  if (redirectPending) {
    altcp_recved(conn, ptr->tot_len);
    pbuf_free(ptr);
    return ERR_OK;
  }

  if (pending == NULL) {
    pending = ptr;
  } else {
    pbuf_cat(pending, ptr);
  }
  pendingConn = conn;
  downloadStatus = DOWNLOAD_STATUS_IN_PROGRESS;
  return ERR_OK;
}

// The response's headers. Only a 2xx reaches the file. A redirect records its
// target and lets the response finish and close normally: aborting it left
// lwIP unable to open later connections, in Booster. Anything else is aborted
// before its body.
static err_t httpClientHeaderCheckSizeFn(__unused httpc_state_t *connection,
                                         __unused void *arg, struct pbuf *hdr,
                                         u16_t hdrLen,
                                         __unused u32_t contentLen) {
  char *headerData = malloc(hdrLen + 1);
  if (headerData == NULL) {
    downloadError = DOWNLOAD_TRANSFER_ERROR;
    return ERR_MEM;  // Memory allocation failed
  }
  pbuf_copy_partial(hdr, headerData, hdrLen, 0);
  headerData[hdrLen] = '\0';  // Null-terminate the string

  // The status line comes first: "HTTP/1.x NNN ...".
  httpStatus = 0;
  if (strncmp(headerData, "HTTP/", 5) == 0) {
    const char *statusStart = strchr(headerData, ' ');
    if (statusStart != NULL) {
      httpStatus = atoi(statusStart + 1);
    }
  }
  DPRINTF("HTTP status %d\n", httpStatus);

  if (httpStatus == 301 || httpStatus == 302 || httpStatus == 303 ||
      httpStatus == 307 || httpStatus == 308) {
    bool tooLong = false;
    if (redirectHops >= DOWNLOAD_MAX_REDIRECTS) {
      DPRINTF("More than %d redirects\n", DOWNLOAD_MAX_REDIRECTS);
      downloadError = DOWNLOAD_TOOMANYREDIRECTS_ERROR;
    } else if (findHeaderValue(headerData, "Location:", location,
                               sizeof(location), &tooLong) &&
               resolveRedirectUrl(location, nextUrl, sizeof(nextUrl))) {
      DPRINTF("HTTP %d redirect to: %s\n", httpStatus, nextUrl);
      redirectPending = true;
      downloadStatus = DOWNLOAD_STATUS_IN_PROGRESS;
    } else {
      DPRINTF("HTTP %d without a usable Location\n", httpStatus);
      downloadError =
          tooLong ? DOWNLOAD_URLTOOLONG_ERROR : DOWNLOAD_HTTPSTATUS_ERROR;
    }
    free(headerData);
    return redirectPending ? ERR_OK : ERR_ABRT;
  }

  free(headerData);
  if (httpStatus < 200 || httpStatus >= 300) {
    DPRINTF("HTTP status %d: aborting before the body\n", httpStatus);
    downloadError = DOWNLOAD_HTTPSTATUS_ERROR;
    return ERR_ABRT;
  }
  downloadStatus = DOWNLOAD_STATUS_IN_PROGRESS;
  return ERR_OK;  // Header check passed
}

static void httpClientResultCompleteFn(void *arg, httpc_result_t httpcResult,
                                       u32_t rxContentLen, u32_t srvRes,
                                       err_t err) {
  HTTPC_REQUEST_T *req = (HTTPC_REQUEST_T *)arg;
  DPRINTF("Request complete: result %d len %u server_response %u err %d\n",
          httpcResult, rxContentLen, srvRes, err);
  req->complete = true;

  // Followed by download_poll(), with the status held at in progress.
  if (redirectPending) {
    return;
  }
  // A failure the callbacks already named: a status, a write, the redirects.
  if (downloadError != DOWNLOAD_OK) {
    fail(downloadError);
    return;
  }
  // httpcResult first: a timeout arrives with err == ERR_OK and srvRes == 0.
  if (httpcResult != HTTPC_RESULT_OK) {
    if (hopRetries < DOWNLOAD_MAX_HOP_RETRIES) {
      dropPending();  // the retry starts the file again
      retryPending = true;
      downloadStatus = DOWNLOAD_STATUS_IN_PROGRESS;
      return;
    }
    fail(httpcResult == HTTPC_RESULT_ERR_TIMEOUT ? DOWNLOAD_TIMEOUT_ERROR
                                                 : DOWNLOAD_TRANSFER_ERROR);
    return;
  }
  if (srvRes < 200 || srvRes >= 300) {
    httpStatus = (int)srvRes;
    fail(DOWNLOAD_HTTPSTATUS_ERROR);
    return;
  }
  transferDone = true;
  downloadStatus = DOWNLOAD_STATUS_IN_PROGRESS;
}

// Start one request: the first, a redirect's target or a retry. The file is
// opened afresh each time, so it only ever holds the final response.
static download_err_t issue(const char *url) {
  download_err_t err = parseUrl(url, &components);
  if (err != DOWNLOAD_OK) {
    DPRINTF("Cannot use the URL: %i\n", err);
    return err;
  }

  // The scheme picks the transport, per URL. A build without HTTPS refuses
  // https:// instead of fetching the same path over plain HTTP.
  int https = httpc_scheme_is_https(components.protocol);
  if (https < 0) {
    DPRINTF("Unsupported scheme: %s\n", components.protocol);
    return DOWNLOAD_UNSUPPORTEDSCHEME_ERROR;
  }
#if APP_DOWNLOAD_HTTPS == 1
  struct altcp_tls_config *tlsConfig = NULL;
  if (https) {
    tlsConfig = httpc_shared_tls_config();
    if (tlsConfig == NULL) {
      return DOWNLOAD_CANNOTSTARTDOWNLOAD_ERROR;
    }
  }
#else
  if (https) {
    DPRINTF("HTTPS is not built in (APP_DOWNLOAD_HTTPS=0)\n");
    return DOWNLOAD_HTTPSNOTBUILT_ERROR;
  }
#endif

  // Open the file for writing to the folder of the apps to the tmp.download
  // file
  char filename[DOWNLOAD_BUFFLINE_SIZE] = {0};
  getTmpFilenamePath(filename);
  DPRINTF("Downloading to file: %s\n", filename);
  FRESULT res;

  // Close any previously open handle
  DPRINTF("Closing any previously open file\n");
  f_close(&file);

  // Clear read-only attribute if necessary
  DPRINTF("Clearing read-only attribute, if any\n");
  f_chmod(filename, 0, AM_RDO);

  // Open file for writing or create if it doesn't exist
  DPRINTF("Opening file for writing\n");
  res = f_open(&file, filename, FA_WRITE | FA_CREATE_ALWAYS);
  if (res == FR_LOCKED) {
    DPRINTF("File is locked. Attempting to resolve...\n");

    // Try to remove the file and create it again
    DPRINTF("Removing file and creating again\n");
    res = f_unlink(filename);
    if (res == FR_OK || res == FR_NO_FILE) {
      DPRINTF("File removed. Creating again\n");
      res = f_open(&file, filename, FA_WRITE | FA_CREATE_ALWAYS);
    }
  }

  if (res != FR_OK) {
    DPRINTF("Error opening file %s: %i\n", filename, res);
    return DOWNLOAD_CANNOTOPENFILE_ERROR;
  }

  if (url != currentUrl) {
    snprintf(currentUrl, sizeof(currentUrl), "%s", url);
  }
  httpStatus = 0;
  transferDone = false;
  if (downloadStatus != DOWNLOAD_STATUS_IN_PROGRESS) {
    downloadStatus = DOWNLOAD_STATUS_STARTED;
  }

  request.url = components.uri;
  request.hostname = components.host;
  request.port = components.port;  // 0 lets httpc pick the scheme's default
  DPRINTF("HOST: %s. PORT: %u. URI: %s\n", components.host, components.port,
          components.uri);
  request.headers_fn = httpClientHeaderCheckSizeFn;
  request.recv_fn = httpClientReceiveFileFn;
  request.result_fn = httpClientResultCompleteFn;
  request.callback_arg = &request;
#if APP_DOWNLOAD_HTTPS == 1
  request.tls_config = tlsConfig;
#endif
  DPRINTF("Download with %s\n", https ? "HTTPS" : "HTTP");
  int result = http_client_request_async(cyw43_arch_async_context(), &request);
  if (result != 0) {
    DPRINTF("Error initializing the download: %i\n", result);
    res = f_close(&file);
    if (res != FR_OK) {
      DPRINTF("Error closing file %s: %i\n", filename, res);
    }
    return DOWNLOAD_CANNOTSTARTDOWNLOAD_ERROR;
  }
  return DOWNLOAD_OK;
}

download_err_t download_start() {
  // Download the file at the URL set with download_setFilepath() to the app
  // folder, through tmp.download. It is saved under the name at the end of
  // that URL, whatever redirects it goes through.
  redirectPending = false;
  retryPending = false;
  settling = false;
  transferDone = false;
  downloadStatus = DOWNLOAD_STATUS_STARTED;
  redirectHops = 0;
  hopRetries = 0;
  httpStatus = 0;
  memset(&fileUrl, 0, sizeof(fileUrl));
  dropPending();
  download_err_t err = filepathTooLong ? DOWNLOAD_URLTOOLONG_ERROR
                                       : parseUrl(filepath, &components);
  if (err == DOWNLOAD_OK && writeChunk == NULL) {
    writeChunk = malloc(DOWNLOAD_WRITE_CHUNK);
    if (writeChunk == NULL) {
      err = DOWNLOAD_TRANSFER_ERROR;
    }
  }
  if (err == DOWNLOAD_OK) {
    filenameFromUri(components.uri, &fileUrl);
    err = issue(filepath);
  }
  downloadError = err;
  if (err != DOWNLOAD_OK) {
    downloadStatus = DOWNLOAD_STATUS_FAILED;
  }
  return err;
}

download_poll_t download_poll() {
  if (pending != NULL) {
    writePendingChunk();
  }
  if (!request.complete) {
    // Never wait here: the caller's loop also answers the ST. Waiting up to
    // 100 ms per call for network work made the ST's commands wait as long
    // (measured on an ST: 153 ms per small command and 2 of 100 given up
    // during a download, against 2.1 ms without one).
    async_context_poll(cyw43_arch_async_context());
    return DOWNLOAD_POLL_CONTINUE;
  }
  if (pending != NULL) {
    return DOWNLOAD_POLL_CONTINUE;  // what arrived before the close
  }
  if (redirectPending || retryPending) {
    if (!settling) {
      // The request just closed: wait before opening the next connection.
      settling = true;
      downloadStatus = DOWNLOAD_STATUS_IN_PROGRESS;
      reissueAt = make_timeout_time_ms(DOWNLOAD_SETTLE_MS);
    }
    if (absolute_time_diff_us(get_absolute_time(), reissueAt) > 0) {
      async_context_poll(cyw43_arch_async_context());
      return DOWNLOAD_POLL_CONTINUE;
    }
    const char *url = currentUrl;
    if (redirectPending) {
      redirectHops++;
      hopRetries = 0;
      url = nextUrl;
      DPRINTF("Following redirect %d/%d\n", redirectHops,
              DOWNLOAD_MAX_REDIRECTS);
    } else {
      hopRetries++;
      DPRINTF("Retrying, attempt %d/%d\n", hopRetries,
              DOWNLOAD_MAX_HOP_RETRIES);
    }
    redirectPending = false;
    retryPending = false;
    settling = false;
    download_err_t err = issue(url);
    if (err != DOWNLOAD_OK) {
      fail(err);
      return DOWNLOAD_POLL_ERROR;
    }
    return DOWNLOAD_POLL_CONTINUE;
  }
  if (downloadStatus == DOWNLOAD_STATUS_FAILED || !transferDone) {
    return DOWNLOAD_POLL_ERROR;
  }
  downloadStatus = DOWNLOAD_STATUS_COMPLETED;
  return DOWNLOAD_POLL_COMPLETED;
}

download_err_t download_finish() {
  free(writeChunk);
  writeChunk = NULL;
  dropPending();
  if (downloadStatus != DOWNLOAD_STATUS_COMPLETED) {
    // The failure already closed and deleted the temporary file.
    f_close(&file);
    DPRINTF("Error downloading: %i\n", downloadError);
    return (downloadError != DOWNLOAD_OK) ? downloadError
                                          : DOWNLOAD_FORCEDABORT_ERROR;
  }

  // Close the file
  int res = f_close(&file);
  if (res != FR_OK) {
    DPRINTF("Error closing tmp file: %i\n", res);
    return DOWNLOAD_CANNOTCLOSEFILE_ERROR;
  }
  DPRINTF("File downloaded\n");

  return DOWNLOAD_OK;
}

download_err_t download_confirm() {
  // Get the filename of
  char fname[DOWNLOAD_BUFFLINE_SIZE] = {0};
  snprintf(
      fname, sizeof(fname), "%s/%s",
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER)->value,
      fileUrl.filename);

  DPRINTF("Writing file %s\n", fname);

  // Try to delete the file if they exist
  f_unlink(fname);

  // Now rename the tmp file to the final filename
  char tmpFname[DOWNLOAD_BUFFLINE_SIZE] = {0};
  getTmpFilenamePath(tmpFname);

  // Rename the file to the final filename
  FRESULT res = f_rename(tmpFname, fname);
  if (res != FR_OK) {
    DPRINTF("Error renaming file: %i\n", res);
    return DOWNLOAD_CANNOTRENAMEFILE_ERROR;
  }
  DPRINTF("Written file %s\n", fname);
  return DOWNLOAD_OK;
}

download_status_t download_getStatus() { return downloadStatus; }

void download_setStatus(download_status_t status) { downloadStatus = status; }

const char *download_getFilepath() { return filepath; }

void download_setFilepath(const char *path) {
  // A URL that does not fit is refused by download_start(), never cut short.
  filepathTooLong = strlen(path) >= sizeof(filepath);
  strncpy(filepath, path, sizeof(filepath) - 1);
  filepath[sizeof(filepath) - 1] = '\0';
}

const download_url_components_t *download_getUrlComponents() {
  return &components;
}

const char *download_getFilename() { return fileUrl.filename; }

download_err_t download_getError() { return downloadError; }

int download_getHttpStatus() { return httpStatus; }
