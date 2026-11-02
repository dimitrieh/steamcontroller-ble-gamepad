# steamcontroller-ble-gamepad

Radio firmware that makes the original 2015 Steam Controller show up over
Bluetooth as a **standard gamepad**, so it works on Android TV, phones and
anything else with a normal HID stack. Valve's own Bluetooth firmware shows up
as a mouse and keyboard instead.

Tested on real hardware with macOS and Android TV.

## How it works

The controller has two chips: an LPC11U37 (main) and an nRF51822 (radio).
Only the radio is replaced. The main chip keeps Valve's own BLE firmware,
which still does the trackpads, stick, calibration, haptics and power
management.

```
host <--USB--> LPC11U37 <--UART, Valve's protocol--> nRF51822 --BLE HID gamepad--> TV / phone / PC
               (Valve, unmodified)                   (this firmware)
```

The radio speaks Valve's chip-to-chip protocol
([docs/VALVE_LINK.md](docs/VALVE_LINK.md)), turns lizard mode off, decodes the
controller state and publishes it as one HID gamepad. Background and evidence:
[docs/FINDINGS.md](docs/FINDINGS.md).

## Install

1. Close Steam. Plug the controller in by USB.
2. Flash with the browser tool at [steamcontroller.tools](https://steamcontroller.tools)
   (Chrome or Edge) and pick **BLE Gamepad**. That option is proposed upstream
   and not merged yet; binaries are under [Releases](../../releases).
3. Unplug. Hold **Y** and press **Steam** to start in Bluetooth mode. Pair it
   as "Steam Controller".

**Revert:** flash **BLE** or **Production** with the same tool. The bootloader
is never touched, so the tool can always reach the controller.

After every reflash, forget "Steam Controller" on every paired device first.

Step-by-step test guide: [docs/TESTING.md](docs/TESTING.md), or the
interactive version at the project's GitHub Pages site (`test.html`).

## Mapping

| Controller | HID | Android |
|---|---|---|
| A, B, X, Y | Buttons 1, 2, 4, 5 | `BUTTON_A`, `_B`, `_X`, `_Y` |
| Bumpers | Buttons 7, 8 | `BUTTON_L1`, `_R1` |
| Triggers, full pull | Buttons 9, 10 | `BUTTON_L2`, `_R2` |
| Triggers, analog | Brake, Accelerator | `AXIS_BRAKE`, `AXIS_GAS` |
| Back, Start, Steam | Buttons 11, 12, 13 | `BUTTON_SELECT`, `_START`, `_MODE` |
| Joystick, click | X, Y; button 14 | `AXIS_X`, `_Y`; `BUTTON_THUMBL` |
| Right trackpad, click | Z, Rz; button 15 | `AXIS_Z`, `_RZ` (right stick); `BUTTON_THUMBR` |
| Left trackpad arrows, clicked | Hat switch | D-pad, including TV menus |
| Left trackpad, clicked elsewhere | Button 19 | `BTN_TRIGGER_HAPPY3` |
| Grips | Buttons 17, 18 | `BTN_TRIGGER_HAPPY1`, `2` |

Buttons 3, 6 and 16 are left empty on purpose so Linux/Android evdev order
lines up. The right trackpad is rotated 15° like Steam does.

**Not supported:** gyro, rumble from games, mouse mode, and Steam's own
Bluetooth support for this controller. Battery level reads a fixed 100%.

The device uses the Bluetooth SIG test vendor ID `0xFFFF`. Valve's ID would
make SDL and Steam try Valve's protocol on it.

## Build

Needs `arm-none-eabi-gcc` and the official **nRF51 SDK 10.0.0** (the last one
for S110 8.0.0, the SoftDevice already on the controller).

```sh
curl -LO https://files.nordicsemi.com/artifactory/nRF5-SDK/external/nRF51_SDK_v10.x.x/nRF51_SDK_10.0.0_dc26b5e.zip
echo "2c1a0ba191f5fde020487e1e86c4dc2df28daed7986e916e40b2583a2fd16397  nRF51_SDK_10.0.0_dc26b5e.zip" | sha256sum -c
unzip -q nRF51_SDK_10.0.0_dc26b5e.zip -d nRF5_SDK_10.0.0

NRF_SDK=$PWD/nRF5_SDK_10.0.0 tools/build.sh
```

`tools/build.sh` builds, runs every check and writes `dist/`:
`sc_ble_gamepad.bin` (written at `0x18000`), `build.json` and `SHA256SUMS`.
The version is the last commit's timestamp, so the same commit always gives
the same binary.

## Possible later work

On request, not planned:

- Register a real USB/BLE product ID at [pid.codes](https://pid.codes)
  (vendor `0x1209`) instead of the test ID.
- With that ID, add a mapping to
  [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) so
  desktop games get the right layout.

## Credits

- [OpenSteamController](https://github.com/greggersaurus/OpenSteamController)
  by greggersaurus: hardware reverse engineering this builds on.
- [steam_controller_tools](https://github.com/mitchmikusek/steam_controller_tools)
  by mitchmikusek: the browser flash tool.
- [SteamControllerTool](https://github.com/lilphil/SteamControllerTool) by
  lilphil: the original flashing protocol.
- [SDL](https://github.com/libsdl-org/SDL): its Steam Controller parser was the
  reference for the state decoder tests.

Not affiliated with Valve. Steam is a trademark of Valve Corporation.

## License

MIT, see [LICENSE](LICENSE). Files in `nrf51/config/` are adapted from Nordic's
SDK and keep Nordic's license header. The build links against the nRF51 SDK
and runs on Nordic's S110 SoftDevice, which are under Nordic's license.
