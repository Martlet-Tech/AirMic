#pragma once
#include "driver/gpio.h"
#include <stdbool.h>

#define SD_MOUNT_POINT  "/sdcard"

bool sdcard_mount(gpio_num_t clk, gpio_num_t cmd,
                  gpio_num_t d0,  gpio_num_t d1,
                  gpio_num_t d2,  gpio_num_t d3);

void sdcard_unmount(void);

// 扫描 /sdcard 找最大 AM_XXXX.wav 序号，返回下一个可用序号
// 比如已有 AM_0003.wav，返回 4
int sdcard_next_index(void);
