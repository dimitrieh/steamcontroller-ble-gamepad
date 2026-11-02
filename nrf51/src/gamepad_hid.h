/**
 * @file gamepad_hid.h
 * @brief HID report map and report packing for the Steam Controller BLE gamepad.
 */

#ifndef GAMEPAD_HID_H__
#define GAMEPAD_HID_H__

#include <stdint.h>
#include "sc_state.h"

/** Report ID of the gamepad input report. */
#define GAMEPAD_INPUT_REP_ID    1u

/**
 * Length of the gamepad input report, in bytes.
 *
 *   3  buttons 1..19 + 5 bits padding
 *   1  hat switch + 4 bits padding
 *   8  X, Y (joystick), Z, Rz (right trackpad), int16 each
 *   2  Brake, Accelerator (left, right trigger), 0..255
 *
 * S110 8.0.0 has a fixed ATT_MTU of 23, so a notification carries at most 20
 * bytes of value. 14 fits in one packet.
 */
#define GAMEPAD_INPUT_REP_LEN   14u

/** The HID report map (report descriptor). */
extern const uint8_t  gamepad_report_map[];
/** Size of @ref gamepad_report_map in bytes. */
extern const uint16_t gamepad_report_map_size;

/**
 * @brief Pack a controller state into a HID gamepad input report.
 *
 * @param[in]  p_state  Decoded controller state.
 * @param[out] p_report Buffer of at least @ref GAMEPAD_INPUT_REP_LEN bytes.
 */
void gamepad_pack_report(const sc_state_t * p_state, uint8_t * p_report);

#endif /* GAMEPAD_HID_H__ */
