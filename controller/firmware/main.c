/*
 * Giant Game Boy controller — Raspberry Pi Pico USB HID gamepad
 *
 *  - 11 buttons on GP0-GP10 (active low, internal pull-ups)
 *      GP0 power switch (bistable)  GP1 Left    GP2 Select   GP3 Down
 *      GP4 Right                    GP5 Start   GP6 B        GP7 Menu
 *      GP8 A                        GP9 Up      GP10 Right bumper
 *  - Volume pot on GP27 / ADC1, reported as an absolute Rz axis
 *  - Red power LED on GP15 (via 2N2222 low-side switch) follows the power switch
 *  - Debounce: a button must read the same for 3 polls (30 ms) to change
 *
 * Shows up as "Custom Gamepad" (VID 0xCAFE, PID 0x4004). No drivers needed.
 *
 * Copyright (c) 2026 Nick Alvarez — MIT License.
 * Based on the TinyUSB hid_composite example (MIT, Ha Thach / tinyusb.org).
 */
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "usb_descriptors.h"

/* ---------------- Pin map ---------------- */
#define BUTTON_COUNT    11
#define POWER_BUTTON    0          /* index of the bistable power switch  */
#define LED_PIN         15         /* power LED driver                     */
#define POT_GPIO        27         /* ADC1                                 */
#define POT_ADC_INPUT   1

static const uint8_t button_pins[BUTTON_COUNT] = {
    0,   /* power switch  */
    1,   /* D-pad left    */
    2,   /* select        */
    3,   /* D-pad down    */
    4,   /* D-pad right   */
    5,   /* start         */
    6,   /* B             */
    7,   /* menu          */
    8,   /* A             */
    9,   /* D-pad up      */
    10,  /* right bumper  */
};

/* ---------------- Tuning ---------------- */
#define POLL_MS          10
#define DEBOUNCE_COUNT   3          /* 3 x 10 ms = 30 ms                    */
#define POT_SAMPLES      32         /* averaging                             */
#define POT_DEADBAND     3          /* ignore jitter of +/-3 axis counts     */
/* Measured pot range: 47k pot with 47k pull-up to 3V3 gives ~0-1.65 V,
 * which reads as roughly 17-2048 counts on the 12-bit ADC. */
#define POT_RAW_MIN      17
#define POT_RAW_MAX      2048

static uint16_t stable_buttons = 0;

static void buttons_init(void) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        gpio_init(button_pins[i]);
        gpio_set_dir(button_pins[i], GPIO_IN);
        gpio_pull_up(button_pins[i]);
    }
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);
}

static uint16_t read_buttons_raw(void) {
    uint16_t state = 0;
    for (int i = 0; i < BUTTON_COUNT; i++)
        if (!gpio_get(button_pins[i])) state |= (1u << i);   /* pressed = low */
    return state;
}

/* Per-button counter debounce: long wires in the shell pick up noise,
 * which used to cause phantom Start presses. */
static void debounce_update(void) {
    static uint8_t count[BUTTON_COUNT];
    uint16_t raw = read_buttons_raw();
    for (int i = 0; i < BUTTON_COUNT; i++) {
        uint16_t bit = 1u << i;
        if ((raw & bit) != (stable_buttons & bit)) {
            if (++count[i] >= DEBOUNCE_COUNT) {
                stable_buttons ^= bit;
                count[i] = 0;
            }
        } else {
            count[i] = 0;
        }
    }
}

static void pot_init(void) {
    adc_init();
    adc_gpio_init(POT_GPIO);
    adc_select_input(POT_ADC_INPUT);
}

/* Returns the pot position as -127..127 for the Rz axis. */
static int8_t read_pot_axis(void) {
    static int8_t last = 0;
    uint32_t sum = 0;
    for (int i = 0; i < POT_SAMPLES; i++) sum += adc_read();
    int32_t raw = (int32_t)(sum / POT_SAMPLES);
    if (raw < POT_RAW_MIN) raw = POT_RAW_MIN;
    if (raw > POT_RAW_MAX) raw = POT_RAW_MAX;
    int32_t axis = (raw - POT_RAW_MIN) * 254 / (POT_RAW_MAX - POT_RAW_MIN) - 127;
    if (abs(axis - last) > POT_DEADBAND || axis == 127 || axis == -127) last = (int8_t)axis;
    return last;
}

/* The red power LED mirrors the power switch. */
static void power_led_task(void) {
    gpio_put(LED_PIN, (stable_buttons & (1u << POWER_BUTTON)) ? 1 : 0);
}

static void hid_task(void) {
    static uint32_t next_ms = 0;
    static uint16_t sent_buttons = 0xFFFF;
    static int8_t   sent_rz = 0;

    uint32_t now = board_millis();
    if ((int32_t)(now - next_ms) < 0) return;
    next_ms = now + POLL_MS;

    debounce_update();
    power_led_task();
    int8_t rz = read_pot_axis();

    if (tud_suspended() && stable_buttons) { tud_remote_wakeup(); return; }
    if (!tud_hid_ready()) return;
    if (stable_buttons == sent_buttons && rz == sent_rz) return;   /* only send changes */

    hid_gamepad_report_t report = {
        .x = 0, .y = 0, .z = 0, .rz = rz, .rx = 0, .ry = 0,
        .hat = GAMEPAD_HAT_CENTERED,
        .buttons = stable_buttons,
    };
    if (tud_hid_report(REPORT_ID_GAMEPAD, &report, sizeof(report))) {
        sent_buttons = stable_buttons;
        sent_rz = rz;
    }
}

int main(void) {
    board_init();
    buttons_init();
    pot_init();
    tud_init(BOARD_TUD_RHPORT);
    if (board_init_after_tusb) board_init_after_tusb();

    while (1) {
        tud_task();
        hid_task();
    }
}

/* ---------------- TinyUSB callbacks ---------------- */
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)instance; (void)report; (void)len;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)type; (void)buffer; (void)bufsize;
}
