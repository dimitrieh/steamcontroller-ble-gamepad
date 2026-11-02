# Valve's LPC-to-radio link

What Valve's own firmware says over the USART between the LPC11U37 and the
nRF51822, derived by disassembling both ends. This is the protocol a radio-only
firmware has to speak so that Valve's LPC firmware can stay on the
controller untouched.

**Nothing here was observed on a wire.** It is all static analysis, labelled the
same way as [FINDINGS.md](FINDINGS.md). The cross-checks that make it
trustworthy are that both ends agree with each other, and that the one format
Valve's radio forwards to the host unchanged matches the parser SDL already ships
for it.

Images analysed, both from the flash tool's BLE firmware set:

| Chip | Image | Analysed range |
|---|---|---|
| LPC11U37 | `ble/vcf_wired_controller_d0g_5b0f21bd.bin` | application at `0x2000`+ |
| nRF51822 | `ble/vcf_wired_controller_d0g_5a0e3f348_radio.bin` | `0x16A00`–`0x1F013` |

The LPC image is **not** the one OpenSteamController's decompilation covers.
That trace is of `vcf_wired_controller_d0g_57bf5c10`, the pre-BLE firmware, and
it is a pinkySim execution trace, so it only holds code the simulation reached.
The addresses below are in `5b0f21bd`.

---

## Verdict

**Replacing only the radio is viable.** The link is not handshaked, sequence-checked, or otherwise
stateful in a way that has to be imitated:

- The LPC streams controller state on every main-loop tick whenever USB is not
  configured (`0x7200`). It does not wait for, count, or acknowledge anything
  from the radio before sending.
- Everything the LPC does expect from the radio can be sent statically: a
  one-byte "connected" / "disconnected" status, and replies to two queries.
- The one real obstacle is **lizard mode**, covered in the next section. It is
  cleared by sending the LPC the same feature command Steam sends. That makes it
  a fixed frame, not a protocol.

## The lizard-mode catch

In BLE mode the LPC does **not** send raw controller state by default. It picks
the `0x51` payload at `0xA114` by testing setting 8
(`SETTING_RIGHT_TRACKPAD_MODE`):

| Setting 8 | `0x51` payload | What Valve's radio does with it |
|---|---|---|
| anything but 7 (default: lizard mode) | `01` + 8-byte keyboard report + 4-byte mouse report | Sends them as HID report IDs 2 and 1 |
| 7 (`TRACKPAD_NONE`) | a BLE state record, below | Splits it into 19-byte segments on report ID 3 |

So a stock controller that nobody has configured sends the radio a keyboard and
a mouse, and no stick, trigger or button state. That is why Valve's BLE
firmware looks like a mouse and keyboard to everything except Steam. Steam
turns lizard mode off with a feature report, and the radio relays it to the LPC
as a `0x50` frame.

A replacement radio therefore has to send that feature report itself, on every
connection:

```
0x50  0x81 0x00                                    ID_CLEAR_DIGITAL_MAPPINGS
0x50  0x87 0x0C  31 02 00  07 07 00  08 07 00  18 00 00
      ID_SET_SETTINGS_VALUES, 12 bytes:
        0x31 WIRELESS_PACKET_VERSION = 2
        0x07 LEFT_TRACKPAD_MODE      = 7 (NONE)
        0x08 RIGHT_TRACKPAD_MODE     = 7 (NONE)   <- the bit 0xA114 tests
        0x18 SMOOTH_ABSOLUTE_MOUSE   = 0
```

These are the settings SDL writes in `SDL_hidapi_steam.c`, and the values were
checked against SDL's `controller_constants.h`. The feature-report layout
inside a `0x50` frame is `[command][length][data...]`, the USB layout minus
the report ID. Valve's radio already builds one this way:
`50 9F 04 6F 66 66 21` at `0x18710` is `ID_TURN_OFF_CONTROLLER` with its
`"off!"` payload. On the LPC side, `0x50` frames go to the same command switch
at `0x3430` that serves USB feature reports.

**Unverified:** whether the LPC ever reverts these settings on its own, for
example through `SETTING_STEAM_WATCHDOG_ENABLE`. Re-sending the frames
periodically, not only on connect, costs almost nothing and covers it.

---

## Physical layer

| | Value | Evidence |
|---|---|---|
| Baud | 921600 nominal. The LPC's divisors give 916,667; the radio uses `Baud921600` (`0x0EBED000`) | LPC `0xB9B8`; radio config struct at `0x1EF84` |
| Format | 8N1, no flow control | LPC `LCR = 0x03` |
| Radio RX | **P0.15** | radio boot code sets `nrf_gpio_cfg(15, input)` and samples it for the mode strap |
| Radio TX | **P0.14** | radio boot code sets `nrf_gpio_cfg(14, output)` |

This corrects finding 7, which has the two radio pins the other way round.

### Flow-control line: P0.11

**Missed in the first analysis, and found on hardware.** A radio that sets up
only the UART hears almost nothing: the LPC sent 8 bytes after boot and then
fell silent.

The LPC does not stream freely. Its send routine (`0xBA54`) writes at most 4
bytes into the UART FIFO and puts the rest in a 256-byte ring. Its THRE
interrupt is never enabled (`IER = 5` at `0xB9F8`), so the ring is drained only
by a pin interrupt: `0xD370` arms one on a pulled-up pin (port/pin from the
config at `0x100012F0`, most likely `PIO1_5`), and its callback `0xBA10` loads
the next 4 bytes if the FIFO has emptied. That pin is the radio's P0.11.

Valve's radio drives P0.11 as an output idling high (GPIOTE config
`00 01 00 00` at `0x17E30`) and pulls it low:

- for 20 us right after its UART init (`0x16BE8`), and
- in its UART receive handler, for every byte received (`0x17894`).

So the radio pulls the LPC's data, 4 bytes per edge. A radio that never pulses
gets the first 4 bytes of the LPC's boot traffic and nothing else. On the
LPC side, `0xD41C` also drives the same pin low briefly and then releases it,
before its version query (`0xD82C`), apparently to wake the radio.

The LPC's radio bring-up (`0xD560`): drive the mode strap, pulse the radio's
reset (`0xD030`), wait 5 ms, start the UART (`0xD370`), send the `0x5D` bond
push in BLE mode (`0xD79C`), wait 50 ms, send the `0x56` version query
(`0xD80C`). All of it waits in the ring until the radio pulls.

## Framing

Identical in both directions. The LPC encoder is at `0xBAD4` and its receive
state machine at `0xB874`.

| Byte | Meaning |
|---|---|
| `0x02` | start of frame |
| `0x03` | end of frame |
| `0x1F` | escape: the next byte on the wire is the data byte XOR `0x20` |

Every `0x02`, `0x03` and `0x1F` inside a frame is escaped. There is **no length
field and no checksum.** The first byte of a frame is an opcode in `0x50..0x5F`.
The LPC's receive buffer is 256 bytes with a `uint8_t` index, so frames have to
stay well under that.

The LPC stops accepting bytes (state 3) after a frame until its PendSV handler
has taken it (`0x95D4`, `0x7DB0`). Frames that arrive back-to-back faster than
that are dropped, not corrupted.

## LPC to radio

| Op | Payload | When | LPC sender |
|---|---|---|---|
| `0x51` | controller state, below | every main-loop tick while USB is not configured | `0xA114` |
| `0x55` | `02` + Valve status/report message | e.g. the ~1 s status message once `WIRELESS_PACKET_VERSION` ≥ 2 and connected; replies destined for the host | `0xA1B4` |
| `0x50` | Valve feature-report reply | after handling a `0x50` from the radio, if the command has a reply | `0xD844` |
| `0x52` | none, or 16 bytes | start advertising / reconnect (power-management task) | `0xD494`, `0xD4A4`, `0xD4D8` |
| `0x53` | none | stop | `0xD50C` |
| `0x54` | 1 byte | radio enable / advertising variant | `0xD5E8` |
| `0x56` | none | query radio firmware version; repeated until answered | `0xD80C` |
| `0x57` | `"RESET"` | reset the radio (`NVIC_SystemReset`) | `0xD780` |
| `0x58` | none | query radio device ID; one-shot callback | `0xD52C` |
| `0x59` | `"doze"` | put the radio to sleep | `0xD7F0` |
| `0x5A` | `"wipe"` | wipe pairing | `0xD51C` |
| `0x5B` | 24 bytes | dongle pairing info (mode 2 only) | `0xD878` |
| `0x5D` | 128 bytes | push stored BLE bond data at boot (mode 3 only) | `0xD79C` |

The meanings of `0x52`/`0x54` variants are inferred from where they are called
and how the radio handles them. They are not pinned down.

### `0x51` controller state in BLE mode

With lizard mode off, the payload after the opcode is the record the LPC builds
at `0x8C10`. Valve's radio does not parse it: it checks `(p[0] & 0x0F) == 4` and
forwards it in segments. SDL parses the same record
(`UpdateBLESteamControllerState`), which gives the layout:

```
byte 0     low nibble  = 4 (k_EBLEReportState)
           high nibble = chunk flags 0x10, 0x20, 0x40, 0x80
byte 1     chunk flags 0x100 .. 0x1000 (shifted down by 8)
then, in this order, only the chunks whose flag is set:
  0x0010  buttons, 3 bytes (Valve's USB button bit order)
  0x0020  triggers, 2 bytes: left, right (8-bit)
  0x0040  more buttons, 3 bytes
  0x0080  left joystick, 2 x int16
  0x0100  left trackpad, 2 x int16
  0x0200  right trackpad, 2 x int16
  0x0400  accelerometer, 3 x int16
  0x0800  gyro, 3 x int16
  0x1000  orientation quaternion, 4 x int16
```

**Chunks are deltas.** `0x8C10` compares against the previous values and only
includes what changed, so a decoder has to keep state between frames the way
SDL does, starting from zero. Whether the LPC ever sends a full keyframe is not
known. If it never does, a dropped frame leaves a value stale until it next
changes.

With lizard mode on, the payload is `01` + 8-byte keyboard report + 4-byte mouse
report instead. A decoder that sees that should re-send the lizard-off frames.

In mode 2 (dongle) the payload is instead 16 bytes from offsets 8–23 of Valve's
USB input report, plus the keyboard and mouse reports. Valve's radio
delta-compresses that for its 2.4 GHz packets. It is not relevant to a BLE
firmware, but it is why the two modes must not be confused.

## Radio to LPC

| Op | Payload | Meaning | LPC handler |
|---|---|---|---|
| `0x52` | none | **BLE host connected** (mode 3). Mode 2 appends a 5-byte dongle address | `0xD654` |
| `0x53` | none | **host disconnected** | `0xD672` |
| `0x50` | `[cmd][len][data]` | Valve feature report from the host, run through the LPC's command switch | `0xD640` → `0x3430` |
| `0x56` | 4 bytes | reply to the version query (Valve sends the word at `0x18018`) | `0xD682` |
| `0x58` | 8 bytes | reply to the device-ID query (Valve sends FICR `DEVICEID`) | `0xD632` |
| `0x5D` | ≥ 9 bytes | read from the LPC-side bond store; the LPC replies with `0x5D` | `0xD698` |
| `0x5E` | ≥ 9 bytes | write to the LPC-side bond store (deferred EEPROM write) | `0xD708` |
| `0x5F` | u16 LE | BLE connection interval in 1.25 ms units. The LPC stores it × 1250 µs and reprograms its CT32B0 report timer from it | `0xD740` → `0xB3BC` |

The LPC keeps a "radio connected" flag, set by `0x52` and cleared by `0x53`. Its
power-management task (`0x72D8`) uses it to decide when an idle, unconnected
controller dozes and then turns off. Without `0x52` the controller behaves as if
it were never paired and powers itself down on the usual timeout. The state
stream itself does not depend on the flag.

## Keyframes and dropped frames

Two more details, found while implementing, decide how robust the radio can be.

**The LPC drops frames that arrive while it is busy.** When a frame
completes, the receive state machine hands it to the PendSV handler. If
another frame completes before the handler has finished with the first
(`0x95D4` returns 0), the machine enters state 3 and ignores every byte until
the handler catches up. So a sender must space its frames out: back-to-back
feature commands lose all but the first two.

**Feature command `0x8E` (`ID_LOAD_DEFAULT_SETTINGS`) forces a keyframe.**
Its handler (`0x3640`) loads the default settings and then clears the LPC's
copy of the last state record it sent (`memset(0x100013AC, 0, 0x28)`). Every
chunk is compared against that copy, so the next record carries every chunk
that is not zero. A decoder that zeroes its own state at the same moment ends
up with the full state. That is the only recovery from a lost delta the
protocol offers, and it is the first command SDL sends as well. It also puts
lizard mode back on, so it always has to be followed by the lizard-off
settings.

The radio can also send a `0x56` version frame without being asked: the LPC
stores any 5-byte one.

## What this means for the radio firmware

What the radio has to do, in order of necessity:

1. Use RX = P0.15 and TX = P0.14 at 921600 8N1, and parse the `02`/`03`/`1F`
   framing. Drive P0.11 high, pulse it low once after UART init, and pulse it
   low after every byte received: that is what makes the LPC send.
2. Send `0x52` on BLE connect and `0x53` on disconnect.
3. After each connect, and periodically after that, send the two lizard-off
   `0x50` frames above.
4. Decode `0x51` type-4 records into persistent state and map that state to the
   HID gamepad report.
5. Reply to `0x56` and `0x58` with a version word and 8 bytes of device ID.
6. Optionally, send `0x5F` with the negotiated connection interval, so the LPC
   paces its reports to the link instead of overrunning it. That gives the
   radio a lever on review item N2 that the review assumed did not exist. The
   exact timer formula in `0xB3BC` is not fully decoded.

Two pieces of later work also get easier:

- **Rumble (A4)** does not need a new reverse-engineered format. It is
  `0x50` + `ID_TRIGGER_HAPTIC_PULSE` (`0x8F`), a documented feature command
  that the LPC's switch handles.
- **Bonding (A3)** has an alternative to pstorage on the radio. Valve keeps BLE
  bond data in the LPC's EEPROM, pushes it to the radio at boot (`0x5D`, 128
  bytes), and takes updates back (`0x5D`/`0x5E`). Reusing that store would free
  the radio's flash budget, but its record layout is not decoded.

Still unknown, and worth confirming first on hardware with a logic analyser on
P0.14/P0.15:

- The `0x55` status message layout, which is where battery level most likely
  arrives.
- Whether the state record is ever a full keyframe.
- Whether Valve's LPC reverts the lizard-mode settings on its own.
