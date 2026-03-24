#pragma once
#include "driver/gpio.h"

/**
 * 单色 LED 驱动 - 录音状态指示
 * 初始化时传入引脚，与硬件版本解耦
 */

typedef enum {
    LED_MODE_IDLE,        // 待机：慢闪（1s间隔），模仿录音笔待机
    LED_MODE_RECORDING,   // 录音中：快闪（0.2s间隔），模仿录音笔录音
    LED_MODE_OFF,         // 熄灭
} led_mode_t;

void led_init(gpio_num_t pin);
void led_set_mode(led_mode_t mode);
