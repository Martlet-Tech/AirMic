#include "fc_link.h"
#include "recorder.h"
#include "led.h"
#include "mic.h"
#include "sdcard.h"
#include "bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h" // 引入信号量头文件
#include "driver/uart.h"
#include <stdio.h>
#include <string.h>
#include "ble_nus.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "rid.h"

#define FC_UART_BAUD 115200
#define POLL_INTERVAL_MS 100
#define TAG "FC_LINK"

extern uint16_t g_conn_handle;
static uint16_t l_conn_handle;

// MSP STATUS 请求包
const uint8_t MSP_STATUS_REQ[] = { '$', 'M', '<', 0x00, 0x65, 0x65 };

// version cmd:  size=0, cmd=1, checksum=0x01
const uint8_t MSP_API_VERSION_REQ[] = { '$', 'M', '<', 0x00, 0x01, 0x01 };

static const uint8_t MSP_UID_REQ[] = { '$', 'M', '<', 0x00, 0xA0, 0xA0 };

static bool s_uid_fetched = false;

static TaskHandle_t s_poll_task = NULL;
static bool s_armed = false;
static bool s_polling = true;

void uart_init(void)
{
	uart_config_t cfg = {
		.baud_rate = FC_UART_BAUD,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
	};
	uart_driver_install(FC_UART_PORT, 256, 256, 0, NULL, 0);
	uart_param_config(FC_UART_PORT, &cfg);
	uart_set_pin(FC_UART_PORT, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void uart_deinit(void)
{
	if (uart_is_driver_installed(FC_UART_PORT)) {
		uart_driver_delete(FC_UART_PORT);
	}
}

// ── MSP 解析 armed ───────────────────────────────────────────
static bool parse_armed(const uint8_t *buf, int len)
{
	if (len < 13)
		return false;
	if (buf[0] != '$' || buf[1] != 'M' || buf[2] != '>')
		return false;
	uint8_t payload_size = buf[3];
	if (len < 5 + payload_size + 1 || payload_size < 8)
		return false;
	uint16_t flags = buf[5 + 6] | (buf[5 + 7] << 8);
	return (flags & 0x0001) != 0;
}

// ── FC 轮询任务 ──────────────────────────────────────────────
static void poll_task(void *arg)
{
	while (s_polling) {
		if (l_conn_handle != g_conn_handle) {
			ESP_LOGI(TAG, "conn_handle changed");
			if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE)
				led_set_mode(LED_MODE_OFF);
			else
				led_set_mode(LED_MODE_IDLE);
		}

		if (l_conn_handle == BLE_HS_CONN_HANDLE_NONE && g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
			uart_flush_input(FC_UART_PORT);
		}

		l_conn_handle = g_conn_handle;

		// 轮询飞控是否已 arming
		if (l_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
			uint8_t rx_buf[512];
			uart_write_bytes(FC_UART_PORT, MSP_STATUS_REQ, sizeof(MSP_STATUS_REQ));
			vTaskDelay(pdMS_TO_TICKS(20));
			int len = uart_read_bytes(FC_UART_PORT, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(20));
			if (len > 0) {
				bool armed = parse_armed(rx_buf, len);
				if (armed != s_armed) {
					s_armed = armed;
					ESP_LOGI(TAG, "FC armed=%d", armed);

					if (armed) {
						recorder_start();
						if (rid_get_enabled())
							rid_start();
					} else {
						recorder_stop();
						if (rid_get_enabled())
							rid_stop();
					}
				}
			}
			vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
		} else { // 蓝牙调参桥启动
			uint8_t rx_buf[512];
			int len = uart_read_bytes(FC_UART_PORT, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(10));
			if (len > 0) {
				ble_nus_send(rx_buf, len);
			}
		}
	}
	s_poll_task = NULL; // 清空句柄
	vTaskDelete(NULL);
}

void fc_link_start(void)
{
	uart_init();
	vTaskDelay(pdMS_TO_TICKS(100));

	uint8_t rx_buf[512];

	if (!s_uid_fetched) {
		uart_write_bytes(FC_UART_PORT, MSP_UID_REQ, sizeof(MSP_UID_REQ));
		vTaskDelay(pdMS_TO_TICKS(50));
		int len = uart_read_bytes(FC_UART_PORT, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(50));
		if (len >= 17) { // $M> + size(1) + cmd(1) + payload(12) + crc(1)
			rid_init(&rx_buf[5], 12);
			s_uid_fetched = true;
		}
	}

	xTaskCreate(poll_task, "fc_poll", 4096, NULL, 5, &s_poll_task);
}

void fc_link_stop(void)
{
	s_polling = false;
}
