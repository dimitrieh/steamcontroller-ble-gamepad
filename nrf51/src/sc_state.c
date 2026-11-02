/**
 * @file sc_state.c
 * @brief Controller state, rebuilt from the LPC's delta-coded state records.
 *
 * The record format is the one SDL parses in UpdateBLESteamControllerState:
 * byte 0 carries the record type in its low nibble and chunk flags in its high
 * nibble, byte 1 carries the rest of the chunk flags, and the chunks follow in
 * flag order. The LPC builds it at 0x8C10 in vcf_wired_controller_d0g_5b0f21bd.
 */

#include <string.h>

#include "sc_state.h"
#include "valve_proto.h"

/** Every chunk flag this decoder knows the size of. */
#define KNOWN_CHUNKS (VALVE_CHUNK_BUTTONS | VALVE_CHUNK_TRIGGERS | VALVE_CHUNK_BUTTONS_EXT | \
                      VALVE_CHUNK_LSTICK | VALVE_CHUNK_LPAD | VALVE_CHUNK_RPAD |            \
                      VALVE_CHUNK_ACCEL | VALVE_CHUNK_GYRO | VALVE_CHUNK_QUAT)

typedef struct
{
    uint16_t flag;
    uint8_t  size;
} chunk_t;

/** In wire order. */
static const chunk_t m_chunks[] =
{
    { VALVE_CHUNK_BUTTONS,     3u },
    { VALVE_CHUNK_TRIGGERS,    2u },
    { VALVE_CHUNK_BUTTONS_EXT, 3u },
    { VALVE_CHUNK_LSTICK,      4u },
    { VALVE_CHUNK_LPAD,        4u },
    { VALVE_CHUNK_RPAD,        4u },
    { VALVE_CHUNK_ACCEL,       6u },
    { VALVE_CHUNK_GYRO,        6u },
    { VALVE_CHUNK_QUAT,        8u },
};

#define CHUNK_COUNT (sizeof(m_chunks) / sizeof(m_chunks[0]))

static int16_t get_le16(const uint8_t * p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void get_le16_array(int16_t * p_dst, const uint8_t * p_src, uint8_t count)
{
    for (uint8_t i = 0u; i < count; i++)
    {
        p_dst[i] = get_le16(&p_src[2u * i]);
    }
}

void sc_state_reset(sc_state_t * p_state)
{
    memset(p_state, 0, sizeof(*p_state));
}

sc_rec_result_t sc_state_apply(sc_state_t *    p_state,
                               const uint8_t * p_rec,
                               uint8_t         len,
                               uint16_t *      p_flags,
                               uint8_t *       p_extra)
{
    if (len < 1u)
    {
        return SC_REC_TRUNCATED;
    }

    if (p_rec[0] == VALVE_REC_TYPE_LIZARD)
    {
        return SC_REC_LIZARD;
    }

    if ((p_rec[0] & 0x0Fu) != VALVE_REC_TYPE_STATE)
    {
        return SC_REC_UNKNOWN;
    }

    if (len < 2u)
    {
        return SC_REC_TRUNCATED;
    }

    uint16_t flags = (uint16_t)((p_rec[0] & 0xF0u) | ((uint16_t)p_rec[1] << 8));

    if (p_flags != NULL)
    {
        *p_flags = flags;
    }

    if ((flags & (uint16_t)~KNOWN_CHUNKS) != 0u)
    {
        /* A chunk we cannot size: every chunk after it would be misread. */
        return SC_REC_UNKNOWN;
    }

    /* Validate the whole record before touching the state. */
    uint16_t need = 2u;
    for (uint8_t i = 0u; i < CHUNK_COUNT; i++)
    {
        if ((flags & m_chunks[i].flag) != 0u)
        {
            need = (uint16_t)(need + m_chunks[i].size);
        }
    }

    if (len < need)
    {
        return SC_REC_TRUNCATED;
    }

    if (p_extra != NULL)
    {
        *p_extra = (uint8_t)(len - need);
    }

    const uint8_t * p = &p_rec[2];

    if (flags & VALVE_CHUNK_BUTTONS)
    {
        p_state->buttons = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        p += 3;
    }
    if (flags & VALVE_CHUNK_TRIGGERS)
    {
        p_state->ltrig = p[0];
        p_state->rtrig = p[1];
        p += 2;
    }
    if (flags & VALVE_CHUNK_BUTTONS_EXT)
    {
        memcpy(p_state->buttons_ext, p, 3u);
        p += 3;
    }
    if (flags & VALVE_CHUNK_LSTICK)
    {
        p_state->lstick_x = get_le16(&p[0]);
        p_state->lstick_y = get_le16(&p[2]);
        p += 4;
    }
    if (flags & VALVE_CHUNK_LPAD)
    {
        p_state->lpad_x = get_le16(&p[0]);
        p_state->lpad_y = get_le16(&p[2]);
        p += 4;
    }
    if (flags & VALVE_CHUNK_RPAD)
    {
        p_state->rpad_x = get_le16(&p[0]);
        p_state->rpad_y = get_le16(&p[2]);
        p += 4;
    }
    if (flags & VALVE_CHUNK_ACCEL)
    {
        get_le16_array(p_state->accel, p, 3u);
        p += 6;
    }
    if (flags & VALVE_CHUNK_GYRO)
    {
        get_le16_array(p_state->gyro, p, 3u);
        p += 6;
    }
    if (flags & VALVE_CHUNK_QUAT)
    {
        get_le16_array(p_state->quat, p, 4u);
    }

    return SC_REC_OK;
}
