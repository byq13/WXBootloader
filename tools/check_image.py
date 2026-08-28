#!/usr/bin/env python3
"""
Check whether a Davis firmware image satisfies the bootloader's assumptions.

    ./tools/check_image.py image1.bin image2.bin ...

Verifies the three things app_valid() in davis_boot.c relies on:

  1. image size == exactly the whole application section
     (Vue 0x1F000 = 126976 B, VP2/Envoy 0x1F800 = 129024 B)
  2. the last two bytes == CRC-16/XMODEM of everything before them,
     stored big-endian (poly 0x1021, init 0x0000, MSB first)
  3. the reset vector is not 0xFFFF

If any of these fails, the bootloader will not hand over to the
application and will stay in command mode instead. The console is not
bricked - the 'A' command still starts the application - but it will not
boot on its own.

Verified against three real images: Vue 4.30 (0xD795), VP2 3.80 (0xF6CD)
and VP2 3.88 (0x9E6B). Only one Vue version has been checked so far, so
running this against any further image is genuinely useful.
"""

import sys
import os

SECTIONS = {
    0x1F000: ("Vue",        "BOOTSZ=01, 4 KiB boot section"),
    0x1F800: ("VP2/Envoy",  "BOOTSZ=10, 2 KiB boot section"),
    0x1FC00: ("?",          "BOOTSZ=11, 1 KiB boot section"),
    0x1E000: ("?",          "BOOTSZ=00, 8 KiB boot section"),
}


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def check(path):
    with open(path, "rb") as fh:
        d = fh.read()

    print("=== %s ===" % os.path.basename(path))
    print("  size           %d B = 0x%05X" % (len(d), len(d)))

    ok = True

    plat = SECTIONS.get(len(d))
    if plat:
        print("  platform       %s (%s)" % plat)
    else:
        print("  platform       UNKNOWN - size matches no boot section layout")
        print("                 the bootloader computes the CRC up to")
        print("                 BOOT_START-2, so the footer would sit at the")
        print("                 wrong address and the check would fail")
        ok = False

    if len(d) < 4:
        print("  image too short")
        return False

    calc = crc16_xmodem(d[:-2])
    stored = (d[-2] << 8) | d[-1]
    good = calc == stored
    print("  CRC computed   %04X" % calc)
    print("  CRC in footer  %04X   %s" % (stored, "MATCH" if good else "MISMATCH"))
    ok = ok and good

    rst = d[0] | (d[1] << 8)
    print("  reset vector   %04X %s" % (rst, "(empty!)" if rst == 0xFFFF else ""))
    ok = ok and rst != 0xFFFF

    # Word just before the footer: holds the version x100 on Vue, 0xFFFF on
    # VP2. Not universal, so it is reported but never relied upon.
    ver = d[-4] | (d[-3] << 8)
    if ver != 0xFFFF:
        print("  word before CRC %04X = %d  (Vue: version x100; VP2: FFFF)"
              % (ver, ver))

    print("  --> %s" % ("OK, the bootloader will start this application"
                        if ok else "BOOTLOADER WILL STAY IN COMMAND MODE"))
    print()
    return ok


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    results = [check(p) for p in sys.argv[1:]]
    n_ok = sum(results)
    print("%d / %d images pass" % (n_ok, len(results)))
    sys.exit(0 if n_ok == len(results) else 1)
