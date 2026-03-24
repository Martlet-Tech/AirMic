#pragma once

/**
 * 蓝牙调参桥模式
 * - UART 完全透传：BLE NUS ↔ FC UART
 * - 不轮询解锁状态，不录音
 * - 与录音机模式互斥
 */

void ble_bridge_start(void);
void ble_bridge_stop(void);
