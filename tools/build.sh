#!/bin/sh
# Build the radio firmware, run every check, and write dist/.
#
#   NRF_SDK=/path/to/nRF5_SDK_10.0.0 tools/build.sh
#
# The version is the last commit's time, so a commit always builds the same
# binary. Override with BUILD_UNIX_TIME.
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
: "${NRF_SDK:?set NRF_SDK to the nRF51 SDK 10.0.0 directory}"
: "${BUILD_UNIX_TIME:=$(git -C "$root" log -1 --format=%ct)}"
build="$root/nrf51/armgcc/_build"
dist="$root/dist"

make -s -C "$root/nrf51/armgcc" clean
make -s -C "$root/nrf51/armgcc" NRF_SDK="$NRF_SDK" BUILD_UNIX_TIME="$BUILD_UNIX_TIME" 2>&1 \
    | grep -v -e 'is not implemented and will always fail' -e 'does not take linker garbage collection' || true
[ -f "$build/sc_ble_gamepad.bin" ] || { echo "FAILED: build"; exit 1; }

"$root/tools/test/run.sh"
"$root/tools/check_image.py" "$build/sc_ble_gamepad.bin" --elf "$build/sc_ble_gamepad.elf"
"$root/tools/check_report_map.py" --bin "$build/sc_ble_gamepad.bin" --expect-input-len 14

rm -rf "$dist" && mkdir -p "$dist"
cp "$build/sc_ble_gamepad.bin" "$build/build.json" "$dist/"
(cd "$dist" && sha256sum sc_ble_gamepad.bin build.json > SHA256SUMS)
echo "built $(cat "$dist/build.json")"
