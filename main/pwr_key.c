/*
 * pwr_key — see pwr_key.h.
 *
 * Register map (AXP2101, I2C 0x34; bit layout verified against
 * XPowersLib's xpowers_axp2101_irq_t):
 *
 *   INTEN2  0x41  IRQ enable, bank 2
 *   INTSTS2 0x49  IRQ status, bank 2 — write 1 to clear
 *     bit 3  PWRON short press
 *     bit 2  PWRON long press
 *     bit 1  PWRON negative edge
 *     bit 0  PWRON positive edge
 */

#include "pwr_key.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "pwr_key";

#define AXP2101_ADDR          0x34
#define AXP2101_REG_INTEN2    0x41
#define AXP2101_REG_INTSTS2   0x49
#define AXP2101_PKEY_SHORT    (1u << 3)
#define AXP2101_PKEY_LONG     (1u << 2)

#define PWR_POLL_MS           150
#define PWR_I2C_TIMEOUT_MS    50

static i2c_master_dev_handle_t s_dev;
static pwr_key_cb_t            s_cb;
static void                   *s_cb_data;
static bool                    s_warned;

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1,
                                       PWR_I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), PWR_I2C_TIMEOUT_MS);
}

static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PWR_POLL_MS));
        uint8_t sts = 0;
        esp_err_t err = reg_read(AXP2101_REG_INTSTS2, &sts);
        if (err != ESP_OK) {
            if (!s_warned) {
                s_warned = true;
                ESP_LOGW(TAG, "INTSTS2 read failed: %s (will keep trying)",
                         esp_err_to_name(err));
            }
            continue;
        }
        s_warned = false;
        if (sts == 0) continue;
        /* Write-1-clear exactly what we saw, so a press latched between
         * read and write isn't lost. */
        reg_write(AXP2101_REG_INTSTS2, sts);
        if (sts & AXP2101_PKEY_SHORT) {
            pwr_key_cb_t cb = s_cb;
            if (cb) cb(s_cb_data);
        }
    }
}

bool pwr_key_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(TAG, "no I2C bus — PWR key disabled");
        return false;
    }
    if (i2c_master_probe(bus, AXP2101_ADDR, PWR_I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not responding at 0x%02X — PWR key disabled",
                 AXP2101_ADDR);
        return false;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "add_device failed — PWR key disabled");
        return false;
    }

    /* Enable the short-press IRQ latch (read-modify-write keeps whatever
     * the PMU shipped with) and drop any stale status. */
    uint8_t en = 0;
    if (reg_read(AXP2101_REG_INTEN2, &en) == ESP_OK) {
        reg_write(AXP2101_REG_INTEN2, en | AXP2101_PKEY_SHORT);
    }
    reg_write(AXP2101_REG_INTSTS2, 0xFF);

    BaseType_t ok = xTaskCreate(poll_task, "pwr_key", 3072, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "poll task create failed");
        return false;
    }
    ESP_LOGI(TAG, "PWR key ready (AXP2101 poll every %dms)", PWR_POLL_MS);
    return true;
}

void pwr_key_set_handler(pwr_key_cb_t cb, void *usr_data)
{
    s_cb_data = usr_data;
    s_cb = cb;
}
