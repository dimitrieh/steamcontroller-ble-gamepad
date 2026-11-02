# Testing on a controller

An interactive version of these steps, with automatic checks, is
`tools/web/test.html`, published on this repo's GitHub Pages site.

Stop at the first step that fails and open an issue with what you saw. Each
step checks one more link in the chain, so the first failure narrows down the
cause.

You need: the controller with fresh AA batteries, a USB cable, Chrome or Edge
on a computer, and the device you want to use it with. **Close Steam** first:
it grabs the controller over USB.

**Revert** at any time: flash **BLE** or **Production** with the flash tool.

## 1. Flash

Plug in by USB, open [steamcontroller.tools](https://steamcontroller.tools),
choose **BLE Gamepad** and start. Chrome asks for device permission several
times as the controller switches modes; pick the Steam Controller each time.

**Expect:** the flash completes. If it fails, flash **BLE** to restore the
controller and include the tool's log in the issue.

## 2. Radio version over USB

Close the flash tool tab. On the test page, click **Read versions over USB**.

**Expect:** Firmware `0x5b0f21bd` (Valve's BLE firmware) and Radio equal to
the release's `build.json`. That proves the radio boots and the link between
the chips works both ways.

| You see | Meaning |
|---|---|
| Radio matches | Pass |
| Radio `N/A` | Read again. Still N/A: the radio is not answering |
| Another radio value | The flash did not take |

## 3. Bluetooth mode

Unplug USB. Turn the controller off (hold Steam), then on in Bluetooth mode:
**hold Y, press Steam**. The mode is remembered afterwards.

**Expect:** "Steam Controller" appears when a phone or computer scans. Don't
pair from the OS yet.

## 4. Radio diagnostics (Web Bluetooth)

On the test page, click **Connect to controller**.

**Expect:** `BLE mode`, state records climbing, 0 bad records, no fault. Every
button, the stick, both pads and both triggers show in the live view, and
**Buzz** makes the pads buzz. Click **Copy report** if anything is off.

| You see | Meaning |
|---|---|
| `not BLE mode` | Dongle mode or on USB. Power off, then on holding Y |
| Bytes from LPC = 0 | The main chip sends nothing: still on USB? |
| Bytes climbing, frames 0 | Framing or baud problem |
| Lizard payloads, no state records | Lizard-off command ignored. Click **Resync** |
| Last fault shows file:line | The radio reset on an error |

Click **Disconnect** when done.

## 5. Computer as host

Pair from the OS Bluetooth settings and press a button.

**Expect:** in the test page's Gamepad section, each physical button lights
exactly one entry, the stick and right pad each move a pair of axes, and the
triggers are analog. Remove the pairing before moving to another device.

## 6. Android TV

Settings › Remotes & Accessories › Pair accessory › "Steam Controller".

**Expect:** left-pad arrows (clicked) move through menus, A selects, B goes
back. In a game or tester app: stick, right pad as right stick, analog
triggers, bumpers, Start, Back, Steam.

## 7. Reconnect

Turn the controller off and on, then the TV off and on.

**Expect:** it reconnects by itself each time, with no pairing menu.

## After every reflash

Flashing wipes stored pairings. Forget "Steam Controller" on every device
before pairing again, or it will fail to reconnect without saying why.
