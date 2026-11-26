#!/usr/bin/env python3
"""Merge the last PlatformIO build into one full-flash image for fresh boards.

Standalone and manual -- the build never runs this. Run it after a normal
`pio run`, whenever you want a newer image (it doesn't need refreshing per
release: flash a fresh board once with any image, then update it over the air):

    python provisioning/make_image.py

Writes provisioning/vibrashock-v<version>-full-flash.bin (replacing older ones):
bootloader + partition table + otadata + app at their real offsets, gaps (incl.
nvs) left erased, to be written at 0x0 by flasher.pyw or a browser flasher.
USB ONLY -- never upload it through the dashboard's firmware update (use the
plain vibrashock-v<version>.bin there).
"""

import csv
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
ENV = "esp32-s3-selftest"
BUILD = os.path.join(ROOT, ".pio", "build", ENV)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def partition_offsets():
    """otadata + first app offsets from partitions_ota.csv (Name,Type,SubType,Offset,...)."""
    otadata = app = None
    lines = [ln for ln in read(os.path.join(ROOT, "partitions_ota.csv")).splitlines()
             if ln.strip() and not ln.lstrip().startswith("#")]
    for row in csv.reader(lines):
        row = [c.strip() for c in row]
        if row[1] == "data" and row[2] == "ota":
            otadata = row[3]
        elif row[1] == "app" and app is None:
            app = row[3]
    return otadata, app


def esptool_cmd():
    """PlatformIO's own esptool (what the build uses), else a pip-installed one."""
    pio = os.path.join(os.path.expanduser("~"), ".platformio")
    py = os.path.join(pio, "penv", "Scripts" if os.name == "nt" else "bin",
                      "python.exe" if os.name == "nt" else "python")
    tool = os.path.join(pio, "packages", "tool-esptoolpy", "esptool.py")
    if os.path.isfile(py) and os.path.isfile(tool):
        return [py, tool]
    return [sys.executable, "-m", "esptool"]


def main():
    version = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"',
                        read(os.path.join(ROOT, "src", "version.h"))).group(1)
    m = re.search(r"^CONFIG_PARTITION_TABLE_OFFSET=(\S+)",
                  read(os.path.join(ROOT, "sdkconfig." + ENV)), re.M)
    pt_off = m.group(1) if m else "0x8000"
    otadata_off, app_off = partition_offsets()

    parts = [("0x0", "bootloader.bin"), (pt_off, "partitions.bin"),
             (otadata_off, "ota_data_initial.bin"), (app_off, "firmware.bin")]
    missing = [f for _, f in parts if not os.path.isfile(os.path.join(BUILD, f))]
    if missing:
        sys.exit("Missing build output (%s) -- run `pio run` first." % ", ".join(missing))

    out = os.path.join(HERE, "vibrashock-v%s-full-flash.bin" % version)
    # keep = flash mode/freq/size exactly as in the built bootloader header,
    # i.e. what the board boots with after a normal `pio run -t upload`.
    cmd = esptool_cmd() + ["--chip", "esp32s3", "merge_bin", "-o", out,
                           "--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", "keep"]
    for off, f in parts:
        cmd += [off, os.path.join(BUILD, f)]
    if subprocess.run(cmd).returncode != 0:
        sys.exit("merge failed")
    for old in glob.glob(os.path.join(HERE, "*-full-flash.bin")):
        if os.path.abspath(old) != os.path.abspath(out):
            os.remove(old)  # one image here, so the flasher can't pick a stale one
    print("\nFull-flash image: %s" % out)
    for off, f in parts:
        print("  %-8s %s" % (off, f))


if __name__ == "__main__":
    main()
