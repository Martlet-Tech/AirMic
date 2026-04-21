#pragma once
#include "driver/gpio.h"
#include <stdint.h>
#include <stddef.h>

/**
 * 麦克风驱动统一接口
 *
 * 支持两种硬件：
 *   ICS43434  — I2S 标准模式，32bit 输出（有效 24bit），双麦立体声
 *   ICS41350  — PDM 模式，16bit 输出（硬件抽取），双麦立体声
 *
 * 通过 menuconfig → AirMic Driver → Microphone type 选择。
 * 上层代码无需关心底层接口差异，mic_read() 始终返回交错立体声 PCM。
 */

/* ── 采样参数（两种麦克风共用） ─────────────────────────── */
#define MIC_SAMPLE_RATE     48000
#define MIC_CHANNELS        2       /* 立体声（双麦） */

/* 位深由底层实现决定，上层通过 mic_bits() 查询 */

/* ── DMA 缓冲区 ────────────────────────────────────────── */
#define MIC_DMA_BUF_COUNT   8
#define MIC_DMA_BUF_LEN     512     /* 每个 DMA 描述符的帧数（必须为偶数） */

/* ── 初始化配置 ─────────────────────────────────────────── */
typedef struct {
    gpio_num_t clk;   /* I2S: BCLK    PDM: CLK  */
    gpio_num_t ws;    /* I2S: WS/LRCK PDM: 未用（传 GPIO_NUM_NC） */
    gpio_num_t data;  /* I2S: DIN     PDM: DATA */
} mic_gpio_t;

/**
 * 初始化麦克风，启动 I2S/PDM 通道。
 * 调用前确保 GPIO 未被其他驱动占用。
 */
void mic_init(const mic_gpio_t *gpio);

/** 停止并释放通道资源 */
void mic_deinit(void);

/**
 * 读取 PCM 数据。
 * 返回实际读取字节数；buf 里是交错立体声整型样本：
 *   ICS43434 → int32_t L, int32_t R, int32_t L, ...  (每样本 4 字节)
 *   ICS41350 → int16_t L, int16_t R, int16_t L, ...  (每样本 2 字节)
 */
int mic_read(void *buf, size_t buf_bytes, uint32_t timeout_ms);

/**
 * 返回每个采样的位数（16 或 32）。
 * WAV 文件头 / 录音模块用这个值填 bits_per_sample。
 */
int mic_bits(void);
