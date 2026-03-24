#pragma once
#include "driver/gpio.h"
#include <stdint.h>
#include <stddef.h>

/**
 * ICS43434 I2S 麦克风驱动
 * 双麦克风：左右声道分别接 WS 高低电平
 */

#define MIC_SAMPLE_RATE     48000
#define MIC_BITS            32      // ICS43434 输出 32bit，有效位 24bit
#define MIC_CHANNELS        2       // 立体声（双麦）
#define MIC_DMA_BUF_COUNT   8
#define MIC_DMA_BUF_LEN     511

void mic_init(gpio_num_t ws, gpio_num_t clk, gpio_num_t sd);
void mic_deinit(void);

// 读取 PCM 数据，返回实际读取字节数
// buf 里是 int32_t 交错的立体声样本
int  mic_read(void *buf, size_t buf_bytes, uint32_t timeout_ms);