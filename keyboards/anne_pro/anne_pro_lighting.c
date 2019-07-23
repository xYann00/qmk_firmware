/* Copyright 2019 Michiel Visser
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "anne_pro_lighting.h"
#include "uart_tx_ringbuf.h"
#include "quantum.h"
#include "ch.h"
#include "hal.h"

#define LED_UART (&UARTD3)
/* UART transmission ringbuffer for the LED UART */
static uint8_t led_uart_tx_buffer[256] = {0};
static uart_tx_ringbuf_t led_uart_ringbuf = {
    .buf = led_uart_tx_buffer,
    .uart = LED_UART,
    .size = sizeof(led_uart_tx_buffer),
    .sending_elements = 0,
    .head = 0,
    .tail = 0,
};

/* Handler for finsihed LED UART transmissions */
static void led_uart_txend(UARTDriver *uart) {
    uart_tx_ringbuf_finish_transmission(&led_uart_ringbuf);
}

/* LED UART configuration */
static UARTConfig led_uart_cfg = {
    .txend1_cb = led_uart_txend,
    .txend2_cb = NULL,
    .rxend_cb = NULL,
    .rxchar_cb = NULL,
    .rxerr_cb = NULL,
    .speed = 38400,
    .cr1 = 0,
    .cr2 = USART_CR2_LINEN,
    .cr3 = 0
};

/* State of the leds on the keyboard */
static bool leds_awake = false;
static bool backlight_active = false;
static bool capslock_active = false;

void anne_pro_lighting_init(void) {
    /* Turn on lighting controller */
    writePinLow(C15);
    setPinOutput(C15);

    /* Initialize the lighting UART */
    uartStart(LED_UART, &led_uart_cfg);
    palSetPadMode(GPIOB, 10, PAL_MODE_ALTERNATE(7));
    palSetPadMode(GPIOB, 11, PAL_MODE_ALTERNATE(7));
}

/* Handle state changes of keyboard LEDs (numlock, capslock) */
void led_set_kb(uint8_t usb_led) {
    /* Handle CapsLock led */
    if (IS_LED_ON(usb_led, USB_LED_CAPS_LOCK)) {
        capslock_active = true;
        /* Make sure the lighting chip is awake */
        anne_pro_lighting_on();
        /* Turn the capslock indicator on */
        uart_tx_ringbuf_write(&led_uart_ringbuf, 4, "\x09\x02\x0c\x01");
        uart_tx_ringbuf_start_transmission(&led_uart_ringbuf);
    } else {
        /* Turn the capslock indicator off */
        uart_tx_ringbuf_write(&led_uart_ringbuf, 4, "\x09\x02\x0c\x00");
        uart_tx_ringbuf_start_transmission(&led_uart_ringbuf);
        /* Mark capslock as incative, this will automatically sleep the led
           controller when possible */
        capslock_active = false;
    }

    /* Handle any possible user code */
    led_set_user(usb_led);
}

/* Buffer for the keystate packet */
static uint8_t keystate[12] = {9, 10, 7, 0};

/* Update the dynamic lighting packet based on a keypress */
void anne_pro_lighting_update_dynamic(keyrecord_t *record) {
    /* Make sure this is actually a keypress event */
    if (IS_NOEVENT(record->event)) return;
    /* Only update dynamic lighting modes when leds are enabled */
    if (backlight_active) {
        /* Calculate the position of the key that was pressed */
        uint8_t row = record->event.key.row;
        uint8_t col = record->event.key.col;
        int position = row * MATRIX_COLS + col;
        int index = position / 8;
        int bit = position % 8;

        /* Update the keystate based on the location */
        if (record->event.pressed) {
            keystate[3 + index] |= (1 << bit);
        } else {
            keystate[3 + index] &= ~(1 << bit);
        }

        /* Send the keystate to the LED controller */
        uart_tx_ringbuf_write(&led_uart_ringbuf, 12, keystate);
        uart_tx_ringbuf_start_transmission(&led_uart_ringbuf);
    }
}

/* Update lighting, should be called every matrix scan */
void anne_pro_lighting_update(void) {
    if (!uart_tx_ringbuf_empty(&led_uart_ringbuf)) {
        uart_tx_ringbuf_start_transmission(&led_uart_ringbuf);
    }

    /* Check if there are users of the lighting system, either backlight or capslock indicator */
    bool user_active = backlight_active || capslock_active;
    /* If the leds are awake and there are no active users, sleep the lighting controller */
    if (leds_awake && !user_active) {
        anne_pro_lighting_off();
    }
}

/* Toggle the lighting on/off */
void anne_pro_lighting_toggle(void) {
    if (!backlight_active) {
        anne_pro_lighting_on();
        anne_pro_lighting_mode_last();
    } else {
        anne_pro_lighting_mode(APL_MODE_OFF);
    }
}

/* Turn the lighting on */
void anne_pro_lighting_on(void) {
    if (leds_awake) return;
    /* Wake up the LED controller */
    writePinHigh(C15);
    chThdSleepMilliseconds(50);
    leds_awake = true;
}

/* Turn the lighting off */
void anne_pro_lighting_off(void) {
    /* Sleep the LED controller */
    writePinLow(C15);
    leds_awake = false;
}

/* Is the backlight enabled? */
bool anne_pro_lighting_enabled(void) {
    return backlight_active;
}

/* Select the next effect rate */
void anne_pro_lighting_rate_next(void) {
    if (backlight_active) {
        uart_tx_ringbuf_write(&led_uart_ringbuf, 6, "\x09\x04\x05\x00\x01\x00");
    }
}

/* Select the next brightness */
void anne_pro_lighting_brightness_next(void) {
    if (backlight_active) {
        uart_tx_ringbuf_write(&led_uart_ringbuf, 6, "\x09\x04\x05\x00\x00\x01");
    }
}

/* Select the next lighting mode */
void anne_pro_lighting_mode_next(void) {
    if (backlight_active) {
        uart_tx_ringbuf_write(&led_uart_ringbuf, 6, "\x09\x04\x05\x01\x00\x00");
    }
}

/* Set the lighting mode */
void anne_pro_lighting_mode(uint8_t mode) {
    if (leds_awake) {
        uint8_t buf[] = {9, 2, 1, mode};
        uart_tx_ringbuf_write(&led_uart_ringbuf, 4, buf);
        uart_tx_ringbuf_start_transmission(&led_uart_ringbuf);
        backlight_active = (mode != 0);
    }
}

/* Set the lighting mode to the last lighting mode */
void anne_pro_lighting_mode_last(void) {
    if (leds_awake) {
        uart_tx_ringbuf_write(&led_uart_ringbuf, 3, "\x09\x01\x01");
        uart_tx_ringbuf_start_transmission(&led_uart_ringbuf);
        backlight_active = true;
    }
}

/* Set the rate and brightness */
void anne_pro_lighting_rate_brightness(uint8_t rate, uint8_t brightness) {
    if (backlight_active) {
        if (brightness > 10) brightness = 10;

        uint8_t buf[] = {9, 4, 2, rate, brightness, 0};
        uart_tx_ringbuf_write(&led_uart_ringbuf, 6, buf);
        uart_tx_ringbuf_start_transmission(&led_uart_ringbuf);
    }
}

/* Set lighting for individual keys, this takes number of keys and a payload
   that describes which keys and the colors of those keys */
void anne_pro_lighting_set_keys(uint8_t keys, uint8_t *payload) {
    if (leds_awake) {
        uint8_t buf[] = {9, 3 + 5 * keys, 11, 202, keys};
        uart_tx_ringbuf_write(&led_uart_ringbuf, 5, buf);
        uart_tx_ringbuf_write(&led_uart_ringbuf, 5 * keys, payload);
        backlight_active = true;
    }
}
