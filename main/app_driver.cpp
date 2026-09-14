/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_log.h>
#include <esp_timer.h>
#include <stdlib.h>
#include <string.h>

#include <esp_matter.h>
#include <app_priv.h>
#include <common_macros.h>

#include <device.h>
#include <iot_button.h>
#include <button_gpio.h>

#include "rf_fan.h"

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";
extern uint16_t fan_endpoint_id;

/* A dummy token returned as the driver handle. The RF driver keeps its own
 * state internally (see rf_fan.cpp), so the handle is only used to satisfy the
 * esp-matter priv_data plumbing. */
static uint8_t s_fan_driver_token;

/* Remember the last speed we transmitted. The FanControl cluster updates
 * several coupled attributes (FanMode + PercentSetting) for a single user
 * action, so we de-bounce here to avoid sending the same RF burst twice. */
static rf_fan_speed_t s_last_speed = RF_FAN_SPEED_MAX; /* invalid -> force first send */

/* One-shot timer that mirrors the remote's "15 min max power" button: after
 * the boost, the fan returns to speed 0. */
static esp_timer_handle_t s_boost_timer = NULL;

static void app_driver_fan_boost_timeout_cb(void *arg)
{
    ESP_LOGI(TAG, "Boost elapsed (%d min) -> returning fan to speed 0", FAN_BOOST_MINUTES);
    /* Drive the Matter FanMode back to Off (speed 0 / Power 1). This cascades
     * through app_driver_attribute_update() to transmit the RF code, and keeps
     * the state reported to Matter controllers in sync. */
    attribute_t *attribute = attribute::get(fan_endpoint_id, FanControl::Id, FanControl::Attributes::FanMode::Id);
    if (!attribute) {
        return;
    }
    esp_matter_attr_val_t val;
    attribute::get_val(attribute, &val);
    val.val.u8 = static_cast<uint8_t>(FanControl::FanModeEnum::kOff);
    attribute::update(fan_endpoint_id, FanControl::Id, FanControl::Attributes::FanMode::Id, &val);
}

static void app_driver_fan_boost_timer_start()
{
    if (!s_boost_timer) {
        return;
    }
    esp_timer_stop(s_boost_timer);   /* harmless if not running */
    esp_timer_start_once(s_boost_timer, (uint64_t)FAN_BOOST_MINUTES * 60ULL * 1000000ULL);
}

static void app_driver_fan_boost_timer_stop()
{
    if (s_boost_timer) {
        esp_timer_stop(s_boost_timer);
    }
}

/* -------------------------------------------------------------------------- */
/*  Mapping helpers: Matter FanControl <-> discrete RF speeds                  */
/* -------------------------------------------------------------------------- */

/* Matter FanMode -> remote button:
 *   Off/speed0 = Power 1, Low = Power 2, Medium = Power 3, High = 15 min boost */
static rf_fan_speed_t fanmode_to_speed(uint8_t fan_mode)
{
    switch (static_cast<FanControl::FanModeEnum>(fan_mode)) {
    case FanControl::FanModeEnum::kOff:    return RF_FAN_OFF;
    case FanControl::FanModeEnum::kLow:    return RF_FAN_LOW;
    case FanControl::FanModeEnum::kMedium: return RF_FAN_MEDIUM;
    case FanControl::FanModeEnum::kHigh:
    case FanControl::FanModeEnum::kOn:     return RF_FAN_HIGH;
    case FanControl::FanModeEnum::kAuto:
    case FanControl::FanModeEnum::kSmart:  return RF_FAN_MEDIUM;
    default:                               return RF_FAN_OFF;
    }
}

static rf_fan_speed_t percent_to_speed(uint8_t percent)
{
    if (percent == 0)   return RF_FAN_OFF;
    if (percent <= 33)  return RF_FAN_LOW;
    if (percent <= 66)  return RF_FAN_MEDIUM;
    return RF_FAN_HIGH;
}

static uint8_t speed_to_fanmode(rf_fan_speed_t speed)
{
    switch (speed) {
    case RF_FAN_LOW:    return static_cast<uint8_t>(FanControl::FanModeEnum::kLow);
    case RF_FAN_MEDIUM: return static_cast<uint8_t>(FanControl::FanModeEnum::kMedium);
    case RF_FAN_HIGH:   return static_cast<uint8_t>(FanControl::FanModeEnum::kHigh);
    case RF_FAN_OFF:
    default:            return static_cast<uint8_t>(FanControl::FanModeEnum::kOff);
    }
}

static uint8_t speed_to_percent(rf_fan_speed_t speed)
{
    switch (speed) {
    case RF_FAN_LOW:    return 33;
    case RF_FAN_MEDIUM: return 66;
    case RF_FAN_HIGH:   return 100;
    case RF_FAN_OFF:
    default:            return 0;
    }
}

static esp_err_t app_driver_fan_apply_speed(rf_fan_speed_t speed)
{
    if (speed == RF_FAN_HIGH) {
        /* "15 min max power" button. (Re)arm the auto-off timer and always
         * (re)transmit - re-selecting boost should restart the fan's own timer,
         * so we intentionally bypass the de-bounce here. */
        app_driver_fan_boost_timer_start();
        s_last_speed = speed;
        return rf_fan_set_speed(speed);
    }

    /* Any other speed cancels a running boost. */
    app_driver_fan_boost_timer_stop();

    if (speed == s_last_speed) {
        return ESP_OK;   /* de-bounce coupled FanMode/PercentSetting updates */
    }
    s_last_speed = speed;
    return rf_fan_set_speed(speed);
}

/* Report an 8-bit FanControl attribute to Matter WITHOUT invoking the driver
 * callback. attribute::report() updates the value and notifies subscribers but
 * skips PRE_UPDATE, so it does not echo an RF transmit back out. We read the
 * current value first to preserve the exact attribute type (enum8 / nullable). */
static void app_driver_fan_report_u8(uint32_t attribute_id, uint8_t value)
{
    attribute_t *attribute = attribute::get(fan_endpoint_id, FanControl::Id, attribute_id);
    if (!attribute) {
        return;
    }
    esp_matter_attr_val_t val;
    attribute::get_val(attribute, &val);
    val.val.u8 = value;
    attribute::report(fan_endpoint_id, FanControl::Id, attribute_id, &val);
}

/* Called from the RF receiver task when a known remote code is heard, so the
 * Matter state follows what someone did with the physical remote. */
static void app_driver_fan_on_rf_received(rf_fan_speed_t speed)
{
    ESP_LOGI(TAG, "Physical remote detected -> syncing Matter state to speed %d", (int)speed);

    /* Mirror the boost auto-off behaviour: if the remote started the 15-min
     * max, arm our timer too; any other button cancels it. */
    if (speed == RF_FAN_HIGH) {
        app_driver_fan_boost_timer_start();
    } else {
        app_driver_fan_boost_timer_stop();
    }

    /* Remember it as the current state so a later identical Matter command is
     * de-bounced (fan is already there, no need to transmit). */
    s_last_speed = speed;

    /* Reflect the state to Matter controllers without transmitting. */
    app_driver_fan_report_u8(FanControl::Attributes::FanMode::Id, speed_to_fanmode(speed));
    app_driver_fan_report_u8(FanControl::Attributes::PercentSetting::Id, speed_to_percent(speed));
    app_driver_fan_report_u8(FanControl::Attributes::PercentCurrent::Id, speed_to_percent(speed));
}

/* -------------------------------------------------------------------------- */
/*  Local button: cycle Off -> Low -> Medium -> High -> Off                    */
/* -------------------------------------------------------------------------- */

static void app_driver_button_toggle_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Fan button pressed");
    uint16_t endpoint_id = fan_endpoint_id;
    uint32_t cluster_id = FanControl::Id;
    uint32_t attribute_id = FanControl::Attributes::FanMode::Id;

    attribute_t *attribute = attribute::get(endpoint_id, cluster_id, attribute_id);

    esp_matter_attr_val_t val;
    attribute::get_val(attribute, &val);

    /* Cycle within the Off/Low/Medium/High sequence (0..3). */
    uint8_t mode = val.val.u8;
    mode = (mode >= static_cast<uint8_t>(FanControl::FanModeEnum::kHigh)) ? 0 : (mode + 1);
    val.val.u8 = mode;

    /* Updating the attribute triggers app_attribute_update_cb -> the RF TX. */
    attribute::update(endpoint_id, cluster_id, attribute_id, &val);
}

/* -------------------------------------------------------------------------- */
/*  esp-matter driver hooks                                                    */
/* -------------------------------------------------------------------------- */

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    esp_err_t err = ESP_OK;
    if (endpoint_id == fan_endpoint_id && cluster_id == FanControl::Id) {
        if (attribute_id == FanControl::Attributes::FanMode::Id) {
            err = app_driver_fan_apply_speed(fanmode_to_speed(val->val.u8));
        } else if (attribute_id == FanControl::Attributes::PercentSetting::Id) {
            /* PercentSetting is nullable; a null is delivered as 0xFF. */
            if (val->val.u8 <= 100) {
                err = app_driver_fan_apply_speed(percent_to_speed(val->val.u8));
            }
        }
    }
    return err;
}

esp_err_t app_driver_fan_set_defaults(uint16_t endpoint_id)
{
    esp_matter_attr_val_t val;
    attribute_t *attribute = attribute::get(endpoint_id, FanControl::Id, FanControl::Attributes::FanMode::Id);
    attribute::get_val(attribute, &val);
    return app_driver_fan_apply_speed(fanmode_to_speed(val.val.u8));
}

app_driver_handle_t app_driver_fan_init()
{
    esp_err_t err = rf_fan_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize 433 MHz RF fan driver, err:%d", err);
        return NULL;
    }

    /* Keep Matter state in sync when the physical remote is used. */
    rf_fan_register_rx_cb(app_driver_fan_on_rf_received);

    /* Create (but don't start) the 15-minute boost auto-off timer. */
    const esp_timer_create_args_t boost_timer_args = {
        .callback = app_driver_fan_boost_timeout_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "fan_boost",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&boost_timer_args, &s_boost_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create boost timer, err:%d", err);
    }

    return (app_driver_handle_t)&s_fan_driver_token;
}

app_driver_handle_t app_driver_button_init()
{
    /* Initialize button */
    button_handle_t handle = NULL;
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = button_driver_get_config();

    if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button device");
        return NULL;
    }

    iot_button_register_cb(handle, BUTTON_PRESS_DOWN, NULL, app_driver_button_toggle_cb, NULL);
    return (app_driver_handle_t)handle;
}
