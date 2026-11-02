/**
 * @file sc_state.h
 * @brief Controller state, rebuilt from the LPC's delta-coded state records.
 *
 * The LPC includes a chunk in a record only when that chunk changed since the
 * last record it sent, so the decoder has to keep state between records.
 * Chunks that are absent keep their previous value. See docs/VALVE_LINK.md.
 */

#ifndef SC_STATE_H__
#define SC_STATE_H__

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    uint32_t buttons;         /**< VALVE_BTN_* bits 0..23.                  */
    uint8_t  buttons_ext[3];  /**< Button bits 40..63; unused by the SC.   */
    uint8_t  ltrig;           /**< 0..255.                                  */
    uint8_t  rtrig;
    int16_t  lstick_x;        /**< Y grows upwards, as the controller sends */
    int16_t  lstick_y;
    int16_t  lpad_x;
    int16_t  lpad_y;
    int16_t  rpad_x;
    int16_t  rpad_y;
    int16_t  accel[3];
    int16_t  gyro[3];
    int16_t  quat[4];
} sc_state_t;

typedef enum
{
    SC_REC_OK = 0,        /**< Applied.                                        */
    SC_REC_LIZARD,        /**< Keyboard+mouse payload: lizard mode is still on. */
    SC_REC_TRUNCATED,     /**< Shorter than its chunk flags say. Not applied.   */
    SC_REC_UNKNOWN,       /**< Unrecognised type or chunk flag. Not applied.    */
} sc_rec_result_t;

/** Zero the state, as after a keyframe request. */
void sc_state_reset(sc_state_t * p_state);

/**
 * @brief Apply one state record.
 *
 * The record is validated in full before anything is written, so a record
 * that fails leaves @p p_state untouched.
 *
 * @param[in,out] p_state  State to update.
 * @param[in]     p_rec    Record, i.e. the frame body after the opcode byte.
 * @param[in]     len      Record length.
 * @param[out]    p_flags  If not NULL, receives the record's chunk flags.
 * @param[out]    p_extra  If not NULL, receives the number of bytes beyond the
 *                         last chunk (expected to be 0; reported for testing).
 */
sc_rec_result_t sc_state_apply(sc_state_t *    p_state,
                               const uint8_t * p_rec,
                               uint8_t         len,
                               uint16_t *      p_flags,
                               uint8_t *       p_extra);

#endif /* SC_STATE_H__ */
