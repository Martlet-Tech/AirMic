#pragma once
#include "driver/gpio.h"

/**
 * WS2812B RGB LED 驱动
 * 用于蓝牙连接状态指示
 */

// RGB颜色结构体
typedef struct {
    uint8_t r; // 红色通道 (0-255)
    uint8_t g; // 绿色通道 (0-255)
    uint8_t b; // 蓝色通道 (0-255)
} rgb_color_t;

// RGB LED状态
typedef enum {
    RGB_LED_MODE_OFF,        // 关闭
    RGB_LED_MODE_BREATHING,  // 呼吸变色模式
} rgb_led_mode_t;

// 函数声明
void rgb_led_init(gpio_num_t pin);
void rgb_led_set_mode(rgb_led_mode_t mode);
void rgb_led_set_color(rgb_color_t color);
void rgb_led_stop(void);
