#include "http_server.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_ota_ops.h"

static const char *TAG = "AirMic_HTTP";

// HTTP 服务器相关变量
static httpd_handle_t s_http_server = NULL;
static file_server_data_t *s_server_data = NULL;

static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
	if (strstr(filename, ".wav")) {
		return httpd_resp_set_type(req, "audio/wav");
	} else if (strstr(filename, ".html")) {
		return httpd_resp_set_type(req, "text/html");
	} else if (strstr(filename, ".ico")) {
		return httpd_resp_set_type(req, "image/x-icon");
	}
	return httpd_resp_set_type(req, "text/plain");
}

static esp_err_t download_get_handler(httpd_req_t *req)
{
	char filepath[FILE_PATH_MAX];
	FILE *fd = NULL;
	struct stat file_stat;
	long start = 0, end = 0;

	// 解析 URL 参数，获取文件名和范围
	char *filename = strchr(req->uri, '?');
	if (!filename) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
		return ESP_FAIL;
	}
	filename++;

	// 提取文件名和范围参数
	char *param_start = strchr(filename, '&');
	if (param_start) {
		// 提取文件名
		*param_start = '\0';
		// 解析范围参数
		char *start_param = strstr(param_start + 1, "start=");
		char *end_param = strstr(param_start + 1, "end=");
		if (start_param) {
			start = strtol(start_param + 6, NULL, 10);
		}
		if (end_param) {
			end = strtol(end_param + 4, NULL, 10);
		}
	}

	// 构建完整路径
	snprintf(filepath, sizeof(filepath), "/sdcard/%s", filename);

	if (stat(filepath, &file_stat) == -1) {
		ESP_LOGE(TAG, "Failed to stat file: %s", filepath);
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
		return ESP_FAIL;
	}

	// 验证范围参数
	if (start < 0)
		start = 0;
	if (end <= 0 || end > file_stat.st_size)
		end = file_stat.st_size;
	if (start >= end) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid range");
		return ESP_FAIL;
	}

	fd = fopen(filepath, "r");
	if (!fd) {
		ESP_LOGE(TAG, "Failed to open file: %s", filepath);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
		return ESP_FAIL;
	}

	// 定位到起始位置
	if (fseek(fd, start, SEEK_SET) != 0) {
		fclose(fd);
		ESP_LOGE(TAG, "Failed to seek file: %s", filepath);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to seek file");
		return ESP_FAIL;
	}

	// 计算要发送的字节数
	long send_size = end - start;
	ESP_LOGI(TAG, "Sending file: %s (range: %ld-%ld, %ld bytes)", filename, start, end, send_size);
	set_content_type_from_file(req, filename);

	// 添加 CORS 头
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	// 添加 Content-Length 头
	char content_length[32];
	sprintf(content_length, "%ld", send_size);
	httpd_resp_set_hdr(req, "Content-Length", content_length);
	// 添加 Content-Range 头
	char content_range[64];
	sprintf(content_range, "bytes %ld-%ld/%ld", start, end - 1, file_stat.st_size);
	httpd_resp_set_hdr(req, "Content-Range", content_range);
	// 设置响应状态码为 206 Partial Content
	httpd_resp_set_status(req, "206 Partial Content");

	// 分配缓冲区来存储要发送的数据
	char *buffer = NULL;
	// 检查是否启用了 PSRAM
	if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > send_size) {
		// 使用 PSRAM 分配缓冲区
		buffer = (char *)heap_caps_malloc(send_size, MALLOC_CAP_SPIRAM);
	} else {
		ESP_LOGW(TAG, "PSRAM not available, using RAM");
	}
	// 如果 PSRAM 分配失败，尝试使用普通 RAM
	if (!buffer) {
		buffer = (char *)malloc(send_size);
	}
	if (!buffer) {
		ESP_LOGE(TAG, "Failed to allocate buffer");
		fclose(fd);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate buffer");
		return ESP_FAIL;
	}

	int64_t t0 = esp_timer_get_time();

	// 一次性读取所有数据
	size_t bytes_read = fread(buffer, 1, send_size, fd);
	fclose(fd);

	if (bytes_read != send_size) {
		free(buffer);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read file");
		return ESP_FAIL;
	}

	int64_t t1 = esp_timer_get_time();

	// 一次性发送所有数据
	ESP_LOGI(TAG, "Sending file data: %ld bytes", send_size);
	int send_result = httpd_resp_send(req, buffer, send_size);
	// 释放缓冲区
	if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0) {
		heap_caps_free(buffer);
	} else {
		free(buffer);
	}

	int64_t t2 = esp_timer_get_time();

	ESP_LOGI(TAG, "fread: %lldms  send: %lldms  total: %lldms", (t1 - t0) / 1000, (t2 - t1) / 1000,
		 (t2 - t0) / 1000);

	if (send_result != ESP_OK) {
		ESP_LOGE(TAG, "File sending failed: %d", send_result);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "File sending complete, sent %ld bytes", send_size);
	ESP_LOGI(TAG, "Download request handled successfully");
	return ESP_OK;
}

static esp_err_t play_get_handler(httpd_req_t *req)
{
	char filepath[FILE_PATH_MAX];
	FILE *fd = NULL;
	struct stat file_stat;

	// 解析 URL 参数，获取文件名
	char *filename = strchr(req->uri, '?');
	if (!filename) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
		return ESP_FAIL;
	}
	filename++;

	// 提取文件名，忽略查询参数
	char *param_start = strchr(filename, '&');
	if (param_start) {
		*param_start = '\0';
	}

	// 构建完整路径
	snprintf(filepath, sizeof(filepath), "/sdcard/%s", filename);

	if (stat(filepath, &file_stat) == -1) {
		ESP_LOGE(TAG, "Failed to stat file: %s", filepath);
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
		return ESP_FAIL;
	}

	fd = fopen(filepath, "r");
	if (!fd) {
		ESP_LOGE(TAG, "Failed to open file: %s", filepath);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Playing file: %s (%ld bytes)", filename, file_stat.st_size);
	set_content_type_from_file(req, filename);

	// 添加 CORS 头
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
	// 添加 Content-Length 头
	char content_length[32];
	sprintf(content_length, "%ld", file_stat.st_size);
	httpd_resp_set_hdr(req, "Content-Length", content_length);

	// 分配缓冲区来存储要发送的数据
	char *buffer = NULL;
	// 检查是否启用了 PSRAM
	if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > file_stat.st_size) {
		// 使用 PSRAM 分配缓冲区
		buffer = (char *)heap_caps_malloc(file_stat.st_size, MALLOC_CAP_SPIRAM);
	}
	// 如果 PSRAM 分配失败，尝试使用普通 RAM
	if (!buffer) {
		buffer = (char *)malloc(file_stat.st_size);
	}
	if (!buffer) {
		fclose(fd);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate buffer");
		return ESP_FAIL;
	}

	// 一次性读取所有数据
	size_t bytes_read = fread(buffer, 1, file_stat.st_size, fd);
	fclose(fd);

	if (bytes_read != file_stat.st_size) {
		// 释放缓冲区
		if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0) {
			heap_caps_free(buffer);
		} else {
			free(buffer);
		}
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read file");
		return ESP_FAIL;
	}

	// 一次性发送所有数据
	int send_result = httpd_resp_send(req, buffer, file_stat.st_size);
	// 释放缓冲区
	if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0) {
		heap_caps_free(buffer);
	} else {
		free(buffer);
	}

	if (send_result != ESP_OK) {
		return ESP_FAIL;
	}

	return ESP_OK;
}

static esp_err_t files_get_handler(httpd_req_t *req)
{
	// 打开SD卡目录
	DIR *dir = opendir("/sdcard");
	if (!dir) {
		ESP_LOGE(TAG, "Failed to open SD card directory");
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open SD card directory");
		return ESP_FAIL;
	}

	// 计算文件数量
	int file_count = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.' || entry->d_type == DT_DIR) {
			continue;
		}
		file_count++;
	}
	rewinddir(dir);

	// 构建JSON响应
	cJSON *root = cJSON_CreateObject();
	cJSON *files = cJSON_CreateArray();
	cJSON_AddItemToObject(root, "files", files);
	cJSON_AddNumberToObject(root, "count", file_count);

	// 遍历文件，添加到JSON
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.' || entry->d_type == DT_DIR) {
			continue;
		}

		// 获取文件大小
		struct stat stat_buf;
		char path[265];
		snprintf(path, sizeof(path), "/sdcard/%s", entry->d_name);
		if (stat(path, &stat_buf) != 0) {
			continue;
		}

		// 创建文件对象
		cJSON *file_obj = cJSON_CreateObject();
		cJSON_AddStringToObject(file_obj, "name", entry->d_name);
		cJSON_AddNumberToObject(file_obj, "size", stat_buf.st_size);
		cJSON_AddItemToArray(files, file_obj);
	}

	closedir(dir);

	// 转换为JSON字符串
	char *json_str = cJSON_Print(root);
	cJSON_Delete(root);

	if (!json_str) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON");
		return ESP_FAIL;
	}

	// 设置响应头
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");

	// 发送响应
	esp_err_t ret = httpd_resp_send(req, json_str, strlen(json_str));
	free(json_str);

	return ret;
}

// GET /ota  → 返回上传页面
static esp_err_t ota_page_handler(httpd_req_t *req) {
    const char *html =
        "<html><body>"
        "<h2>AirMic OTA</h2>"
        "<input type='file' id='f' accept='.bin'>"
        "<button onclick='"
        "var f=document.getElementById(\"f\").files[0];"
        "if(!f){alert(\"select a file\");return;}"
        "var x=new XMLHttpRequest();"
        "x.open(\"POST\",\"/ota\");"
        "x.onload=function(){document.getElementById(\"s\").textContent=x.responseText;};"
        "x.onerror=function(){document.getElementById(\"s\").textContent=\"Error\";};"
        "document.getElementById(\"s\").textContent=\"Uploading...\";"
        "x.send(f);"  // 直接发 File 对象，是裸二进制
        "'>Upload</button>"
        "<p id='s'>—</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

// POST /ota → 接收固件，写入OTA分区
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
	esp_ota_handle_t ota_handle;
	const esp_partition_t *ota_partition = esp_ota_get_next_update_partition(NULL);

	ESP_LOGI(TAG, "OTA target partition: %s", ota_partition->label);
	ESP_ERROR_CHECK(esp_ota_begin(ota_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle));

	char buf[1024];
	int received = 0;
	int remaining = req->content_len;

	while (remaining > 0) {
		int len = httpd_req_recv(req, buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
		if (len <= 0) {
			esp_ota_abort(ota_handle);
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
			return ESP_FAIL;
		}
		ESP_ERROR_CHECK(esp_ota_write(ota_handle, buf, len));
		remaining -= len;
		received += len;
		ESP_LOGI(TAG, "OTA progress: %d / %d bytes", received, req->content_len);
	}

	ESP_ERROR_CHECK(esp_ota_end(ota_handle));
	ESP_ERROR_CHECK(esp_ota_set_boot_partition(ota_partition));

	httpd_resp_sendstr(req, "OK - rebooting...");
	vTaskDelay(pdMS_TO_TICKS(500));
	esp_restart();
	return ESP_OK;
}

esp_err_t http_server_start(void)
{
	esp_err_t err = ESP_OK;

	if (s_http_server) {
		ESP_LOGE(TAG, "HTTP server already started");
		return ESP_ERR_INVALID_STATE;
	}

	s_server_data = calloc(1, sizeof(file_server_data_t));
	if (!s_server_data) {
		ESP_LOGE(TAG, "Failed to allocate memory for server data");
		return ESP_ERR_NO_MEM;
	}
	strcpy(s_server_data->base_path, "/sdcard");

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.uri_match_fn = httpd_uri_match_wildcard;
	config.max_open_sockets = 7; // 减小最大连接数
	config.lru_purge_enable = true; // 启用 LRU 缓存清理
	config.stack_size = 4096 * 4; // 减小任务堆栈大小
	config.recv_wait_timeout = 30; // 增加接收超时时间（毫秒）
	config.send_wait_timeout = 30; // 增加发送超时时间（毫秒）
	config.global_user_ctx = s_server_data; // 设置全局用户上下文
	config.enable_so_linger = true; // 启用 SO_LINGER 选项
	config.linger_timeout = 10; // 设置 SO_LINGER 超时时间

	ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
	err = httpd_start(&s_http_server, &config);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
		free(s_server_data);
		s_server_data = NULL;
		return ESP_FAIL;
	}

	httpd_uri_t download_uri = {
		.uri = "/download", .method = HTTP_GET, .handler = download_get_handler, .user_ctx = s_server_data
	};
	httpd_register_uri_handler(s_http_server, &download_uri);

	httpd_uri_t play_uri = {
		.uri = "/play", .method = HTTP_GET, .handler = play_get_handler, .user_ctx = s_server_data
	};
	httpd_register_uri_handler(s_http_server, &play_uri);

	httpd_uri_t files_uri = {
		.uri = "/files", .method = HTTP_GET, .handler = files_get_handler, .user_ctx = s_server_data
	};
	httpd_register_uri_handler(s_http_server, &files_uri);

	httpd_uri_t ota_page = { .uri = "/ota", .method = HTTP_GET, .handler = ota_page_handler };
	httpd_uri_t ota_upload = { .uri = "/ota", .method = HTTP_POST, .handler = ota_upload_handler };
	httpd_register_uri_handler(s_http_server, &ota_page);
	httpd_register_uri_handler(s_http_server, &ota_upload);

	ESP_LOGI(TAG, "HTTP server started");
	ESP_LOGI(TAG, "Registered /files endpoint for file list retrieval");

	return ESP_OK;
}

esp_err_t http_server_stop(void)
{
	if (!s_http_server) {
		ESP_LOGE(TAG, "HTTP server not started");
		return ESP_ERR_INVALID_STATE;
	}

	if (httpd_stop(s_http_server) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to stop HTTP server!");
		return ESP_FAIL;
	}

	free(s_server_data);
	s_server_data = NULL;
	s_http_server = NULL;

	ESP_LOGI(TAG, "HTTP server stopped");
	return ESP_OK;
}
