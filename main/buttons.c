/*
 * buttons — see buttons.h.
 *
 * Both pins are active-low with internal pull-up. The BOOT pin (GPIO 0)
 * is the same one the chip samples at reset to enter download mode;
 * after boot it's free to be used as a button. USER is GPIO 18 on the
 * Waveshare ESP32-S3-Touch-AMOLED-2.16 board — see the aurora reference
 * `peripherals/keys.c` for the wiring source.
 */

#include "buttons.h"

#include <string.h>

#include "esp_log.h"
#include "iot_button.h"
#include "button_gpio.h"

static const char *TAG = "buttons";

#define GPIO_BOOT  0
#define GPIO_USER  18

static button_handle_t   s_handles[BUTTON_COUNT];
static button_press_cb_t s_handlers[BUTTON_COUNT];
static void             *s_handler_data[BUTTON_COUNT];

static void dispatch_cb(void *btn, void *usr_data)
{
    button_id_t which = (button_id_t)(uintptr_t)usr_data;
    if (which >= BUTTON_COUNT) return;
    button_press_cb_t cb = s_handlers[which];
    void *data = s_handler_data[which];
    if (cb) cb(btn, data);
}

static bool create_one(int gpio, button_id_t which)
{
    button_config_t cfg = {
        .long_press_time  = 0,   /* default */
        .short_press_time = 0,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num     = gpio,
        .active_level = 0,        /* active-low */
        .enable_power_save = false,
        .disable_pull = false,
    };
    button_handle_t h = NULL;
    esp_err_t err = iot_button_new_gpio_device(&cfg, &gpio_cfg, &h);
    if (err != ESP_OK || h == NULL) {
        ESP_LOGE(TAG, "iot_button_new_gpio_device gpio=%d failed: %s",
                 gpio, esp_err_to_name(err));
        return false;
    }
    iot_button_register_cb(h, BUTTON_SINGLE_CLICK, NULL,
                           dispatch_cb, (void *)(uintptr_t)which);
    s_handles[which] = h;
    return true;
}

bool buttons_init(void)
{
    memset(s_handlers, 0, sizeof(s_handlers));
    memset(s_handler_data, 0, sizeof(s_handler_data));
    bool a = create_one(GPIO_BOOT, BUTTON_BOOT);
    bool b = create_one(GPIO_USER, BUTTON_USER);
    if (a && b) ESP_LOGI(TAG, "buttons ready (BOOT=GPIO0, USER=GPIO18)");
    return a && b;
}

void buttons_set_handler(button_id_t which, button_press_cb_t cb, void *usr_data)
{
    if (which >= BUTTON_COUNT) return;
    s_handlers[which] = cb;
    s_handler_data[which] = usr_data;
}
