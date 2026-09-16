#include "shtc3.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SHTC3_ADDR 0x70
#define I2C_TIMEOUT_MS 100

static const char *TAG = "shtc3";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_ready;

esp_err_t shtc3_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = SHTC3_SDA_GPIO,
        .scl_io_num = SHTC3_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "шина I2C: %s", esp_err_to_name(err));
        return err;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHTC3_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "устройство 0x%02x: %s", SHTC3_ADDR, esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    ESP_LOGI(TAG, "SHTC3 на шине (SDA %d, SCL %d)", SHTC3_SDA_GPIO, SHTC3_SCL_GPIO);
    return ESP_OK;
}

static esp_err_t cmd(uint16_t c)
{
    uint8_t b[2] = { (uint8_t)(c >> 8), (uint8_t)(c & 0xFF) };
    return i2c_master_transmit(s_dev, b, sizeof(b), I2C_TIMEOUT_MS);
}

bool shtc3_read(float *t_c, float *rh)
{
    if (!s_ready) {
        return false;
    }
    /* пробуждение → измерение (T первым, clock stretching) → чтение 6 байт → сон */
    if (cmd(0x3517) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
    if (cmd(0x7CA2) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
    uint8_t raw[6] = {0};
    if (i2c_master_receive(s_dev, raw, sizeof(raw), I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    cmd(0xB098);                       /* сон — так делает Arduino-версия */
    uint16_t t_raw = (uint16_t)((raw[0] << 8) | raw[1]);
    uint16_t rh_raw = (uint16_t)((raw[3] << 8) | raw[4]);
    if (t_c) {
        *t_c = -45.0f + 175.0f * (float)t_raw / 65535.0f;
    }
    if (rh) {
        *rh = 100.0f * (float)rh_raw / 65535.0f;
    }
    return true;
}
