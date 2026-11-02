/**
 * @file vframe.c
 * @brief Encoder and decoder for Valve's LPC <-> radio UART framing.
 */

#include "vframe.h"
#include "valve_proto.h"

void vframe_rx_init(vframe_rx_t * p_rx)
{
    p_rx->len      = 0u;
    p_rx->in_frame = false;
    p_rx->escaped  = false;
    p_rx->corrupt  = false;
    p_rx->oversize = false;
}

void vframe_rx_error(vframe_rx_t * p_rx)
{
    if (p_rx->in_frame)
    {
        p_rx->corrupt = true;
    }
}

vframe_rx_result_t vframe_rx_byte(vframe_rx_t * p_rx, uint8_t byte)
{
    if (byte == VALVE_FRAME_START)
    {
        /* Starting over mid-frame throws the partial frame away. Report that
         * as a drop so the caller's error counters see lost ENDs too. */
        bool was_in_frame = p_rx->in_frame;

        p_rx->len      = 0u;
        p_rx->in_frame = true;
        p_rx->escaped  = false;
        p_rx->corrupt  = false;
        p_rx->oversize = false;
        return was_in_frame ? VFRAME_RX_DROPPED : VFRAME_RX_NONE;
    }

    if (!p_rx->in_frame)
    {
        /* Line noise, or the tail of a frame we joined late. */
        return VFRAME_RX_NONE;
    }

    if (byte == VALVE_FRAME_END)
    {
        bool damaged = p_rx->corrupt || p_rx->escaped || (p_rx->len == 0u);

        p_rx->in_frame = false;
        p_rx->escaped  = false;
        if (damaged)
        {
            return VFRAME_RX_DROPPED;
        }
        /* Intact, just longer than anything this firmware needs to read. Told
         * apart from damage because damage may have cost a state delta and an
         * oversize frame cannot have: no state record comes near the limit. */
        return p_rx->oversize ? VFRAME_RX_OVERSIZE : VFRAME_RX_FRAME;
    }

    if (byte == VALVE_FRAME_ESC)
    {
        if (p_rx->escaped)
        {
            /* Two escapes in a row: the encoder never produces that. */
            p_rx->corrupt = true;
        }
        p_rx->escaped = true;
        return VFRAME_RX_NONE;
    }

    if (p_rx->escaped)
    {
        byte ^= VALVE_FRAME_XOR;
        p_rx->escaped = false;
    }

    if (p_rx->len < VFRAME_MAX_BODY)
    {
        p_rx->body[p_rx->len++] = byte;
    }
    else
    {
        p_rx->oversize = true;
    }

    return VFRAME_RX_NONE;
}

/** True for the bytes the LPC's encoder escapes (0xBAD4 in 5b0f21bd). */
static bool needs_escape(uint8_t byte)
{
    return (byte == VALVE_FRAME_START) || (byte == VALVE_FRAME_END) ||
           (byte == VALVE_FRAME_ESC);
}

uint16_t vframe_encode(const uint8_t * p_body, uint8_t len, uint8_t * p_out, uint16_t out_max)
{
    uint16_t n = 0u;

    if (out_max < 2u)
    {
        return 0u;
    }

    p_out[n++] = VALVE_FRAME_START;

    for (uint8_t i = 0u; i < len; i++)
    {
        uint8_t byte = p_body[i];

        if (needs_escape(byte))
        {
            if ((uint16_t)(n + 2u) >= out_max)
            {
                return 0u;
            }
            p_out[n++] = VALVE_FRAME_ESC;
            p_out[n++] = (uint8_t)(byte ^ VALVE_FRAME_XOR);
        }
        else
        {
            if ((uint16_t)(n + 1u) >= out_max)
            {
                return 0u;
            }
            p_out[n++] = byte;
        }
    }

    p_out[n++] = VALVE_FRAME_END;
    return n;
}
