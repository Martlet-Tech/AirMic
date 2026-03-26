#include "recorder.h"
#include "led.h"
#include "mic.h"
#include "sdcard.h"
#include "bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h" // 引入信号量头文件
#include "driver/uart.h"
#include <stdio.h>
#include <string.h>
#include "ble_nus.h"

static const char *TAG = "recorder";

// WAV 参数
#define WAV_SAMPLE_RATE MIC_SAMPLE_RATE // 48000
#define WAV_CHANNELS MIC_CHANNELS // 2
#define WAV_BITS 16 // 存 16bit（从 32bit 截取高 16bit）
#define WAV_BYTE_RATE (WAV_SAMPLE_RATE * WAV_CHANNELS * WAV_BITS / 8)
#define WAV_BLOCK_ALIGN (WAV_CHANNELS * WAV_BITS / 8)

// 每次读取的帧数
#define READ_BUF_FRAMES 512
#define READ_BUF_BYTES (READ_BUF_FRAMES * WAV_CHANNELS * sizeof(int32_t))

static TaskHandle_t s_record_task = NULL;
static bool s_recording = false;

// ── WAV 头结构 ────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
	char riff[4]; // "RIFF"
	uint32_t chunk_size; // 文件总大小 - 8
	char wave[4]; // "WAVE"
	char fmt[4]; // "fmt "
	uint32_t fmt_size; // 16
	uint16_t audio_fmt; // 1 = PCM
	uint16_t channels;
	uint32_t sample_rate;
	uint32_t byte_rate;
	uint16_t block_align;
	uint16_t bits;
	char data[4]; // "data"
	uint32_t data_size; // PCM 数据字节数
} wav_header_t;

static void wav_header_update(FILE *f, uint32_t data_bytes)
{
	wav_header_t h;
	memcpy(h.riff, "RIFF", 4);
	h.chunk_size = data_bytes + sizeof(wav_header_t) - 8;
	memcpy(h.wave, "WAVE", 4);
	memcpy(h.fmt, "fmt ", 4);
	h.fmt_size = 16;
	h.audio_fmt = 1;
	h.channels = WAV_CHANNELS;
	h.sample_rate = WAV_SAMPLE_RATE;
	h.byte_rate = WAV_BYTE_RATE;
	h.block_align = WAV_BLOCK_ALIGN;
	h.bits = WAV_BITS;
	memcpy(h.data, "data", 4);
	h.data_size = data_bytes;

	fseek(f, 0, SEEK_SET);
	fwrite(&h, sizeof(h), 1, f);
	fflush(f);
	fseek(f, 0, SEEK_END);
}

// ── 录音任务 ─────────────────────────────────────────────────
static void record_task(void *arg)
{
	int idx = sdcard_next_index();
	char path[64];
	snprintf(path, sizeof(path), "%s/AM_%04d.wav", SD_MOUNT_POINT, idx);

	FILE *f = fopen(path, "wb");
	if (!f) {
		ESP_LOGE(TAG, "cannot open %s", path);
		vTaskDelete(NULL);
		return;
	}

	// 先写空头占位
	wav_header_t h_empty = { 0 };
	fwrite(&h_empty, sizeof(h_empty), 1, f);

	int32_t *raw = malloc(READ_BUF_BYTES);
	int16_t *pcm = malloc(READ_BUF_FRAMES * WAV_CHANNELS * sizeof(int16_t));
	if (!raw || !pcm) {
		ESP_LOGE(TAG, "malloc failed");
		fclose(f);
		free(raw);
		free(pcm);
		vTaskDelete(NULL);
		return;
	}

	uint32_t data_bytes = 0;
	int64_t last_hdr_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

	ESP_LOGI(TAG, "recording → %s", path);
	led_set_mode(LED_MODE_RECORDING);

	while (s_recording) {
		int bytes = mic_read(raw, READ_BUF_BYTES, 100);
		if (bytes <= 0)
			continue;

		int samples = bytes / sizeof(int32_t);

		// 32bit → 16bit，取高位
		for (int i = 0; i < samples; i++) {
			pcm[i] = (int16_t)(raw[i] >> 16);
		}

		size_t wb = samples * sizeof(int16_t);
		fwrite(pcm, 1, wb, f);
		data_bytes += wb;

		// 每秒刷新文件头，断电保护
		int64_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
		if (now - last_hdr_ms >= 1000) {
			wav_header_update(f, data_bytes);
			last_hdr_ms = now;
			ESP_LOGD(TAG, "header refreshed, %lu bytes", data_bytes);
		}
	}

	// 停止时写最终正确头
	wav_header_update(f, data_bytes);
	fclose(f);
	free(raw);
	free(pcm);

	ESP_LOGI(TAG, "saved: %s (%lu bytes)", path, data_bytes);
	led_set_mode(LED_MODE_IDLE);
	s_record_task = NULL;
	vTaskDelete(NULL);
}

// ── 对外接口 ─────────────────────────────────────────────────
void recorder_start(void)
{
	led_set_mode(LED_MODE_RECORDING);

	if (s_recording || s_record_task != NULL)
		return;

	xTaskCreate(record_task, "record", 8192, NULL, 5, &s_record_task);
	s_recording = true;
	ESP_LOGI(TAG, "recorder started");
}

void recorder_stop(void)
{
	led_set_mode(LED_MODE_IDLE);

	s_recording = false;
	ESP_LOGI(TAG, "recorder stopped");
}

void recorder_toggle(void)
{
	if (s_recording) {
		recorder_stop();

	} else {
		recorder_start();
	}
}
