#!/usr/bin/env python3
"""
Parse a HID report descriptor and report, per report ID, how many bytes each
input/output/feature report occupies.

Used to check that the gamepad report map compiled into the radio firmware
agrees with GAMEPAD_INPUT_REP_LEN, and to dump Valve's stock report map for
comparison.

    ./check_report_map.py --bin path/to/sc_ble_gamepad.bin
    ./check_report_map.py --file raw_descriptor.bin
    ./check_report_map.py --bin radio.bin --offset 0x1eda0 --length 153
"""

import argparse
import sys
from collections import defaultdict

# Main item tags (bTag of a main item, bType == 0)
MAIN_INPUT, MAIN_OUTPUT, MAIN_FEATURE = 0x8, 0x9, 0xB
MAIN_COLLECTION, MAIN_END_COLLECTION = 0xA, 0xC

USAGE_PAGES = {0x01: "Generic Desktop", 0x07: "Keyboard/Keypad", 0x08: "LED",
               0x09: "Button", 0x0C: "Consumer"}

GD_USAGES = {0x01: "Pointer", 0x02: "Mouse", 0x04: "Joystick", 0x05: "Gamepad",
             0x06: "Keyboard", 0x30: "X", 0x31: "Y", 0x32: "Z", 0x33: "Rx",
             0x34: "Ry", 0x35: "Rz", 0x38: "Wheel", 0x39: "Hat switch"}


def page_name(page):
    if page is None:
        return "?"
    if page in USAGE_PAGES:
        return USAGE_PAGES[page]
    if 0xFF00 <= page <= 0xFFFF:
        return f"Vendor 0x{page:04X}"
    return f"0x{page:04X}"


def parse(desc):
    """Walk the descriptor, returning (reports, collections, errors, items)."""
    # bits[(report_id, kind)] = total bits
    bits = defaultdict(int)
    errors = []
    items = []

    report_id = 0          # 0 means "no Report ID item seen"
    report_size = None
    report_count = None
    usage_page = None
    depth = 0
    max_depth = 0
    saw_report_id = False

    i = 0
    n = len(desc)
    while i < n:
        prefix = desc[i]
        if prefix == 0xFE:
            errors.append(f"offset {i}: long items are not supported")
            break

        size_code = prefix & 0x03
        size = {0: 0, 1: 1, 2: 2, 3: 4}[size_code]
        itype = (prefix >> 2) & 0x03
        tag = (prefix >> 4) & 0x0F

        if i + 1 + size > n:
            errors.append(f"offset {i}: item data runs past end of descriptor")
            break

        data = desc[i + 1:i + 1 + size]
        uval = int.from_bytes(data, "little") if data else 0

        if itype == 1:      # Global
            if tag == 0x0:
                usage_page = uval
            elif tag == 0x7:
                report_size = uval
            elif tag == 0x8:
                report_id = uval
                saw_report_id = True
                if uval == 0:
                    errors.append(f"offset {i}: Report ID 0 is reserved")
            elif tag == 0x9:
                report_count = uval
        elif itype == 0:    # Main
            if tag == MAIN_COLLECTION:
                depth += 1
                max_depth = max(max_depth, depth)
            elif tag == MAIN_END_COLLECTION:
                depth -= 1
                if depth < 0:
                    errors.append(f"offset {i}: End Collection without Collection")
            elif tag in (MAIN_INPUT, MAIN_OUTPUT, MAIN_FEATURE):
                kind = {MAIN_INPUT: "Input", MAIN_OUTPUT: "Output",
                        MAIN_FEATURE: "Feature"}[tag]
                if report_size is None or report_count is None:
                    errors.append(f"offset {i}: {kind} before Report Size/Count")
                else:
                    bits[(report_id, kind)] += report_size * report_count
                    items.append((report_id, kind, report_size, report_count,
                                  page_name(usage_page)))
        i += 1 + size

    if depth != 0:
        errors.append(f"unbalanced collections: {depth} left open")

    reports = {}
    for (rid, kind), nbits in sorted(bits.items()):
        if nbits % 8:
            errors.append(f"Report ID {rid} {kind}: {nbits} bits is not a whole "
                          f"number of bytes")
        reports[(rid, kind)] = (nbits + 7) // 8

    return reports, max_depth, errors, items, saw_report_id


def find_in_image(img):
    """Find a plausible report map in a firmware image.

    Looks for a Usage Page (Generic Desktop) + Usage (Gamepad/Mouse/Keyboard/
    Joystick) opening and follows the descriptor until its collections balance.
    """
    found = []
    for start in range(len(img) - 4):
        if img[start] != 0x05 or img[start + 1] != 0x01:
            continue
        if img[start + 2] != 0x09 or img[start + 3] not in (0x02, 0x04, 0x05, 0x06):
            continue
        # Walk forward until collection depth returns to zero.
        i, depth, opened = start, 0, False
        while i < len(img):
            prefix = img[i]
            size = {0: 0, 1: 1, 2: 2, 3: 4}[prefix & 0x03]
            itype = (prefix >> 2) & 0x03
            tag = (prefix >> 4) & 0x0F
            i += 1 + size
            if itype == 0:
                if tag == MAIN_COLLECTION:
                    depth += 1
                    opened = True
                elif tag == MAIN_END_COLLECTION:
                    depth -= 1
                    if opened and depth == 0:
                        found.append((start, i - start))
                        break
            if i - start > 512:
                break
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", help="firmware image to search for a report map")
    ap.add_argument("--file", help="file containing a raw report descriptor")
    ap.add_argument("--offset", type=lambda x: int(x, 0),
                    help="explicit offset into --bin")
    ap.add_argument("--length", type=lambda x: int(x, 0),
                    help="explicit length at --offset")
    ap.add_argument("--expect-input-len", type=int,
                    help="fail unless every Input report is this many bytes")
    args = ap.parse_args()

    candidates = []
    if args.file:
        candidates.append((args.file, 0, open(args.file, "rb").read()))
    elif args.bin:
        img = open(args.bin, "rb").read()
        if args.offset is not None:
            length = args.length or (len(img) - args.offset)
            candidates.append((args.bin, args.offset,
                               img[args.offset:args.offset + length]))
        else:
            hits = find_in_image(img)
            if not hits:
                print(f"no report map found in {args.bin}", file=sys.stderr)
                return 1
            for off, length in hits:
                candidates.append((args.bin, off, img[off:off + length]))
    else:
        ap.error("need --bin or --file")

    rc = 0
    for name, off, desc in candidates:
        print(f"=== {name} @ 0x{off:x}, {len(desc)} bytes ===")
        reports, depth, errors, items, saw_rid = parse(desc)

        for rid, kind, rsize, rcount, page in items:
            print(f"  ID {rid}  {kind:<7} {rcount:>3} x {rsize:>2} bits"
                  f"   ({page})")

        print("  --")
        for (rid, kind), nbytes in sorted(reports.items()):
            total = nbytes + (1 if saw_rid else 0)
            print(f"  Report ID {rid} {kind}: {nbytes} bytes payload"
                  f" ({total} bytes on the wire with the ID prefix)")
            # S110 8.0.0 fixes ATT_MTU at 23, so a notification carries <= 20.
            if kind == "Input" and nbytes > 20:
                print(f"    WARNING: {nbytes} bytes exceeds the 20-byte limit of"
                      f" a single ATT notification at ATT_MTU 23")
                rc = 1
            if args.expect_input_len and kind == "Input" \
                    and nbytes != args.expect_input_len:
                print(f"    ERROR: expected {args.expect_input_len} bytes")
                rc = 1

        print(f"  max collection depth: {depth}")
        if errors:
            rc = 1
            for e in errors:
                print(f"  ERROR: {e}")
        else:
            print("  descriptor is well formed")
        print()

    return rc


if __name__ == "__main__":
    sys.exit(main())
