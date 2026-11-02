#!/usr/bin/env python3
"""
Static checks on a built radio image.

These are the properties that can be established from the binary alone. What
this does NOT establish is that anything registers or behaves correctly at
runtime: only hardware can show that. In particular, a GATT characteristic UUID
appearing in the image proves the constant was compiled in, not that
sd_ble_gatts_characteristic_add succeeded on the device.

    ./check_image.py nrf51/armgcc/_build/sc_ble_gamepad.bin \
        --elf nrf51/armgcc/_build/sc_ble_gamepad.elf
"""

import argparse
import struct
import subprocess
import sys

S110_APP_BASE = 0x18000
# pstorage keeps bonds in the last two 1 kB pages of flash. On a 128 kB part
# (docs/FINDINGS.md finding 5) that is 0x1F800 up, and the image must stop
# short of it. On a larger part this is merely cautious.
APP_CEILING = 0x1F800
# Top of a 16 kB part is 0x20004000; the last 16 bytes hold main.c's fault
# record, which survives a soft reset, so the stack starts below it.
STACK_TOP = 0x20003FF0

# Vector slots whose handler must be the real one, not the startup file's
# weak default. A default there means an interrupt that hangs the radio the
# first time it fires, which no amount of host testing reveals. HardFault is
# on the list because the default spins instead of recording and resetting.
REQUIRED_VECTORS = {
    3: "HardFault_Handler",
    16 + 2: "UART0_IRQHandler",
    16 + 17: "RTC1_IRQHandler",
    16 + 20: "SWI0_IRQHandler",
    16 + 22: "SWI2_IRQHandler",
}

# 5c0e0000-7b3a-4e2d-9a51-3d8c2f6b1e47, the diagnostics service base UUID,
# as the little-endian bytes compiled into diag.c.
DIAG_BASE_UUID = bytes([0x47, 0x1E, 0x6B, 0x2F, 0x8C, 0x3D, 0x51, 0x9A,
                        0x2D, 0x4E, 0x3A, 0x7B, 0x00, 0x00, 0x0E, 0x5C])

HID_UUIDS = {
    0x2A4A: "HID Information",
    0x2A4B: "Report Map",
    0x2A4C: "HID Control Point",
    0x2A4D: "Report",
    0x2A4E: "Protocol Mode",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--elf", help="matching ELF, to check vector table symbols")
    args = ap.parse_args()

    img = open(args.image, "rb").read()
    failures = []

    def check(ok, msg):
        print(f"  {'ok  ' if ok else 'FAIL'}  {msg}")
        if not ok:
            failures.append(msg)

    print(f"=== {args.image} ({len(img)} bytes) ===")

    if len(img) < 8:
        print("  FAIL  image is too small to hold a vector table")
        return 1

    initial_sp, reset_vector = struct.unpack_from("<II", img, 0)

    check(
        initial_sp == STACK_TOP,
        f"initial stack pointer is 0x{initial_sp:08x} (expected 0x{STACK_TOP:08x}, "
        f"the top of a 16 kB part less the fault record)",
    )
    check(
        S110_APP_BASE <= (reset_vector & ~1) < S110_APP_BASE + len(img),
        f"reset vector 0x{reset_vector:08x} points inside the image",
    )
    check(reset_vector & 1 == 1, "reset vector has the Thumb bit set")
    check(len(img) % 4 == 0, f"image length is a whole number of words ({len(img)} bytes)")

    end = S110_APP_BASE + len(img)
    check(
        end <= APP_CEILING,
        f"image occupies 0x{S110_APP_BASE:05x}-0x{end - 1:05x}, within the "
        f"0x{APP_CEILING:05x} budget ({APP_CEILING - end} bytes spare)",
    )

    print("  --")
    for uuid, name in sorted(HID_UUIDS.items()):
        n = img.count(uuid.to_bytes(2, "little"))
        check(n > 0, f"UUID 0x{uuid:04X} ({name}) is compiled into the image")
    check(DIAG_BASE_UUID in img, "diagnostics service base UUID is compiled into the image")

    if args.elf:
        print("  --")
        nm = subprocess.run(["arm-none-eabi-nm", args.elf], capture_output=True,
                            text=True, check=True).stdout
        # The startup file declares every handler as a weak alias of
        # Default_Handler, so nm lists e.g. "W UART0_IRQHandler" at the default
        # handler's address even when no real handler was linked. Require a
        # strong text symbol ('T'), and not at Default_Handler's address.
        strong = {}
        default_addr = None
        for line in nm.splitlines():
            parts = line.split()
            if len(parts) != 3:
                continue
            addr, kind, name = int(parts[0], 16), parts[1], parts[2]
            if name == "Default_Handler":
                default_addr = addr
            if kind == "T":
                strong.setdefault(addr, []).append(name)
        for slot, want in sorted(REQUIRED_VECTORS.items()):
            addr = struct.unpack_from("<I", img, slot * 4)[0] & ~1
            names = strong.get(addr, [])
            ok = want in names and addr != default_addr
            check(ok, f"vector {slot} -> 0x{addr:05x} {'/'.join(names) or 'weak/default'} "
                      f"(want a real {want})")

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED")
        return 1
    print("all static checks passed")
    print("NOTE: runtime registration and host binding are NOT verified by this script.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
