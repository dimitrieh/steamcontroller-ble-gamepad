/**
 * @file main.c
 * @brief Steam Controller BLE HID gamepad -- nRF51822 radio application.
 *
 * Replaces Valve's stock radio application (which advertises a HID Mouse +
 * Keyboard + vendor collection and therefore appears as a mouse to anything
 * but Steam) with a single HID Gamepad collection that generic HID stacks,
 * Android's in particular, bind as a game controller.
 *
 * The LPC11U37 keeps Valve's own BLE firmware (vcf_wired_controller_d0g_
 * 5b0f21bd). This application speaks that firmware's UART protocol, decoded
 * in docs/VALVE_LINK.md: it turns lizard mode off so the LPC streams full
 * controller state, decodes that stream, and publishes it as a gamepad.
 * Nothing on the LPC changes, so the controller stays flashable over USB with
 * the browser tool, and the radio stays reflashable through Valve's SWD bridge.
 *
 * Runs on the SoftDevice already present on the controller: stock Nordic S110
 * 8.0.0, verified byte-identical to the nRF5 SDK 10.0.0 release. Linked at
 * 0x18000, the S110 application base.
 *
 * Contexts:
 *   UART0 interrupt, APP_IRQ_PRIORITY_HIGH -- receives and frames bytes only
 *   SoftDevice events and app_timer, APP_IRQ_PRIORITY_LOW -- set flags only
 *   main loop, thread mode -- everything else, including every UART send
 */

#include <stdint.h>
#include <string.h>

#include "nordic_common.h"
#include "nrf.h"
#include "nrf_assert.h"
#include "app_error.h"
#include "app_timer.h"
#include "app_util_platform.h"
#include "ble.h"
#include "ble_hci.h"
#include "ble_srv_common.h"
#include "ble_advdata.h"
#include "ble_conn_params.h"
#include "ble_hids.h"
#include "ble_bas.h"
#include "ble_dis.h"
#include "softdevice_handler.h"
#include "device_manager.h"
#include "pstorage.h"

#include "valve_proto.h"
#include "uart_link.h"
#include "vframe.h"
#include "sc_state.h"
#include "gamepad_hid.h"
#include "diag.h"
#include "cccd.h"
#include "app_util.h"

/* ---- Identity ------------------------------------------------------------ */

#define DEVICE_NAME             "Steam Controller"
#define MANUFACTURER_NAME       "Valve Corporation"
#define MODEL_NUMBER            "Steam Controller"
#define FIRMWARE_REVISION       "sc-ble-gamepad 0.2"

/*
 * Deliberately not Valve's USB IDs. Valve's 0x28DE is what SDL, Steam and some
 * Android key layouts use to recognise a Steam Controller, and every one of
 * them would then try to speak Valve's vendor protocol to a device that no
 * longer has it. 0xFFFF is the Bluetooth SIG company ID reserved for testing,
 * so nothing special-cases it and the host treats this as a generic gamepad.
 */
#define PNP_ID_VENDOR_ID_SOURCE 0x01        /**< Bluetooth SIG company ID. */
#define PNP_ID_VENDOR_ID        0xFFFF
#define PNP_ID_PRODUCT_ID       0x5C01
#define PNP_ID_PRODUCT_VERSION  0x0002

#ifndef BUILD_UNIX_TIME
#define BUILD_UNIX_TIME         0u
#endif

/* ---- Timing -------------------------------------------------------------- */

#define APP_TIMER_PRESCALER     0
#define APP_TIMER_OP_QUEUE_SIZE 8

#define MS_TO_TICKS(ms)         APP_TIMER_TICKS(ms, APP_TIMER_PRESCALER)

/** Housekeeping tick: uptime, timeouts, diagnostics. */
#define TICK_INTERVAL_MS        50u

/** Advertising interval, 0.625 ms units: 40 ms. */
#define APP_ADV_INTERVAL        64
/** Advertise indefinitely; the LPC powers the controller down when idle. */
#define APP_ADV_TIMEOUT_S       0

/*
 * 11.25 ms to 15 ms. A gamepad wants the shortest interval a host will
 * actually grant; Android accepts 11.25 ms for HID peripherals, and one input
 * report fits in a single packet.
 */
#define MIN_CONN_INTERVAL       MSEC_TO_UNITS(11.25, UNIT_1_25_MS)
#define MAX_CONN_INTERVAL       MSEC_TO_UNITS(15, UNIT_1_25_MS)
#define SLAVE_LATENCY           0
#define CONN_SUP_TIMEOUT        MSEC_TO_UNITS(3000, UNIT_10_MS)

#define FIRST_CONN_PARAMS_UPDATE_DELAY  MS_TO_TICKS(5000)
#define NEXT_CONN_PARAMS_UPDATE_DELAY   MS_TO_TICKS(30000)
#define MAX_CONN_PARAMS_UPDATE_COUNT    3

/*
 * Every frame to the LPC goes out at least this far apart. The LPC ignores all
 * bytes that arrive while its PendSV handler is still busy with the previous
 * frame (0x95D4 in 5b0f21bd), so back-to-back frames -- a resync sequence, or a
 * reply sent right after one -- would lose all but the first two.
 */
#define LPC_FRAME_GAP_MS        20u
#define LPC_TXQ_SLOTS           8u
#define LPC_TX_MAX_BODY         16u

/*
 * Resync: load defaults, clear mappings, lizard off. Loading defaults clears
 * the LPC's copy of the last state it sent, so the first state record after
 * the sequence carries every non-zero chunk. Give up after this long.
 */
#define RESYNC_SETTLE_MS        300u
/** First resync after boot, once the LPC has had time to bring the link up. */
#define RESYNC_BOOT_DELAY_MS    300u
/** Minimum spacing of resyncs triggered by errors or lizard payloads. */
#define RESYNC_MIN_SPACING_MS   1000u
/** Attempts at a resync the LPC visibly acted on before giving up on it. */
#define RESYNC_MAX_ATTEMPTS     3u

/** At most this many HID notifications queued in the SoftDevice. Queuing more
 *  only adds latency: each would carry a state that is already stale. */
#define MAX_REPORTS_IN_FLIGHT   2u

/** Live diagnostics notification spacing. */
#define LIVE_INTERVAL_MS        50u

/* ---- Security ------------------------------------------------------------ */

/* Just Works pairing with bonding, so a TV reconnects on its own at power-on. */
#define SEC_PARAM_BOND          1
#define SEC_PARAM_MITM          0
#define SEC_PARAM_IO_CAPS       BLE_GAP_IO_CAPS_NONE
#define SEC_PARAM_OOB           0
#define SEC_PARAM_MIN_KEY_SIZE  7
#define SEC_PARAM_MAX_KEY_SIZE  16

/* ---- HID ----------------------------------------------------------------- */

/* Protocol Mode characteristic values, from the HID Service specification. */
#define HID_PROTOCOL_MODE_BOOT   0x00
#define HID_PROTOCOL_MODE_REPORT 0x01

#define INPUT_REPORT_COUNT      1
#define INPUT_REP_GAMEPAD_INDEX 0

/* ---- Fault record -------------------------------------------------------- */

/*
 * The last 16 bytes of RAM are left out of the linker's RAM region (see
 * sc_ble_gamepad.ld), so they survive a soft reset. app_error_handler writes
 * what went wrong there before resetting, and the next boot reports it over
 * the diagnostics service. Without this a fault is just a radio that
 * disappears for a moment.
 */
#define FAULT_RECORD_ADDR       0x20003FF0u
#define FAULT_MAGIC             0x5CFA0171u

typedef struct
{
    uint32_t magic;
    uint32_t code;
    uint16_t line;
    char     file[6];
} fault_record_t;

#define m_fault (*(volatile fault_record_t *)FAULT_RECORD_ADDR)

/* ---- Pending work, set from event contexts, run by the main loop -------- */

#define ACT_SEND_CONNECTED      (1u << 0)
#define ACT_SEND_DISCONNECTED   (1u << 1)
#define ACT_RESYNC              (1u << 2)
#define ACT_TX_GAP              (1u << 3)
#define ACT_HAPTIC              (1u << 4)
#define ACT_TICK                (1u << 7)
#define ACT_ADVERTISE           (1u << 8)
/** Any SoftDevice or system event: nothing to do but re-run the main loop's
 *  report and diagnostics pass (a TX_COMPLETE frees room for a report, say). */
#define ACT_EVENT               (1u << 9)

static volatile uint32_t m_actions;

static void action_post(uint32_t bits)
{
    CRITICAL_REGION_ENTER();
    m_actions |= bits;
    CRITICAL_REGION_EXIT();
}

static uint32_t actions_take(void)
{
    uint32_t bits;

    CRITICAL_REGION_ENTER();
    bits      = m_actions;
    m_actions = 0u;
    CRITICAL_REGION_EXIT();

    return bits;
}

/* ---- State --------------------------------------------------------------- */

static ble_hids_t m_hids;
static ble_bas_t  m_bas;

static dm_application_instance_t m_app_handle;
static dm_handle_t               m_peer_handle;
static bool                      m_dm_ok;
static bool                      m_peer_bonded;

static volatile uint16_t m_conn_handle = BLE_CONN_HANDLE_INVALID;
static volatile bool     m_hid_reporting;
static volatile bool     m_link_encrypted;
static volatile bool     m_advertising;
/** Notifications queued in the SoftDevice. Incremented by the main loop,
 *  decremented by TX_COMPLETE in the SoftDevice event context, so every
 *  change goes through a critical region: a lost update would either stall
 *  reporting for good or let the queue fill with stale reports. */
static volatile uint8_t  m_reports_in_flight;
static volatile uint32_t m_last_tx_complete_ms;
static bool              m_dozing;
static bool              m_wipe_pending;
static bool              m_wipe_started;

/** Controller state as decoded from the LPC. Main loop only. */
static sc_state_t m_state;

/** Last report the host actually accepted, so unchanged state is not resent. */
static uint8_t m_last_report[GAMEPAD_INPUT_REP_LEN];
static bool    m_last_report_valid;

/*
 * Resync state. A resync is "active" from sending the sequence until the first
 * full record after it, or the settle timeout. The decoder is zeroed when the
 * first lizard payload arrives: that is the point from which the LPC sends no
 * more deltas against its old copy, and the next state record is the keyframe.
 * Reports are held only between that zeroing and the keyframe, so the host
 * never sees the half-built state.
 */
static bool     m_resync_active;
static bool     m_resync_zeroed;
static uint32_t m_resync_started_ms;
static uint32_t m_last_resync_ms;
static bool     m_resync_ever;
static uint8_t  m_resync_attempts;
/** A resync is owed. Requests are latched rather than dropped: one that comes
 *  too soon after the last resync, or while one runs, is carried out later. */
static bool     m_resync_wanted;

/** The LPC signalled BLE mode at boot, or a diagnostics resync overrode it.
 *  Only then is the 0x51 stream a BLE state record stream worth decoding, and
 *  only then is turning lizard mode off ours to do: over USB it would take the
 *  desktop's mouse emulation away. */
static bool     m_ble_mode;
static bool     m_host_suspended;
static volatile uint32_t m_last_in_flight_ms;

/* Paced frames to the LPC: main loop only. */
typedef struct
{
    uint8_t len;
    uint8_t body[LPC_TX_MAX_BODY];
} lpc_tx_t;

static lpc_tx_t m_lpc_txq[LPC_TXQ_SLOTS];
static uint8_t  m_lpc_txq_head;
static uint8_t  m_lpc_txq_tail;
static bool     m_lpc_tx_gap;       /**< Waiting out LPC_FRAME_GAP_MS. */

static uint32_t m_uptime_ms;
static uint32_t m_last_lizard_ms;
static bool     m_lizard_seen;
static uint32_t m_last_live_ms;
static uint32_t m_last_error_total;

static diag_status_t m_diag;
static diag_live_t   m_live;

APP_TIMER_DEF(m_tick_timer);
APP_TIMER_DEF(m_tx_gap_timer);

static void advertising_start(void);
static void ble_mode_set(bool on);

/* ---- Error handling ------------------------------------------------------ */

/**
 * @brief Last-resort error handler.
 *
 * A reset is the right response on this hardware: the LPC stays up regardless,
 * so a radio that resets is always recoverable, whereas a radio that spins in
 * a loop looks like a dead controller.
 */
void app_error_handler(uint32_t error_code, uint32_t line_num, const uint8_t * p_file_name)
{
    const char * p_base = (const char *)p_file_name;

    if (p_base != NULL)
    {
        for (const char * p = p_base; *p != '\0'; p++)
        {
            if ((*p == '/') || (*p == '\\'))
            {
                p_base = p + 1;
            }
        }
    }

    m_fault.code = error_code;
    m_fault.line = (uint16_t)line_num;
    for (uint8_t i = 0u; i < sizeof(m_fault.file); i++)
    {
        char c = ((p_base != NULL) && (p_base[0] != '\0')) ? *p_base++ : '\0';
        m_fault.file[i] = c;
        if (c == '\0')
        {
            p_base = NULL;
        }
    }
    m_fault.magic = FAULT_MAGIC;

    NVIC_SystemReset();
}

/**
 * @brief HardFault: record where it happened, then reset.
 *
 * The startup file's default spins forever, which on this hardware looks like
 * a dead radio and leaves nothing to diagnose. The fault record gets the
 * faulting PC as its code and "HFAULT" as its file.
 */
__attribute__((used)) void hardfault_record(const uint32_t * p_frame)
{
    static const char tag[] = "HFAULT";

    m_fault.code = p_frame[6];     /* Stacked PC. */
    m_fault.line = 0xFFFFu;
    for (uint8_t i = 0u; i < sizeof(m_fault.file); i++)
    {
        m_fault.file[i] = tag[i];
    }
    m_fault.magic = FAULT_MAGIC;

    NVIC_SystemReset();
}

/* Naked so the stack pointer is still the exception frame's. Cortex-M0 has
 * only the MSP in use here, so no MSP/PSP test is needed. */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile ("mrs r0, msp      \n"
                    "ldr r1, =hardfault_record \n"
                    "bx  r1           \n");
}

void assert_nrf_callback(uint16_t line_num, const uint8_t * p_file_name)
{
    app_error_handler(0xDEADBEEF, line_num, p_file_name);
}

static void service_error_handler(uint32_t nrf_error)
{
    APP_ERROR_HANDLER(nrf_error);
}

static void conn_params_error_handler(uint32_t nrf_error)
{
    APP_ERROR_HANDLER(nrf_error);
}

/* ---- Time ---------------------------------------------------------------- */

static uint32_t now_ms(void)
{
    return m_uptime_ms;
}

static bool elapsed(uint32_t since_ms, uint32_t interval_ms)
{
    return (uint32_t)(now_ms() - since_ms) >= interval_ms;
}

/* ---- Notification accounting ---------------------------------------------- */

/**
 * @brief Claim a notification slot before sending.
 *
 * The slot is taken before the send, not after it succeeds: TX_COMPLETE runs
 * at a higher priority than the main loop and can arrive between a successful
 * send and the code after it, and an increment that lands after its own
 * completion would leak a slot for good.
 */
static bool in_flight_reserve(void)
{
    bool ok;

    CRITICAL_REGION_ENTER();
    ok = (m_reports_in_flight < MAX_REPORTS_IN_FLIGHT);
    if (ok)
    {
        m_reports_in_flight++;
        m_last_in_flight_ms = m_uptime_ms;
    }
    CRITICAL_REGION_EXIT();

    return ok;
}

/** Give back a slot whose send failed. */
static void in_flight_release(void)
{
    CRITICAL_REGION_ENTER();
    if (m_reports_in_flight != 0u)
    {
        m_reports_in_flight--;
    }
    CRITICAL_REGION_EXIT();
}

/* ---- Link to the LPC ----------------------------------------------------- */

/** Send the next queued frame if the gap since the last one has passed. */
static void lpc_tx_pump(void)
{
    if (m_lpc_tx_gap || (m_lpc_txq_tail == m_lpc_txq_head))
    {
        return;
    }

    const lpc_tx_t * p_frame = &m_lpc_txq[m_lpc_txq_tail];

    /* A refused frame is counted in the link stats. Every frame this firmware
     * sends is either repeated later or harmless to lose. */
    (void)uart_link_send(p_frame->body, p_frame->len);
    m_lpc_txq_tail = (uint8_t)((m_lpc_txq_tail + 1u) % LPC_TXQ_SLOTS);

    m_lpc_tx_gap = true;
    if (app_timer_start(m_tx_gap_timer, MS_TO_TICKS(LPC_FRAME_GAP_MS), NULL) != NRF_SUCCESS)
    {
        /* Without the timer the queue would stall for good. Carry on unpaced
         * instead: a dropped frame is recoverable, a stuck queue is not. */
        m_lpc_tx_gap = false;
        action_post(ACT_TX_GAP);
    }
}

/** Queue one frame for the LPC. Main loop only. */
static void lpc_send(const uint8_t * p_body, uint8_t len)
{
    uint8_t next = (uint8_t)((m_lpc_txq_head + 1u) % LPC_TXQ_SLOTS);

    if ((len > LPC_TX_MAX_BODY) || (next == m_lpc_txq_tail))
    {
        m_diag.lpc_tx_dropped++;
        return;
    }

    m_lpc_txq[m_lpc_txq_head].len = len;
    memcpy(m_lpc_txq[m_lpc_txq_head].body, p_body, len);
    m_lpc_txq_head = next;

    lpc_tx_pump();
}

static void lpc_send_op(uint8_t op)
{
    lpc_send(&op, 1u);
}

/**
 * @brief Tell the LPC our firmware version without being asked.
 *
 * The LPC stores any 5-byte 0x56 frame (0xD682 in 5b0f21bd), asked for or
 * not, and reports it as the radio revision. It only asks at its own boot and
 * when a host reads its attributes, and its boot-time question can arrive
 * before this radio has its UART up, so the answer is volunteered too.
 */
static void version_announce(void)
{
    uint8_t frame[5];

    frame[0] = VALVE_OP_VERSION;
    (void)uint32_encode(BUILD_UNIX_TIME, &frame[1]);
    lpc_send(frame, sizeof(frame));
}

/**
 * @brief Start the resync sequence: defaults, clear mappings, lizard off.
 *
 * All four frames are queued at once; the transmit pacing spaces them out.
 * See the resync state comment for how the keyframe is recognised.
 */
static void resync_begin(void)
{
    static const uint8_t load_defaults[] =
    {
        VALVE_OP_FEATURE, VALVE_CMD_LOAD_DEFAULT_SETTINGS, 0x00
    };
    static const uint8_t clear_mappings[] =
    {
        VALVE_OP_FEATURE, VALVE_CMD_CLEAR_DIGITAL_MAPPINGS, 0x00
    };
    static const uint8_t lizard_off[] =
    {
        VALVE_OP_FEATURE, VALVE_CMD_SET_SETTINGS_VALUES, 12u,
        VALVE_SETTING_WIRELESS_PACKET_VERSION, 0x02, 0x00,
        VALVE_SETTING_LEFT_TRACKPAD_MODE,      VALVE_TRACKPAD_NONE, 0x00,
        VALVE_SETTING_RIGHT_TRACKPAD_MODE,     VALVE_TRACKPAD_NONE, 0x00,
        VALVE_SETTING_SMOOTH_ABSOLUTE_MOUSE,   0x00, 0x00,
    };

    if (m_resync_active || !m_ble_mode)
    {
        return;
    }

    lpc_send(load_defaults, sizeof(load_defaults));
    lpc_send(clear_mappings, sizeof(clear_mappings));
    lpc_send(lizard_off, sizeof(lizard_off));
    /* Covers an LPC that rebooted without resetting us. */
    version_announce();

    m_resync_active     = true;
    m_resync_zeroed     = false;
    m_resync_started_ms = now_ms();
    m_last_resync_ms    = now_ms();
    m_resync_ever       = true;
    m_resync_attempts++;
    m_diag.resyncs++;
}

/** Called when the resync ends, whichever way. */
static void resync_end(void)
{
    m_resync_active = false;
    m_resync_zeroed = false;
}

/** Ask for a resync. It runs from tick() as soon as spacing allows. */
static void resync_request(void)
{
    if (m_ble_mode)
    {
        m_resync_wanted = true;
    }
}

static void haptic_buzz(void)
{
    /* ID_TRIGGER_HAPTIC_PULSE: side, on us, off us, repeat count, gain.
     * 150 pulses of 2 ms on, 2 ms off: a buzz of about 0.6 s. */
    static const uint8_t buzz[] =
    {
        VALVE_OP_FEATURE, VALVE_CMD_TRIGGER_HAPTIC_PULSE, 8u,
        VALVE_HAPTIC_SIDE_BOTH,
        0xD0, 0x07,     /* 2000 us on  */
        0xD0, 0x07,     /* 2000 us off */
        0x96, 0x00,     /* 150 pulses  */
        0x00,           /* gain 0 dB   */
    };

    lpc_send(buzz, sizeof(buzz));
}

static bool body_is(const uint8_t * p_body, uint8_t len, const char * p_text)
{
    uint8_t n = (uint8_t)strlen(p_text);
    return (len >= (uint8_t)(1u + n)) && (memcmp(&p_body[1], p_text, n) == 0);
}

/** Handle one frame from the LPC. Main loop only. */
static void lpc_frame_handle(const uint8_t * p_body, uint8_t len)
{
    switch (p_body[0])
    {
        case VALVE_OP_STATE:
        {
            if (!m_ble_mode)
            {
                /*
                 * Only BLE mode sends a 13-byte payload starting 0x01 (lizard
                 * mode's keyboard + mouse); dongle mode sends 16 bytes of the
                 * USB report plus keyboard and mouse, and over USB nothing
                 * comes at all. So this payload is proof of BLE mode even if
                 * the strap was misread, and the stream can be trusted.
                 */
                if ((len == 14u) && (p_body[1] == VALVE_REC_TYPE_LIZARD))
                {
                    ble_mode_set(true);
                    m_diag.mode_strap |= DIAG_MODE_DETECTED;
                }
                else
                {
                    /* Decoding dongle frames as BLE records would be garbage. */
                    m_diag.other_ops++;
                    m_diag.last_other_op = p_body[0];
                    break;
                }
            }

            uint16_t        flags = 0u;
            uint8_t         extra = 0u;
            sc_rec_result_t res   = sc_state_apply(&m_state, &p_body[1],
                                                   (uint8_t)(len - 1u), &flags, &extra);
            switch (res)
            {
                case SC_REC_OK:
                    m_diag.rec_ok++;
                    m_diag.last_rec_flags = flags;
                    m_diag.last_rec_extra = extra;
                    if (m_resync_zeroed)
                    {
                        /* The first state record since the LPC left lizard mode
                         * is the keyframe, now applied onto a zeroed state. */
                        resync_end();
                        m_resync_attempts = 0u;
                    }
                    break;

                case SC_REC_LIZARD:
                    m_diag.rec_lizard++;
                    m_last_lizard_ms = now_ms();
                    m_lizard_seen    = true;
                    if (m_resync_active)
                    {
                        /* From here the LPC sends no deltas against its old copy
                         * of the state, so zeroing now cannot lose one, and the
                         * next state record carries every non-zero chunk. */
                        if (!m_resync_zeroed)
                        {
                            sc_state_reset(&m_state);
                            m_resync_zeroed = true;
                        }
                    }
                    else
                    {
                        /* The LPC went back to lizard mode on its own. */
                        resync_request();
                    }
                    break;

                default:
                    m_diag.rec_bad++;
                    resync_request();
                    break;
            }
            break;
        }

        case VALVE_OP_HOST_MSG:
            m_diag.host_msgs++;
            m_diag.last_host_msg_len = len;
            memcpy(m_diag.last_host_msg, p_body,
                   MIN(len, sizeof(m_diag.last_host_msg)));
            break;

        case VALVE_OP_FEATURE:
            m_diag.feature_replies++;
            memcpy(m_diag.last_feature_reply, p_body,
                   MIN(len, sizeof(m_diag.last_feature_reply)));
            break;

        case VALVE_OP_CONNECTED:
        case VALVE_OP_ENABLE:
            /* The LPC asks the radio to be available. We always are, unless it
             * told us to doze earlier. If a host is already connected, say so
             * again, as Valve's radio does. */
            m_dozing = false;
            if (m_conn_handle != BLE_CONN_HANDLE_INVALID)
            {
                lpc_send_op(VALVE_OP_CONNECTED);
            }
            else if (!m_advertising)
            {
                advertising_start();
            }
            break;

        case VALVE_OP_DISCONNECTED:
            /*
             * "Stop" means drop the host, not stop advertising: Valve's radio
             * disconnects here (0x18324), and its disconnect handler (0x191B8)
             * reports 0x53 and starts advertising again. Going quiet is what
             * "doze" is for. So re-advertising after this is intended.
             */
            if (m_conn_handle != BLE_CONN_HANDLE_INVALID)
            {
                (void)sd_ble_gap_disconnect(m_conn_handle,
                                            BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            }
            break;

        case VALVE_OP_VERSION:
            /* The LPC reports this word as the radio firmware revision (at
             * offset 28 of its 0x83 attribute reply), so the flash tool shows
             * it as a date: the build time proves which image is running. */
            version_announce();
            m_diag.lpc_queries++;
            break;

        case VALVE_OP_DEVICE_ID:
        {
            uint8_t reply[9];
            reply[0] = VALVE_OP_DEVICE_ID;
            (void)uint32_encode(NRF_FICR->DEVICEID[0], &reply[1]);
            (void)uint32_encode(NRF_FICR->DEVICEID[1], &reply[5]);
            lpc_send(reply, sizeof(reply));
            m_diag.lpc_queries++;
            break;
        }

        case VALVE_OP_RESET:
            if (body_is(p_body, len, "RESET"))
            {
                NVIC_SystemReset();
            }
            break;

        case VALVE_OP_DOZE:
            if (body_is(p_body, len, "doze"))
            {
                /* Valve's radio powers down here until the LPC resets it. We go
                 * quiet instead -- no link, no advertising -- which the LPC
                 * cannot tell apart, and wake on its next 0x52/0x54 or reset. */
                m_dozing = true;
                if (m_conn_handle != BLE_CONN_HANDLE_INVALID)
                {
                    (void)sd_ble_gap_disconnect(m_conn_handle,
                                                BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
                }
                else if (m_advertising)
                {
                    (void)sd_ble_gap_adv_stop();
                    m_advertising = false;
                }
            }
            break;

        case VALVE_OP_WIPE:
            if (body_is(p_body, len, "wipe"))
            {
                static const uint8_t ack[] = { VALVE_OP_WIPE, 0x01 };
                lpc_send(ack, sizeof(ack));
                m_wipe_pending = true;
            }
            break;

        default:
            /* 0x5B dongle pairing, 0x5D bond push and anything unknown. */
            m_diag.other_ops++;
            m_diag.last_other_op = p_body[0];
            break;
    }
}

/* ---- HID reporting ------------------------------------------------------- */

/** True if the host has notifications on for the gamepad input report. */
static bool hid_cccd_enabled(void)
{
    return cccd_notify_enabled(m_conn_handle,
                               m_hids.inp_rep_array[INPUT_REP_GAMEPAD_INDEX].char_handles.cccd_handle);
}

/** Send the current state if the host is listening and it changed. */
static void gamepad_report_update(void)
{
    uint8_t report[GAMEPAD_INPUT_REP_LEN];

    /* Encrypted only: a bonded host's CCCDs can be restored (on
     * SYS_ATTR_MISSING) before it has re-encrypted the link, and input must
     * not go out in the clear. The HID characteristics require encryption to
     * access, so a real host always encrypts first. */
    if ((m_conn_handle == BLE_CONN_HANDLE_INVALID) || !m_link_encrypted ||
        m_resync_zeroed || m_host_suspended)
    {
        return;
    }

    if (!m_hid_reporting)
    {
        if (!hid_cccd_enabled())
        {
            return;
        }
        m_hid_reporting     = true;
        m_last_report_valid = false;
    }

    gamepad_pack_report(&m_state, report);

    if (m_last_report_valid && (memcmp(report, m_last_report, sizeof(report)) == 0))
    {
        return;
    }

    if (!in_flight_reserve())
    {
        /* Two reports already queued; the next TX_COMPLETE brings us back
         * here with the freshest state. */
        return;
    }

    uint32_t err_code = ble_hids_inp_rep_send(&m_hids, INPUT_REP_GAMEPAD_INDEX,
                                              GAMEPAD_INPUT_REP_LEN, report);
    if (err_code == NRF_SUCCESS)
    {
        memcpy(m_last_report, report, sizeof(report));
        m_last_report_valid = true;
        m_diag.reports_sent++;
    }
    else
    {
        /* Out of buffers, link not ready, connection just dropped: all
         * transient. m_last_report is left alone, so the next pass retries
         * with whatever the state is by then. */
        in_flight_release();
        m_diag.report_errors++;
    }
}

/* ---- Diagnostics --------------------------------------------------------- */

static uint8_t diag_flags(void)
{
    uint8_t flags = 0u;

    if (m_conn_handle != BLE_CONN_HANDLE_INVALID) flags |= DIAG_F_CONNECTED;
    if (m_link_encrypted)                         flags |= DIAG_F_ENCRYPTED;
    if (m_hid_reporting)                          flags |= DIAG_F_HID_REPORTING;
    if (m_peer_bonded)                            flags |= DIAG_F_BONDED_PEER;
    if (m_dm_ok)                                  flags |= DIAG_F_BONDING_OK;
    if (m_lizard_seen)                            flags |= DIAG_F_LIZARD;
    if (m_advertising)                            flags |= DIAG_F_ADVERTISING;
    if (m_dozing)                                 flags |= DIAG_F_DOZING;

    return flags;
}

static void diag_refresh(void)
{
    uart_link_stats_t stats;

    uart_link_stats_get(&stats);

    /* Any new receive-side loss means a delta may have gone missing. Oversize
     * frames do not count: no state record comes close to VFRAME_MAX_BODY, so
     * a frame that long (the LPC's 128-byte bond push, say) was not one. */
    uint32_t errors = (uint32_t)stats.rx_uart_errors + stats.rx_dropped + stats.rx_queue_full;
    if (errors != m_last_error_total)
    {
        m_last_error_total = errors;
        if (m_resync_ever)
        {
            resync_request();
        }
    }

    m_diag.flags          = diag_flags();
    m_diag.uptime_ms      = m_uptime_ms;
    m_diag.rx_bytes       = stats.rx_bytes;
    m_diag.rx_frames      = stats.rx_frames;
    m_diag.rx_uart_errors = stats.rx_uart_errors;
    m_diag.rx_dropped     = stats.rx_dropped;
    m_diag.rx_queue_full  = stats.rx_queue_full;
    m_diag.tx_full        = stats.tx_full;
    m_diag.rx_oversize    = stats.rx_oversize;
    m_diag.rx_starts      = stats.rx_starts;
    m_diag.rx_ends        = stats.rx_ends;
    m_diag.rx_errorsrc    = stats.rx_errorsrc;
    STATIC_ASSERT(sizeof(m_diag.rx_first) == sizeof(stats.rx_first));
    memcpy(m_diag.rx_first, stats.rx_first, sizeof(m_diag.rx_first));
    memcpy(m_diag.rx_last, stats.rx_last, sizeof(m_diag.rx_last));
}

static void diag_live_update(void)
{
    if (!elapsed(m_last_live_ms, LIVE_INTERVAL_MS) || !diag_live_enabled(m_conn_handle))
    {
        return;
    }

    m_last_live_ms = now_ms();

    m_live.seq++;
    m_live.flags      = diag_flags();
    m_live.buttons[0] = (uint8_t)m_state.buttons;
    m_live.buttons[1] = (uint8_t)(m_state.buttons >> 8);
    m_live.buttons[2] = (uint8_t)(m_state.buttons >> 16);
    m_live.ltrig      = m_state.ltrig;
    m_live.rtrig      = m_state.rtrig;
    m_live.lstick_x   = m_state.lstick_x;
    m_live.lstick_y   = m_state.lstick_y;
    m_live.lpad_x     = m_state.lpad_x;
    m_live.lpad_y     = m_state.lpad_y;
    m_live.rpad_x     = m_state.rpad_x;
    m_live.rpad_y     = m_state.rpad_y;
    m_live.rec_count  = (uint8_t)m_diag.rec_ok;

    if (!in_flight_reserve())
    {
        return;
    }
    if (!diag_live_notify(m_conn_handle, &m_live))
    {
        in_flight_release();
    }
}

/** Control characteristic write. SoftDevice event context: post, don't act. */
static void diag_cmd_handler(uint8_t cmd)
{
    switch (cmd)
    {
        case DIAG_CMD_RESYNC:
            /* Only in BLE mode (resync_begin checks): anyone in range can
             * write this, and outside BLE mode turning lizard mode off would
             * take a USB user's mouse emulation away. */
            action_post(ACT_RESYNC);
            break;
        case DIAG_CMD_HAPTIC: action_post(ACT_HAPTIC); break;
        default:                                       break;
    }
}

/* ---- Bond storage -------------------------------------------------------- */

static uint32_t device_manager_evt_handler(dm_handle_t const * p_handle,
                                           dm_event_t const *  p_event,
                                           ret_code_t          event_result)
{
    /* Security failures and storage hiccups are the host's or the flash's
     * problem, not a reason to reset the radio. They show up as a host that
     * does not stay paired, which the diagnostics make visible. */
    UNUSED_PARAMETER(event_result);

    switch (p_event->event_id)
    {
        case DM_EVT_CONNECTION:
            m_peer_handle = *p_handle;
            break;

        case DM_EVT_DEVICE_CONTEXT_LOADED:
            /* A host we have a bond with reconnected. */
            m_peer_handle = *p_handle;
            m_peer_bonded = true;
            break;

        case DM_EVT_SECURITY_SETUP_COMPLETE:
            m_peer_handle = *p_handle;
            m_peer_bonded = (event_result == NRF_SUCCESS);
            break;

        case DM_EVT_DISCONNECTION:
            m_peer_bonded = false;
            break;

        default:
            break;
    }

    return NRF_SUCCESS;
}

static void device_manager_init(void)
{
    uint32_t               err_code;
    dm_init_param_t        init_param = { .clear_persistent_data = false };
    dm_application_param_t register_param;

    m_dm_ok = false;

    (void)dm_handle_initialize(&m_peer_handle);

    err_code = pstorage_init();
    if (err_code != NRF_SUCCESS)
    {
        return;
    }

    err_code = dm_init(&init_param);
    if (err_code != NRF_SUCCESS)
    {
        return;
    }

    memset(&register_param, 0, sizeof(register_param));
    register_param.sec_param.bond         = SEC_PARAM_BOND;
    register_param.sec_param.mitm         = SEC_PARAM_MITM;
    register_param.sec_param.io_caps      = SEC_PARAM_IO_CAPS;
    register_param.sec_param.oob          = SEC_PARAM_OOB;
    register_param.sec_param.min_key_size = SEC_PARAM_MIN_KEY_SIZE;
    register_param.sec_param.max_key_size = SEC_PARAM_MAX_KEY_SIZE;
    register_param.evt_handler            = device_manager_evt_handler;
    register_param.service_type           = DM_PROTOCOL_CNTXT_GATT_SRVR_ID;

    err_code = dm_register(&m_app_handle, &register_param);
    m_dm_ok  = (err_code == NRF_SUCCESS);
}

/** Store the host's CCCD settings with its bond, so a reconnecting host does
 *  not have to re-enable notifications. */
static void bond_store_cccds(void)
{
    dm_service_context_t service_context;

    if (!m_dm_ok)
    {
        return;
    }

    service_context.service_type        = DM_PROTOCOL_CNTXT_GATT_SRVR_ID;
    service_context.context_data.len    = 0;
    service_context.context_data.p_data = NULL;

    /* NRF_ERROR_INVALID_STATE means the host is not newly bonded; the device
     * manager then stores the CCCDs itself on disconnect. */
    (void)dm_service_context_set(&m_peer_handle, &service_context);
}

/** Forget every host once no host is connected, wait for flash, then reset. */
static void wipe_bonds_run(void)
{
    if (!m_wipe_pending)
    {
        return;
    }

    if (m_conn_handle != BLE_CONN_HANDLE_INVALID)
    {
        (void)sd_ble_gap_disconnect(m_conn_handle,
                                    BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        return;
    }

    if (!m_wipe_started)
    {
        m_wipe_started = true;
        if (m_dm_ok)
        {
            (void)dm_device_delete_all(&m_app_handle);
        }
        return;
    }

    /* Reset only once the flash work is done and the acknowledgement queued
     * for the LPC has actually left the UART. */
    uint32_t pending = 0u;
    bool     flash_busy = (pstorage_access_status_get(&pending) == NRF_SUCCESS) && (pending != 0u);
    bool     tx_busy    = (m_lpc_txq_tail != m_lpc_txq_head) || !uart_link_tx_idle();

    if (!flash_busy && !tx_busy)
    {
        NVIC_SystemReset();
    }
}

/* ---- HID service --------------------------------------------------------- */

static void on_hids_evt(ble_hids_t * p_hids, ble_hids_evt_t * p_evt)
{
    switch (p_evt->evt_type)
    {
        case BLE_HIDS_EVT_NOTIF_ENABLED:
            m_hid_reporting     = true;
            /* Force one report so the host starts from a valid neutral state
             * rather than waiting for the first stick movement. */
            m_last_report_valid = false;
            bond_store_cccds();
            break;

        case BLE_HIDS_EVT_NOTIF_DISABLED:
            m_hid_reporting = false;
            break;

        case BLE_HIDS_EVT_HOST_SUSP:
            m_host_suspended = true;
            break;

        case BLE_HIDS_EVT_HOST_EXIT_SUSP:
            m_host_suspended    = false;
            m_last_report_valid = false;
            break;

        case BLE_HIDS_EVT_REPORT_MODE_ENTERED:
            m_last_report_valid = false;
            break;

        case BLE_HIDS_EVT_BOOT_MODE_ENTERED:
        {
            /*
             * A host asked for boot protocol mode. We publish no boot report
             * characteristics, so there is nothing there for it to read.
             *
             * The characteristic is written without response and with wr_auth
             * clear, so the SoftDevice has already committed the host's 0x00 to
             * the attribute table by the time this event arrives; there was no
             * opportunity to reject it. Left alone, a host that reads Protocol
             * Mode back would see Boot mode and then wait for boot reports that
             * never come. So put the attribute back to Report mode, which is the
             * only mode this service implements.
             */
            uint8_t           report_mode = HID_PROTOCOL_MODE_REPORT;
            ble_gatts_value_t gatts_value;

            memset(&gatts_value, 0, sizeof(gatts_value));
            gatts_value.len     = sizeof(report_mode);
            gatts_value.offset  = 0;
            gatts_value.p_value = &report_mode;

            (void)sd_ble_gatts_value_set(m_conn_handle,
                                         p_hids->protocol_mode_handles.value_handle,
                                         &gatts_value);
            break;
        }

        default:
            break;
    }
}

/* ---- GAP, services, advertising ----------------------------------------- */

static void gap_params_init(void)
{
    uint32_t                err_code;
    ble_gap_conn_params_t   gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

    err_code = sd_ble_gap_device_name_set(&sec_mode,
                                          (const uint8_t *)DEVICE_NAME,
                                          strlen(DEVICE_NAME));
    APP_ERROR_CHECK(err_code);

    /* The appearance value is what makes a host draw a gamepad icon and, on
     * some stacks, pick the game-controller input pipeline. */
    err_code = sd_ble_gap_appearance_set(BLE_APPEARANCE_HID_GAMEPAD);
    APP_ERROR_CHECK(err_code);

    memset(&gap_conn_params, 0, sizeof(gap_conn_params));
    gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;
    gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;
    gap_conn_params.slave_latency     = SLAVE_LATENCY;
    gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;

    err_code = sd_ble_gap_ppcp_set(&gap_conn_params);
    APP_ERROR_CHECK(err_code);
}

static void advertising_init(void)
{
    uint32_t      err_code;
    ble_advdata_t advdata;
    ble_advdata_t scanrsp;
    ble_uuid_t    adv_uuids[] = { { BLE_UUID_HUMAN_INTERFACE_DEVICE_SERVICE,
                                    BLE_UUID_TYPE_BLE } };

    /*
     * Flags (3) + appearance (4) + HID UUID (4) + name (2 + 16) = 29 of the 31
     * bytes available, which is why the diagnostics UUID goes in the scan
     * response instead. A longer DEVICE_NAME would overflow this and make
     * ble_advdata_set fail at boot.
     */
    memset(&advdata, 0, sizeof(advdata));
    advdata.name_type               = BLE_ADVDATA_FULL_NAME;
    advdata.include_appearance      = true;
    advdata.flags                   = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
    advdata.uuids_complete.uuid_cnt = sizeof(adv_uuids) / sizeof(adv_uuids[0]);
    advdata.uuids_complete.p_uuids  = adv_uuids;

    /* The diagnostics service UUID, so the web test page can filter on it. The
     * vendor UUID type is the one diag_init registered, which is always the
     * first vendor-specific type: BLE_UUID_TYPE_VENDOR_BEGIN. */
    ble_uuid_t diag_uuid = { DIAG_UUID_SERVICE, BLE_UUID_TYPE_VENDOR_BEGIN };
    memset(&scanrsp, 0, sizeof(scanrsp));
    scanrsp.uuids_complete.uuid_cnt = 1;
    scanrsp.uuids_complete.p_uuids  = &diag_uuid;

    err_code = ble_advdata_set(&advdata, &scanrsp);
    if (err_code != NRF_SUCCESS)
    {
        /* Without the scan response the controller still works; only the web
         * page's filter loses a hint. Fall back to advertising data alone. */
        err_code = ble_advdata_set(&advdata, NULL);
        APP_ERROR_CHECK(err_code);
    }
}

static void advertising_start(void)
{
    uint32_t             err_code;
    ble_gap_adv_params_t adv_params;

    if (m_dozing || (m_conn_handle != BLE_CONN_HANDLE_INVALID))
    {
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.type        = BLE_GAP_ADV_TYPE_ADV_IND;
    adv_params.p_peer_addr = NULL;
    adv_params.fp          = BLE_GAP_ADV_FP_ANY;
    adv_params.interval    = APP_ADV_INTERVAL;
    adv_params.timeout     = APP_ADV_TIMEOUT_S;

    err_code = sd_ble_gap_adv_start(&adv_params);
    if (err_code == NRF_SUCCESS)
    {
        m_advertising = true;
    }
    else if (err_code != NRF_ERROR_INVALID_STATE)
    {
        APP_ERROR_HANDLER(err_code);
    }
}

static void dis_init(void)
{
    uint32_t         err_code;
    ble_dis_init_t   dis_init_obj;
    ble_dis_pnp_id_t pnp_id;

    memset(&dis_init_obj, 0, sizeof(dis_init_obj));
    ble_srv_ascii_to_utf8(&dis_init_obj.manufact_name_str, MANUFACTURER_NAME);
    ble_srv_ascii_to_utf8(&dis_init_obj.model_num_str,     MODEL_NUMBER);
    ble_srv_ascii_to_utf8(&dis_init_obj.fw_rev_str,        FIRMWARE_REVISION);

    pnp_id.vendor_id_source = PNP_ID_VENDOR_ID_SOURCE;
    pnp_id.vendor_id        = PNP_ID_VENDOR_ID;
    pnp_id.product_id       = PNP_ID_PRODUCT_ID;
    pnp_id.product_version  = PNP_ID_PRODUCT_VERSION;
    dis_init_obj.p_pnp_id   = &pnp_id;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&dis_init_obj.dis_attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&dis_init_obj.dis_attr_md.write_perm);

    err_code = ble_dis_init(&dis_init_obj);
    APP_ERROR_CHECK(err_code);
}

/*
 * The HID over GATT profile requires a Battery Service. The level is fixed at
 * 100 for now: Valve's LPC does report battery state, most likely in its 0x55
 * status messages, but that layout is not decoded yet (docs/VALVE_LINK.md).
 */
static void bas_init(void)
{
    uint32_t       err_code;
    ble_bas_init_t bas_init_obj;

    memset(&bas_init_obj, 0, sizeof(bas_init_obj));
    bas_init_obj.evt_handler          = NULL;
    bas_init_obj.support_notification = true;
    bas_init_obj.p_report_ref         = NULL;
    bas_init_obj.initial_batt_level   = 100;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&bas_init_obj.battery_level_char_attr_md.cccd_write_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&bas_init_obj.battery_level_char_attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&bas_init_obj.battery_level_char_attr_md.write_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&bas_init_obj.battery_level_report_read_perm);

    err_code = ble_bas_init(&m_bas, &bas_init_obj);
    APP_ERROR_CHECK(err_code);
}

/**
 * @brief Add the Protocol Mode characteristic to the HID service.
 *
 * ble_hids_init only adds Protocol Mode when the device declares keyboard or
 * mouse boot mode, and a gamepad declares neither, so it has to be added here
 * instead. The HID Service specification lists it as mandatory, and hosts do
 * read it when deciding how to drive a HID-over-GATT device.
 *
 * The metadata mirrors the SDK's own protocol_mode_char_add exactly, and the
 * handles are stored in the ble_hids_t the same way the SDK stores them, which
 * is what makes ble_hids treat the characteristic as its own: it resets the
 * value to Report mode on every connection and routes writes to on_hids_evt.
 *
 * @note The SoftDevice can only append a characteristic to the most recently
 *       added service, so this must run directly after ble_hids_init.
 */
static uint32_t protocol_mode_char_add(ble_hids_t *                    p_hids,
                                       const ble_srv_security_mode_t * p_sec_mode)
{
    ble_gatts_char_md_t char_md;
    ble_gatts_attr_md_t attr_md;
    ble_gatts_attr_t    attr_char_value;
    ble_uuid_t          ble_uuid;
    uint8_t             initial_protocol_mode;

    memset(&char_md, 0, sizeof(char_md));
    char_md.char_props.read          = 1;
    char_md.char_props.write_wo_resp = 1;

    BLE_UUID_BLE_ASSIGN(ble_uuid, BLE_UUID_PROTOCOL_MODE_CHAR);

    memset(&attr_md, 0, sizeof(attr_md));
    attr_md.read_perm  = p_sec_mode->read_perm;
    attr_md.write_perm = p_sec_mode->write_perm;
    attr_md.vloc       = BLE_GATTS_VLOC_STACK;
    attr_md.rd_auth    = 0;
    attr_md.wr_auth    = 0;
    attr_md.vlen       = 0;

    initial_protocol_mode = HID_PROTOCOL_MODE_REPORT;

    memset(&attr_char_value, 0, sizeof(attr_char_value));
    attr_char_value.p_uuid    = &ble_uuid;
    attr_char_value.p_attr_md = &attr_md;
    attr_char_value.init_len  = sizeof(uint8_t);
    attr_char_value.init_offs = 0;
    attr_char_value.max_len   = sizeof(uint8_t);
    attr_char_value.p_value   = &initial_protocol_mode;

    return sd_ble_gatts_characteristic_add(p_hids->service_handle,
                                           &char_md,
                                           &attr_char_value,
                                           &p_hids->protocol_mode_handles);
}

static void hids_init(void)
{
    uint32_t                err_code;
    ble_hids_init_t         hids_init_obj;
    ble_hids_inp_rep_init_t inp_rep_array[INPUT_REPORT_COUNT];

    memset(inp_rep_array, 0, sizeof(inp_rep_array));

    inp_rep_array[INPUT_REP_GAMEPAD_INDEX].max_len             = GAMEPAD_INPUT_REP_LEN;
    inp_rep_array[INPUT_REP_GAMEPAD_INDEX].rep_ref.report_id   = GAMEPAD_INPUT_REP_ID;
    inp_rep_array[INPUT_REP_GAMEPAD_INDEX].rep_ref.report_type = BLE_HIDS_REP_TYPE_INPUT;
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(
        &inp_rep_array[INPUT_REP_GAMEPAD_INDEX].security_mode.cccd_write_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(
        &inp_rep_array[INPUT_REP_GAMEPAD_INDEX].security_mode.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(
        &inp_rep_array[INPUT_REP_GAMEPAD_INDEX].security_mode.write_perm);

    memset(&hids_init_obj, 0, sizeof(hids_init_obj));
    hids_init_obj.evt_handler                    = on_hids_evt;
    hids_init_obj.error_handler                  = service_error_handler;
    /*
     * Boot protocol mode only defines keyboard and mouse reports, so a gamepad
     * declares neither. Setting is_mouse here would create a Boot Mouse Input
     * Report characteristic and reintroduce exactly the mouse identity this
     * firmware exists to remove.
     */
    hids_init_obj.is_kb                          = false;
    hids_init_obj.is_mouse                       = false;
    hids_init_obj.inp_rep_count                  = INPUT_REPORT_COUNT;
    hids_init_obj.p_inp_rep_array                = inp_rep_array;
    hids_init_obj.outp_rep_count                 = 0;
    hids_init_obj.p_outp_rep_array               = NULL;
    hids_init_obj.feature_rep_count              = 0;
    hids_init_obj.p_feature_rep_array            = NULL;
    hids_init_obj.rep_map.data_len               = gamepad_report_map_size;
    hids_init_obj.rep_map.p_data                 = (uint8_t *)gamepad_report_map;
    hids_init_obj.hid_information.bcd_hid        = 0x0101;
    hids_init_obj.hid_information.b_country_code = 0;
    hids_init_obj.hid_information.flags          = HID_INFO_FLAG_REMOTE_WAKE_MSK |
                                                   HID_INFO_FLAG_NORMALLY_CONNECTABLE_MSK;
    hids_init_obj.included_services_count        = 0;
    hids_init_obj.p_included_services_array      = NULL;

    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&hids_init_obj.rep_map.security_mode.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&hids_init_obj.rep_map.security_mode.write_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&hids_init_obj.hid_information.security_mode.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&hids_init_obj.hid_information.security_mode.write_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&hids_init_obj.security_mode_protocol.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&hids_init_obj.security_mode_protocol.write_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&hids_init_obj.security_mode_ctrl_point.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&hids_init_obj.security_mode_ctrl_point.write_perm);

    err_code = ble_hids_init(&m_hids, &hids_init_obj);
    APP_ERROR_CHECK(err_code);

    /*
     * Must follow ble_hids_init immediately: the SoftDevice appends to the last
     * added service. Deliberately not fatal: Protocol Mode is an interop nicety,
     * and a reset loop over it would be strictly worse than going without.
     */
    (void)protocol_mode_char_add(&m_hids, &hids_init_obj.security_mode_protocol);
}

static void on_conn_params_evt(ble_conn_params_evt_t * p_evt)
{
    /*
     * A host that will not grant our preferred interval keeps its own. That is
     * slower, not broken, and dropping the link over it would leave a TV user
     * with a controller that connects and immediately disconnects forever.
     */
    UNUSED_PARAMETER(p_evt);
}

static void conn_params_init(void)
{
    uint32_t               err_code;
    ble_conn_params_init_t cp_init;

    memset(&cp_init, 0, sizeof(cp_init));
    cp_init.p_conn_params                  = NULL;
    cp_init.first_conn_params_update_delay = FIRST_CONN_PARAMS_UPDATE_DELAY;
    cp_init.next_conn_params_update_delay  = NEXT_CONN_PARAMS_UPDATE_DELAY;
    cp_init.max_conn_params_update_count   = MAX_CONN_PARAMS_UPDATE_COUNT;
    cp_init.start_on_notify_cccd_handle    = BLE_GATT_HANDLE_INVALID;
    cp_init.disconnect_on_fail             = false;
    cp_init.evt_handler                    = on_conn_params_evt;
    cp_init.error_handler                  = conn_params_error_handler;

    err_code = ble_conn_params_init(&cp_init);
    APP_ERROR_CHECK(err_code);
}

/* ---- BLE event handling ------------------------------------------------- */

/** Security handling for when bond storage failed to initialise: pair without
 *  bonding, as the firmware did before bonding existed, rather than not at all. */
static void on_ble_evt_no_bonding(ble_evt_t * p_ble_evt)
{
    static ble_gap_enc_key_t    enc_key;
    static ble_gap_sec_keyset_t keyset;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
        {
            ble_gap_sec_params_t sec_params;

            memset(&sec_params, 0, sizeof(sec_params));
            sec_params.bond         = 0;
            sec_params.mitm         = SEC_PARAM_MITM;
            sec_params.io_caps      = SEC_PARAM_IO_CAPS;
            sec_params.oob          = SEC_PARAM_OOB;
            sec_params.min_key_size = SEC_PARAM_MIN_KEY_SIZE;
            sec_params.max_key_size = SEC_PARAM_MAX_KEY_SIZE;

            memset(&enc_key, 0, sizeof(enc_key));
            memset(&keyset,  0, sizeof(keyset));
            keyset.keys_periph.p_enc_key = &enc_key;

            (void)sd_ble_gap_sec_params_reply(m_conn_handle, BLE_GAP_SEC_STATUS_SUCCESS,
                                              &sec_params, &keyset);
            break;
        }

        case BLE_GAP_EVT_SEC_INFO_REQUEST:
            (void)sd_ble_gap_sec_info_reply(m_conn_handle, NULL, NULL, NULL);
            break;

        case BLE_GATTS_EVT_SYS_ATTR_MISSING:
            (void)sd_ble_gatts_sys_attr_set(m_conn_handle, NULL, 0, 0);
            break;

        default:
            break;
    }
}

static void on_ble_evt(ble_evt_t * p_ble_evt)
{
    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_CONNECTED:
            m_conn_handle       = p_ble_evt->evt.gap_evt.conn_handle;
            m_host_suspended    = false;
            m_advertising       = false;
            m_link_encrypted    = false;
            m_last_report_valid = false;
            m_reports_in_flight = 0u;
            action_post(ACT_SEND_CONNECTED);
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            m_conn_handle       = BLE_CONN_HANDLE_INVALID;
            m_hid_reporting     = false;
            m_link_encrypted    = false;
            m_last_report_valid = false;
            m_reports_in_flight = 0u;
            action_post(ACT_SEND_DISCONNECTED | ACT_ADVERTISE);
            break;

        case BLE_GAP_EVT_CONN_SEC_UPDATE:
            m_link_encrypted =
                (p_ble_evt->evt.gap_evt.params.conn_sec_update.conn_sec.sec_mode.sm == 1) &&
                (p_ble_evt->evt.gap_evt.params.conn_sec_update.conn_sec.sec_mode.lv >= 2);
            break;

        case BLE_EVT_TX_COMPLETE:
        {
            /* Runs at the same priority as every other writer except the main
             * loop, whose writes are in critical regions: no lock needed here. */
            uint8_t done = p_ble_evt->evt.common_evt.params.tx_complete.count;
            m_reports_in_flight = (done >= m_reports_in_flight) ?
                                  0u : (uint8_t)(m_reports_in_flight - done);
            m_last_tx_complete_ms = m_uptime_ms;
            break;
        }

        case BLE_GAP_EVT_TIMEOUT:
            if (p_ble_evt->evt.gap_evt.params.timeout.src == BLE_GAP_TIMEOUT_SRC_ADVERTISING)
            {
                m_advertising = false;
                action_post(ACT_ADVERTISE);
            }
            break;

        default:
            break;
    }

    if (!m_dm_ok)
    {
        on_ble_evt_no_bonding(p_ble_evt);
    }
}

static void ble_evt_dispatch(ble_evt_t * p_ble_evt)
{
    action_post(ACT_EVENT);
    if (m_dm_ok)
    {
        dm_ble_evt_handler(p_ble_evt);
    }
    ble_conn_params_on_ble_evt(p_ble_evt);
    ble_hids_on_ble_evt(&m_hids, p_ble_evt);
    ble_bas_on_ble_evt(&m_bas, p_ble_evt);
    diag_on_ble_evt(p_ble_evt);
    on_ble_evt(p_ble_evt);
}

static void sys_evt_dispatch(uint32_t sys_evt)
{
    action_post(ACT_EVENT);
    pstorage_sys_event_handler(sys_evt);
}

static void ble_stack_init(void)
{
    uint32_t err_code;

    /*
     * The Steam Controller board has no 32.768 kHz crystal: Valve's own radio
     * firmware calls softdevice_handler_init with clock source 17, the RC
     * oscillator with temperature-triggered calibration every 4 s (see
     * docs/FINDINGS.md). Asking for a crystal here would hang at startup.
     */
    SOFTDEVICE_HANDLER_INIT(NRF_CLOCK_LFCLKSRC_RC_250_PPM_TEMP_4000MS_CALIBRATION, NULL);

    ble_enable_params_t ble_enable_params;
    memset(&ble_enable_params, 0, sizeof(ble_enable_params));
    ble_enable_params.gatts_enable_params.service_changed = 0;

    err_code = sd_ble_enable(&ble_enable_params);
    APP_ERROR_CHECK(err_code);

    err_code = softdevice_ble_evt_handler_set(ble_evt_dispatch);
    APP_ERROR_CHECK(err_code);

    err_code = softdevice_sys_evt_handler_set(sys_evt_dispatch);
    APP_ERROR_CHECK(err_code);

    /*
     * Keep the 16 MHz crystal running. Otherwise the SoftDevice starts it only
     * for radio events and the UART runs from the internal RC oscillator in
     * between, whose tolerance is a real fraction of the timing margin at
     * 921600 baud, on top of the LPC's own -0.53%. Valve's radio lives with
     * that; a fraction of a milliamp buys it back. Not fatal if refused.
     */
    (void)sd_clock_hfclk_request();
}

/* ---- Timers -------------------------------------------------------------- */

static void tick_timeout_handler(void * p_context)
{
    UNUSED_PARAMETER(p_context);
    m_uptime_ms += TICK_INTERVAL_MS;
    action_post(ACT_TICK);
}

static void tx_gap_timeout_handler(void * p_context)
{
    UNUSED_PARAMETER(p_context);
    action_post(ACT_TX_GAP);
}

static void timers_init(void)
{
    uint32_t err_code;

    APP_TIMER_INIT(APP_TIMER_PRESCALER, APP_TIMER_OP_QUEUE_SIZE, NULL);

    err_code = app_timer_create(&m_tick_timer, APP_TIMER_MODE_REPEATED,
                                tick_timeout_handler);
    APP_ERROR_CHECK(err_code);

    err_code = app_timer_create(&m_tx_gap_timer, APP_TIMER_MODE_SINGLE_SHOT,
                                tx_gap_timeout_handler);
    APP_ERROR_CHECK(err_code);
}

/* ---- Main loop ----------------------------------------------------------- */

static void tick(void)
{
    /* The LPC only sends when pulled; this restarts a stalled stream. */
    uart_link_pull();

    if (m_lizard_seen && elapsed(m_last_lizard_ms, 1000u))
    {
        m_lizard_seen = false;
    }

    if (m_ble_mode && !m_resync_ever && (m_uptime_ms >= RESYNC_BOOT_DELAY_MS))
    {
        m_resync_wanted = true;
    }

    if (m_resync_wanted && !m_resync_active &&
        (!m_resync_ever || elapsed(m_last_resync_ms, RESYNC_MIN_SPACING_MS)))
    {
        m_resync_wanted = false;
        resync_begin();
    }

    /* Belt and braces: TX_COMPLETE is never missed as far as the SoftDevice
     * documentation goes, but if the count were ever left high, reporting would
     * stop for good. A second without progress means nothing is in flight. */
    /* Measured from the last activity either way, not just the last
     * completion: after an idle spell the last completion is old, and a slot
     * reserved a moment ago must not be cleared as if it were stuck. */
    if ((m_reports_in_flight != 0u) && elapsed(m_last_tx_complete_ms, 1000u) &&
        elapsed(m_last_in_flight_ms, 1000u))
    {
        CRITICAL_REGION_ENTER();
        m_reports_in_flight   = 0u;
        m_last_tx_complete_ms = m_uptime_ms;
        CRITICAL_REGION_EXIT();
    }

    if (m_resync_active && elapsed(m_resync_started_ms, RESYNC_SETTLE_MS))
    {
        /* No keyframe came: report what we have rather than freeze, and try
         * again later -- unless the LPC has now ignored the sequence several
         * times running (never even went through lizard mode), in which case
         * stop pestering it until an error or a lizard payload asks again. */
        bool ignored = !m_resync_zeroed;
        resync_end();
        if (ignored && (m_resync_attempts >= RESYNC_MAX_ATTEMPTS))
        {
            m_resync_attempts = 0u;
        }
        else
        {
            m_resync_wanted = true;
        }
    }
}

static void actions_run(uint32_t act)
{
    if (act & ACT_TICK)              tick();
    if (act & ACT_SEND_CONNECTED)    lpc_send_op(VALVE_OP_CONNECTED);
    if (act & ACT_SEND_DISCONNECTED) lpc_send_op(VALVE_OP_DISCONNECTED);
    if (act & ACT_ADVERTISE)         advertising_start();
    if (act & ACT_TX_GAP)            { m_lpc_tx_gap = false; lpc_tx_pump(); }
    if (act & ACT_RESYNC)            resync_begin();
    if (act & ACT_HAPTIC)            haptic_buzz();
}

/*
 * The mode survives a soft reset in GPREGRET. The LPC drives the strap only
 * around a hardware reset of the radio; after a soft reset (our own
 * NVIC_SystemReset, a fault, the LPC's RESET command) the pin is its idle UART
 * line, which reads high whatever the mode is.
 */
#define GPREGRET_MODE_TAG   0xB0u
#define GPREGRET_MODE_MASK  0xF0u
#define GPREGRET_MODE_BLE   0x01u

/** Set the BLE mode flag, and remember it across a soft reset. Main loop only,
 *  after the SoftDevice is enabled. */
static void ble_mode_set(bool on)
{
    m_ble_mode = on;
    if (on)
    {
        (void)sd_power_gpregret_set(GPREGRET_MODE_BLE);
        m_diag.mode_strap |= DIAG_MODE_BLE;
        resync_request();
    }
}

/** Decide the mode at boot. Runs before the SoftDevice owns POWER. */
static bool boot_mode_decide(bool strap, uint32_t reset_reason)
{
    uint32_t saved     = NRF_POWER->GPREGRET;
    bool     hard_boot = (reset_reason == 0u) ||
                         ((reset_reason & POWER_RESETREAS_RESETPIN_Msk) != 0u);
    bool     mode      = strap;

    if (!hard_boot && ((saved & GPREGRET_MODE_MASK) == GPREGRET_MODE_TAG))
    {
        mode = (saved & GPREGRET_MODE_BLE) != 0u;
        m_diag.mode_strap |= DIAG_MODE_CARRIED;
    }

    NRF_POWER->GPREGRET = GPREGRET_MODE_TAG | (mode ? GPREGRET_MODE_BLE : 0u);
    if (mode)
    {
        m_diag.mode_strap |= DIAG_MODE_BLE;
    }
    return mode;
}

static void boot_record_init(bool mode_strap)
{
    memset(&m_diag, 0, sizeof(m_diag));
    m_diag.version    = DIAG_STATUS_VERSION;
    m_diag.mode_strap = mode_strap ? DIAG_MODE_STRAP : 0u;

    m_diag.reset_reason = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = m_diag.reset_reason;   /* Write-one-to-clear. */

    if (m_fault.magic == FAULT_MAGIC)
    {
        m_diag.fault_code = m_fault.code;
        m_diag.fault_line = m_fault.line;
        for (uint8_t i = 0u; i < sizeof(m_fault.file); i++)
        {
            m_diag.fault_file[i] = m_fault.file[i];
        }
    }
    m_fault.magic = 0u;
}

int main(void)
{
    uint8_t frame[VFRAME_MAX_BODY];
    uint8_t frame_len;

    /* Sample the mode strap first: the LPC only holds it until it starts its
     * own UART, a few milliseconds after releasing us from reset. */
    bool mode_strap = uart_link_read_mode_strap();

    boot_record_init(mode_strap);
    m_ble_mode = boot_mode_decide(mode_strap, m_diag.reset_reason);
    sc_state_reset(&m_state);

    timers_init();
    ble_stack_init();
    device_manager_init();
    gap_params_init();
    dis_init();
    bas_init();
    /* hids_init appends Protocol Mode to the service it just added, so no
     * other service may be added between the two. */
    hids_init();
    /* Diagnostics last: if the attribute table were ever full, only this
     * service would be lost, not the gamepad. */
    (void)diag_init(&m_diag, diag_cmd_handler);
    advertising_init();
    conn_params_init();

    /* The link comes up regardless of the mode strap: even over USB, where the
     * strap reads low, the LPC queries our version, and answering it is how
     * the flash tool can show which radio image is running. */
    uart_link_init();
    version_announce();

    uint32_t err_code = app_timer_start(m_tick_timer, MS_TO_TICKS(TICK_INTERVAL_MS), NULL);
    APP_ERROR_CHECK(err_code);

    advertising_start();

    for (;;)
    {
        bool work = false;

        while (uart_link_get_frame(frame, &frame_len))
        {
            lpc_frame_handle(frame, frame_len);
            work = true;
        }

        uint32_t act = actions_take();
        if (act != 0u)
        {
            actions_run(act);
            work = true;
        }

        /* The UART interrupt wakes us for every byte, about 11 us apart, and
         * most wakes complete no frame. Only a frame, an event or a timer tick
         * can change what there is to report. */
        if (work)
        {
            wipe_bonds_run();
            gamepad_report_update();
            diag_live_update();
            diag_refresh();
        }

        /* Sleeps until the next interrupt of any kind; returns at once if one
         * already happened since the last call. */
        err_code = sd_app_evt_wait();
        APP_ERROR_CHECK(err_code);
    }
}
