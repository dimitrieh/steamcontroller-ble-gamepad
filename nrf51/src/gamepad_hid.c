/**
 * @file gamepad_hid.c
 * @brief HID report map and report packing.
 *
 * Valve's stock BLE radio firmware exposes a Mouse collection (report ID 1), a
 * Keyboard collection (report ID 2) and a vendor-defined page 0xFF00 collection
 * (report ID 3) -- and no gamepad at all, which is why the controller shows up
 * as a mouse and keyboard to anything that is not Steam. This file is the
 * replacement: one Gamepad application collection, laid out for Android.
 *
 * Android does not read HID usages directly. A BLE HID device goes through the
 * Linux HID input layer, which turns usages into evdev codes, and Android's
 * Generic.kl turns those into key codes and axes. Everything below is chosen
 * for where it lands at the end of that chain:
 *
 *   HID button n of a Gamepad collection -> evdev BTN_GAMEPAD + (n - 1) for
 *   n <= 16, and BTN_TRIGGER_HAPPY1 + (n - 17) above that. The evdev gamepad
 *   range has BTN_C and BTN_Z in it, which modern pads do not have, so buttons
 *   3 and 6 are deliberately left unused; filling them would shift every
 *   button after them onto the wrong Android key code.
 *
 *    1 BTN_SOUTH  -> BUTTON_A        A
 *    2 BTN_EAST   -> BUTTON_B        B (Android falls back to BACK)
 *    3 BTN_C         unused
 *    4 BTN_NORTH  -> BUTTON_X        X
 *    5 BTN_WEST   -> BUTTON_Y        Y
 *    6 BTN_Z         unused
 *    7 BTN_TL     -> BUTTON_L1       left bumper
 *    8 BTN_TR     -> BUTTON_R1       right bumper
 *    9 BTN_TL2    -> BUTTON_L2       left trigger, full pull
 *   10 BTN_TR2    -> BUTTON_R2       right trigger, full pull
 *   11 BTN_SELECT -> BUTTON_SELECT   Back (left arrow)
 *   12 BTN_START  -> BUTTON_START    Start (right arrow)
 *   13 BTN_MODE   -> BUTTON_MODE     Steam button
 *   14 BTN_THUMBL -> BUTTON_THUMBL   joystick click
 *   15 BTN_THUMBR -> BUTTON_THUMBR   right trackpad click (it is the right stick)
 *   16 (unnamed evdev code, no Android key) unused
 *   17 BTN_TRIGGER_HAPPY1            left grip
 *   18 BTN_TRIGGER_HAPPY2            right grip
 *   19 BTN_TRIGGER_HAPPY3            left trackpad click away from the arrows
 *
 *   Hat switch -> ABS_HAT0X/Y -> AXIS_HAT_X/Y, which Android also turns into
 *   DPAD key events for TV navigation. Driven by the D-pad bits Valve's LPC
 *   firmware derives from where the left trackpad is clicked.
 *
 *   X, Y  -> AXIS_X, AXIS_Y     joystick
 *   Z, Rz -> AXIS_Z, AXIS_RZ    right trackpad; Android's right-stick axes
 *   Simulation Brake / Accelerator -> ABS_BRAKE / ABS_GAS -> AXIS_BRAKE /
 *   AXIS_GAS, which Android documents as the trigger axes alongside
 *   AXIS_LTRIGGER / AXIS_RTRIGGER. Z / Rz would be read as centred axes.
 */

#include "gamepad_hid.h"
#include "valve_proto.h"

const uint8_t gamepad_report_map[] =
{
    0x05, 0x01,       /* Usage Page (Generic Desktop)                        */
    0x09, 0x05,       /* Usage (Gamepad)                                     */
    0xA1, 0x01,       /* Collection (Application)                            */
    0x85, GAMEPAD_INPUT_REP_ID,
                      /*   Report ID (1)                                     */

    /* ---- Buttons 1..19 -------------------------------------------------- */
    0x05, 0x09,       /*   Usage Page (Button)                               */
    0x19, 0x01,       /*   Usage Minimum (Button 1)                          */
    0x29, 0x13,       /*   Usage Maximum (Button 19)                         */
    0x15, 0x00,       /*   Logical Minimum (0)                               */
    0x25, 0x01,       /*   Logical Maximum (1)                               */
    0x75, 0x01,       /*   Report Size (1)                                   */
    0x95, 0x13,       /*   Report Count (19)                                 */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute)                  */
    0x75, 0x01,       /*   Report Size (1)                                   */
    0x95, 0x05,       /*   Report Count (5)                                  */
    0x81, 0x03,       /*   Input (Constant) -- pad the buttons to 3 bytes    */

    /* ---- Hat switch ----------------------------------------------------- */
    0x05, 0x01,       /*   Usage Page (Generic Desktop)                      */
    0x09, 0x39,       /*   Usage (Hat switch)                                */
    0x15, 0x01,       /*   Logical Minimum (1)                               */
    0x25, 0x08,       /*   Logical Maximum (8)                               */
    0x35, 0x00,       /*   Physical Minimum (0)                              */
    0x46, 0x3B, 0x01, /*   Physical Maximum (315)                            */
    0x65, 0x14,       /*   Unit (English Rotation: Degrees)                  */
    0x75, 0x04,       /*   Report Size (4)                                   */
    0x95, 0x01,       /*   Report Count (1)                                  */
    0x81, 0x42,       /*   Input (Data, Variable, Absolute, Null State)      */
    /* Physical range and unit are global items: reset them, or 0..315 degrees
     * applies to every axis below. */
    0x65, 0x00,       /*   Unit (None)                                       */
    0x35, 0x00,       /*   Physical Minimum (0)                              */
    0x45, 0x00,       /*   Physical Maximum (0) -- same as logical           */
    0x75, 0x04,       /*   Report Size (4)                                   */
    0x95, 0x01,       /*   Report Count (1)                                  */
    0x81, 0x03,       /*   Input (Constant) -- pad the hat out to a byte     */

    /* ---- Sticks: X, Y (joystick), Z, Rz (right trackpad) ---------------- */
    0x09, 0x01,       /*   Usage (Pointer)                                   */
    0xA1, 0x00,       /*   Collection (Physical)                             */
    0x09, 0x30,       /*     Usage (X)                                       */
    0x09, 0x31,       /*     Usage (Y)                                       */
    0x09, 0x32,       /*     Usage (Z)                                       */
    0x09, 0x35,       /*     Usage (Rz)                                      */
    0x16, 0x01, 0x80, /*     Logical Minimum (-32767)                        */
    0x26, 0xFF, 0x7F, /*     Logical Maximum (32767)                         */
    0x75, 0x10,       /*     Report Size (16)                                */
    0x95, 0x04,       /*     Report Count (4)                                */
    0x81, 0x02,       /*     Input (Data, Variable, Absolute)                */
    0xC0,             /*   End Collection (Physical)                         */

    /* ---- Analog triggers ------------------------------------------------ */
    0x05, 0x02,       /*   Usage Page (Simulation Controls)                  */
    0x09, 0xC5,       /*   Usage (Brake) -- left trigger                     */
    0x09, 0xC4,       /*   Usage (Accelerator) -- right trigger              */
    0x15, 0x00,       /*   Logical Minimum (0)                               */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255)                             */
    0x75, 0x08,       /*   Report Size (8)                                   */
    0x95, 0x02,       /*   Report Count (2)                                  */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute)                  */

    0xC0              /* End Collection (Application)                        */
};

const uint16_t gamepad_report_map_size = sizeof(gamepad_report_map);

/* ---- Button mapping ----------------------------------------------------- */

typedef struct
{
    uint8_t  hid_button;   /**< 1-based HID button number. */
    uint32_t valve_mask;
} button_map_t;

static const button_map_t m_button_map[] =
{
    {  1u, VALVE_BTN_A },
    {  2u, VALVE_BTN_B },
    {  4u, VALVE_BTN_X },
    {  5u, VALVE_BTN_Y },
    {  7u, VALVE_BTN_LB },
    {  8u, VALVE_BTN_RB },
    {  9u, VALVE_BTN_LT_FULL },
    { 10u, VALVE_BTN_RT_FULL },
    { 11u, VALVE_BTN_BACK },
    { 12u, VALVE_BTN_START },
    { 13u, VALVE_BTN_STEAM },
    { 14u, VALVE_BTN_STICK_CLICK },
    { 15u, VALVE_BTN_RPAD_CLICK },
    { 17u, VALVE_BTN_LGRIP },
    { 18u, VALVE_BTN_RGRIP },
    /* 19, the left pad click, is handled separately. */
};

#define BUTTON_MAP_COUNT        (sizeof(m_button_map) / sizeof(m_button_map[0]))
#define HID_BUTTON_LPAD_CLICK   19u

/* ---- Right trackpad as a stick ------------------------------------------ */

/*
 * The trackpads sit on the controller at an angle. SDL rotates the right pad
 * by +15 degrees so that "up" on the pad is up for the player, and so do we.
 * cos(15) and sin(15) in Q15.
 */
#define COS15_Q15   31651
#define SIN15_Q15    8481

/* ---- Helpers ------------------------------------------------------------ */

static void put_le16(uint8_t * p_dst, int32_t val)
{
    p_dst[0] = (uint8_t)((uint32_t)val & 0xFFu);
    p_dst[1] = (uint8_t)(((uint32_t)val >> 8) & 0xFFu);
}

/** Clamp to the descriptor's symmetric range so -32768 never reaches the host. */
static int32_t axis_clamp(int32_t val)
{
    if (val > 32767)
    {
        return 32767;
    }
    if (val < -32767)
    {
        return -32767;
    }
    return val;
}

/** HID hat value, 1 = N clockwise to 8 = NW, 0 = centred (the null state). */
static uint8_t hat_from_dpad(uint32_t buttons)
{
    bool up    = (buttons & VALVE_BTN_DPAD_UP)    != 0u;
    bool down  = (buttons & VALVE_BTN_DPAD_DOWN)  != 0u;
    bool left  = (buttons & VALVE_BTN_DPAD_LEFT)  != 0u;
    bool right = (buttons & VALVE_BTN_DPAD_RIGHT) != 0u;

    /* Opposing directions cancel rather than picking one arbitrarily. */
    if (up && down)
    {
        up = down = false;
    }
    if (left && right)
    {
        left = right = false;
    }

    if (up)
    {
        return right ? 2u : (left ? 8u : 1u);
    }
    if (down)
    {
        return right ? 4u : (left ? 6u : 5u);
    }
    if (right)
    {
        return 3u;
    }
    if (left)
    {
        return 7u;
    }
    return 0u;
}

void gamepad_pack_report(const sc_state_t * p_state, uint8_t * p_report)
{
    uint32_t hid_buttons = 0u;

    for (uint8_t i = 0u; i < BUTTON_MAP_COUNT; i++)
    {
        if ((p_state->buttons & m_button_map[i].valve_mask) != 0u)
        {
            hid_buttons |= 1uL << (m_button_map[i].hid_button - 1u);
        }
    }

    /* A click on one of the pad's arrows is already the hat; only a click
     * anywhere else on the left pad is a button of its own. */
    if (((p_state->buttons & VALVE_BTN_LPAD_CLICK) != 0u) &&
        ((p_state->buttons & VALVE_BTN_DPAD_ANY) == 0u))
    {
        hid_buttons |= 1uL << (HID_BUTTON_LPAD_CLICK - 1u);
    }

    p_report[0] = (uint8_t)(hid_buttons & 0xFFu);
    p_report[1] = (uint8_t)((hid_buttons >> 8) & 0xFFu);
    p_report[2] = (uint8_t)((hid_buttons >> 16) & 0x07u);

    p_report[3] = hat_from_dpad(p_state->buttons);

    /* HID Y grows downwards and the controller's Y grows upwards: negate. */
    put_le16(&p_report[4], axis_clamp(p_state->lstick_x));
    put_le16(&p_report[6], axis_clamp(-(int32_t)p_state->lstick_y));

    int32_t rx = 0;
    int32_t ry = 0;
    if ((p_state->buttons & VALVE_BTN_RPAD_TOUCH) != 0u)
    {
        int32_t x = p_state->rpad_x;
        int32_t y = p_state->rpad_y;

        /* Arithmetic right shift of a negative value is implementation
         * defined in C, but arm-none-eabi-gcc and the host gcc both shift
         * arithmetically, which is what this wants. */
        rx = (COS15_Q15 * x - SIN15_Q15 * y) >> 15;
        ry = (SIN15_Q15 * x + COS15_Q15 * y) >> 15;
    }
    /* Finger off the pad: the stick is centred, not wherever it was left. */
    put_le16(&p_report[8],  axis_clamp(rx));
    put_le16(&p_report[10], axis_clamp(-ry));

    p_report[12] = p_state->ltrig;
    p_report[13] = p_state->rtrig;
}
