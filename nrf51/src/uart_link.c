/**
 * @file uart_link.c
 * @brief Register-level UART0 driver for the link to Valve's LPC firmware.
 */

#include <string.h>

#include "nrf.h"
#include "nrf_soc.h"
#include "nrf_gpio.h"
#include "nrf_delay.h"
#include "app_util_platform.h"

#include "uart_link.h"
#include "vframe.h"
#include "valve_proto.h"

/* ---- Receive: frames, decoded in the interrupt -------------------------- */

/** Frames the main loop can fall behind by. A state frame arrives every few
 *  milliseconds, and the main loop drains the queue each time it wakes. */
#define RXQ_SLOTS   8u

typedef struct
{
    uint8_t len;
    uint8_t body[VFRAME_MAX_BODY];
} rx_slot_t;

static rx_slot_t        m_rxq[RXQ_SLOTS];
static volatile uint8_t m_rxq_head;   /**< Written by the interrupt only. */
static volatile uint8_t m_rxq_tail;   /**< Written by the main loop only. */
static vframe_rx_t      m_rx;
/** Bytes still to be treated as possibly adjacent to a lost byte. */
static uint8_t          m_rx_taint;

/*
 * How many bytes around a UART error to distrust. An overrun loses the byte
 * that arrived while the receive FIFO (6 bytes on nRF51) was full, so the
 * loss sits somewhere after the bytes still waiting in it, and before bytes
 * that arrive while they are drained. Every frame in progress across this many
 * bytes is dropped: the loss is somewhere in there, and with no checksum a
 * frame that lost a byte cannot be told apart from a good one afterwards.
 */
#define RX_TAINT_BYTES  8u

/** Ask the LPC for its next bytes: a falling edge on the flow-control line. */
static void lpc_pull(void)
{
    nrf_gpio_pin_clear(VALVE_PIN_WAKE);
    nrf_delay_us(1u);
    nrf_gpio_pin_set(VALVE_PIN_WAKE);
}

/* ---- Transmit: raw bytes, drained by the interrupt ---------------------- */

#define TXQ_SIZE    256u   /* Power of two: indices wrap with a mask. */
#define TXQ_MASK    (TXQ_SIZE - 1u)

static uint8_t           m_txq[TXQ_SIZE];
static volatile uint16_t m_txq_head;  /**< Written by the main loop only. */
static volatile uint16_t m_txq_tail;  /**< Written by the interrupt only. */
static volatile bool     m_tx_busy;   /**< Touched by the interrupt only. */

static volatile uart_link_stats_t m_stats;

bool uart_link_read_mode_strap(void)
{
    /* After reset the pin's input buffer is disconnected, so connect it
     * first. No pull: the LPC is driving the line. */
    nrf_gpio_cfg_input(VALVE_PIN_RX, NRF_GPIO_PIN_NOPULL);

    /* Let the input buffer settle for a few cycles before sampling. */
    for (volatile uint32_t i = 0u; i < 16u; i++)
    {
    }

    return nrf_gpio_pin_read(VALVE_PIN_RX) != 0u;
}

void uart_link_init(void)
{
    vframe_rx_init(&m_rx);
    m_rx_taint = 0u;
    m_rxq_head = m_rxq_tail = 0u;
    m_txq_head = m_txq_tail = 0u;
    m_tx_busy  = false;
    memset((void *)&m_stats, 0, sizeof(m_stats));

    /* The wake line idles high. Set it before making it an output so the LPC
     * sees no edge until the pulse below. */
    nrf_gpio_pin_set(VALVE_PIN_WAKE);
    nrf_gpio_cfg_output(VALVE_PIN_WAKE);

    /* TXD idles high; drive it high before the UART owns it so the LPC never
     * sees a spurious start bit. */
    nrf_gpio_pin_set(VALVE_PIN_TX);
    nrf_gpio_cfg_output(VALVE_PIN_TX);
    nrf_gpio_cfg_input(VALVE_PIN_RX, NRF_GPIO_PIN_NOPULL);

    NRF_UART0->PSELTXD  = VALVE_PIN_TX;
    NRF_UART0->PSELRXD  = VALVE_PIN_RX;
    NRF_UART0->PSELRTS  = 0xFFFFFFFFu;
    NRF_UART0->PSELCTS  = 0xFFFFFFFFu;
    NRF_UART0->CONFIG   = 0u;   /* No flow control, no parity. */
    NRF_UART0->BAUDRATE = UART_BAUDRATE_BAUDRATE_Baud921600;
    NRF_UART0->ENABLE   = UART_ENABLE_ENABLE_Enabled;

    NRF_UART0->EVENTS_RXDRDY = 0u;
    NRF_UART0->EVENTS_TXDRDY = 0u;
    NRF_UART0->EVENTS_ERROR  = 0u;
    NRF_UART0->ERRORSRC      = NRF_UART0->ERRORSRC;

    NRF_UART0->INTENSET = UART_INTENSET_RXDRDY_Msk |
                          UART_INTENSET_TXDRDY_Msk |
                          UART_INTENSET_ERROR_Msk;

    NRF_UART0->TASKS_STARTRX = 1u;
    NRF_UART0->TASKS_STARTTX = 1u;

    /* Tell the LPC the radio is up, as Valve's radio does right after its
     * own UART init (0x16BE8). This also releases whatever the LPC queued
     * before we were listening: its bond push and version query go out a few
     * milliseconds after it releases us from reset. */
    nrf_gpio_pin_clear(VALVE_PIN_WAKE);
    nrf_delay_us(VALVE_WAKE_BOOT_PULSE_US);
    nrf_gpio_pin_set(VALVE_PIN_WAKE);

    /*
     * APP_IRQ_PRIORITY_HIGH, above the SoftDevice's event interrupt and the
     * main loop. At 921600 baud a byte lands every 11 us and the UART holds
     * only a few, so this handler must not wait behind BLE event processing.
     * It never calls into the SoftDevice, which is what makes the higher
     * priority allowed.
     */
    (void)sd_nvic_ClearPendingIRQ(UART0_IRQn);
    (void)sd_nvic_SetPriority(UART0_IRQn, APP_IRQ_PRIORITY_HIGH);
    (void)sd_nvic_EnableIRQ(UART0_IRQn);
}

static void rx_queue_push(const vframe_rx_t * p_rx)
{
    uint8_t head = m_rxq_head;
    uint8_t next = (uint8_t)((head + 1u) % RXQ_SLOTS);

    if (next == m_rxq_tail)
    {
        m_stats.rx_queue_full++;
        return;
    }

    m_rxq[head].len = p_rx->len;
    memcpy(m_rxq[head].body, p_rx->body, p_rx->len);
    m_rxq_head = next;
    m_stats.rx_frames++;
}

void UART0_IRQHandler(void)
{
    if (NRF_UART0->EVENTS_ERROR)
    {
        NRF_UART0->EVENTS_ERROR = 0u;
        /* ERRORSRC bits are cleared by writing them back. The receiver keeps
         * running. */
        uint32_t errorsrc = NRF_UART0->ERRORSRC;
        NRF_UART0->ERRORSRC = errorsrc;
        m_stats.rx_errorsrc |= (uint8_t)errorsrc;
        m_stats.rx_uart_errors++;
        m_rx_taint = RX_TAINT_BYTES;
    }

    bool received = false;

    while (NRF_UART0->EVENTS_RXDRDY)
    {
        received = true;
        /* Clear the event before reading RXD, or a byte that arrives in
         * between is never signalled. */
        NRF_UART0->EVENTS_RXDRDY = 0u;
        uint8_t byte = (uint8_t)NRF_UART0->RXD;

        if (m_rx_taint != 0u)
        {
            /* Before the byte, so an END inside the window completes a
             * frame that is already marked, and a START inside it begins
             * one that the next byte marks. */
            vframe_rx_error(&m_rx);
            m_rx_taint--;
        }

        uint32_t n = m_stats.rx_bytes;
        if (n < UART_LINK_RAW_LEN)
        {
            m_stats.rx_first[n] = byte;
        }
        m_stats.rx_last[n % UART_LINK_RAW_LEN] = byte;
        if (byte == VALVE_FRAME_START) m_stats.rx_starts++;
        if (byte == VALVE_FRAME_END)   m_stats.rx_ends++;
        m_stats.rx_bytes = n + 1u;

        switch (vframe_rx_byte(&m_rx, byte))
        {
            case VFRAME_RX_FRAME:
                rx_queue_push(&m_rx);
                break;

            case VFRAME_RX_DROPPED:
                m_stats.rx_dropped++;
                break;

            case VFRAME_RX_OVERSIZE:
                m_stats.rx_oversize++;
                break;

            default:
                break;
        }
    }

    /* The LPC sends 4 bytes per edge and refills only once its FIFO is
     * empty, so pulling after every byte keeps it streaming. Edges while its
     * FIFO still holds bytes are ignored on its side. */
    if (received)
    {
        lpc_pull();
    }

    if (NRF_UART0->EVENTS_TXDRDY)
    {
        NRF_UART0->EVENTS_TXDRDY = 0u;
        m_tx_busy = false;
    }

    /* Also reached through a software-pended interrupt when uart_link_send
     * queues bytes while the transmitter is idle. */
    if (!m_tx_busy && (m_txq_tail != m_txq_head))
    {
        uint16_t tail = m_txq_tail;
        NRF_UART0->TXD = m_txq[tail];
        m_txq_tail = (uint16_t)((tail + 1u) & TXQ_MASK);
        m_tx_busy  = true;
    }
}

bool uart_link_get_frame(uint8_t * p_body, uint8_t * p_len)
{
    uint8_t tail = m_rxq_tail;

    if (tail == m_rxq_head)
    {
        return false;
    }

    *p_len = m_rxq[tail].len;
    memcpy(p_body, m_rxq[tail].body, m_rxq[tail].len);
    m_rxq_tail = (uint8_t)((tail + 1u) % RXQ_SLOTS);
    return true;
}

bool uart_link_send(const uint8_t * p_body, uint8_t len)
{
    uint8_t  encoded[VFRAME_ENCODED_MAX(VFRAME_MAX_BODY)];
    uint16_t n = vframe_encode(p_body, len, encoded, sizeof(encoded));

    if (n == 0u)
    {
        m_stats.tx_full++;
        return false;
    }

    uint16_t head = m_txq_head;
    uint16_t used = (uint16_t)((head - m_txq_tail) & TXQ_MASK);
    uint16_t free_space = (uint16_t)(TXQ_SIZE - 1u - used);

    if (n > free_space)
    {
        /* All or nothing: half a frame costs the LPC this frame and the next. */
        m_stats.tx_full++;
        return false;
    }

    for (uint16_t i = 0u; i < n; i++)
    {
        m_txq[head] = encoded[i];
        head = (uint16_t)((head + 1u) & TXQ_MASK);
    }
    m_txq_head = head;

    /* Let the interrupt start the transmitter if it is idle. */
    (void)sd_nvic_SetPendingIRQ(UART0_IRQn);
    return true;
}

void uart_link_pull(void)
{
    /* At the interrupt's priority, so it cannot land inside a pulse the
     * interrupt is making. */
    CRITICAL_REGION_ENTER();
    lpc_pull();
    CRITICAL_REGION_EXIT();
}

bool uart_link_tx_idle(void)
{
    return (m_txq_tail == m_txq_head) && !m_tx_busy;
}

void uart_link_stats_get(uart_link_stats_t * p_stats)
{
    /* The counters are updated at a higher priority; a torn read of one
     * counter only ever affects a diagnostic display. */
    memcpy(p_stats, (const void *)&m_stats, sizeof(*p_stats));
}
