/**
 * @file uart_link.h
 * @brief Register-level UART0 driver for the link to Valve's LPC firmware.
 *
 * Why not app_uart: in nRF5 SDK 10, a single UART error (an overrun, say)
 * makes nrf_drv_uart mask the receive interrupt, and app_uart_fifo never
 * re-arms it, so reception stops for good while the BLE side carries on
 * looking healthy. This driver keeps receiving through errors, marks the frame
 * an error landed in as corrupt, and runs the framing decoder in the interrupt
 * so the main loop only ever sees whole frames.
 *
 * Threading: the UART interrupt produces received frames and consumes queued
 * transmit bytes. Everything else -- uart_link_get_frame and uart_link_send --
 * must be called from one context only, the main loop. Both queues are
 * single-producer single-consumer, so no locking is needed.
 */

#ifndef UART_LINK_H__
#define UART_LINK_H__

#include <stdint.h>
#include <stdbool.h>

/** Bytes kept by each raw capture buffer. */
#define UART_LINK_RAW_LEN 32u

typedef struct
{
    uint32_t rx_bytes;
    uint32_t rx_frames;        /**< Frames delivered to the main loop.          */
    uint16_t rx_uart_errors;   /**< Overrun, framing, parity or break.          */
    uint16_t rx_dropped;       /**< Damaged frames and lost ENDs.               */
    uint16_t rx_oversize;      /**< Intact frames too long to keep.             */
    uint16_t rx_queue_full;    /**< Good frames lost because the main loop lagged. */
    uint16_t tx_full;          /**< Frames refused for lack of transmit space. */
    /* Raw capture, for bring-up: what the LPC actually sends. */
    uint16_t rx_starts;        /**< START bytes seen, escaped or not.           */
    uint16_t rx_ends;          /**< END bytes seen.                             */
    uint8_t  rx_errorsrc;      /**< ERRORSRC bits seen, ORed together.          */
    uint8_t  rx_first[UART_LINK_RAW_LEN]; /**< First bytes since boot.         */
    uint8_t  rx_last[UART_LINK_RAW_LEN];  /**< Ring: byte n at n % UART_LINK_RAW_LEN. */
} uart_link_stats_t;

/**
 * @brief Read the boot-time mode strap on the RX pin.
 *
 * Valve's LPC drives its TXD high before releasing the radio from reset when
 * it is in BLE mode and not on USB, and low otherwise. Call this before
 * uart_link_init, which hands the pin to the UART.
 *
 * @return true if the LPC signalled BLE mode.
 */
bool uart_link_read_mode_strap(void);

/**
 * @brief Start the UART at 921600 8N1 on the pins Valve's firmware uses.
 *
 * Also takes the LPC's flow-control line (VALVE_PIN_WAKE) and pulses it once.
 * The receive interrupt pulses it after every byte, which is what keeps the
 * LPC sending.
 *
 * Must be called after the SoftDevice is enabled: it uses the sd_nvic API.
 */
void uart_link_init(void);

/**
 * @brief Take the oldest received frame, if any.
 *
 * @param[out] p_body Buffer of at least VFRAME_MAX_BODY bytes.
 * @param[out] p_len  Body length. The first body byte is the opcode.
 */
bool uart_link_get_frame(uint8_t * p_body, uint8_t * p_len);

/**
 * @brief Queue one frame for transmission, all or nothing.
 *
 * @return false if the frame did not fit, in which case nothing was queued.
 */
bool uart_link_send(const uint8_t * p_body, uint8_t len);

/**
 * @brief Pulse the flow-control line once, outside the receive path.
 *
 * Insurance against a stall: if an edge ever reaches the LPC while its FIFO
 * still holds a byte, it ignores it, and without another received byte the
 * receive path would never pulse again. Call it periodically.
 */
void uart_link_pull(void);

/** @brief True when every queued byte has left the UART. */
bool uart_link_tx_idle(void);

/** @brief Copy of the link counters. */
void uart_link_stats_get(uart_link_stats_t * p_stats);

#endif /* UART_LINK_H__ */
