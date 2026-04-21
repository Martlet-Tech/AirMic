#include "rgb_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "rgb_led";

#define RMT_CHANNEL RMT_CHANNEL_0
#define RMT_CLK_DIV 80 // 80MHz / 80 = 1MHz, 1 tick = 1us
#define RMT_TICKS_PER_US 1

// WS2812B时序参数（单位：微秒）
#define WS2812B_T0H 0.4 // 0码高电平时间
#define WS2812B_T0L 0.85 // 0码低电平时间
#define WS2812B_T1H 0.8 // 1码高电平时间
#define WS2812B_T1L 0.45 // 1码低电平时间
#define WS2812B_RESET 50 // 复位时间

static gpio_num_t s_pin;
static rmt_channel_handle_t s_rmt_channel = NULL;
static rmt_encoder_handle_t s_encoder = NULL;
static rgb_led_mode_t s_mode = RGB_LED_MODE_OFF;
static TaskHandle_t s_rgb_task = NULL;

// 发送一个RGB颜色数据到WS2812B
static void rgb_led_send_color(rgb_color_t color)
{
	// WS2812B颜色顺序是GRB
	uint8_t grb_data[3] = { color.g, color.r, color.b };

	// 发送数据
	rmt_transmit_config_t tx_config = {
        .loop_count = 0, // 发送一次
        .flags = {
            .eot_level = 0, // 发送结束后输出低电平
            .queue_nonblocking = false, // 阻塞直到发送完成
        },
    };

	esp_err_t err = rmt_transmit(s_rmt_channel, s_encoder, grb_data, sizeof(grb_data), &tx_config);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to transmit RGB data: %d", err);
	}

	// 等待发送完成
	rmt_tx_wait_all_done(s_rmt_channel, portMAX_DELAY);

	// 等待复位时间
	vTaskDelay(pdMS_TO_TICKS(WS2812B_RESET / 1000 + 1));
}

// 生成呼吸效果的颜色
static rgb_color_t rgb_led_get_breathing_color(uint32_t tick)
{
	// 使用正弦函数生成呼吸效果
	float t = (tick % 1000) / 1000.0f;
	float intensity = (sin(t * 2 * 3.14159265) + 1) / 2; // 0-1

	// 颜色循环：红 -> 绿 -> 蓝 -> 红
	float hue = (tick % 3000) / 3000.0f; // 0-1
	rgb_color_t color;

	if (hue < 1.0f / 3.0f) {
		// 红到绿
		float ratio = hue * 3;
		color.r = 255 * (1 - ratio);
		color.g = 255 * ratio;
		color.b = 0;
	} else if (hue < 2.0f / 3.0f) {
		// 绿到蓝
		float ratio = (hue - 1.0f / 3.0f) * 3;
		color.r = 0;
		color.g = 255 * (1 - ratio);
		color.b = 255 * ratio;
	} else {
		// 蓝到红
		float ratio = (hue - 2.0f / 3.0f) * 3;
		color.r = 255 * ratio;
		color.g = 0;
		color.b = 255 * (1 - ratio);
	}

	// 应用呼吸强度
	color.r = color.r * intensity;
	color.g = color.g * intensity;
	color.b = color.b * intensity;

	return color;
}

// RGB LED任务
static void rgb_led_task(void *arg)
{
	uint32_t tick = 0;

	while (1) {
		switch (s_mode) {
		case RGB_LED_MODE_BREATHING:
			// 呼吸变色效果
			rgb_color_t color = rgb_led_get_breathing_color(tick);
			rgb_led_send_color(color);
			tick += 50;
			vTaskDelay(pdMS_TO_TICKS(50));
			break;

		case RGB_LED_MODE_OFF:
			// 发送黑色（关闭）
			rgb_color_t black = { 0, 0, 0 };
			rgb_led_send_color(black);
			vTaskDelay(pdMS_TO_TICKS(100));
			break;
		}
	}
}

void rgb_led_init(gpio_num_t pin)
{
	s_pin = pin;

	// 配置RMT TX通道
	rmt_tx_channel_config_t config = {
		.gpio_num = pin,
		.clk_src = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = 10000000, // 1MHz，1 tick = 1us
		.mem_block_symbols = 64, // 足够存储WS2812B的24位数据
		.trans_queue_depth = 4,
		.intr_priority = 0,
		.flags = {
			.invert_out = false,
			.with_dma = false, // 对于WS2812B，不需要DMA
			.io_loop_back = false,
			.io_od_mode = false,
			.allow_pd = false,
			.init_level = 0,
		},
	};

	// 创建RMT TX通道
	if (rmt_new_tx_channel(&config, &s_rmt_channel) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to create RMT TX channel");
		return;
	}

	// 启用RMT通道
	if (rmt_enable(s_rmt_channel) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to enable RMT channel");
		return;
	}

	// 创建WS2812B编码器
	rmt_bytes_encoder_config_t encoder_config = {
		.bit0 = {
			.duration0 = 4,   // T0H: 0.4us = 4 ticks ← 改
			.level0 = 1,
			.duration1 = 9,   // T0L: 0.85us ≈ 9 ticks ← 改
			.level1 = 0,
		},
		.bit1 = {
			.duration0 = 8,   // T1H: 0.8us = 8 ticks ← 改
			.level0 = 1,
			.duration1 = 5,   // T1L: 0.45us ≈ 5 ticks ← 改
			.level1 = 0,
		},
		.flags.msb_first = true,
	};

	if (rmt_new_bytes_encoder(&encoder_config, &s_encoder) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to create bytes encoder");
		return;
	}

	// 创建RGB LED任务
	xTaskCreate(rgb_led_task, "rgb_led", 2048, NULL, 3, &s_rgb_task);

	ESP_LOGI(TAG, "RGB LED initialized on pin %d", pin);
}

void rgb_led_set_mode(rgb_led_mode_t mode)
{
	s_mode = mode;
	ESP_LOGI(TAG, "RGB LED mode set to %d", mode);
}

void rgb_led_set_color(rgb_color_t color)
{
	rgb_led_send_color(color);
	ESP_LOGI(TAG, "RGB LED color set to R:%d, G:%d, B:%d", color.r, color.g, color.b);
}

void rgb_led_stop(void)
{
	if (s_rgb_task) {
		vTaskDelete(s_rgb_task);
		s_rgb_task = NULL;
	}

	if (s_encoder) {
		rmt_del_encoder(s_encoder);
		s_encoder = NULL;
	}

	if (s_rmt_channel) {
		rmt_disable(s_rmt_channel);
		rmt_del_channel(s_rmt_channel);
		s_rmt_channel = NULL;
	}

	ESP_LOGI(TAG, "RGB LED stopped");
}
