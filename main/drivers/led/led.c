#include "led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

static gpio_num_t s_pin;
static led_mode_t s_mode = LED_MODE_OFF;

static void led_task(void *arg)
{
    while (1) {
        switch (s_mode) {
        case LED_MODE_IDLE:
            // 慢闪：亮200ms 灭800ms，录音笔待机感
            gpio_set_level(s_pin, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(s_pin, 0);
            vTaskDelay(pdMS_TO_TICKS(800));
            break;

        case LED_MODE_RECORDING:
            // 快闪：亮100ms 灭100ms，录音进行中
            gpio_set_level(s_pin, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(s_pin, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case LED_MODE_OFF:
            gpio_set_level(s_pin, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}

void led_init(gpio_num_t pin)
{
    s_pin = pin;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(pin, 0);
    xTaskCreate(led_task, "led", 2048, NULL, 3, NULL);
}

void led_set_mode(led_mode_t mode)
{
    s_mode = mode;
}
