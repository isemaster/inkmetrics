#include "buttons.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BTN_BOOT_GPIO   0
#define BTN_PWR_GPIO   18
#define DEBOUNCE_MS    30
#define LONG_MS       800
#define HOLD_MS      3000
#define POLL_MS        10

static const char *TAG = "buttons";

typedef struct {
    btn_id_t id;
    int gpio;
    bool level;        /* текущее подтверждённое состояние: true = нажата */
    bool raw;          /* последнее прочитанное */
    int  stable_ms;    /* сколько держится raw */
    int64_t pressed_us;
    bool hold_sent;
} btn_state_t;

static btn_state_t s_btn[2] = {
    { BTN_BOOT, BTN_BOOT_GPIO, false, false, 0, 0, false },
    { BTN_PWR,  BTN_PWR_GPIO,  false, false, 0, 0, false },
};
static btn_cb_t s_cb;

static void buttons_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < 2; i++) {
            btn_state_t *b = &s_btn[i];
            bool raw = (gpio_get_level((gpio_num_t)b->gpio) == 0);   /* активный низкий */
            if (raw != b->raw) {
                b->raw = raw;
                b->stable_ms = 0;
                continue;
            }
            b->stable_ms += POLL_MS;
            if (b->stable_ms < DEBOUNCE_MS || b->level == raw) {
                continue;
            }
            b->level = raw;                                  /* подтверждённое изменение */
            if (raw) {
                b->pressed_us = now;
                b->hold_sent = false;
                continue;
            }
            /* отпускание: короткое или длинное */
            if (b->pressed_us && !b->hold_sent && s_cb) {
                int64_t held_ms = (now - b->pressed_us) / 1000;
                s_cb(b->id, held_ms >= LONG_MS ? BTN_EV_LONG : BTN_EV_SHORT);
            }
            b->pressed_us = 0;
        }
        /* удержание */
        for (int i = 0; i < 2; i++) {
            btn_state_t *b = &s_btn[i];
            if (b->level && b->pressed_us && !b->hold_sent &&
                (esp_timer_get_time() - b->pressed_us) / 1000 >= HOLD_MS) {
                b->hold_sent = true;
                if (s_cb) {
                    s_cb(b->id, BTN_EV_HOLD);
                }
            }
        }
    }
}

void buttons_init(btn_cb_t cb)
{
    s_cb = cb;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BTN_BOOT_GPIO) | (1ULL << BTN_PWR_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    xTaskCreate(buttons_task, "buttons", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "кнопки: BOOT=%d, PWR=%d (короткое <800 мс, длинное до 3 с, удержание 3 с)",
             BTN_BOOT_GPIO, BTN_PWR_GPIO);
}
