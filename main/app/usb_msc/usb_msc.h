#pragma once
#include <stdbool.h>
#include "driver/gpio.h"

/**
 * USB MSC (Mass Storage Class) 模块
 * 把 SD 卡虚拟成 U 盘暴露给电脑
 *
 * 使用条件：
 *   - 不能和 FATFS 同时运行，必须在 sdcard_mount 之前调用
 *   - sdkconfig 需要：
 *       CONFIG_TINYUSB_MSC_ENABLED=y
 *       CONFIG_TINYUSB_MSC_BUFSIZE=4096
 */

 void usb_msc_init_vbus(gpio_num_t pin);

// 初始化并启动 USB MSC，阻塞直到 USB 断开
// 在 BOOT_MODE_USB_MSC 分支里调用，不返回（或返回后重启）
void usb_msc_start(void);

// 请求切换到 USB MSC 模式：写 NVS 标志 + 重启
// 在 airmicprotocol 收到 CMD_REBOOT_MSC 时调用
void usb_msc_request_switch(void);

// 检测 USB 主机是否接入（VBUS 检测）
bool usb_host_connected(void);
