/**
 * mic_pdm.c — ICS41350 双麦驱动
 *
 * 接口：PDM（Pulse Density Modulation）
 * 输出：ESP32-S3 PDM RX 硬件抽取后输出 16bit PCM
 * 立体声：左麦 SELECT=GND，右麦 SELECT=VDD，共用同一组 CLK+DATA
 *         两颗麦克风在 CLK 的奇偶沿分别输出，硬件自动交织成立体声
 *
 * 注意：PDM 模式没有 WS 引脚，mic_gpio_t.ws 传 GPIO_NUM_NC 即可。
 *
 * 启用条件：menuconfig CONFIG_MIC_TYPE_PDM=y
 */

#include "mic.h"
#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "mic_pdm";
static i2s_chan_handle_t s_rx_handle = NULL;

void mic_init(const mic_gpio_t *gpio)
{
    /* ── 1. 申请通道 ─────────────────────────────────────── */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = MIC_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = MIC_DMA_BUF_LEN;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle));

    /* ── 2. PDM RX 配置 ──────────────────────────────────── */
    i2s_pdm_rx_config_t pdm_cfg = {
        /*
         * 时钟配置：
         *   sample_rate_hz  设目标采样率，驱动自动计算 PDM 过采样时钟
         *   clk_src         默认 APLL（精度更高），也可用 I2S_CLK_SRC_DEFAULT
         *   mclk_multiple   PDM RX 固定用 I2S_MCLK_MULTIPLE_256
         *   dn_sample_mode  I2S_PDM_DSP_HALF_SAMPLE_RATE = 单侧抽取，
         *                   实际输出是 sample_rate_hz / 2，所以这里填 2x
         *                   使用 I2S_PDM_DSPD_SAMPLE_RATE 保持 1:1 最简单
         */
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),

        /*
         * 槽位配置：
         *   data_bit_width  PDM RX 抽取后输出 16bit（硬件固定）
         *   slot_mode       STEREO：从同一 DATA 线采集左右双声道
         *                   ICS41350 L/R 通过 SELECT pin 区分，
         *                   CLK 上升沿 = LEFT，下降沿 = RIGHT
         */
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_STEREO),

        /* ── GPIO ─────────────────────────────────────────── */
        .gpio_cfg = {
            .clk = gpio->clk,
            .din = gpio->data,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_rx_handle, &pdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_handle));

    ESP_LOGI(TAG, "ICS41350 PDM init ok: %dHz 16bit stereo", MIC_SAMPLE_RATE);
    ESP_LOGI(TAG, "pins: clk=%d data=%d (ws not used)", gpio->clk, gpio->data);
}

void mic_deinit(void)
{
    if (!s_rx_handle) return;
    i2s_channel_disable(s_rx_handle);
    i2s_del_channel(s_rx_handle);
    s_rx_handle = NULL;
}

int mic_read(void *buf, size_t buf_bytes, uint32_t timeout_ms)
{
    size_t bytes_read = 0;
    i2s_channel_read(s_rx_handle, buf, buf_bytes, &bytes_read, pdMS_TO_TICKS(timeout_ms));
    return (int)bytes_read;
}

int mic_bits(void)
{
    return 16;
}
