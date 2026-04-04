#pragma once
#include <stdint.h>

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