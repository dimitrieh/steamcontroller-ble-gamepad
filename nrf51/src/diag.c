/**
 * @file diag.c
 * @brief Diagnostics GATT service, for bring-up and testing.
 */

#include <string.h>

#include "diag.h"
#include "cccd.h"

/* 5c0e0000-7b3a-4e2d-9a51-3d8c2f6b1e47, little-endian, bytes 12..13 zero. */
static const ble_uuid128_t m_base_uuid =
{
    { 0x47, 0x1E, 0x6B, 0x2F, 0x8C, 0x3D, 0x51, 0x9A,
      0x2D, 0x4E, 0x3A, 0x7B, 0x00, 0x00, 0x0E, 0x5C }
};

#define CONTROL_MAX_LEN 8u

static uint8_t                  m_uuid_type;
static uint16_t                 m_service_handle;
static ble_gatts_char_handles_t m_status_handles;
static ble_gatts_char_handles_t m_live_handles;
static ble_gatts_char_handles_t m_control_handles;
static diag_cmd_handler_t       m_cmd_handler;

static uint32_t char_add(uint16_t                   uuid16,
                         const ble_gatts_char_md_t * p_char_md,
                         uint8_t                    vloc,
                         uint8_t *                  p_value,
                         uint16_t                   init_len,
                         uint16_t                   max_len,
                         bool                       writable,
                         ble_gatts_char_handles_t * p_handles)
{
    ble_uuid_t          uuid = { .uuid = uuid16, .type = m_uuid_type };
    ble_gatts_attr_md_t attr_md;
    ble_gatts_attr_t    attr;

    memset(&attr_md, 0, sizeof(attr_md));
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
    if (writable)
    {
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.write_perm);
    }
    else
    {
        BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&attr_md.write_perm);
    }
    attr_md.vloc = vloc;
    attr_md.vlen = writable ? 1 : 0;

    memset(&attr, 0, sizeof(attr));
    attr.p_uuid    = &uuid;
    attr.p_attr_md = &attr_md;
    attr.init_len  = init_len;
    attr.max_len   = max_len;
    attr.p_value   = p_value;

    return sd_ble_gatts_characteristic_add(m_service_handle, p_char_md, &attr, p_handles);
}

uint32_t diag_init(diag_status_t * p_status, diag_cmd_handler_t handler)
{
    uint32_t            err_code;
    ble_uuid_t          service_uuid;
    ble_gatts_char_md_t char_md;
    ble_gatts_attr_md_t cccd_md;
    diag_live_t         live_init;
    uint8_t             control_init = 0u;

    m_cmd_handler = handler;

    err_code = sd_ble_uuid_vs_add(&m_base_uuid, &m_uuid_type);
    if (err_code != NRF_SUCCESS)
    {
        return err_code;
    }

    service_uuid.type = m_uuid_type;
    service_uuid.uuid = DIAG_UUID_SERVICE;
    err_code = sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &service_uuid,
                                        &m_service_handle);
    if (err_code != NRF_SUCCESS)
    {
        return err_code;
    }

    /* Status: read-only, served from the caller's RAM so it is always
     * current without a value_set call per counter update. */
    memset(&char_md, 0, sizeof(char_md));
    char_md.char_props.read = 1;
    err_code = char_add(DIAG_UUID_STATUS, &char_md, BLE_GATTS_VLOC_USER,
                        (uint8_t *)p_status, sizeof(*p_status), sizeof(*p_status),
                        false, &m_status_handles);
    if (err_code != NRF_SUCCESS)
    {
        return err_code;
    }

    /* Live: read and notify. */
    memset(&cccd_md, 0, sizeof(cccd_md));
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.write_perm);
    cccd_md.vloc = BLE_GATTS_VLOC_STACK;

    memset(&char_md, 0, sizeof(char_md));
    char_md.char_props.read   = 1;
    char_md.char_props.notify = 1;
    char_md.p_cccd_md         = &cccd_md;

    memset(&live_init, 0, sizeof(live_init));
    err_code = char_add(DIAG_UUID_LIVE, &char_md, BLE_GATTS_VLOC_STACK,
                        (uint8_t *)&live_init, sizeof(live_init), sizeof(live_init),
                        false, &m_live_handles);
    if (err_code != NRF_SUCCESS)
    {
        return err_code;
    }

    /* Control: write, with or without response. */
    memset(&char_md, 0, sizeof(char_md));
    char_md.char_props.write         = 1;
    char_md.char_props.write_wo_resp = 1;
    err_code = char_add(DIAG_UUID_CONTROL, &char_md, BLE_GATTS_VLOC_STACK,
                        &control_init, 1u, CONTROL_MAX_LEN, true, &m_control_handles);

    return err_code;
}

void diag_on_ble_evt(const ble_evt_t * p_ble_evt)
{
    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GATTS_EVT_WRITE:
        {
            const ble_gatts_evt_write_t * p_write = &p_ble_evt->evt.gatts_evt.params.write;

            if ((p_write->handle == m_control_handles.value_handle) &&
                     (p_write->len >= 1u) && (m_cmd_handler != NULL))
            {
                m_cmd_handler(p_write->data[0]);
            }
            break;
        }

        default:
            break;
    }
}

bool diag_live_enabled(uint16_t conn_handle)
{
    return cccd_notify_enabled(conn_handle, m_live_handles.cccd_handle);
}

bool diag_live_notify(uint16_t conn_handle, const diag_live_t * p_live)
{
    ble_gatts_hvx_params_t hvx;
    uint16_t               len = sizeof(*p_live);

    if (conn_handle == BLE_CONN_HANDLE_INVALID)
    {
        return false;
    }

    memset(&hvx, 0, sizeof(hvx));
    hvx.handle = m_live_handles.value_handle;
    hvx.type   = BLE_GATT_HVX_NOTIFICATION;
    hvx.offset = 0;
    hvx.p_len  = &len;
    hvx.p_data = (uint8_t *)p_live;

    /* Out of buffers or not ready: this is a debug view, skip the frame. */
    return sd_ble_gatts_hvx(conn_handle, &hvx) == NRF_SUCCESS;
}
