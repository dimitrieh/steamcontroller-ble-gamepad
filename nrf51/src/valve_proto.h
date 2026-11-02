/**
 * @file valve_proto.h
 * @brief Constants for Valve's LPC11U37 <-> nRF51822 UART protocol.
 *
 * Decoded from Valve's own firmware images; see docs/VALVE_LINK.md for the
 * evidence behind every value here. The radio talks to Valve's unmodified
 * LPC firmware (vcf_wired_controller_d0g_5b0f21bd), so none of this is ours to
 * change -- it has to match what that firmware sends and expects.
 */

#ifndef VALVE_PROTO_H__
#define VALVE_PROTO_H__

#include <stdint.h>

/* ---- Physical layer ------------------------------------------------------ */

/** nRF51 pin wired to the LPC's USART0 TXD. Also the boot-time mode strap. */
#define VALVE_PIN_RX            15u
/** nRF51 pin wired to the LPC's USART0 RXD. */
#define VALVE_PIN_TX            14u
/**
 * nRF51 output the LPC uses as transmit flow control. Idles high. The LPC
 * puts at most 4 bytes in its UART FIFO and keeps the rest in a ring buffer;
 * only a falling edge here (pin interrupt -> 0xBA10 in 5b0f21bd) makes it
 * load the next 4, and only once its FIFO has emptied. Its THRE interrupt is
 * not enabled (IER = 5 at 0xB9F8), so nothing else drains the ring.
 *
 * Valve's radio pulses it low for 20 us once its UART is up (0x16BE8), and
 * again in its UART receive handler for every byte it receives (0x17894).
 */
#define VALVE_PIN_WAKE          11u
/** Length of the boot-time wake pulse, as Valve's radio times it. */
#define VALVE_WAKE_BOOT_PULSE_US 20u

/* ---- Framing ------------------------------------------------------------- */

#define VALVE_FRAME_START       0x02u
#define VALVE_FRAME_END         0x03u
/** Escape: the next byte on the wire is the data byte XOR VALVE_FRAME_XOR. */
#define VALVE_FRAME_ESC         0x1Fu
#define VALVE_FRAME_XOR         0x20u

/* ---- Opcodes: first byte of every frame ---------------------------------- */

/** Both directions: Valve feature report (host command to the LPC, or its reply). */
#define VALVE_OP_FEATURE        0x50u
/** LPC -> radio: controller state. */
#define VALVE_OP_STATE          0x51u
/** LPC -> radio: start advertising. Radio -> LPC: a host connected. */
#define VALVE_OP_CONNECTED      0x52u
/** LPC -> radio: stop. Radio -> LPC: the host disconnected. */
#define VALVE_OP_DISCONNECTED   0x53u
/** LPC -> radio: radio enable / advertising variant, one argument byte. */
#define VALVE_OP_ENABLE         0x54u
/** LPC -> radio: a report or status message bound for the host. */
#define VALVE_OP_HOST_MSG       0x55u
/** LPC -> radio: query radio firmware version. Radio replies with 4 bytes. */
#define VALVE_OP_VERSION        0x56u
/** LPC -> radio: "RESET". */
#define VALVE_OP_RESET          0x57u
/** LPC -> radio: query device ID. Radio replies with 8 bytes. */
#define VALVE_OP_DEVICE_ID      0x58u
/** LPC -> radio: "doze". */
#define VALVE_OP_DOZE           0x59u
/** LPC -> radio: "wipe" pairing. Radio acknowledges with [0x5A, 1]. */
#define VALVE_OP_WIPE           0x5Au
/** LPC -> radio: dongle pairing info (mode 2 only). */
#define VALVE_OP_DONGLE_PAIR    0x5Bu
/** Radio -> LPC read, or LPC -> radio push, of the LPC-side bond store. */
#define VALVE_OP_BOND_READ      0x5Du
/** Radio -> LPC: write to the LPC-side bond store. */
#define VALVE_OP_BOND_WRITE     0x5Eu
/** Radio -> LPC: connection interval in 1.25 ms units, u16 LE. */
#define VALVE_OP_CONN_INTERVAL  0x5Fu

/* ---- State record (payload of VALVE_OP_STATE in BLE mode) ---------------- */

/** Low nibble of byte 0 for a full-state record (SDL's k_EBLEReportState). */
#define VALVE_REC_TYPE_STATE    0x04u
/** Byte 0 of the lizard-mode payload: 01 + 8-byte keyboard + 4-byte mouse. */
#define VALVE_REC_TYPE_LIZARD   0x01u

/** Chunk flags: byte 0 high nibble, then byte 1 shifted up by 8. */
#define VALVE_CHUNK_BUTTONS     0x0010u   /**< 3 bytes: button bits 0..23   */
#define VALVE_CHUNK_TRIGGERS    0x0020u   /**< 2 bytes: left, right         */
#define VALVE_CHUNK_BUTTONS_EXT 0x0040u   /**< 3 bytes: button bits 40..63  */
#define VALVE_CHUNK_LSTICK      0x0080u   /**< 2 x int16                    */
#define VALVE_CHUNK_LPAD        0x0100u   /**< 2 x int16                    */
#define VALVE_CHUNK_RPAD        0x0200u   /**< 2 x int16                    */
#define VALVE_CHUNK_ACCEL       0x0400u   /**< 3 x int16                    */
#define VALVE_CHUNK_GYRO        0x0800u   /**< 3 x int16                    */
#define VALVE_CHUNK_QUAT        0x1000u   /**< 4 x int16                    */

/* ---- Button bits, as SDL's STEAM_*_MASK --------------------------------- */

#define VALVE_BTN_RT_FULL       0x00000001u
#define VALVE_BTN_LT_FULL       0x00000002u
#define VALVE_BTN_RB            0x00000004u
#define VALVE_BTN_LB            0x00000008u
#define VALVE_BTN_Y             0x00000010u
#define VALVE_BTN_B             0x00000020u
#define VALVE_BTN_X             0x00000040u
#define VALVE_BTN_A             0x00000080u
#define VALVE_BTN_DPAD_UP       0x00000100u
#define VALVE_BTN_DPAD_RIGHT    0x00000200u
#define VALVE_BTN_DPAD_LEFT     0x00000400u
#define VALVE_BTN_DPAD_DOWN     0x00000800u
#define VALVE_BTN_BACK          0x00001000u   /**< SDL: MENU, the left arrow   */
#define VALVE_BTN_STEAM         0x00002000u
#define VALVE_BTN_START         0x00004000u   /**< SDL: ESCAPE, the right arrow */
#define VALVE_BTN_LGRIP         0x00008000u
#define VALVE_BTN_RGRIP         0x00010000u
#define VALVE_BTN_LPAD_CLICK    0x00020000u
#define VALVE_BTN_RPAD_CLICK    0x00040000u
#define VALVE_BTN_LPAD_TOUCH    0x00080000u
#define VALVE_BTN_RPAD_TOUCH    0x00100000u
#define VALVE_BTN_STICK_CLICK   0x00400000u

#define VALVE_BTN_DPAD_ANY      (VALVE_BTN_DPAD_UP | VALVE_BTN_DPAD_RIGHT | \
                                 VALVE_BTN_DPAD_LEFT | VALVE_BTN_DPAD_DOWN)

/* ---- Feature commands (inside a VALVE_OP_FEATURE frame) ------------------ */
/* Layout is [command][length][data...], the USB feature report minus its ID. */

#define VALVE_CMD_CLEAR_DIGITAL_MAPPINGS  0x81u
#define VALVE_CMD_SET_SETTINGS_VALUES     0x87u
/** Loads default settings and also clears the LPC's copy of the last state it
 *  sent, so the next record carries every non-zero chunk: a keyframe. */
#define VALVE_CMD_LOAD_DEFAULT_SETTINGS   0x8Eu
#define VALVE_CMD_TRIGGER_HAPTIC_PULSE    0x8Fu

#define VALVE_SETTING_LEFT_TRACKPAD_MODE      0x07u
#define VALVE_SETTING_RIGHT_TRACKPAD_MODE     0x08u
#define VALVE_SETTING_SMOOTH_ABSOLUTE_MOUSE   0x18u
#define VALVE_SETTING_WIRELESS_PACKET_VERSION 0x31u
#define VALVE_TRACKPAD_NONE                   0x07u

/** Haptic pulse side. Left and right are swapped on this report. */
#define VALVE_HAPTIC_SIDE_BOTH  0x02u

#endif /* VALVE_PROTO_H__ */
