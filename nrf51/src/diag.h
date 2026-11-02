/**
 * @file diag.h
 * @brief Diagnostics GATT service, for bring-up and testing.
 *
 * A vendor service any BLE client can read without pairing -- a browser over
 * Web Bluetooth, or a phone app such as nRF Connect. It exists because this
 * firmware has no other window into what it is doing: no debugger, no log
 * port, and a HID report that only shows the end of a long chain.
 *
 *   Status  (read)          link counters, decoder results, last fault
 *   Live    (read, notify)  the decoded controller state, as received
 *   Control (write)         test commands, DIAG_CMD_*
 *
 * UUIDs: base 5c0e____-7b3a-4e2d-9a51-3d8c2f6b1e47, with the 16-bit parts
 * below. The test guide, tools/web/test.html, uses the same values.
 */

#ifndef DIAG_H__
#define DIAG_H__

#include <stdint.h>
#include <stdbool.h>
#include "ble.h"

#define DIAG_UUID_SERVICE   0x0001u
#define DIAG_UUID_STATUS    0x0002u
#define DIAG_UUID_LIVE      0x0003u
#define DIAG_UUID_CONTROL   0x0004u

/** Bump when diag_status_t changes layout; the web page checks it. */
#define DIAG_STATUS_VERSION 4u

/* diag_status_t.mode_strap bits */
#define DIAG_MODE_STRAP     0x01u   /**< The strap read high at boot.                  */
#define DIAG_MODE_BLE       0x02u   /**< BLE mode is in effect.                        */
#define DIAG_MODE_CARRIED   0x04u   /**< Mode carried over a soft reset, strap ignored. */
#define DIAG_MODE_DETECTED  0x08u   /**< BLE mode recognised from the stream itself.   */

/* diag_status_t.flags */
#define DIAG_F_CONNECTED    0x01u
#define DIAG_F_ENCRYPTED    0x02u
#define DIAG_F_HID_REPORTING 0x04u  /**< Host enabled input report notifications. */
#define DIAG_F_BONDED_PEER  0x08u   /**< This link belongs to a stored bond.      */
#define DIAG_F_BONDING_OK   0x10u   /**< Bond storage initialised.                */
#define DIAG_F_LIZARD       0x20u   /**< A lizard-mode payload arrived in the last second. */
#define DIAG_F_ADVERTISING  0x40u
#define DIAG_F_DOZING       0x80u   /**< The LPC told the radio to doze.          */

/*
 * Control commands: first byte written to the Control characteristic.
 *
 * The characteristic is writable without pairing, by anyone in range while the
 * controller advertises, so it only carries commands that are harmless in a
 * stranger's hands. Forgetting bonds or resetting the radio would let any
 * phone nearby unpair the user's TV or keep the radio rebooting; neither is
 * offered here. (0x03 and 0x04 were those commands; they stay unassigned.)
 */
#define DIAG_CMD_RESYNC      0x01u  /**< Re-send the lizard-off sequence now.       */
#define DIAG_CMD_HAPTIC      0x02u  /**< Short haptic buzz on both pads.            */

typedef struct __attribute__((packed))
{
    uint8_t  version;            /**< DIAG_STATUS_VERSION                        */
    uint8_t  mode_strap;         /**< DIAG_MODE_* bits                           */
    uint8_t  flags;              /**< DIAG_F_*                                   */
    uint8_t  bonds;              /**< Unused for now, always 0                   */
    uint32_t reset_reason;       /**< NRF_POWER->RESETREAS at boot               */
    uint32_t uptime_ms;
    uint32_t rx_bytes;
    uint32_t rx_frames;
    uint16_t rx_uart_errors;
    uint16_t rx_dropped;
    uint16_t rx_queue_full;
    uint16_t tx_full;
    uint16_t rec_ok;             /**< State records applied                      */
    uint16_t rec_bad;            /**< Truncated or unknown records               */
    uint16_t rec_lizard;         /**< Keyboard+mouse payloads                    */
    uint16_t resyncs;            /**< Lizard-off sequences sent                  */
    uint16_t host_msgs;          /**< 0x55 frames                                */
    uint16_t feature_replies;    /**< 0x50 frames from the LPC                   */
    uint16_t other_ops;          /**< Frames with any other opcode               */
    uint8_t  last_other_op;
    uint8_t  last_rec_extra;     /**< Bytes after the last chunk; expected 0     */
    uint16_t last_rec_flags;
    uint16_t reports_sent;
    uint16_t report_errors;
    uint8_t  lpc_queries;        /**< Version and device-ID queries answered     */
    uint8_t  last_host_msg_len;
    uint8_t  last_host_msg[12];  /**< Start of the last 0x55 frame body          */
    uint8_t  last_feature_reply[8];
    uint32_t fault_code;         /**< From the error that caused the last reset  */
    uint16_t fault_line;
    char     fault_file[10];
    /* Version 2 */
    uint16_t rx_oversize;        /**< Intact frames too long to keep             */
    uint16_t lpc_tx_dropped;     /**< Frames to the LPC lost to a full queue     */
    /* Version 4: raw receive capture, from uart_link_stats_t */
    uint16_t rx_starts;
    uint16_t rx_ends;
    uint8_t  rx_errorsrc;
    uint8_t  rx_first[32];
    uint8_t  rx_last[32];        /**< Ring: byte n at n % 32, n counted by rx_bytes */
} diag_status_t;

typedef struct __attribute__((packed))
{
    uint8_t  seq;                /**< Increments with every notification         */
    uint8_t  flags;              /**< DIAG_F_*                                   */
    uint8_t  buttons[3];         /**< VALVE_BTN_* bits 0..23                     */
    uint8_t  ltrig;
    uint8_t  rtrig;
    int16_t  lstick_x;
    int16_t  lstick_y;
    int16_t  lpad_x;
    int16_t  lpad_y;
    int16_t  rpad_x;
    int16_t  rpad_y;
    uint8_t  rec_count;          /**< Low byte of rec_ok                         */
} diag_live_t;

/** Called in the SoftDevice event context; keep it short. */
typedef void (*diag_cmd_handler_t)(uint8_t cmd);

/**
 * @brief Add the service.
 *
 * @param[in] p_status Status block, served straight from RAM on every read;
 *                     keep it up to date and keep it alive.
 * @param[in] handler  Control command handler.
 */
uint32_t diag_init(diag_status_t * p_status, diag_cmd_handler_t handler);

void diag_on_ble_evt(const ble_evt_t * p_ble_evt);

/** True when the client on @p conn_handle has Live notifications on. */
bool diag_live_enabled(uint16_t conn_handle);

/** Notify the Live characteristic. @return true if the notification was queued. */
bool diag_live_notify(uint16_t conn_handle, const diag_live_t * p_live);

#endif /* DIAG_H__ */
