#include "wifi.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_vfs.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <esp_timer.h>

static const char *TAG = "AirMic_WiFi";
static char s_ssid[33] = {0};
static char s_password[65] = {0};
static int s_connected = 0;
static char s_ip_address[16] = {0}; // Store IP address when obtained
static wifi_status_callback_t s_status_callback = NULL; // Callback function

// HTTP 服务器相关变量
#define FILE_PATH_MAX 265
#define SCRATCH_BUFSIZE 1024
#define MAX_CHUNK_SIZE 655360 // 最大分块大小 64KB

struct file_server_data {
    char base_path[FILE_PATH_MAX];
    char scratch[SCRATCH_BUFSIZE];
};

static httpd_handle_t s_http_server = NULL;
static struct file_server_data *s_server_data = NULL;

static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename) {
    if (strstr(filename, ".wav")) {
        return httpd_resp_set_type(req, "audio/wav");
    } else if (strstr(filename, ".html")) {
        return httpd_resp_set_type(req, "text/html");
    } else if (strstr(filename, ".ico")) {
        return httpd_resp_set_type(req, "image/x-icon");
    }
    return httpd_resp_set_type(req, "text/plain");
}

static esp_err_t download_get_handler(httpd_req_t *req) {
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
    if (start < 0) start = 0;
    if (end <= 0 || end > file_stat.st_size) end = file_stat.st_size;
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

    ESP_LOGI(TAG, "fread: %lldms  send: %lldms  total: %lldms",
         (t1-t0)/1000, (t2-t1)/1000, (t2-t0)/1000);

    if (send_result != ESP_OK) {
        ESP_LOGE(TAG, "File sending failed: %d", send_result);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "File sending complete, sent %ld bytes", send_size);
    ESP_LOGI(TAG, "Download request handled successfully");
    return ESP_OK;
}

static esp_err_t play_get_handler(httpd_req_t *req) {
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

static esp_err_t files_get_handler(httpd_req_t *req) {
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

static void print_wifi_status(void)
{
    // 打印当前 WiFi 状态
    wifi_mode_t mode;
    esp_err_t ret = esp_wifi_get_mode(&mode);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "WiFi not initialized");
        return;
    }

    // 检查连接状态
    wifi_ap_record_t ap_info;
    ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret == ESP_OK && ap_info.ssid[0] != 0) {
        // 已连接
        ESP_LOGI(TAG, "WiFi connected to: %s", (char*)ap_info.ssid);
    } else {
        ESP_LOGI(TAG, "WiFi not connected, mode: %d", mode);
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, 
                              int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = 0;
        s_ip_address[0] = '\0';
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
        // 停止 HTTP 服务器
        wifi_stop_http_server();
        // Trigger callback for status change
        if (s_status_callback) {
            s_status_callback();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connected = 1;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(s_ip_address, sizeof(s_ip_address), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "WiFi connected, IP address: %s", s_ip_address);
        // 启动 HTTP 服务器
        esp_err_t err = wifi_start_http_server();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        }
        // Trigger callback for status change
        if (s_status_callback) {
            s_status_callback();
        }
    }
}

void wifi_init(void)
{
    // 检查 WiFi 是否已初始化
    wifi_mode_t mode;
    esp_err_t ret = esp_wifi_get_mode(&mode);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi already initialized, mode: %d", mode);
        return;
    }

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, 
                                                        ESP_EVENT_ANY_ID, 
                                                        &wifi_event_handler, 
                                                        NULL, 
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, 
                                                        IP_EVENT_STA_GOT_IP, 
                                                        &wifi_event_handler, 
                                                        NULL, 
                                                        &instance_got_ip));

    ESP_LOGI(TAG, "WiFi initialized");
}

int wifi_set_config(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32) {
        ESP_LOGE(TAG, "Invalid SSID");
        return -1;
    }

    if (password && strlen(password) > 64) {
        ESP_LOGE(TAG, "Invalid password");
        return -1;
    }

    strncpy(s_ssid, ssid, sizeof(s_ssid));
    if (password) {
        strncpy(s_password, password, sizeof(s_password));
    } else {
        s_password[0] = '\0';
    }

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",
            .password = "",
        },
    };

    strncpy((char *)wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, s_password, sizeof(wifi_config.sta.password));

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(err));
        return -1;
    }
    
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        // Handle specific error cases gracefully
        if (err == ESP_ERR_WIFI_STATE) {
            ESP_LOGW(TAG, "WiFi is in connecting state, cannot set config. This is normal if already connecting.");
            // Return success since we're already trying to connect
            return 0;
        }
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "WiFi config set: SSID=%s", s_ssid);
    return 0;
}

int wifi_start(void)
{
    if (strlen(s_ssid) == 0) {
        ESP_LOGE(TAG, "WiFi config not set");
        return -1;
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi started, connecting to %s...", s_ssid);
    
    // 打印当前 WiFi 状态
    print_wifi_status();
    
    return 0;
}

int wifi_stop(void)
{
    ESP_ERROR_CHECK(esp_wifi_stop());
    s_connected = 0;
    ESP_LOGI(TAG, "WiFi stopped");
    return 0;
}

int wifi_get_status(void)
{
    // 使用 ESP-IDF API 检查 WiFi 连接状态
    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret == ESP_OK) {
        // 检查是否有有效的 SSID
        if (ap_info.ssid[0] != 0) {
            return 1; // 已连接
        }
    }
    return 0; // 未连接
}

bool wifi_get_ip(char *ip_str)
{
    if (!ip_str) {
        return false;
    }
    
    // Clear the output buffer first
    ip_str[0] = '\0';
    
    // If we have a stored IP address and are connected, return it
    if (s_connected && s_ip_address[0] != '\0') {
        strncpy(ip_str, s_ip_address, 15);
        ip_str[15] = '\0'; // Ensure null termination
        return true;
    }
    
    return false;
}

bool wifi_is_connected_to_ssid(const char *ssid)
{
    if (!ssid || !s_ssid[0]) {
        return false;
    }
    
    // Check if we have stored the same SSID and are connected
    if (s_connected && strcmp(s_ssid, ssid) == 0) {
        return true;
    }
    
    // Fallback: query current connection if needed
    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret == ESP_OK && ap_info.ssid[0] != 0) {
        if (strcmp((char*)ap_info.ssid, ssid) == 0) {
            return true;
        }
    }
    
    return false;
}

void wifi_set_status_callback(wifi_status_callback_t callback)
{
    s_status_callback = callback;
}

esp_err_t wifi_start_http_server(void)
{
    esp_err_t err = ESP_OK;

    if (s_http_server) {
        ESP_LOGE(TAG, "HTTP server already started");
        return ESP_ERR_INVALID_STATE;
    }

    // 检查 WiFi 是否连接
    if (!s_connected) {
        ESP_LOGE(TAG, "WiFi not connected, cannot start HTTP server");
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_server_data = calloc(1, sizeof(struct file_server_data));
    if (!s_server_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for server data");
        return ESP_ERR_NO_MEM;
    }
    strcpy(s_server_data->base_path, "/sdcard");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = 4; // 减小最大连接数
    config.lru_purge_enable = true; // 启用 LRU 缓存清理
    config.stack_size = 8192; // 减小任务堆栈大小
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
        .uri       = "/download",
        .method    = HTTP_GET,
        .handler   = download_get_handler,
        .user_ctx  = s_server_data
    };
    httpd_register_uri_handler(s_http_server, &download_uri);

    httpd_uri_t play_uri = {
        .uri       = "/play",
        .method    = HTTP_GET,
        .handler   = play_get_handler,
        .user_ctx  = s_server_data
    };
    httpd_register_uri_handler(s_http_server, &play_uri);

    httpd_uri_t files_uri = {
        .uri       = "/files",
        .method    = HTTP_GET,
        .handler   = files_get_handler,
        .user_ctx  = s_server_data
    };
    httpd_register_uri_handler(s_http_server, &files_uri);

    ESP_LOGI(TAG, "HTTP server started");
    ESP_LOGI(TAG, "Registered /files endpoint for file list retrieval");

    return ESP_OK;
}

esp_err_t wifi_stop_http_server(void)
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