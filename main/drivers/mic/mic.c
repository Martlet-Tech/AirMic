#include "mic.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "mic";
static i2s_chan_handle_t s_rx_handle = NULL;

void mic_init(gpio_num_t ws, gpio_num_t clk, gpio_num_t sd)
{
	i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
	chan_cfg.dma_desc_num = MIC_DMA_BUF_COUNT;
	chan_cfg.dma_frame_num = MIC_DMA_BUF_LEN;

	ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle));

	i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        MIC_BITS, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = clk,
            .ws   = ws,
            .dout = I2S_GPIO_UNUSED,
            .din  = sd,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

	ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_handle, &std_cfg));
	ESP_ERROR_CHECK(i2s_channel_enable(s_rx_handle));
	ESP_LOGI(TAG, "mic init ok, %dHz %dbit %dch", MIC_SAMPLE_RATE, MIC_BITS, MIC_CHANNELS);
}

void mic_deinit(void)
{
	if (!s_rx_handle)
		return;
	i2s_channel_disable(s_rx_handle);
	i2s_del_channel(s_rx_handle);
	s_rx_handle = NULL;
}

int mic_read(void *buf, size_t buf_bytes, uint32_t timeout_ms)
{
	size_t bytes_read = 0;
	esp_err_t ret = i2s_channel_read(s_rx_handle, buf, buf_bytes, &bytes_read, pdMS_TO_TICKS(timeout_ms));
	ESP_LOGI("mic", "read ret=%d bytes_read=%d", ret, (int)bytes_read);

	//if (ret != ESP_OK)
	//	return -1;
	return (int)bytes_read;
}
