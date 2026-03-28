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

/**
 * @brief 暂停 NUS 可连接广播（RID 模式进入前调用）
 *
 * 会主动断开当前已连接的 BF Configurator，然后停止广播。
 * RID 模块随后可以独占广播信道发送 OpenDroneID 帧。
 */
void ble_nus_pause_advertising(void);

/**
 * @brief 恢复 NUS 可连接广播（RID 模式退出后调用）
 *
 * 重新启动 NUS 广播，让 BF Configurator 能再次连入。
 */
void ble_nus_resume_advertising(void);