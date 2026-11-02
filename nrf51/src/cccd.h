/**
 * @file cccd.h
 * @brief Read a CCCD's notification bit from the attribute table.
 */

#ifndef CCCD_H__
#define CCCD_H__

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ble.h"
#include "ble_srv_common.h"

/**
 * @brief True if notifications are on in the CCCD at @p cccd_handle.
 *
 * Read from the attribute table rather than tracked from CCCD write events: a
 * bonded host that reconnects gets its CCCDs restored by the device manager
 * and never writes them again, so write events alone would miss it.
 */
static inline bool cccd_notify_enabled(uint16_t conn_handle, uint16_t cccd_handle)
{
    uint8_t           cccd[2] = { 0, 0 };
    ble_gatts_value_t value;

    if (conn_handle == BLE_CONN_HANDLE_INVALID)
    {
        return false;
    }

    memset(&value, 0, sizeof(value));
    value.len     = sizeof(cccd);
    value.offset  = 0;
    value.p_value = cccd;

    if (sd_ble_gatts_value_get(conn_handle, cccd_handle, &value) != NRF_SUCCESS)
    {
        return false;
    }

    return ble_srv_is_notification_enabled(cccd);
}

#endif /* CCCD_H__ */
