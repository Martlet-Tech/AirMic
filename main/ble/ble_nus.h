#pragma once
#include <stdint.h>

/**
 * 初始化 BLE NUS，在 app_main() 里调 1 次
 */
void ble_nus_init(void);

/**
 * 把数据 notify 给已连接的 Central（Betaflight Configurator）
 * 在 FC UART RX task 里调用
 */
void ble_nus_send(const uint8_t *data, uint16_t len);

/**
 * @brief 把数据 notify 给已连接的 Central（AirMic）
 * 
 * @param conn_handle 
 * @param attr_handle 
 * @param data 
 * @param len 
 */
void ble_airmic_notify(uint16_t conn_handle, uint16_t attr_handle, const uint8_t *data, uint16_t len);