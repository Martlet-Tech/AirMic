#pragma once
#include "driver/gpio.h"

/**
 * 按钮驱动 - 支持单击 / 双击 / 长按
 * 业务层注册回调，驱动层负责消抖和手势识别
 */

typedef enum {
    BTN_EVENT_SINGLE_CLICK,   // 单击：录音模式下手动开始/停止录音
    BTN_EVENT_DOUBLE_CLICK,   // 双击：切换工作模式
    BTN_EVENT_LONG_PRESS,     // 长按：保留，比如强制停止/重置
} btn_event_t;

typedef void (*btn_callback_t)(btn_event_t event);

void button_init(gpio_num_t pin, btn_callback_t cb);
