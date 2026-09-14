/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>

#include <esp_log.h>
#include <esp_check.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <driver/rmt_tx.h>
#include <driver/rmt_rx.h>
#include <driver/rmt_encoder.h>

#include "rf_fan.h"

static const char *TAG = "rf_fan";

/* ========================================================================= */
/*  FAN CODE TABLE  --  >>> EDIT THIS <<<                                     */
/*                                                                           */
/*  These are the OOK code words your fan's remote sends for each speed.     */
/*  They start as 0x000000 placeholders, so the fan will NOT do anything     */
/*  until you fill them in.                                                  */
/*                                                                           */
/*  How to learn them:                                                       */
/*   1. Flash & run this firmware (the sniffer starts automatically).        */
/*   2. Watch the serial monitor and press a button on your real remote.     */
/*   3. Copy the "LEARN: captured NN-bit code = 0x......" value below.       */
/*   4. Also set RF_BIT_BASE_US in rf_fan.h to the reported "base ~NN us".   */
/* ========================================================================= */
typedef struct {
    uint32_t code;   /* the code word to transmit                       */
    uint8_t  nbits;  /* number of bits in the code word (often 24)      */
} rf_command_t;

/* Order MUST match rf_fan_speed_t. Each entry is one remote button. */
static const rf_command_t s_fan_codes[RF_FAN_SPEED_MAX] = {
    /* RF_FAN_OFF    -> remote "Power 1"  (lowest)        */ { 0x000000, 24 },
    /* RF_FAN_LOW    -> remote "Power 2"                  */ { 0x000000, 24 },
    /* RF_FAN_MEDIUM -> remote "Power 3"                  */ { 0x000000, 24 },
    /* RF_FAN_HIGH   -> remote "15 min max power" (boost) */ { 0x000000, 24 },
};

/* Callback fired when the receiver recognises one of the codes above. */
static rf_fan_rx_cb_t s_rx_cb = NULL;

void rf_fan_register_rx_cb(rf_fan_rx_cb_t cb)
{
    s_rx_cb = cb;
}

/* ========================================================================= */
/*  RMT transmitter (OOK)                                                    */
/* ========================================================================= */

#define RF_RESOLUTION_HZ   1000000            /* 1 tick = 1 us               */
#define RF_MEM_BLOCK_SYMS  48                 /* safe across all ESP32 SoCs  */

static rmt_channel_handle_t s_tx_chan   = NULL;
static rmt_encoder_handle_t s_copy_enc  = NULL;
static SemaphoreHandle_t    s_tx_mutex  = NULL;
static QueueHandle_t        s_tx_queue  = NULL;   /* speed requests -> TX task */

/* Build one RMT symbol: carrier HIGH for high_us, then LOW for low_us. */
static inline rmt_symbol_word_t rf_symbol(uint16_t high_us, uint16_t low_us)
{
    rmt_symbol_word_t s;
    s.level0    = 1;
    s.duration0 = high_us;
    s.level1    = 0;
    s.duration1 = low_us;
    return s;
}

/* Encode a single EV1527/PT2262-style frame (MSB first) followed by the sync
 * gap into buf. Returns the number of symbols written. */
static size_t rf_encode_frame(rmt_symbol_word_t *buf, uint32_t code, uint8_t nbits)
{
    const uint16_t a = RF_BIT_BASE_US;
    size_t n = 0;
    for (int i = nbits - 1; i >= 0; i--) {
        if ((code >> i) & 0x1) {
            buf[n++] = rf_symbol(3 * a, 1 * a);   /* '1' : long-high, short-low  */
        } else {
            buf[n++] = rf_symbol(1 * a, 3 * a);   /* '0' : short-high, long-low  */
        }
    }
    buf[n++] = rf_symbol(1 * a, RF_SYNC_LOW_MULT * a);   /* sync / inter-frame gap */
    return n;
}

esp_err_t rf_fan_send_ev1527(uint32_t code, uint8_t nbits)
{
    if (nbits == 0 || nbits > RF_MAX_CODE_BITS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }

    /* One frame = nbits data symbols + 1 sync symbol, repeated RF_TX_REPEATS. */
    static rmt_symbol_word_t buf[(RF_MAX_CODE_BITS + 1) * RF_TX_REPEATS];

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);

    size_t one = rf_encode_frame(buf, code, nbits);
    for (int r = 1; r < RF_TX_REPEATS; r++) {
        memcpy(&buf[r * one], buf, one * sizeof(rmt_symbol_word_t));
    }
    size_t total = one * RF_TX_REPEATS;

    rmt_transmit_config_t txc = {};
    txc.loop_count = 0;   /* the repeats are already baked into the buffer */

    esp_err_t err = rmt_transmit(s_tx_chan, s_copy_enc, buf,
                                 total * sizeof(rmt_symbol_word_t), &txc);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_tx_chan, 1000 /* ms */);
    }

    xSemaphoreGive(s_tx_mutex);
    return err;
}

/* Transmit the code word for a speed. Called only from the TX task, because a
 * full burst blocks for a few hundred milliseconds. */
static void rf_transmit_speed(rf_fan_speed_t speed)
{
    const rf_command_t *c = &s_fan_codes[speed];
    if (c->code == 0x000000) {
        ESP_LOGW(TAG, "Speed %d has no code yet - learn it with the sniffer and "
                      "fill s_fan_codes[] in rf_fan.cpp", (int)speed);
        return;
    }
    ESP_LOGI(TAG, "TX fan speed %d -> code 0x%06lX (%u bits)",
             (int)speed, (unsigned long)c->code, c->nbits);
    rf_fan_send_ev1527(c->code, c->nbits);
}

static void rf_tx_task(void *arg)
{
    rf_fan_speed_t speed;
    while (true) {
        if (xQueueReceive(s_tx_queue, &speed, portMAX_DELAY) == pdTRUE) {
            rf_transmit_speed(speed);
        }
    }
}

esp_err_t rf_fan_set_speed(rf_fan_speed_t speed)
{
    if (speed < 0 || speed >= RF_FAN_SPEED_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_tx_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Non-blocking: hand the request to the TX task so callers (the Matter
     * stack thread, the button task, the boost timer) never block on the
     * multi-hundred-ms RF burst. */
    if (xQueueSend(s_tx_queue, &speed, 0) != pdTRUE) {
        ESP_LOGW(TAG, "TX queue full, dropping fan speed %d", (int)speed);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ========================================================================= */
/*  RMT receiver / code-learning sniffer                                     */
/* ========================================================================= */
#if RF_FAN_ENABLE_SNIFFER

#define RF_RX_BUF_SYMBOLS  64

/* Debug logging for the OOK sniffer. Kept off: the target remote uses an
 * nRF905 (GFSK) transceiver, which the SYN480R (OOK/ASK) cannot demodulate, so
 * this sniffer only ever produces receiver noise for this device. */
#define RF_RX_DEBUG        0

static rmt_channel_handle_t s_rx_chan = NULL;
static QueueHandle_t        s_rx_queue = NULL;
static rmt_symbol_word_t    s_rx_symbols[RF_RX_BUF_SYMBOLS];

/* De-bounce state for recognised codes (see rf_decode_and_log). */
static rf_fan_speed_t s_last_rx_speed = RF_FAN_SPEED_MAX;
static int64_t        s_last_rx_time  = 0;

static bool IRAM_ATTR rf_rx_done_cb(rmt_channel_handle_t chan,
                                    const rmt_rx_done_event_data_t *edata,
                                    void *ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    size_t num = edata->num_symbols;
    xQueueSendFromISR(s_rx_queue, &num, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

/* Decode one clean EV1527/PT2262 word out of a captured pulse train. If it
 * matches a known fan code, fire the sync callback; otherwise log it so it can
 * be pasted into s_fan_codes[]. */
static void rf_decode_and_log(const rmt_symbol_word_t *sym, size_t num)
{
#if RF_RX_DEBUG
    ESP_LOGI(TAG, "RXdbg: captured %u symbols", (unsigned)num);
    for (size_t i = 0; i < num && i < 10; i++) {
        ESP_LOGI(TAG, "RXdbg:   [%2u] hi=%5u us  lo=%5u us",
                 (unsigned)i, sym[i].duration0, sym[i].duration1);
    }
#endif
    if (num < 12) {
#if RF_RX_DEBUG
        ESP_LOGW(TAG, "RXdbg: dropped (only %u symbols, need >=12)", (unsigned)num);
#endif
        return;   /* too short to be a real remote code */
    }

    /* Estimate the base time unit (alpha) as the shortest observed pulse. */
    uint32_t alpha = UINT32_MAX;
    for (size_t i = 0; i < num; i++) {
        if (sym[i].duration0 && sym[i].duration0 < alpha) alpha = sym[i].duration0;
        if (sym[i].duration1 && sym[i].duration1 < alpha) alpha = sym[i].duration1;
    }
#if RF_RX_DEBUG
    ESP_LOGI(TAG, "RXdbg: shortest pulse (alpha) = %u us", (unsigned)alpha);
#endif
    if (alpha == UINT32_MAX || alpha < 100) {
#if RF_RX_DEBUG
        ESP_LOGW(TAG, "RXdbg: dropped (alpha %u < 100 us -> noise)", (unsigned)alpha);
#endif
        return;   /* implausible -> most likely receiver noise */
    }
    const uint32_t sync_thresh = 8 * alpha;   /* a low longer than this = sync */

    /* Skip to the symbol right after the first sync gap so we decode a whole
     * word that sits between two frames. */
    size_t start = 0;
    for (size_t i = 0; i < num; i++) {
        if (sym[i].duration1 && sym[i].duration1 > sync_thresh) {
            start = i + 1;
            break;
        }
    }

    uint32_t code = 0;
    int bits = 0;
    for (size_t i = start; i < num && bits < RF_MAX_CODE_BITS; i++) {
        uint32_t hi = sym[i].duration0;
        uint32_t lo = sym[i].duration1;
        if (hi == 0) break;                     /* end of buffer            */
        if (lo == 0 || lo > sync_thresh) break; /* reached the next sync    */
        code = (code << 1) | (hi > lo ? 1u : 0u);
        bits++;
    }

    if (bits < 12) {
#if RF_RX_DEBUG
        ESP_LOGW(TAG, "RXdbg: dropped (decoded only %d bits, need >=12)", bits);
#endif
        return;
    }

    /* Does this match one of our known fan codes? If so, keep Matter in sync
     * with the physical remote. */
    for (int s = 0; s < RF_FAN_SPEED_MAX; s++) {
        if (s_fan_codes[s].code != 0x000000 &&
            s_fan_codes[s].code == code && s_fan_codes[s].nbits == bits) {
            /* De-bounce: a held button repeats the same code many times. Only
             * act on a change of speed, or after ~1.5 s of quiet (a fresh press). */
            int64_t now = esp_timer_get_time();
            bool changed = ((rf_fan_speed_t)s != s_last_rx_speed);
            bool fresh   = (now - s_last_rx_time) > 1500000;
            if (changed || fresh || s_last_rx_speed == RF_FAN_SPEED_MAX) {
                ESP_LOGI(TAG, "RX: recognised remote code 0x%0*lX -> fan speed %d (syncing)",
                         (bits + 3) / 4, (unsigned long)code, s);
                if (s_rx_cb) {
                    s_rx_cb((rf_fan_speed_t)s);
                }
            }
            s_last_rx_speed = (rf_fan_speed_t)s;
            s_last_rx_time  = now;
            return;
        }
    }

    /* Unknown code -> learning aid. */
    ESP_LOGI(TAG,
             "LEARN: captured %d-bit code = 0x%0*lX  (base ~%u us)  "
             "<- paste into s_fan_codes[] and set RF_BIT_BASE_US",
             bits, (bits + 3) / 4, (unsigned long)code, (unsigned)alpha);
}

static void rf_rx_task(void *arg)
{
    rmt_receive_config_t rxc = {};
    /* Glitch filter min pulse width. The RMT RX filter is an 8-bit counter
     * clocked at 80 MHz APB, so the max is 255/80MHz ~= 3187 ns on the ESP32-C6
     * (larger values make rmt_receive() return ESP_ERR_INVALID_ARG). 3 us is
     * safely under that and still far below a real ~350 us bit pulse; the
     * software decoder rejects any remaining noise. */
    rxc.signal_range_min_ns = 3 * 1000;          /* ignore < 3 us glitches   */
    rxc.signal_range_max_ns = 15 * 1000 * 1000;  /* > 15 ms idle ends a frame */

    ESP_ERROR_CHECK(rmt_receive(s_rx_chan, s_rx_symbols, sizeof(s_rx_symbols), &rxc));

    size_t num;
    while (true) {
        if (xQueueReceive(s_rx_queue, &num, portMAX_DELAY) == pdTRUE) {
            rf_decode_and_log(s_rx_symbols, num);
            /* re-arm for the next frame */
            rmt_receive(s_rx_chan, s_rx_symbols, sizeof(s_rx_symbols), &rxc);
        }
    }
}

static esp_err_t rf_rx_init(void)
{
    if (RF_RX_GPIO == GPIO_NUM_NC) {
        ESP_LOGI(TAG, "Sniffer disabled (RF_RX_GPIO == NC)");
        return ESP_OK;
    }

    rmt_rx_channel_config_t rx_cfg = {};
    rx_cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
    rx_cfg.resolution_hz     = RF_RESOLUTION_HZ;
    rx_cfg.mem_block_symbols = RF_MEM_BLOCK_SYMS;   /* HW staging block (<=48) */
    rx_cfg.gpio_num          = RF_RX_GPIO;
    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&rx_cfg, &s_rx_chan), TAG, "rx channel");

    s_rx_queue = xQueueCreate(4, sizeof(size_t));
    ESP_RETURN_ON_FALSE(s_rx_queue, ESP_ERR_NO_MEM, TAG, "rx queue");

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = rf_rx_done_cb;
    ESP_RETURN_ON_ERROR(rmt_rx_register_event_callbacks(s_rx_chan, &cbs, NULL),
                        TAG, "rx callbacks");
    ESP_RETURN_ON_ERROR(rmt_enable(s_rx_chan), TAG, "rx enable");

    xTaskCreate(rf_rx_task, "rf_sniffer", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "433 MHz receiver running on GPIO%d - learns unknown codes and "
                  "syncs Matter state from known ones", (int)RF_RX_GPIO);
    return ESP_OK;
}
#endif /* RF_FAN_ENABLE_SNIFFER */

/* ========================================================================= */
/*  Init                                                                     */
/* ========================================================================= */

esp_err_t rf_fan_init(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_tx_mutex, ESP_ERR_NO_MEM, TAG, "tx mutex");

    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz     = RF_RESOLUTION_HZ;
    tx_cfg.mem_block_symbols = RF_MEM_BLOCK_SYMS;
    tx_cfg.trans_queue_depth = 4;
    tx_cfg.gpio_num          = RF_TX_GPIO;
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &s_tx_chan), TAG, "tx channel");

    rmt_copy_encoder_config_t copy_cfg = {};
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&copy_cfg, &s_copy_enc), TAG, "copy encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_tx_chan), TAG, "tx enable");

    s_tx_queue = xQueueCreate(8, sizeof(rf_fan_speed_t));
    ESP_RETURN_ON_FALSE(s_tx_queue, ESP_ERR_NO_MEM, TAG, "tx queue");
    xTaskCreate(rf_tx_task, "rf_tx", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "433 MHz transmitter ready on GPIO%d", (int)RF_TX_GPIO);

#if RF_FAN_ENABLE_SNIFFER
    ESP_RETURN_ON_ERROR(rf_rx_init(), TAG, "rx init");
#endif
    return ESP_OK;
}
