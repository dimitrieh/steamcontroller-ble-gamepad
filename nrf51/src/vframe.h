/**
 * @file vframe.h
 * @brief Encoder and decoder for Valve's LPC <-> radio UART framing.
 *
 * Pure logic, no hardware: the UART driver feeds bytes in and takes encoded
 * frames out, and the host-side tests drive the same functions directly.
 *
 * A frame is START, the escaped body, END. There is no length and no checksum,
 * so the decoder is strict about what it does know: a frame is dropped if the
 * UART flagged an error while it was arriving, if it overflows the buffer, or
 * if it ends in the middle of an escape.
 */

#ifndef VFRAME_H__
#define VFRAME_H__

#include <stdint.h>
#include <stdbool.h>

/** Largest frame body we keep. Bigger frames (the LPC's 128-byte bond push at
 *  boot) are counted and dropped; nothing this firmware needs is that long. */
#define VFRAME_MAX_BODY     64u

/** Worst-case encoded size of a body of @p n bytes: every byte escaped. */
#define VFRAME_ENCODED_MAX(n)   (2u + 2u * (n))

typedef enum
{
    VFRAME_RX_NONE = 0,     /**< Byte consumed, no frame finished.          */
    VFRAME_RX_FRAME,        /**< A frame finished; body is in the decoder.  */
    VFRAME_RX_DROPPED,      /**< A frame finished but was damaged: discarded. */
    VFRAME_RX_OVERSIZE,     /**< A frame finished intact but too long to keep. */
} vframe_rx_result_t;

typedef struct
{
    uint8_t body[VFRAME_MAX_BODY];
    uint8_t len;
    bool    in_frame;
    bool    escaped;
    bool    corrupt;     /**< Set by a UART error or bad escape mid-frame.  */
    bool    oversize;    /**< More than VFRAME_MAX_BODY bytes arrived.       */
} vframe_rx_t;

/** Reset the decoder to "waiting for START". */
void vframe_rx_init(vframe_rx_t * p_rx);

/**
 * @brief Feed one received byte.
 *
 * A START byte always begins a new frame, even in the middle of one. The LPC
 * escapes every START inside a body, so a bare START mid-frame means the END
 * of the previous frame was lost, and resynchronising on it beats gluing two
 * frames together.
 */
vframe_rx_result_t vframe_rx_byte(vframe_rx_t * p_rx, uint8_t byte);

/** Mark the frame currently arriving as corrupt, e.g. after a UART overrun. */
void vframe_rx_error(vframe_rx_t * p_rx);

/**
 * @brief Encode one frame.
 *
 * @param[in]  p_body  Body, opcode first.
 * @param[in]  len     Body length.
 * @param[out] p_out   Output buffer.
 * @param[in]  out_max Size of @p p_out.
 *
 * @return Encoded length, or 0 if it would not fit.
 */
uint16_t vframe_encode(const uint8_t * p_body, uint8_t len, uint8_t * p_out, uint16_t out_max);

#endif /* VFRAME_H__ */
