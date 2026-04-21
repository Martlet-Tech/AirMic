/**
 * mic_i2s.c — ICS43434 双麦驱动
 *
 * 接口：I2S 标准（Philips）模式
 * 输出：32bit，有效位 24bit，高位对齐
 * 立体声：左麦 WS=LOW，右麦 WS=HIGH（ICS43434 硬件 L/R 选择）
 *
 * 启用条件：menuconfig CONFIG_MIC_TYPE_I2S=y
 */

#include "mic.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "mic_i2s";
static i2s_chan_handle_t s_rx_handle = NULL;

void mic_init(const mic_gpio_t *gpio)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = MIC_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = MIC_DMA_BUF_LEN;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = gpio->clk,
            .ws   = gpio->ws,
            .dout = I2S_GPIO_UNUSED,
            .din  = gpio->data,
            .invert_flags = { false, false, false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_handle));

    ESP_LOGI(TAG, "ICS43434 I2S init ok: %dHz 32bit stereo", MIC_SAMPLE_RATE);
    ESP_LOGI(TAG, "pins: bclk=%d ws=%d din=%d", gpio->clk, gpio->ws, gpio->data);
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
    return 32;
}
