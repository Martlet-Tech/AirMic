#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
// WiFi status change callback type
typedef void (*wifi_status_callback_t)(void);

/**
 * WiFi 模块初始化
 */
void wifi_init(void);

/**
 * 设置 WiFi 连接参数
 * @param ssid WiFi 名称
 * @param password WiFi 密码
 * @return 0 成功，其他失败
 */
int wifi_set_config(const char *ssid, const char *password);

/**
 * 启动 WiFi 连接
 * @return 0 成功，其他失败
 */
int wifi_start(void);

/**
 * 停止 WiFi 连接
 * @return 0 成功，其他失败
 */
int wifi_stop(void);

/**
 * 获取 WiFi 连接状态
 * @return 0 未连接，1 已连接
 */
int wifi_get_status(void);

/**
 * 获取 WiFi IP 地址
 * @param ip_str 输出参数，用于存储IP地址字符串（至少16字节）
 * @return true 如果已获取IP地址，false 否则
 */
bool wifi_get_ip(char *ip_str);

/**
 * 检查是否已连接到指定的SSID
 * @param ssid 要检查的SSID
 * @return true 如果已连接到指定SSID，false 否则
 */
bool wifi_is_connected_to_ssid(const char *ssid);

/**
 * 设置WiFi状态变化回调函数
 * @param callback 回调函数，当WiFi连接状态或IP地址发生变化时调用
 */
void wifi_set_status_callback(wifi_status_callback_t callback);

/**
 * 启动 HTTP 服务器
 * @return 0 成功，其他失败
 */
esp_err_t wifi_start_http_server(void);

/**
 * 停止 HTTP 服务器
 * @return 0 成功，其他失败
 */
esp_err_t wifi_stop_http_server(void);