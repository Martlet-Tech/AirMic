#include "wifi.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include <string.h>

static const char *TAG = "AirMic_WiFi";
static char s_ssid[33] = {0};
static char s_password[65] = {0};
static int s_connected = 0;

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
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connected = 1;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi connected, IP address: " IPSTR, IP2STR(&event->ip_info.ip));
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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

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