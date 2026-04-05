#include "airmicprotocol.h"
#include "ble_nus.h"
#include "esp_log.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "airmic_proto";

// 函数声明
static void handle_set_wifi(const uint8_t *payload, uint8_t len);
static void handle_get_file_list(void);
static void handle_delete_file(const uint8_t *payload, uint8_t len);
static void handle_rename_file(const uint8_t *payload, uint8_t len);
static void handle_get_wifi_status(void);

static uint16_t s_conn_handle = 0;
static uint16_t s_resp_handle = 0;

// ── 发送响应给手机 ────────────────────────────────────────────
static void send_resp(uint8_t cmd, uint8_t status, const uint8_t *payload, uint8_t payload_len)
{
	uint8_t buf[512]; // 增加缓冲区大小，与文件列表缓冲区一致
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

static void handle_set_wifi(const uint8_t *payload, uint8_t len)
{
	if (len < 2) {
		send_resp(CMD_SET_WIFI, RESP_ERR, NULL, 0);
		return;
	}

	// 解析SSID
	uint8_t ssid_len = payload[0];
	if (ssid_len == 0 || ssid_len > 32) {
		send_resp(CMD_SET_WIFI, RESP_ERR, NULL, 0);
		return;
	}

	if (len < 1 + ssid_len + 1) {
		send_resp(CMD_SET_WIFI, RESP_ERR, NULL, 0);
		return;
	}

	char ssid[33] = {0};
	memcpy(ssid, &payload[1], ssid_len);

	// 解析密码
	uint8_t password_len = payload[1 + ssid_len];
	if (password_len > 64) {
		send_resp(CMD_SET_WIFI, RESP_ERR, NULL, 0);
		return;
	}

	if (len < 1 + ssid_len + 1 + password_len) {
		send_resp(CMD_SET_WIFI, RESP_ERR, NULL, 0);
		return;
	}

	char password[65] = {0};
	memcpy(password, &payload[1 + ssid_len + 1], password_len);

	ESP_LOGI(TAG, "WiFi setup: SSID=%s, Password=%s", ssid, password);

	// 检查当前WiFi状态 - 遵循项目规范：如果已连接并获取IP，直接返回成功
	char current_ip[16] = {0};
	bool already_has_ip = wifi_get_ip(current_ip);
	
	if (already_has_ip && wifi_is_connected_to_ssid(ssid)) {
		// 已经连接到相同的网络且有IP，直接返回成功
		ESP_LOGI(TAG, "Already connected to %s with IP: %s", ssid, current_ip);
		send_resp(CMD_SET_WIFI, RESP_OK, NULL, 0);
		return;
	}

	// 调用WiFi模块初始化（如果未初始化）
	wifi_init();

	if (wifi_set_config(ssid, password) != 0) {
		send_resp(CMD_SET_WIFI, RESP_ERR, NULL, 0);
		return;
	}

	if (wifi_start() != 0) {
		send_resp(CMD_SET_WIFI, RESP_ERR, NULL, 0);
		return;
	}

	ESP_LOGI(TAG, "WiFi started, connecting to %s...", ssid);
	send_resp(CMD_SET_WIFI, RESP_OK, NULL, 0);
}

static void handle_get_file_list(void)
{
	// 打开SD卡目录
	DIR *dir = opendir("/sdcard");
	if (!dir) {
		ESP_LOGE(TAG, "Failed to open SD card directory");
		send_resp(CMD_GET_FILE_LIST, RESP_ERR, NULL, 0);
		return;
	}

	// 构建文件列表响应
	// 格式: [文件数量(2字节)] + [文件名长度(1字节) + 文件名 + 文件大小(4字节)] * N
	uint8_t buf[512]; // 限制响应大小
	uint16_t offset = 0;
	uint16_t file_count = 0;

	// 预留文件数量的位置
	offset += 2;

	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		// 跳过隐藏文件和目录
		if (entry->d_name[0] == '.' || entry->d_type == DT_DIR) {
			continue;
		}

		// 获取文件大小
		struct stat stat_buf;
		char path[265]; // 255字节文件名 + "/sdcard/"前缀(9字节) + 空终止符
		snprintf(path, sizeof(path), "/sdcard/%s", entry->d_name);
		if (stat(path, &stat_buf) != 0) {
			continue;
		}

		// 检查缓冲区空间
		uint8_t name_len = strlen(entry->d_name);
		if (offset + 1 + name_len + 4 > sizeof(buf)) {
			break; // 缓冲区不足
		}

		// 添加文件名长度
		buf[offset++] = name_len;
		// 添加文件名
		memcpy(&buf[offset], entry->d_name, name_len);
		offset += name_len;
		// 添加文件大小（小端序）
		buf[offset++] = (stat_buf.st_size >> 0) & 0xFF;
		buf[offset++] = (stat_buf.st_size >> 8) & 0xFF;
		buf[offset++] = (stat_buf.st_size >> 16) & 0xFF;
		buf[offset++] = (stat_buf.st_size >> 24) & 0xFF;

		file_count++;
	}

	closedir(dir);

	// 写入文件数量（小端序）
	buf[0] = (file_count >> 0) & 0xFF;
	buf[1] = (file_count >> 8) & 0xFF;

	ESP_LOGI(TAG, "File list: %d files", file_count);
	send_resp(CMD_GET_FILE_LIST, RESP_OK, buf, offset);
}

static void handle_delete_file(const uint8_t *payload, uint8_t len)
{
	if (len < 1) {
		send_resp(CMD_DELETE_FILE, RESP_ERR, NULL, 0);
		return;
	}

	// 解析文件名
	uint8_t filename_len = payload[0];
	if (filename_len == 0) {
		send_resp(CMD_DELETE_FILE, RESP_ERR, NULL, 0);
		return;
	}

	if (len < 1 + filename_len) {
		send_resp(CMD_DELETE_FILE, RESP_ERR, NULL, 0);
		return;
	}

	char filename[256] = {0};
	memcpy(filename, &payload[1], filename_len);

	// 构建完整路径
	char path[265]; // 255字节文件名 + "/sdcard/"前缀(9字节) + 空终止符
	snprintf(path, sizeof(path), "/sdcard/%s", filename);

	ESP_LOGI(TAG, "Delete file: %s", path);

	// 删除文件
	if (unlink(path) != 0) {
		ESP_LOGE(TAG, "Failed to delete file: %s", path);
		send_resp(CMD_DELETE_FILE, RESP_ERR, NULL, 0);
		return;
	}

	ESP_LOGI(TAG, "File deleted successfully: %s", filename);
	send_resp(CMD_DELETE_FILE, RESP_OK, NULL, 0);
}

static void handle_rename_file(const uint8_t *payload, uint8_t len)
{
	if (len < 2) {
		send_resp(CMD_RENAME_FILE, RESP_ERR, NULL, 0);
		return;
	}

	// 解析旧文件名
	uint8_t old_filename_len = payload[0];
	if (old_filename_len == 0) {
		send_resp(CMD_RENAME_FILE, RESP_ERR, NULL, 0);
		return;
	}

	if (len < 1 + old_filename_len + 1) {
		send_resp(CMD_RENAME_FILE, RESP_ERR, NULL, 0);
		return;
	}

	char old_filename[256] = {0};
	memcpy(old_filename, &payload[1], old_filename_len);

	// 解析新文件名
	uint8_t new_filename_len = payload[1 + old_filename_len];
	if (new_filename_len == 0) {
		send_resp(CMD_RENAME_FILE, RESP_ERR, NULL, 0);
		return;
	}

	if (len < 1 + old_filename_len + 1 + new_filename_len) {
		send_resp(CMD_RENAME_FILE, RESP_ERR, NULL, 0);
		return;
	}

	char new_filename[256] = {0};
	memcpy(new_filename, &payload[1 + old_filename_len + 1], new_filename_len);

	// 构建完整路径
	char old_path[265]; // 255字节文件名 + "/sdcard/"前缀(9字节) + 空终止符
	char new_path[265];
	snprintf(old_path, sizeof(old_path), "/sdcard/%s", old_filename);
	snprintf(new_path, sizeof(new_path), "/sdcard/%s", new_filename);

	ESP_LOGI(TAG, "Rename file: %s -> %s", old_path, new_path);

	// 重命名文件
	if (rename(old_path, new_path) != 0) {
		ESP_LOGE(TAG, "Failed to rename file: %s -> %s", old_path, new_path);
		send_resp(CMD_RENAME_FILE, RESP_ERR, NULL, 0);
		return;
	}

	ESP_LOGI(TAG, "File renamed successfully: %s -> %s", old_filename, new_filename);
	send_resp(CMD_RENAME_FILE, RESP_OK, NULL, 0);
}

static void send_wifi_status_notification(void)
{
    if (s_conn_handle == 0 || s_resp_handle == 0) {
        // No active BLE connection
        ESP_LOGD(TAG, "No BLE connection, skipping WiFi status notification");
        return;
    }
    
    // 获取WiFi状态
    int wifi_connected = wifi_get_status();
    char ip_str[16] = {0};
    bool got_ip = wifi_get_ip(ip_str);

    // 构建响应
    // 格式: [status(1字节)] + [ip_len(1字节)] + [ip地址]
    // 状态值: 0=未连接, 1=已连接但无IP, 2=已连接且有IP
    uint8_t payload[1 + 1 + 15]; // 最大IP地址长度为15字节
    uint8_t offset = 0;

    // Determine actual status based on both connection and IP availability
    uint8_t status;
    if (wifi_connected == 0) {
        status = 0; // Not connected
    } else if (got_ip) {
        status = 2; // Connected with IP
    } else {
        status = 1; // Connected but no IP
    }

    // 添加状态
    payload[offset++] = status;

    // 添加IP地址长度和内容（只有状态为2时才有IP）
    if (status == 2) {
        uint8_t ip_len = strlen(ip_str);
        payload[offset++] = ip_len;
        memcpy(&payload[offset], ip_str, ip_len);
        offset += ip_len;
    } else {
        // No IP, send length 0
        payload[offset++] = 0;
    }

    ESP_LOGI(TAG, "Sending WiFi status notification: %d, IP: %s", status, status == 2 ? ip_str : "N/A");
    
    // Send as unsolicited notification (not in response to a command)
    uint8_t buf[2 + 15];
    buf[0] = CMD_GET_WIFI_STATUS;
    buf[1] = RESP_OK;
    memcpy(&buf[2], payload, offset);
    ble_airmic_notify(s_conn_handle, s_resp_handle, buf, 2 + offset);
    
    ESP_LOGD(TAG, "WiFi status notification sent successfully");
}

static void handle_get_wifi_status(void)
{
    // 获取WiFi状态
    int wifi_connected = wifi_get_status();
    char ip_str[16] = {0};
    bool got_ip = wifi_get_ip(ip_str);

    // 构建响应
    // 格式: [status(1字节)] + [ip_len(1字节)] + [ip地址]
    // 状态值: 0=未连接, 1=已连接但无IP, 2=已连接且有IP
    uint8_t payload[1 + 1 + 15]; // 最大IP地址长度为15字节
    uint8_t offset = 0;

    // Determine actual status based on both connection and IP availability
    uint8_t status;
    if (wifi_connected == 0) {
        status = 0; // Not connected
    } else if (got_ip) {
        status = 2; // Connected with IP
    } else {
        status = 1; // Connected but no IP
    }

    // 添加状态
    payload[offset++] = status;

    // 添加IP地址长度和内容（只有状态为2时才有IP）
    if (status == 2) {
        uint8_t ip_len = strlen(ip_str);
        payload[offset++] = ip_len;
        memcpy(&payload[offset], ip_str, ip_len);
        offset += ip_len;
    } else {
        // No IP, send length 0
        payload[offset++] = 0;
    }

    ESP_LOGI(TAG, "WiFi status: %d, IP: %s", status, status == 2 ? ip_str : "N/A");
    send_resp(CMD_GET_WIFI_STATUS, RESP_OK, payload, offset);
}

// ── 对外接口 ─────────────────────────────────────────────────
void airmic_protocol_init(uint16_t conn_handle, uint16_t resp_handle)
{
	s_conn_handle = conn_handle;
	s_resp_handle = resp_handle;
	
	// Register WiFi status change callback for automatic notifications
	wifi_set_status_callback(send_wifi_status_notification);
	
	// Send current WiFi status immediately upon BLE connection
	send_wifi_status_notification();
}

void airmic_protocol_disconnect(void)
{
	// Unregister WiFi callback and clear connection state
	wifi_set_status_callback(NULL);
	s_conn_handle = 0;
	s_resp_handle = 0;
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
	case CMD_SET_WIFI:
		handle_set_wifi(payload, pay_len);
		break;
	case CMD_GET_FILE_LIST:
		handle_get_file_list();
		break;
	case CMD_DELETE_FILE:
		handle_delete_file(payload, pay_len);
		break;
	case CMD_RENAME_FILE:
		handle_rename_file(payload, pay_len);
		break;
	case CMD_GET_WIFI_STATUS:
		handle_get_wifi_status();
		break;
	default:
		ESP_LOGW(TAG, "unknown cmd: 0x%02X", cmd);
		send_resp(cmd, RESP_ERR, NULL, 0);
		break;
	}
}