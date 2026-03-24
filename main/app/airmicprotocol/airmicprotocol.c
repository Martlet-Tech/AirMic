#include "airmicprotocol.h"
#include "ble_nus.h"
#include "esp_log.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "airmic_proto";

static uint16_t s_conn_handle = 0;
static uint16_t s_resp_handle = 0;

// ── 发送响应给手机 ────────────────────────────────────────────
static void send_resp(uint8_t cmd, uint8_t status, const uint8_t *payload, uint8_t payload_len)
{
	uint8_t buf[64];
	buf[0] = cmd;
	buf[1] = status;
	if (payload && payload_len > 0) {
		memcpy(&buf[2], payload, payload_len);
	}
	ble_airmic_notify(s_conn_handle, s_resp_handle, buf, 2 + payload_len);
}

// ── 命令处理 ─────────────────────────────────────────────────
static void handle_time_sync(const uint8_t *payload, uint8_t len)
{
	if (len < 8) {
		send_resp(CMD_TIME_SYNC, RESP_ERR, NULL, 0);
		return;
	}

	// 小端序解析 uint64 unix时间戳(ms)
	uint64_t ts_ms = 0;
	for (int i = 0; i < 8; i++) {
		ts_ms |= ((uint64_t)payload[i] << (i * 8));
	}

	struct timeval tv = {
		.tv_sec = (time_t)(ts_ms / 1000),
		.tv_usec = (suseconds_t)((ts_ms % 1000) * 1000),
	};
	settimeofday(&tv, NULL);

	// 打印确认
	time_t now = tv.tv_sec;
	struct tm t;
	localtime_r(&now, &t);
	ESP_LOGI(TAG, "time synced: %04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
		 t.tm_hour, t.tm_min, t.tm_sec);

	send_resp(CMD_TIME_SYNC, RESP_OK, NULL, 0);
}

static void handle_set_samplerate(const uint8_t *payload, uint8_t len)
{
	if (len < 4) {
		send_resp(CMD_SET_SAMPLERATE, RESP_ERR, NULL, 0);
		return;
	}
	uint32_t rate = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24);
	ESP_LOGI(TAG, "set sample rate: %lu", rate);
	// TODO: 保存到 NVS，重启后生效
	send_resp(CMD_SET_SAMPLERATE, RESP_OK, NULL, 0);
}

static void handle_set_channels(const uint8_t *payload, uint8_t len)
{
	if (len < 1) {
		send_resp(CMD_SET_CHANNELS, RESP_ERR, NULL, 0);
		return;
	}
	uint8_t ch = payload[0];
	ESP_LOGI(TAG, "set channels: %d", ch);
	// TODO: 保存到 NVS
	send_resp(CMD_SET_CHANNELS, RESP_OK, NULL, 0);
}

static void handle_get_status(void)
{
	// 返回系统状态
	time_t now;
	time(&now);
	uint8_t payload[5];
	payload[0] = 0; // 0=idle 1=recording（TODO接recorder状态）
	payload[1] = (now >> 0) & 0xFF; // 当前时间戳低32bit
	payload[2] = (now >> 8) & 0xFF;
	payload[3] = (now >> 16) & 0xFF;
	payload[4] = (now >> 24) & 0xFF;
	send_resp(CMD_GET_STATUS, RESP_OK, payload, sizeof(payload));
}

// ── 对外接口 ─────────────────────────────────────────────────
void airmic_protocol_init(uint16_t conn_handle, uint16_t resp_handle)
{
	s_conn_handle = conn_handle;
	s_resp_handle = resp_handle;
}

void airmic_protocol_on_write(uint16_t conn_handle, const uint8_t *data, uint16_t len)
{
	s_conn_handle = conn_handle;

	if (len < 2) {
		ESP_LOGW(TAG, "packet too short: %d bytes", len);
		return;
	}

	uint8_t cmd = data[0];
	uint8_t pay_len = data[1];
	const uint8_t *payload = (len > 2) ? &data[2] : NULL;

	ESP_LOGI(TAG, "cmd=0x%02X len=%d", cmd, pay_len);

	switch (cmd) {
	case CMD_TIME_SYNC:
		handle_time_sync(payload, pay_len);
		break;
	case CMD_SET_SAMPLERATE:
		handle_set_samplerate(payload, pay_len);
		break;
	case CMD_SET_CHANNELS:
		handle_set_channels(payload, pay_len);
		break;
	case CMD_GET_STATUS:
		handle_get_status();
		break;
	default:
		ESP_LOGW(TAG, "unknown cmd: 0x%02X", cmd);
		send_resp(cmd, RESP_ERR, NULL, 0);
		break;
	}
}
