#!/usr/bin/env python3
"""Download real files on the device and check them by MD5.

The files are the ones Booster downloads: its firmware update and its app
catalog. The expected hashes are read at run time (upgrade.md5 and the
catalog's md5 fields), so the cases follow new releases.

  F1  atarist.sidecartridge.com/upgrade.bin, checked against upgrade.md5
  F2  a microfirmware of the catalog hosted on atarist.sidecartridge.com,
      checked against the catalog's MD5
  F3  the catalog itself (the md-store one over https://, Booster's legacy
      one over http://), checked against the copy the host fetches
  F4  a catalog entry behind redirects (downloads.neilrackett.com, then
      github.com, then a signed release-assets URL); its http:// form answers
      a redirect to https://
  F5  a file that does not exist: it must fail and leave no file behind
  PORT  F3 with the scheme's port written out (":80", ":443")
  LONG  a URL longer than the build takes: refused, never cut short

A build with HTTPS (APP_DOWNLOAD_HTTPS=1, "+https" in its build ID) runs every
case over http:// and https://. A build without it runs them over http://,
expects F4 and one https:// URL to fail cleanly, and expects nothing left in
the app folder by a failure.

Needs a debug build on the RP (the download runs in devdownload.c, started
through the devhooks mailbox), the SD card, Wi-Fi and the Debug Probe.

    python3 tools/dev/download_harness.py            # every case
    python3 tools/dev/download_harness.py F1 F5      # some cases
    python3 tools/dev/download_harness.py --url URL  # one URL, just report
    python3 tools/dev/download_harness.py --url URL --no-wait   # start it, free the probe
    python3 tools/dev/download_harness.py --status   # the last download's state

Exits 0 when every check passes. Writes tools/dev/logs/download-harness-<time>.json.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
import time
import urllib.parse
import urllib.request

import swd

HOST = "atarist.sidecartridge.com"
CATALOG = {"https": "https://md-store.sidecartridge.com/atari-st/apps.json",
           "http": "http://atarist.sidecartridge.com/apps.json"}
REDIRECTED_HOST = "downloads.neilrackett.com"
STATE_SYMBOL = "devdownloadState"
MAGIC = 0x4C445644
# DevdownloadState (rp/src/include/devdownload.h)
HEADER = struct.Struct("<9I16s")
URL_OFFSET = HEADER.size
STATES = {0: "idle", 1: "downloading", 2: "hashing", 3: "done", 4: "failed"}
STEPS = {0: "", 1: "download_start", 2: "download_finish", 3: "download_confirm",
         4: "open", 5: "read"}
LEFT_FILE, LEFT_TMP = 1, 2
STEPS_WITH_DOWNLOAD_ERR = ("download_start", "download_finish", "download_confirm")
DOWNLOAD_H = os.path.join(swd.REPO, "rp", "src", "include", "download.h")


def download_errors() -> list[str]:
    """download_err_t's names, in order, from download.h."""
    text = open(DOWNLOAD_H, encoding="utf-8").read()
    body = text[text.index("typedef enum {\n  DOWNLOAD_OK"):]
    body = body[:body.index("} download_err_t;")]
    return re.findall(r"^\s*(DOWNLOAD_\w+)", body, re.M)


def fetch(url: str) -> bytes:
    # atarist.sidecartridge.com answers 403 to Python's default User-Agent.
    req = urllib.request.Request(url, headers={"User-Agent": "download_harness"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


class Device:
    def __init__(self, elf: str):
        self.elf = elf
        sym = swd.elf_symbols(elf, STATE_SYMBOL).get(STATE_SYMBOL)
        if not sym:
            raise swd.SwdError(f"{os.path.basename(elf)} has no {STATE_SYMBOL}: "
                               "a debug build is needed")
        self.base, size = sym
        self.url_size = size - URL_OFFSET
        self.command = swd.include_defines()["DEVHOOKS_APP_DOWNLOAD"]

    def state(self) -> dict:
        v = HEADER.unpack(swd.read_memory(self.base, HEADER.size))
        if v[0] != MAGIC:
            raise swd.SwdError(f"no download state at 0x{self.base:08x}")
        return {"seq": v[1], "state": STATES.get(v[2], v[2]), "step": STEPS.get(v[3], v[3]),
                "error": v[4], "left": v[5], "bytes": v[6], "ms": v[7], "lwip_errors": v[8],
                "md5": v[9].hex()}

    def start(self, url: str) -> int:
        """Start downloading url; returns the sequence number it had before."""
        data = url.encode() + b"\0"
        if len(data) > self.url_size:
            raise swd.SwdError(f"URL longer than the device's {self.url_size - 1} bytes")
        data += b"\0" * (-len(data) % 4)
        swd.openocd(*(f"mww 0x{self.base + URL_OFFSET + i:08x} "
                      f"0x{struct.unpack_from('<I', data, i)[0]:08x}"
                      for i in range(0, len(data), 4)))
        seq = self.state()["seq"]
        if not swd.mailbox_request(self.elf, swd.KIND_APP, self.command, []):
            raise swd.SwdError("the device refused the download: one is running")
        return seq

    def download(self, url: str, timeout: float) -> dict:
        seq = self.start(url)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            s = self.state()
            if s["seq"] != seq and s["state"] in ("done", "failed"):
                return s
            time.sleep(1.0)
        s = self.state()
        s["timeout"] = True
        return s


def counters(elf: str) -> dict:
    sym = swd.elf_symbols(elf, "chandlerDropped", "commOverruns", "chandlerChecksumErrors")
    return {n: struct.unpack("<I", swd.read_memory(a, 4))[0] for n, (a, _) in sym.items()}


def cases(https_build: bool, wanted: list[str]) -> list[dict]:
    """(name, url, expectation) for this build, expectations as dicts."""
    catalog = {s: json.loads(fetch(CATALOG[s])) for s in ("http", "https")}
    apps = catalog["https"] if isinstance(catalog["https"], list) else catalog["https"]["apps"]
    local = next(a for a in apps if f"//{HOST}/" in a.get("binary", ""))
    remote = next(a for a in apps if f"//{REDIRECTED_HOST}/" in a.get("binary", ""))
    out = []
    schemes = ("http", "https") if https_build else ("http",)
    for s in schemes:
        md5 = fetch(f"https://{HOST}/upgrade.md5").decode().split()[0].lower()
        out.append({"case": "F1", "url": f"{s}://{HOST}/upgrade.bin", "md5": md5, "timeout": 600})
        out.append({"case": "F2", "url": local["binary"].replace("https://", f"{s}://", 1),
                    "md5": local["md5"].lower(), "timeout": 300})
        body = fetch(CATALOG[s])
        out.append({"case": "F3", "url": CATALOG[s], "md5": hashlib.md5(body).hexdigest(),
                    "timeout": 120})
        f4 = remote["binary"].replace("https://", f"{s}://", 1)
        if https_build:
            out.append({"case": "F4", "url": f4, "md5": remote["md5"].lower(), "timeout": 300})
        else:
            out.append({"case": "F4", "url": f4, "fail": "DOWNLOAD_HTTPSNOTBUILT_ERROR",
                        "timeout": 120})
        out.append({"case": "F5", "url": f"{s}://{HOST}/does-not-exist-{int(time.time())}.uf2",
                    "fail": "DOWNLOAD_HTTPSTATUS_ERROR", "timeout": 120})
        # Beyond Booster's set: an explicit port, and a URL past the profile's
        # DOWNLOAD_URL_SIZE (256, or 1536 with HTTPS), which must be refused.
        port = 443 if s == "https" else 80
        parts = urllib.parse.urlsplit(CATALOG[s])
        out.append({"case": "PORT", "url": urllib.parse.urlunsplit(
                        parts._replace(netloc=f"{parts.hostname}:{port}")),
                    "md5": hashlib.md5(body).hexdigest(), "timeout": 120})
        limit = 1536 if https_build else 256
        out.append({"case": "LONG", "url": f"{s}://{HOST}/{'a' * limit}.bin",
                    "fail": "DOWNLOAD_URLTOOLONG_ERROR", "timeout": 60})
    if not https_build:
        out.append({"case": "F1", "url": f"https://{HOST}/upgrade.bin",
                    "fail": "DOWNLOAD_HTTPSNOTBUILT_ERROR", "timeout": 120})
    return [c for c in out if not wanted or c["case"] in wanted]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cases", nargs="*", help="F1 ... F5 (default: all)")
    ap.add_argument("--url", help="download this URL and report, no checks")
    ap.add_argument("--no-wait", action="store_true",
                    help="with --url: start it and return, leaving the probe free")
    ap.add_argument("--status", action="store_true", help="print the last download's state")
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--elf")
    args = ap.parse_args()
    try:
        elf = swd.matching_elf(args.elf)
        dev = Device(elf)
        if args.status:
            print(json.dumps(dev.state(), indent=2))
            return 0
        if args.url and args.no_wait:
            dev.start(args.url)
            print("started")
            return 0
        if args.url:
            print(json.dumps(dev.download(args.url, args.timeout), indent=2))
            return 0
        https_build = "+https" in (swd.elf_build_id(elf) or "")
        print(f"build {os.path.basename(elf)}: {'HTTP and HTTPS' if https_build else 'HTTP only'}")
        before = counters(elf)
        lwip_before = dev.state()["lwip_errors"]
        errors = download_errors()
        results = []
        for c in cases(https_build, args.cases):
            s = dev.download(c["url"], c["timeout"])
            size = f"{s['bytes']} bytes in {s['ms'] / 1000:.1f} s"
            if "md5" in c:
                ok = s["state"] == "done" and s["md5"] == c["md5"]
                detail = (f"{s['state']}, {size} ({s['bytes'] / max(s['ms'], 1):.0f} KB/s), "
                          f"md5 {'matches' if s['md5'] == c['md5'] else s['md5'] + ' != ' + c['md5']}")
            else:
                ok = (s["state"] == "failed" and not s["left"] & (LEFT_FILE | LEFT_TMP)
                      and s["step"] in STEPS_WITH_DOWNLOAD_ERR
                      and errors[s["error"]] == c["fail"])
                detail = s["state"] + (f", {size} stored as the file" if s["state"] == "done" else "")
                left = [n for bit, n in ((LEFT_FILE, "the file"), (LEFT_TMP, "tmp.download"))
                        if s["left"] & bit]
                if left:
                    detail += f", left {' and '.join(left)}"
            if s["state"] == "failed":
                name = (errors[s["error"]] if s["step"] in STEPS_WITH_DOWNLOAD_ERR
                        and s["error"] < len(errors) else f"FRESULT {s['error']}")
                detail += f" at {s['step']}: {name}"
            if s.get("timeout"):
                ok, detail = False, detail + ", TIMEOUT"
            want = "the expected MD5" if "md5" in c else f"a clean {c['fail']}"
            print(f"  {'PASS' if ok else 'FAIL'}  {c['case']} {c['url'][:70]}: "
                  f"{want}: {detail}")
            results.append({**c, "ok": ok, "result": s})
        lwip_after = dev.state()["lwip_errors"]
        ok = lwip_after == lwip_before
        print(f"  {'PASS' if ok else 'FAIL'}  lwIP never failed an allocation: "
              f"{lwip_after - lwip_before} failed during the run")
        results.append({"case": "lwip", "ok": ok, "failed": lwip_after - lwip_before})
        after = counters(elf)
        moved = {k: after[k] - before[k] for k in after if after[k] != before[k]}
        ok = not moved
        print(f"  {'PASS' if ok else 'FAIL'}  the command channel stayed clean: "
              f"{moved or 'no drops, overruns or checksum errors'}")
        results.append({"case": "channel", "ok": ok, "moved": moved})
    except (swd.SwdError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    failed = [r for r in results if not r["ok"]]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    report = os.path.join(swd.HERE, "logs", time.strftime("download-harness-%Y%m%d-%H%M%S.json"))
    with open(report, "w") as f:
        json.dump(results, f, indent=2)
    print(f"report: {report}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
