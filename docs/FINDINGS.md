# Reverse engineering notes

Everything here was derived from two public repositories and the firmware blobs
they ship, not measured on hardware. Each finding is labelled with how it was
established and how to re-check it. The firmware built on them has since run on
a real controller.

Sources:

- [greggersaurus/OpenSteamController](https://github.com/greggersaurus/OpenSteamController)
  — LPC11U37 reverse engineering, pinout tables, custom LPC firmware.
- [mitchmikusek/steam_controller_tools](https://github.com/mitchmikusek/steam_controller_tools)
  — browser flash tool, and the firmware blobs bundled under
  `packages/ui/public/fw_images/`.

Byte offsets below refer to `ble/vcf_wired_controller_d0g_5a0e3f348_radio.bin`
from the flash tool, which is a flat image based at nRF51 flash address 0, so
file offset and flash address are the same number.

---

## 1. Arbitrary code can already be put on the nRF51822

**Verified, from source.**

The LPC11U37's BLE firmware implements an SWD programmer for the radio and
exposes it over USB HID. `packages/protocol/src/constants.ts` names the
commands and `controller.ts` drives them:

| Command | ID | Meaning |
|---|---|---|
| `SendIRCode` | `0xC6` | Despite the name, starts the SWD interface (`swdStart` sends `0xC6 04 17 ED FE D0`) |
| `SWDErase` | `0xCA` | Mass-erase the nRF51 |
| `FlashSWD` | `0xC7` | Write up to 56 bytes at an arbitrary 32-bit address |
| `SWDSave` | `0xCB` | Commit |

`FlashSWD` takes a full 32-bit destination address, so this is a general
"write anything anywhere in nRF51 flash" primitive, not a fixed-layout updater.
That is the whole reason this project is feasible from a browser.

It also means **the nRF51 is not a brick risk**. The LPC keeps enumerating over
USB whatever the radio is doing, so a radio image that crashes on boot is
always recoverable by re-running the flash tool.

## 2. The BLE radio image is stock S110 8.0.0 plus a small Valve application

**Verified, byte-for-byte.**

`s110_nrf51_8.0.0_softdevice.bin` (92,440 bytes) is identical to
`components/softdevice/s110/hex/s110_nrf51_8.0.0_softdevice.hex` from nRF5
SDK 10.0.0. Converting the SDK hex to a flat image and comparing gives zero
differing bytes; the only discrepancy is 0x7C0–0xFFF, which the hex leaves
unprogrammed and the `.bin` fills with zeros.

`vcf_wired_controller_d0g_5a0e3f348_radio.bin` (126,996 bytes = 0x1F014) is a
full image from address 0:

| Range | Contents |
|---|---|
| `0x00000`–`0x16917` | S110 8.0.0 SoftDevice (identical to the blob above) |
| `0x16918`–`0x169FF` | Padding |
| `0x16A00`–`0x17E50` | More Valve code: the RADIO and RTC1 interrupt handlers and the UART framer |
| `0x18000`–`0x1F013` | Valve's radio application, 28,692 bytes |

`0x18000` is S110 8.0.0's application base, and the words at `0x18000` are a
plausible vector table: SP `0x20003E28`, reset vector `0x000180F5`.

An earlier version of this table called everything from `0x16918` to `0x17FFF`
padding. It is not: 4.5 kB of it is code the application calls and vectors into
(the RADIO IRQ at `0x16AD8`, RTC1 at `0x16B2C`, the UART frame sender behind
`0x16A46`). Our image does not need it, since it replaces the whole application.

Two consequences:

- A custom application only has to replace `0x18000` upward. The SoftDevice
  below it can stay exactly where it is.
- The flash tool's BLE path writes the SoftDevice at 0 and then writes the radio
  blob at offset 0 as well (`radioAppOffset: 0` in `firmware.ts`), so the second
  write completely covers the first. That is harmless for Valve's blob, because
  it already contains the SoftDevice, but it means the separate SoftDevice write
  is redundant on that path.

## 3. Valve's BLE firmware advertises a mouse and a keyboard, and no gamepad

**Verified, by decoding the descriptor.**

There is exactly one HID report map in the image, 153 bytes at `0x1EDA0`. It
contains three top-level collections:

| Report ID | Collection | Contents |
|---|---|---|
| 1 | Generic Desktop / Mouse | 5 buttons + 3 bits padding, then X, Y, Wheel as 8-bit relative. 4-byte input report. |
| 2 | Generic Desktop / Keyboard | 8 modifier bits, 1 reserved byte, 6 key bytes; 5 LED bits as output. 8-byte input report, 1-byte output report. |
| 3 | Vendor page `0xFF00` | 19-byte input report and 19-byte feature report. |

There is no Gamepad (`09 05`) or Joystick (`09 04`) usage anywhere in the image.

That is the root cause of the behaviour this project exists to fix. Report ID 3
is the channel Steam uses, so Steam gets full controller state while every other
host sees only the mouse and keyboard collections.

`tools/check_report_map.py` re-derives this:

```
$ ./tools/check_report_map.py --bin vcf_wired_controller_d0g_5a0e3f348_radio.bin
=== ... @ 0x1eda0, 54 bytes ===
  Report ID 1 Input: 4 bytes payload (5 bytes on the wire with the ID prefix)
=== ... @ 0x1edd6, 67 bytes ===
  Report ID 2 Input: 8 bytes payload (9 bytes on the wire with the ID prefix)
  Report ID 2 Output: 1 bytes payload (2 bytes on the wire with the ID prefix)
```

The script scans for Generic Desktop collection openings, so it finds the mouse
and keyboard collections and not the vendor one, which opens with `06 00 FF`.

Other identifying strings in the application region: `ValveCorporation` at
`0x18B44` and `SteamController` at `0x18D68`.

## 4. The part has 16 kB of RAM

**Verified from Valve's image.**

The application vector table at `0x18000` declares an initial stack pointer of
`0x20003E28`. A Cortex-M0 stack pointer starts at the top of RAM, so RAM ends at
`0x20004000`: 16 kB from `0x20000000`.

S110 8.0.0 reserves the low 8 kB, which leaves `0x20002000`–`0x20004000` for the
application. The stock nRF5 SDK 10 linker script for this SoftDevice uses
`ORIGIN = 0x20002000` and assumes a 32 kB part (`LENGTH = 0x6000`); on this
hardware the length has to be `0x2000`.

## 5. Flash size is probably 128 kB

**Inferred, not verified.**

Valve's application runs to `0x1F014`, just under a 128 kB ceiling at `0x20000`.
Together with the 16 kB RAM above that points at an nRF51822 with 128 kB of
flash. This is suggestive rather than conclusive — image size does not prove a
ceiling.

The project therefore budgets the application `0x18000`–`0x1FFFF`, 32 kB, which
is correct on a 128 kB part and merely conservative on a larger one.

## 6. There is no 32.768 kHz crystal for the radio

**Verified, by disassembly. This one is easy to get wrong and expensive to debug.**

Disassembling the application region and looking for `svc #0x10`
(`SD_SOFTDEVICE_ENABLE`, from `nrf_sdm.h`: `SDM_SVC_BASE` is `0x10`) finds the
SDK's `softdevice_handler_init` wrapper at `0x1EAA0`, which the application
calls once, at `0x1D36C`:

```
0x1d364  ldr      r3, [pc, #0xf4]     ; evt_schedule_func
0x1d366  movs     r2, #0x48           ; ble_evt_buffer_size = 72
0x1d368  ldr      r1, [pc, #0xf4]     ; p_ble_evt_buffer
0x1d36a  movs     r0, #0x11           ; clock_source = 17
0x1d36c  bl       #0x1eaa0            ; softdevice_handler_init
```

Clock source 17 in `enum NRF_CLOCK_LFCLKSRCS` is
`NRF_CLOCK_LFCLKSRC_RC_250_PPM_TEMP_4000MS_CALIBRATION` — the internal RC
oscillator, recalibrated when the temperature moves, every 4 s. Values 1–8 are
the crystal options and Valve uses none of them.

So a custom application **must** pass the same RC clock source. Asking for a
crystal that is not fitted leaves the SoftDevice waiting on `LFCLKSTARTED`
forever, which presents as a radio that never advertises.

The same call site gives the BLE event buffer size Valve uses: 72 bytes.

## 7. The LPC to nRF51 link is USART0 at 921600 baud

**Pin assignment verified by cross-referencing; baud rate computed from Valve's
register writes.**

`ReverseEngineering/Luna_maiboard_V000456-00_rev3.md` has separate tables for
each chip. Reading them together:

| LPC11U37 | nRF51822 | Direction |
|---|---|---|
| `PIO1_18`, USART0 TXD | `P0.15` (pin 21) | LPC to radio; `P0.15` is the radio's RXD |
| `PIO1_17`, USART0 RXD | `P0.14` (pin 20) | radio to LPC; `P0.14` is the radio's TXD |
| `PIO1_24` | `SWDIO/nRESET` (pin 23) | LPC drives radio reset |
| `PIO1_8` | `SWDCLK` (pin 24) | LPC drives radio SWD clock |

The first two rows were originally the other way round here, following a
reading of the pinout document. Valve's own radio firmware settles it: at boot
it configures P0.14 as an output and P0.15 as an input, and samples P0.15 as a
mode strap that the LPC drives from `PIO1_18`. See
[VALVE_LINK.md](VALVE_LINK.md#mode-strap).

The last two rows resolve an open question in that document, which lists
`PIO1_24` and `PIO1_8` only as "Related to USART maybe? TODO". They are the SWD
pair, which is how the LPC can act as an SWD programmer for the radio at all.
Valve's firmware configures both during radio init, at line 75972 of the
decompilation.

The baud rate comes from the USART0 setup at line 76210 of
`ReverseEngineering/vcf_wired_controller_d0g_57bf5c10.c`:

```
UARTCLKDIV = 1          ; UART_PCLK = system clock = 48 MHz
LCR        = 0x83       ; 8 data bits, 1 stop bit, no parity, DLAB set
DLL        = 3          ; DLM untouched, so the divisor is 3
FDR        = 0xB1       ; DIVADDVAL = 1, MULVAL = 11
LCR        = 0x03       ; DLAB cleared
FCR        = 0x87       ; FIFOs enabled and reset, RX trigger level 14
IER        = 0x07       ; RBR, THRE and RX line status interrupts
```

With the LPC11U3x formula
`baud = PCLK / (16 x (256 x DLM + DLL) x (1 + DIVADDVAL / MULVAL))`:

```
48e6 / (16 x 3 x (1 + 1/11)) = 48e6 / (48 x 12/11) = 916,667 baud
```

That is 921600 with the -0.53% error a fractional divider leaves behind, well
inside what both UARTs tolerate. No hardware flow control appears on these two
pins. `PIO1_5` is documented as USART-related and monitored for change via
`PINT2`; its purpose is still unknown.

## 8. Valve's UART protocol is decoded, and it is not stateful

**Verified by disassembly of both ends; not observed on a wire.**

Keeping Valve's LPC firmware and replacing only the radio depends on this.

In summary, the link uses `02`/`03`/`1F`-escaped frames with an opcode byte and
no checksum. The LPC streams controller state unconditionally, and nothing in
the link needs a handshake that cannot be sent statically. The catch is that in
BLE mode the LPC only sends keyboard and mouse reports until the host turns
lizard mode off with a feature report, so a replacement radio has to send that
report itself.

The full opcode tables, the state-record layout and what they mean for the
radio firmware are in [VALVE_LINK.md](VALVE_LINK.md).

## 9. The two chips are flashed by two different mechanisms

**Verified, from the images and the flashing code.**

This is easy to get wrong, and getting it wrong is what bricks a controller.

The browser tool speaks **Valve's bootloader protocol** over USB HID. Its LPC
writes start at offset `0x2000` (`LPC_DATA_OFFSET` in `bootloader.ts`) and its
checksum is taken from `0x2030`: Valve's bootloader occupies `0x0`-`0x1FFF` and
survives the update, and only the image above that is written. Valve's own
images match, being full flash images whose low 8 kB is that bootloader
(`vcf_wired_controller_d0g.bin`: initial SP `0x10001C20`, reset vector
`0x000000D5`).

OpenSteamController's images are **also** full flash images based at `0x0`
(`OpenSteamControllerNinSwitch.bin`: SP `0x10002000`, reset vector `0x00000171`)
but they carry their own vector table there and no Valve bootloader. They are
loaded by the **LPC's ROM ISP**, the FAT12 "CRP DISABLD" volume that appears when
the right trigger is held during USB connect, which writes from `0x0`.

So feeding an OpenSteamController image to the browser tool would discard its
first 8 kB -- vector table and startup code included -- and write the remainder
at `0x2000`, under a Valve bootloader still expecting a Valve application. The
flash would report success.

This firmware never touches the LPC beyond Valve's own BLE image, written through
Valve's bootloader, so none of the ROM ISP risks apply to it.

---

## Reproducing the binary-level findings

```sh
# HID report maps in any radio image
./tools/check_report_map.py --bin path/to/radio.bin

# SoftDevice comparison, RAM size, clock source
# (needs python3 with capstone for the disassembly)
pip install capstone
```

The SoftDevice comparison flattens the SDK hex and diffs it against the blob.
The clock source is found by a linear Thumb sweep over `0x18000`+, skipping the
192-byte vector table, locating `svc #0x10`, then finding the `bl` to that
wrapper and reading the `movs r0, #imm` before it.
