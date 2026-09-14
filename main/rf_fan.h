/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <driver/gpio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/*  Hardware configuration                                                    */
/* ------------------------------------------------------------------------- */

/* SYN115 transmitter DATA pin. This pin gates the 433 MHz carrier on/off
 * (On-Off-Keying). Wire it to the "DATA"/"ASK" pin of the SYN115 board.
 * NOTE: GPIO22 exists on the classic ESP32 and ESP32-S3. On ESP32-C3 the
 * highest GPIO is 21, so pick a different pin there. */
#define RF_TX_GPIO   GPIO_NUM_22

/* SYN480R receiver DATA pin, used only by the built-in "learning" sniffer so
 * you can capture the codes your original fan remote sends.
 * Set to GPIO_NUM_NC to disable the sniffer entirely. */
#define RF_RX_GPIO   GPIO_NUM_21

/* Build the code-learning sniffer in (1) or leave it out (0). */
#define RF_FAN_ENABLE_SNIFFER   1

/* ------------------------------------------------------------------------- */
/*  OOK / EV1527 protocol timing (typical for cheap 433 MHz remotes)          */
/*  Adjust "base" to the value the sniffer reports for your remote.           */
/* ------------------------------------------------------------------------- */
#define RF_BIT_BASE_US     350   /* one time unit (alpha), microseconds       */
#define RF_SYNC_LOW_MULT   31    /* sync gap = base * 31                       */
#define RF_TX_REPEATS      8     /* how many times each frame is re-sent       */
#define RF_MAX_CODE_BITS   32    /* upper bound for a single code word         */

/* ------------------------------------------------------------------------- */
/*  Public API                                                                */
/* ------------------------------------------------------------------------- */

/** Discrete fan speeds the driver knows how to transmit.
 *  These map 1:1 onto the four buttons of the ventilation remote and onto the
 *  Matter FanMode enum (FanModeSequence = Off-Low-Med-High):
 *
 *    enum value     Matter FanMode   remote button        behaviour
 *    -----------    --------------   ------------------   -----------------------
 *    RF_FAN_OFF     Off   (0)        "Power 1" (lowest)   minimum ventilation
 *    RF_FAN_LOW     Low   (1)        "Power 2"            speed 1
 *    RF_FAN_MEDIUM  Medium(2)        "Power 3"            speed 2
 *    RF_FAN_HIGH    High  (3)        "15 min max power"   boost, auto-returns to
 *                                                         RF_FAN_OFF after 15 min
 *
 *  NOTE: this remote has no true "off"; Power 1 is the lowest continuous speed,
 *  which we expose as Matter's Off/speed-0 per the fan's design.
 */
typedef enum {
    RF_FAN_OFF = 0,
    RF_FAN_LOW,
    RF_FAN_MEDIUM,
    RF_FAN_HIGH,
    RF_FAN_SPEED_MAX,   /* sentinel == number of speeds */
} rf_fan_speed_t;

/** Callback invoked (from the receiver task) when a received 433 MHz code
 *  matches one of the known fan codes in the table. Use it to keep the Matter
 *  state in sync when someone uses the physical remote. */
typedef void (*rf_fan_rx_cb_t)(rf_fan_speed_t speed);

/** Register the callback fired when the receiver recognises a known fan code.
 *  Pass NULL to unregister. */
void rf_fan_register_rx_cb(rf_fan_rx_cb_t cb);

/** Initialise the 433 MHz transmitter (and, if enabled, the receiver/sniffer).
 *
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t rf_fan_init(void);

/** Transmit the RF command mapped to the requested fan speed.
 *  The code words are defined in the table at the top of rf_fan.cpp. */
esp_err_t rf_fan_set_speed(rf_fan_speed_t speed);

/** Low-level helper: transmit an EV1527/PT2262-style OOK code word.
 *  Exposed so you can test individual codes from the console if needed. */
esp_err_t rf_fan_send_ev1527(uint32_t code, uint8_t nbits);

#ifdef __cplusplus
}
#endif
