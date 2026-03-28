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

#define TAG "recorder"

// WAV 参数
#define WAV_SAMPLE_RATE MIC_SAMPLE_RATE // 48000
#define WAV_CHANNELS MIC_CHANNELS // 2
#define WAV_BITS 16 // 存 16bit（从 32bit 截取高 16bit）
#define WAV_BYTE_RATE (WAV_SAMPLE_RATE * WAV_CHANNELS * WAV_BITS / 8)
#define WAV_BLOCK_ALIGN (WAV_CHANNELS * WAV_BITS / 8)

// 每次读取的帧数
#define READ_BUF_FRAMES 512
#define READ_BUF_BYTES (READ_BUF_FRAMES * WAV_CHANNELS * sizeof(int32_t))

#define RING_BUF_BLOCKS 32 // 环形缓冲块数
#define BLOCK_BYTES (READ_BUF_FRAMES * WAV_CHANNELS * sizeof(int16_t))

static int16_t s_ring[RING_BUF_BLOCKS][READ_BUF_FRAMES * WAV_CHANNELS];
static uint8_t s_ring_head = 0; // 生产者写
static uint8_t s_ring_tail = 0; // 消费者读
static SemaphoreHandle_t s_ring_sem = NULL; // 有数据通知消费者

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

	// --- 数字高通滤波器 (HPF) 状态 ---
	static const float HPF_ALPHA = 0.9805f; 
	float hpf_x[2] = {0, 0};
	float hpf_y[2] = {0, 0};

	while (s_recording) {
		int bytes = mic_read(raw, READ_BUF_BYTES, 100);
		if (bytes <= 0)
			continue;

		int samples = bytes / sizeof(int32_t);

		for (int i = 0; i < samples; i++) {
			int ch = i % 2; 
			float x = (float)(raw[i] >> 16);
			
			float y = HPF_ALPHA * (hpf_y[ch] + x - hpf_x[ch]);
			
			hpf_x[ch] = x;
			hpf_y[ch] = y;

			if (y > 32767.0f) y = 32767.0f;
			if (y < -32768.0f) y = -32768.0f;

			pcm[i] = (int16_t)y;
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

// 生产者：只管读麦克风，高优先级
static void mic_capture_task(void *arg)
{
	int32_t *raw = malloc(READ_BUF_BYTES);
	
	// --- 数字高通滤波器 (HPF) 状态 ---
	// 截至频率约 150Hz (fs = 48000Hz)
	// 公式: y[n] = alpha * (y[n-1] + x[n] - x[n-1]), alpha ≈ 0.9805
	static const float HPF_ALPHA = 0.9805f; 
	float hpf_x[2] = {0, 0};
	float hpf_y[2] = {0, 0};

	while (s_recording) {
		int bytes = mic_read(raw, READ_BUF_BYTES, 100);
		if (bytes <= 0)
			continue;

		int samples = bytes / sizeof(int32_t);
		uint8_t next = (s_ring_head + 1) % RING_BUF_BLOCKS;
		if (next == s_ring_tail) {
			ESP_LOGW(TAG, "ring full, drop"); // SD太慢导致
			continue;
		}
		int16_t *dst = s_ring[s_ring_head];
		
		for (int i = 0; i < samples; i++) {
			int ch = i % 2; // 交错的立体声：0=左，1=右
			float x = (float)(raw[i] >> 16);
			
			// 应用一阶高通滤波
			float y = HPF_ALPHA * (hpf_y[ch] + x - hpf_x[ch]);
			
			// 更新状态
			hpf_x[ch] = x;
			hpf_y[ch] = y;

			// 防止浮点转整型的溢出爆音
			if (y > 32767.0f) y = 32767.0f;
			if (y < -32768.0f) y = -32768.0f;

			dst[i] = (int16_t)y;
		}
		
		s_ring_head = next;
		xSemaphoreGive(s_ring_sem);
	}
	free(raw);
	vTaskDelete(NULL);
}

// 消费者：只管写 SD 卡，低优先级
static void sd_write_task(void *arg)
{
	FILE *f = (FILE *)arg;
	uint32_t data_bytes = 0;
	int64_t last_hdr_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

// 攒够 4 块再一次性写入，减少 SD 卡写操作次数
#define WRITE_BATCH 4
	static int16_t write_buf[WRITE_BATCH][READ_BUF_FRAMES * WAV_CHANNELS];

	while (s_recording || s_ring_tail != s_ring_head) {
		int batch = 0;

		// 尽量攒满 WRITE_BATCH 块
		while (batch < WRITE_BATCH && xSemaphoreTake(s_ring_sem, pdMS_TO_TICKS(50)) == pdTRUE) {
			memcpy(write_buf[batch], s_ring[s_ring_tail], BLOCK_BYTES);
			s_ring_tail = (s_ring_tail + 1) % RING_BUF_BLOCKS;
			batch++;
		}

		if (batch > 0) {
			fwrite(write_buf, 1, BLOCK_BYTES * batch, f);
			data_bytes += BLOCK_BYTES * batch;
		}

		int64_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
		if (now - last_hdr_ms >= 1000) {
			wav_header_update(f, data_bytes);
			last_hdr_ms = now;
		}
	}

	wav_header_update(f, data_bytes);
	fclose(f);
	vTaskDelete(NULL);
}
void recorder_init(void)
{
	s_ring_sem = xSemaphoreCreateCounting(RING_BUF_BLOCKS, 0);
}

// ── 对外接口 ─────────────────────────────────────────────────
void recorder_start(void)
{
	if (s_recording)
		return;
	// 1. 先开文件
	int idx = sdcard_next_index();
	char path[64];
	snprintf(path, sizeof(path), "%s/AM_%04d.wav", SD_MOUNT_POINT, idx);

	FILE *f = fopen(path, "wb");
	if (!f) {
		ESP_LOGE(TAG, "cannot open %s", path);
		return;
	}

	// 2. 写空头占位
	wav_header_t h_empty = { 0 };
	fwrite(&h_empty, sizeof(h_empty), 1, f);

	// 3. 初始化环形缓冲
	s_ring_head = 0;
	s_ring_tail = 0;
	s_ring_sem = xSemaphoreCreateCounting(RING_BUF_BLOCKS, 0);

	s_recording = true;

	// 4. f 在这里才能传进去
	xTaskCreate(mic_capture_task, "mic_cap", 4096, NULL, 6, NULL);
	xTaskCreate(sd_write_task, "sd_write", 4096, f, 4, NULL);

	ESP_LOGI(TAG, "recording → %s", path);
	led_set_mode(LED_MODE_RECORDING);
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
