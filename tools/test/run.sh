#!/bin/sh
# Build and run the host-side tests for the radio firmware's pure logic:
# Valve's UART framing, the state-record decoder and the HID report packing.
set -e
cd "$(dirname "$0")"
out="${TMPDIR:-/tmp}/sc_radio_test"
gcc -std=gnu99 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
    -I ../../nrf51/src \
    -o "$out" test_radio.c \
    ../../nrf51/src/vframe.c ../../nrf51/src/sc_state.c ../../nrf51/src/gamepad_hid.c
exec "$out"
