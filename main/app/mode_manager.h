#pragma once

/**
 * 系统工作模式状态机
 * 所有模式切换都通过这里，保证互斥
 */

typedef enum {
    MODE_RECORDER,      // 录音机模式：轮询FC解锁 → 触发录音，BLE可调参
    MODE_BLE_BRIDGE,    // 蓝牙调参桥模式：UART完全透传，不录音
} system_mode_t;

void mode_manager_init(void);
void mode_manager_switch(system_mode_t mode);
system_mode_t mode_manager_get(void);
