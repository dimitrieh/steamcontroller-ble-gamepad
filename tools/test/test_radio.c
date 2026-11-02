/*
 * Host-side tests for the radio firmware's pure logic: Valve's UART framing,
 * the state-record decoder, and the HID report packing.
 *
 * The expected values here are written from the sources the firmware is
 * meant to agree with -- the LPC's own encoder (0xBAD4 in 5b0f21bd), SDL's
 * UpdateBLESteamControllerState, and the Linux HID input / Android Generic.kl
 * button mapping -- not copied from the firmware, so a mistake in the
 * firmware's tables shows up here as a failure.
 *
 * Build and run with tools/test/run.sh.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "valve_proto.h"
#include "vframe.h"
#include "sc_state.h"
#include "gamepad_hid.h"

/* ---- Bookkeeping ---------------------------------------------------------- */

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

#define TEST(name) static void name(void)
#define RUN(name) do { int f = g_failures; name(); \
    printf("%-44s %s\n", #name, f == g_failures ? "ok" : "FAILED"); } while (0)

/* Deterministic PRNG so failures reproduce. */
static uint32_t g_rng = 0x5C0E1234u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}

/* ---- A second, independent encoder: the LPC's, transcribed ----------------
 *
 * 0xBAD4 in 5b0f21bd: write START, then for each run of bytes up to the next
 * 02/03/1F write the run, then write 1F and the byte XOR 0x20, then END.
 */
static size_t lpc_encode(const uint8_t * body, size_t len, uint8_t * out)
{
    size_t n = 0;
    out[n++] = 0x02;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = body[i];
        if (b == 0x02 || b == 0x03 || b == 0x1F) {
            out[n++] = 0x1F;
            out[n++] = b ^ 0x20;
        } else {
            out[n++] = b;
        }
    }
    out[n++] = 0x03;
    return n;
}

/** Feed bytes; collect every completed frame body into frames[]. */
typedef struct { uint8_t body[VFRAME_MAX_BODY]; uint8_t len; } frame_t;

static int g_oversize;   /* Intact-but-too-long frames seen by feed(). */

static int feed(vframe_rx_t * rx, const uint8_t * bytes, size_t n,
                frame_t * frames, int max_frames, int * p_dropped)
{
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        vframe_rx_result_t r = vframe_rx_byte(rx, bytes[i]);
        if (r == VFRAME_RX_FRAME && count < max_frames) {
            memcpy(frames[count].body, rx->body, rx->len);
            frames[count].len = rx->len;
            count++;
        } else if (r == VFRAME_RX_DROPPED && p_dropped) {
            (*p_dropped)++;
        } else if (r == VFRAME_RX_OVERSIZE) {
            g_oversize++;
        }
    }
    return count;
}

/* ---- Framing ---------------------------------------------------------------*/

TEST(frame_encode_plain)
{
    const uint8_t body[] = { 0x51, 0x14, 0x00, 0x80, 0xFF };
    uint8_t out[32];
    uint16_t n = vframe_encode(body, sizeof(body), out, sizeof(out));
    const uint8_t want[] = { 0x02, 0x51, 0x14, 0x00, 0x80, 0xFF, 0x03 };
    CHECK(n == sizeof(want) && memcmp(out, want, n) == 0, "plain body framed as-is");
}

TEST(frame_encode_escapes)
{
    const uint8_t body[] = { 0x50, 0x02, 0x03, 0x1F, 0x22 };
    uint8_t out[32];
    uint16_t n = vframe_encode(body, sizeof(body), out, sizeof(out));
    const uint8_t want[] = { 0x02, 0x50, 0x1F, 0x22, 0x1F, 0x23, 0x1F, 0x3F, 0x22, 0x03 };
    CHECK(n == sizeof(want) && memcmp(out, want, n) == 0, "02/03/1F escaped as 1F + XOR 0x20");
}

TEST(frame_encode_matches_lpc_encoder)
{
    for (int iter = 0; iter < 2000; iter++) {
        uint8_t body[VFRAME_MAX_BODY];
        uint8_t len = (uint8_t)(1 + rnd() % VFRAME_MAX_BODY);
        for (int i = 0; i < len; i++) {
            /* Bias towards the special bytes. */
            uint32_t r = rnd() % 8;
            body[i] = r == 0 ? 0x02 : r == 1 ? 0x03 : r == 2 ? 0x1F : (uint8_t)rnd();
        }
        uint8_t ours[VFRAME_ENCODED_MAX(VFRAME_MAX_BODY)], lpc[VFRAME_ENCODED_MAX(VFRAME_MAX_BODY)];
        uint16_t n1 = vframe_encode(body, len, ours, sizeof(ours));
        size_t   n2 = lpc_encode(body, len, lpc);
        if (n1 != n2 || memcmp(ours, lpc, n2) != 0) {
            CHECK(0, "encoder disagrees with the LPC's on iteration %d", iter);
            return;
        }
    }
    CHECK(1, "completed");
}

TEST(frame_encode_bounds)
{
    const uint8_t body[] = { 0x51, 0x02 };   /* encodes to 02 51 1F 22 03: 5 bytes */
    uint8_t out[8];
    CHECK(vframe_encode(body, sizeof(body), out, 5) == 5, "exact fit accepted");
    CHECK(vframe_encode(body, sizeof(body), out, 4) == 0, "one byte short refused");
    CHECK(vframe_encode(body, sizeof(body), out, 1) == 0, "tiny buffer refused");
}

TEST(frame_roundtrip_random)
{
    vframe_rx_t rx;
    vframe_rx_init(&rx);
    for (int iter = 0; iter < 2000; iter++) {
        uint8_t body[VFRAME_MAX_BODY];
        uint8_t len = (uint8_t)(1 + rnd() % VFRAME_MAX_BODY);
        for (int i = 0; i < len; i++) body[i] = (uint8_t)rnd();
        uint8_t enc[VFRAME_ENCODED_MAX(VFRAME_MAX_BODY)];
        size_t n = lpc_encode(body, len, enc);
        frame_t f[2];
        int got = feed(&rx, enc, n, f, 2, NULL);
        if (got != 1 || f[0].len != len || memcmp(f[0].body, body, len) != 0) {
            CHECK(0, "round trip failed on iteration %d (len %u)", iter, len);
            return;
        }
    }
    CHECK(1, "completed");
}

TEST(frame_noise_and_back_to_back)
{
    vframe_rx_t rx;
    vframe_rx_init(&rx);
    /* Line noise before the first START, including END and ESC bytes, which
     * must be ignored outside a frame; then two frames back to back. */
    const uint8_t bytes[] = { 0xFF, 0x03, 0x1F, 0x00,
                              0x02, 0x56, 0x03,
                              0x02, 0x58, 0x03 };
    frame_t f[4];
    int dropped = 0;
    int got = feed(&rx, bytes, sizeof(bytes), f, 4, &dropped);
    CHECK(got == 2, "two frames (got %d)", got);
    CHECK(got >= 2 && f[0].len == 1 && f[0].body[0] == 0x56 && f[1].body[0] == 0x58,
          "frame contents");
    CHECK(dropped == 0, "noise outside a frame is not a drop");
}

TEST(frame_lost_end_resyncs_on_start)
{
    vframe_rx_t rx;
    vframe_rx_init(&rx);
    /* First frame's END lost: the next START must begin a fresh frame, and
     * the partial one must be reported as dropped, not glued on. */
    const uint8_t bytes[] = { 0x02, 0x51, 0x14, 0x00,
                              0x02, 0x56, 0x03 };
    frame_t f[2];
    int dropped = 0;
    int got = feed(&rx, bytes, sizeof(bytes), f, 2, &dropped);
    CHECK(got == 1 && f[0].len == 1 && f[0].body[0] == 0x56, "second frame intact");
    CHECK(dropped == 1, "partial frame counted as dropped (%d)", dropped);
}

TEST(frame_uart_error_drops_frame)
{
    vframe_rx_t rx;
    vframe_rx_init(&rx);
    const uint8_t head[] = { 0x02, 0x51, 0x14 };
    const uint8_t tail[] = { 0x00, 0x03, 0x02, 0x56, 0x03 };
    frame_t f[2];
    int dropped = 0;
    feed(&rx, head, sizeof(head), f, 2, &dropped);
    vframe_rx_error(&rx);   /* An overrun lost a byte somewhere in there. */
    int got = feed(&rx, tail, sizeof(tail), f, 2, &dropped);
    CHECK(got == 1 && f[0].body[0] == 0x56, "only the clean frame delivered");
    CHECK(dropped == 1, "corrupt frame dropped");

    /* An error between frames does not poison the next one. */
    vframe_rx_error(&rx);
    got = feed(&rx, (const uint8_t[]){ 0x02, 0x58, 0x03 }, 3, f, 2, &dropped);
    CHECK(got == 1, "error outside a frame is harmless");
}

TEST(frame_malformed_endings)
{
    vframe_rx_t rx;
    frame_t f[2];
    int dropped;

    vframe_rx_init(&rx); dropped = 0;
    CHECK(feed(&rx, (const uint8_t[]){ 0x02, 0x03 }, 2, f, 2, &dropped) == 0 && dropped == 1,
          "empty frame dropped");

    vframe_rx_init(&rx); dropped = 0;
    CHECK(feed(&rx, (const uint8_t[]){ 0x02, 0x51, 0x1F, 0x03 }, 4, f, 2, &dropped) == 0 &&
          dropped == 1, "frame ending mid-escape dropped");

    vframe_rx_init(&rx); dropped = 0;
    CHECK(feed(&rx, (const uint8_t[]){ 0x02, 0x51, 0x1F, 0x1F, 0x22, 0x03 }, 6, f, 2, &dropped) == 0 &&
          dropped == 1, "double escape dropped");
}

TEST(frame_oversize_dropped_then_recovers)
{
    vframe_rx_t rx;
    vframe_rx_init(&rx);
    /* The LPC's 128-byte bond push at boot is longer than we keep. */
    uint8_t big[1 + 129 + 1];
    big[0] = 0x02;
    big[1] = 0x5D;
    for (int i = 2; i < 130; i++) big[i] = 0x41;
    big[130] = 0x03;
    frame_t f[2];
    int dropped = 0;
    g_oversize = 0;
    int got = feed(&rx, big, sizeof(big), f, 2, &dropped);
    CHECK(got == 0, "oversize frame not delivered");
    CHECK(g_oversize == 1 && dropped == 0,
          "reported as oversize, not as damage (oversize %d, dropped %d)", g_oversize, dropped);
    got = feed(&rx, (const uint8_t[]){ 0x02, 0x56, 0x03 }, 3, f, 2, &dropped);
    CHECK(got == 1 && f[0].body[0] == 0x56, "next frame fine");

    /* Oversize and damaged at once counts as damage: it may have been anything. */
    vframe_rx_init(&rx);
    dropped = 0;
    g_oversize = 0;
    feed(&rx, big, sizeof(big) - 1, f, 2, &dropped);
    vframe_rx_error(&rx);
    feed(&rx, (const uint8_t[]){ 0x03 }, 1, f, 2, &dropped);
    CHECK(dropped == 1 && g_oversize == 0, "damaged oversize frame counted as damage");
}

/* ---- State records ---------------------------------------------------------
 *
 * A port of SDL's UpdateBLESteamControllerState, minus the pad rotation and
 * finger-down offset SDL applies for its own output: the raw fields it
 * extracts are what sc_state must agree with. *
 * Adapted from SDL (src/joystick/hidapi/steam/controller_steam.c),
 * Copyright (C) Valve Corporation and Sam Lantinga, zlib license:
 * https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt
 */
typedef struct {
    uint64_t buttons;
    int trig_l, trig_r;
    int16_t lsx, lsy, lpx, lpy, rpx, rpy;
    int16_t accel[3], gyro[3], quat[4];
} sdl_state_t;

static void sdl_apply(sdl_state_t * st, const uint8_t * p)
{
    uint32_t mask = (*p++ & 0xF0);
    mask |= (uint32_t)(*p++) << 8;
    if (mask & 0x10) { memcpy(&st->buttons, p, 3); p += 3; }
    if (mask & 0x20) { st->trig_l = p[0]; st->trig_r = p[1]; p += 2; }
    if (mask & 0x40) { uint8_t * b = (uint8_t *)&st->buttons; b[5] = *p++; b[6] = *p++; b[7] = *p++; }
    if (mask & 0x80) { memcpy(&st->lsx, p, 2); memcpy(&st->lsy, p + 2, 2); p += 4; }
    if (mask & 0x100) { memcpy(&st->lpx, p, 2); memcpy(&st->lpy, p + 2, 2); p += 4; }
    if (mask & 0x200) { memcpy(&st->rpx, p, 2); memcpy(&st->rpy, p + 2, 2); p += 4; }
    if (mask & 0x400) { memcpy(st->accel, p, 6); p += 6; }
    if (mask & 0x800) { memcpy(st->gyro, p, 6); p += 6; }
    if (mask & 0x1000) { memcpy(st->quat, p, 8); p += 8; }
}

/** Build a random well-formed record; returns its length. */
static uint8_t random_record(uint8_t * rec)
{
    static const struct { uint16_t flag; uint8_t size; } chunks[] = {
        { 0x10, 3 }, { 0x20, 2 }, { 0x40, 3 }, { 0x80, 4 }, { 0x100, 4 },
        { 0x200, 4 }, { 0x400, 6 }, { 0x800, 6 }, { 0x1000, 8 },
    };
    uint16_t flags = 0;
    for (int i = 0; i < 9; i++) if (rnd() & 1) flags |= chunks[i].flag;
    rec[0] = (uint8_t)((flags & 0xF0) | VALVE_REC_TYPE_STATE);
    rec[1] = (uint8_t)(flags >> 8);
    uint8_t n = 2;
    for (int i = 0; i < 9; i++)
        if (flags & chunks[i].flag)
            for (int j = 0; j < chunks[i].size; j++) rec[n++] = (uint8_t)rnd();
    return n;
}

TEST(state_matches_sdl_parser)
{
    sc_state_t  ours;
    sdl_state_t sdl;
    sc_state_reset(&ours);
    memset(&sdl, 0, sizeof(sdl));

    /* A long run of deltas: every field must track SDL's view throughout. */
    for (int iter = 0; iter < 5000; iter++) {
        uint8_t rec[64];
        uint8_t len = random_record(rec);
        sc_rec_result_t r = sc_state_apply(&ours, rec, len, NULL, NULL);
        sdl_apply(&sdl, rec);
        if (r != SC_REC_OK) { CHECK(0, "record rejected on iteration %d", iter); return; }

        uint8_t ext[3];
        memcpy(ext, (uint8_t *)&sdl.buttons + 5, 3);
        bool same =
            ours.buttons == (uint32_t)(sdl.buttons & 0xFFFFFF) &&
            memcmp(ours.buttons_ext, ext, 3) == 0 &&
            ours.ltrig == sdl.trig_l && ours.rtrig == sdl.trig_r &&
            ours.lstick_x == sdl.lsx && ours.lstick_y == sdl.lsy &&
            ours.lpad_x == sdl.lpx && ours.lpad_y == sdl.lpy &&
            ours.rpad_x == sdl.rpx && ours.rpad_y == sdl.rpy &&
            memcmp(ours.accel, sdl.accel, 6) == 0 &&
            memcmp(ours.gyro, sdl.gyro, 6) == 0 &&
            memcmp(ours.quat, sdl.quat, 8) == 0;
        if (!same) { CHECK(0, "state diverged from SDL on iteration %d", iter); return; }
    }
    CHECK(1, "completed");
}

TEST(state_known_record)
{
    /* Buttons A + LB, triggers 0x40/0xC0, joystick (1000, -2000). */
    const uint8_t rec[] = {
        0x10 | 0x20 | 0x80 | VALVE_REC_TYPE_STATE, 0x00,
        0x88, 0x00, 0x00,          /* A (0x80) + LB (0x08)  */
        0x40, 0xC0,                /* triggers               */
        0xE8, 0x03, 0x30, 0xF8,    /* 1000, -2000            */
    };
    sc_state_t st;
    uint16_t flags = 0; uint8_t extra = 0xFF;
    sc_state_reset(&st);
    CHECK(sc_state_apply(&st, rec, sizeof(rec), &flags, &extra) == SC_REC_OK, "applied");
    CHECK(flags == 0xB0, "flags 0x%04x", flags);
    CHECK(extra == 0, "no trailing bytes");
    CHECK(st.buttons == (VALVE_BTN_A | VALVE_BTN_LB), "buttons 0x%06x", st.buttons);
    CHECK(st.ltrig == 0x40 && st.rtrig == 0xC0, "triggers");
    CHECK(st.lstick_x == 1000 && st.lstick_y == -2000, "joystick %d,%d", st.lstick_x, st.lstick_y);
}

TEST(state_delta_keeps_absent_chunks)
{
    sc_state_t st;
    sc_state_reset(&st);
    const uint8_t first[]  = { 0x10 | 4, 0x00, 0x80, 0x00, 0x00 };           /* A down   */
    const uint8_t second[] = { 0x20 | 4, 0x00, 0x11, 0x22 };                 /* triggers */
    sc_state_apply(&st, first, sizeof(first), NULL, NULL);
    sc_state_apply(&st, second, sizeof(second), NULL, NULL);
    CHECK(st.buttons == VALVE_BTN_A, "button chunk absent: A still held");
    CHECK(st.ltrig == 0x11 && st.rtrig == 0x22, "triggers updated");
}

TEST(state_rejects_without_touching)
{
    sc_state_t st, before;
    sc_state_reset(&st);
    const uint8_t base[] = { 0x10 | 4, 0x00, 0x80, 0x00, 0x00 };
    sc_state_apply(&st, base, sizeof(base), NULL, NULL);
    before = st;

    /* Buttons and triggers flagged, but the trigger bytes are missing: an
     * overrun ate them. Nothing may be applied, buttons included. */
    const uint8_t truncated[] = { 0x10 | 0x20 | 4, 0x00, 0x00, 0x00, 0x00, 0x7F };
    CHECK(sc_state_apply(&st, truncated, sizeof(truncated), NULL, NULL) == SC_REC_TRUNCATED,
          "truncated record reported");
    CHECK(memcmp(&st, &before, sizeof(st)) == 0, "state untouched by truncated record");

    const uint8_t unknown_flag[] = { 0x10 | 4, 0x20, 0x00, 0x00, 0x00 };   /* 0x2000 */
    CHECK(sc_state_apply(&st, unknown_flag, sizeof(unknown_flag), NULL, NULL) == SC_REC_UNKNOWN,
          "unknown chunk flag refused");
    CHECK(memcmp(&st, &before, sizeof(st)) == 0, "state untouched by unknown chunk");

    const uint8_t other_type[] = { 0x05, 0x00 };   /* k_EBLEReportStatus */
    CHECK(sc_state_apply(&st, other_type, sizeof(other_type), NULL, NULL) == SC_REC_UNKNOWN,
          "other record type refused");

    CHECK(sc_state_apply(&st, base, 1, NULL, NULL) == SC_REC_TRUNCATED, "one-byte record");
    CHECK(sc_state_apply(&st, base, 0, NULL, NULL) == SC_REC_TRUNCATED, "empty record");
}

TEST(state_lizard_and_extra)
{
    sc_state_t st;
    sc_state_reset(&st);
    uint8_t lizard[13] = { VALVE_REC_TYPE_LIZARD };
    CHECK(sc_state_apply(&st, lizard, sizeof(lizard), NULL, NULL) == SC_REC_LIZARD,
          "keyboard+mouse payload recognised as lizard mode");

    const uint8_t padded[] = { 0x10 | 4, 0x00, 0x80, 0x00, 0x00, 0xAA, 0xBB };
    uint8_t extra = 0;
    CHECK(sc_state_apply(&st, padded, sizeof(padded), NULL, &extra) == SC_REC_OK &&
          extra == 2, "trailing bytes accepted and counted (%u)", extra);
}

/* ---- HID report ------------------------------------------------------------
 *
 * Expected HID button numbers, from the Linux HID input layer (Gamepad
 * application: button n -> BTN_GAMEPAD + n - 1, then BTN_TRIGGER_HAPPY) and
 * Android's Generic.kl. Written out independently of gamepad_hid.c.
 */
static const struct { uint32_t valve; int hid; const char * what; } k_expected_buttons[] = {
    { VALVE_BTN_A,           1,  "A -> BTN_SOUTH -> BUTTON_A" },
    { VALVE_BTN_B,           2,  "B -> BTN_EAST -> BUTTON_B" },
    { VALVE_BTN_X,           4,  "X -> BTN_NORTH -> BUTTON_X" },
    { VALVE_BTN_Y,           5,  "Y -> BTN_WEST -> BUTTON_Y" },
    { VALVE_BTN_LB,          7,  "LB -> BTN_TL -> BUTTON_L1" },
    { VALVE_BTN_RB,          8,  "RB -> BTN_TR -> BUTTON_R1" },
    { VALVE_BTN_LT_FULL,     9,  "LT click -> BTN_TL2 -> BUTTON_L2" },
    { VALVE_BTN_RT_FULL,     10, "RT click -> BTN_TR2 -> BUTTON_R2" },
    { VALVE_BTN_BACK,        11, "Back -> BTN_SELECT -> BUTTON_SELECT" },
    { VALVE_BTN_START,       12, "Start -> BTN_START -> BUTTON_START" },
    { VALVE_BTN_STEAM,       13, "Steam -> BTN_MODE -> BUTTON_MODE" },
    { VALVE_BTN_STICK_CLICK, 14, "stick click -> BTN_THUMBL" },
    { VALVE_BTN_RPAD_CLICK,  15, "right pad click -> BTN_THUMBR" },
    { VALVE_BTN_LGRIP,       17, "left grip -> BTN_TRIGGER_HAPPY1" },
    { VALVE_BTN_RGRIP,       18, "right grip -> BTN_TRIGGER_HAPPY2" },
    { VALVE_BTN_LPAD_CLICK,  19, "left pad click -> BTN_TRIGGER_HAPPY3" },
};

static uint32_t report_buttons(const uint8_t * rep)
{
    return (uint32_t)rep[0] | ((uint32_t)rep[1] << 8) | ((uint32_t)rep[2] << 16);
}

static int16_t report_axis(const uint8_t * rep, int idx)
{
    return (int16_t)(rep[4 + 2 * idx] | (rep[5 + 2 * idx] << 8));
}

TEST(report_each_button)
{
    for (size_t i = 0; i < sizeof(k_expected_buttons) / sizeof(k_expected_buttons[0]); i++) {
        sc_state_t st;
        uint8_t rep[GAMEPAD_INPUT_REP_LEN];
        sc_state_reset(&st);
        st.buttons = k_expected_buttons[i].valve;
        gamepad_pack_report(&st, rep);
        uint32_t want = 1u << (k_expected_buttons[i].hid - 1);
        CHECK(report_buttons(rep) == want, "%s: got 0x%06x want 0x%06x",
              k_expected_buttons[i].what, report_buttons(rep), want);
        CHECK(rep[3] == 0, "%s: hat centred", k_expected_buttons[i].what);
    }
}

TEST(report_unused_buttons_stay_clear)
{
    /* Every Valve bit at once: HID buttons 3, 6 and 16 (BTN_C, BTN_Z, the
     * unnamed code) and the 5 padding bits must never be set. */
    sc_state_t st;
    uint8_t rep[GAMEPAD_INPUT_REP_LEN];
    sc_state_reset(&st);
    st.buttons = 0xFFFFFF;
    gamepad_pack_report(&st, rep);
    uint32_t b = report_buttons(rep);
    CHECK((b & ((1u << 2) | (1u << 5) | (1u << 15))) == 0, "buttons 3, 6, 16 unused (0x%06x)", b);
    CHECK((b & ~0x7FFFFu) == 0, "padding bits clear");
}

TEST(report_hat)
{
    static const struct { uint32_t dpad; uint8_t hat; } cases[] = {
        { 0, 0 },
        { VALVE_BTN_DPAD_UP, 1 },
        { VALVE_BTN_DPAD_UP | VALVE_BTN_DPAD_RIGHT, 2 },
        { VALVE_BTN_DPAD_RIGHT, 3 },
        { VALVE_BTN_DPAD_DOWN | VALVE_BTN_DPAD_RIGHT, 4 },
        { VALVE_BTN_DPAD_DOWN, 5 },
        { VALVE_BTN_DPAD_DOWN | VALVE_BTN_DPAD_LEFT, 6 },
        { VALVE_BTN_DPAD_LEFT, 7 },
        { VALVE_BTN_DPAD_UP | VALVE_BTN_DPAD_LEFT, 8 },
        { VALVE_BTN_DPAD_UP | VALVE_BTN_DPAD_DOWN, 0 },
        { VALVE_BTN_DPAD_LEFT | VALVE_BTN_DPAD_RIGHT, 0 },
        { VALVE_BTN_DPAD_ANY, 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sc_state_t st;
        uint8_t rep[GAMEPAD_INPUT_REP_LEN];
        sc_state_reset(&st);
        st.buttons = cases[i].dpad;
        gamepad_pack_report(&st, rep);
        CHECK(rep[3] == cases[i].hat, "dpad 0x%03x -> hat %u (want %u)",
              cases[i].dpad, rep[3], cases[i].hat);
    }
}

TEST(report_lpad_click_vs_dpad)
{
    sc_state_t st;
    uint8_t rep[GAMEPAD_INPUT_REP_LEN];
    sc_state_reset(&st);
    st.buttons = VALVE_BTN_LPAD_CLICK | VALVE_BTN_DPAD_UP;
    gamepad_pack_report(&st, rep);
    CHECK((report_buttons(rep) & (1u << 18)) == 0, "arrow click is the hat, not button 19");
    CHECK(rep[3] == 1, "hat up");
}

TEST(report_axes)
{
    sc_state_t st;
    uint8_t rep[GAMEPAD_INPUT_REP_LEN];
    sc_state_reset(&st);

    st.lstick_x = 12345; st.lstick_y = 20000;   /* up is positive on the controller */
    gamepad_pack_report(&st, rep);
    CHECK(report_axis(rep, 0) == 12345, "X passes through");
    CHECK(report_axis(rep, 1) == -20000, "Y inverted for HID (down is positive)");

    st.lstick_x = -32768; st.lstick_y = -32768;
    gamepad_pack_report(&st, rep);
    CHECK(report_axis(rep, 0) == -32767, "X clamped to -32767");
    CHECK(report_axis(rep, 1) == 32767, "Y of -32768 inverts to 32767, not overflow");

    /* Right pad: centred when not touched, whatever the last position was. */
    sc_state_reset(&st);
    st.rpad_x = 20000; st.rpad_y = 20000;
    gamepad_pack_report(&st, rep);
    CHECK(report_axis(rep, 2) == 0 && report_axis(rep, 3) == 0, "untouched pad is centred");

    /* Touched, straight up: rotated by +15 degrees as SDL does. */
    st.buttons = VALVE_BTN_RPAD_TOUCH;
    st.rpad_x = 0; st.rpad_y = 30000;
    gamepad_pack_report(&st, rep);
    int z = report_axis(rep, 2), rz = report_axis(rep, 3);
    /* x' = -sin15 * 30000 = -7765, y' = cos15 * 30000 = 28978, then Y inverted. */
    CHECK(abs(z - (-7765)) <= 2, "Z %d", z);
    CHECK(abs(rz - (-28978)) <= 2, "Rz %d", rz);

    /* Corner beyond the unit circle clamps instead of wrapping. */
    st.rpad_x = 32767; st.rpad_y = 32767;
    gamepad_pack_report(&st, rep);
    CHECK(report_axis(rep, 3) == -32767, "Rz clamped (%d)", report_axis(rep, 3));
}

TEST(report_triggers)
{
    sc_state_t st;
    uint8_t rep[GAMEPAD_INPUT_REP_LEN];
    sc_state_reset(&st);
    st.ltrig = 0; st.rtrig = 255;
    gamepad_pack_report(&st, rep);
    CHECK(rep[12] == 0 && rep[13] == 255, "brake = left, accelerator = right");
}

/* ---- End to end: LPC bytes in, HID report out ------------------------------*/

TEST(end_to_end_frame_to_report)
{
    /* What the LPC puts on the wire for "B held, right trigger half pulled",
     * built with its own encoder. The button byte 0x20 needs no escape, but
     * the trigger value 0x03 does. */
    const uint8_t body[] = { VALVE_OP_STATE, 0x10 | 0x20 | 4, 0x00,
                             0x20, 0x00, 0x00, 0x00, 0x03 };
    uint8_t wire[32];
    size_t n = lpc_encode(body, sizeof(body), wire);

    vframe_rx_t rx;
    vframe_rx_init(&rx);
    frame_t f[1];
    int got = feed(&rx, wire, n, f, 1, NULL);
    CHECK(got == 1 && f[0].body[0] == VALVE_OP_STATE, "frame received");

    sc_state_t st;
    sc_state_reset(&st);
    CHECK(sc_state_apply(&st, &f[0].body[1], (uint8_t)(f[0].len - 1), NULL, NULL) == SC_REC_OK,
          "record applied");

    uint8_t rep[GAMEPAD_INPUT_REP_LEN];
    gamepad_pack_report(&st, rep);
    CHECK(report_buttons(rep) == (1u << 1), "HID button 2 (B)");
    CHECK(rep[13] == 0x03, "right trigger survived the escape");
}

int main(void)
{
    RUN(frame_encode_plain);
    RUN(frame_encode_escapes);
    RUN(frame_encode_matches_lpc_encoder);
    RUN(frame_encode_bounds);
    RUN(frame_roundtrip_random);
    RUN(frame_noise_and_back_to_back);
    RUN(frame_lost_end_resyncs_on_start);
    RUN(frame_uart_error_drops_frame);
    RUN(frame_malformed_endings);
    RUN(frame_oversize_dropped_then_recovers);
    RUN(state_matches_sdl_parser);
    RUN(state_known_record);
    RUN(state_delta_keeps_absent_chunks);
    RUN(state_rejects_without_touching);
    RUN(state_lizard_and_extra);
    RUN(report_each_button);
    RUN(report_unused_buttons_stay_clear);
    RUN(report_hat);
    RUN(report_lpad_click_vs_dpad);
    RUN(report_axes);
    RUN(report_triggers);
    RUN(end_to_end_frame_to_report);

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures == 0) printf("all tests passed\n");
    return g_failures ? 1 : 0;
}
