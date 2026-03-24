/**
 * AirMic - BLE Nordic UART Service (NUS)
 *
 * 实现标准 NUS，兼容 Betaflight Configurator / nRF Toolbox 等工具
 * 同时暴露自定义 AirMic Config Service，两个 Service 互不干扰
 *
 * 依赖：ESP-IDF >= 5.0，NimBLE 协议栈
 * sdkconfig 需要打开：
 *   CONFIG_BT_ENABLED=y
 *   CONFIG_BT_NIMBLE_ENABLED=y
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "bsp.h"

// --------------------------------------------------------------------------
// 设备名，将来改这一行就够了
// --------------------------------------------------------------------------
#define DEVICE_NAME "AirMic"

#define FC_UART_PORT UART_NUM_1

#define FC_UART_BAUD 115200
#define FC_UART_BUF 1024

// --------------------------------------------------------------------------
// Nordic UART Service UUID（固定，BF Configurator 认这套）
// --------------------------------------------------------------------------
// Service:  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
// TX Char:  6E400002-...  (Central → Peripheral，Write)
// RX Char:  6E400003-...  (Peripheral → Central，Notify)
// --------------------------------------------------------------------------
static ble_uuid128_t nus_svc_uuid = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3,
						     0xb5, 0x01, 0x00, 0x40, 0x6e);

static ble_uuid128_t nus_tx_uuid = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3,
						    0xb5, 0x02, 0x00, 0x40, 0x6e);

static ble_uuid128_t nus_rx_uuid = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3,
						    0xb5, 0x03, 0x00, 0x40, 0x6e);

static ble_uuid128_t airmic_svc_uuid = BLE_UUID128_INIT(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
							0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10);

static ble_uuid128_t airmic_status_uuid = BLE_UUID128_INIT(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
							   0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x11);

static const char *TAG = "AirMic_BLE";

// --------------------------------------------------------------------------
// 全局状态
// --------------------------------------------------------------------------
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_nus_rx_handle = 0; // Notify handle（发给手机）
static uint8_t g_armed = 0; // FC 解锁状态，外部写入

// --------------------------------------------------------------------------
// 对外接口：把数据透传给已连接的 BLE Central（Betaflight Configurator）
// 在 UART RX task 里调用这个函数
// --------------------------------------------------------------------------
void ble_nus_send(const uint8_t *data, uint16_t len)
{
	if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || g_nus_rx_handle == 0) {
		return;
	}
	struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
	if (!om)
		return;
	ble_gatts_notify_custom(g_conn_handle, g_nus_rx_handle, om);
}

// --------------------------------------------------------------------------
// NUS TX Characteristic 回调（BF Configurator 写过来 → 转发给 FC UART）
// --------------------------------------------------------------------------
static int nus_tx_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
		uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
		uint8_t buf[512];
		if (len > sizeof(buf))
			len = sizeof(buf);
		os_mbuf_copydata(ctxt->om, 0, len, buf);

		// 实际 UART 发送函数
		uart_write_bytes(FC_UART_PORT, buf, len);
		ESP_LOGI(TAG, "NUS TX recv %d bytes → FC UART", len);
	}
	return 0;
}

// --------------------------------------------------------------------------
// AirMic Status Characteristic 回调（读取录音/解锁状态）
// --------------------------------------------------------------------------
static int airmic_status_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
				void *arg)
{
	if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
		// 返回当前状态，格式随便定，这里先给个简单 JSON
		char status[64];
		snprintf(status, sizeof(status), "{\"armed\":%d,\"recording\":%d}", g_armed, g_armed);
		os_mbuf_append(ctxt->om, status, strlen(status));
	}
	return 0;
}

static int dummy_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	return 0;
}

// --------------------------------------------------------------------------
// GATT Service 表
// --------------------------------------------------------------------------
static const struct ble_gatt_svc_def g_gatt_svcs[] = {

	// ── Service 1: Nordic UART Service ─────────────────────────────────────
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = &nus_svc_uuid.u,
		.characteristics =
			(struct ble_gatt_chr_def[]){

				// TX Char：Central 写数据过来（MSP 命令 → FC）
				{
					.uuid = &nus_tx_uuid.u,
					.access_cb = nus_tx_chr_access,
					.flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
				},

				// RX Char：我们 Notify 给 Central（FC 返回数据 → BF Configurator）
				{
					.uuid = &nus_rx_uuid.u,
					.access_cb = dummy_access_cb,
					.flags = BLE_GATT_CHR_F_NOTIFY,
					.val_handle = &g_nus_rx_handle,
				},

				{ 0 } // 结束符
			},
	},

	// ── Service 2: AirMic 自定义 Service ───────────────────────────────────
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = &airmic_svc_uuid.u,
		.characteristics =
			(struct ble_gatt_chr_def[]){

				// Status Char：只读，返回 armed/recording 状态
				{
					.uuid = &airmic_status_uuid.u,
					.access_cb = airmic_status_access,
					.flags = BLE_GATT_CHR_F_READ,
				},

				// 以后在这里加：config char、command char 等

				{ 0 } },
	},

	{ 0 } // GATT 表结束符
};

// --------------------------------------------------------------------------
// GAP 事件回调
// --------------------------------------------------------------------------
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
	switch (event->type) {
	case BLE_GAP_EVENT_CONNECT:
		if (event->connect.status == 0) {
			g_conn_handle = event->connect.conn_handle;
			ESP_LOGI(TAG, "BLE connected, handle=%d", g_conn_handle);
		} else {
			ESP_LOGW(TAG, "BLE connect failed, restarting adv");
			g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
			// 断开后重新广播
			ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, NULL, gap_event_handler, NULL);
		}
		break;

	case BLE_GAP_EVENT_DISCONNECT:
		ESP_LOGI(TAG, "BLE disconnected, reason=%d", event->disconnect.reason);
		g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
		ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, NULL, gap_event_handler, NULL);
		break;

	case BLE_GAP_EVENT_MTU:
		ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
		break;

	default:
		break;
	}
	return 0;
}

// --------------------------------------------------------------------------
// 开始广播
// --------------------------------------------------------------------------
static void ble_start_advertising(void)
{
	struct ble_hs_adv_fields fields = { 0 };
	fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
	fields.uuids128 = &nus_svc_uuid;
	fields.num_uuids128 = 1;
	fields.uuids128_is_complete = 1;
	fields.name = (uint8_t *)DEVICE_NAME;
	fields.name_len = strlen(DEVICE_NAME);
	fields.name_is_complete = 1;
	ble_gap_adv_set_fields(&fields);

	struct ble_hs_adv_fields rsp_fields = { 0 };
	rsp_fields.name = (uint8_t *)DEVICE_NAME;
	rsp_fields.name_len = strlen(DEVICE_NAME);
	rsp_fields.name_is_complete = 1;
	ble_gap_adv_rsp_set_fields(&rsp_fields); // ← scan response 单独设置

	struct ble_gap_adv_params adv_params = { 0 };
	adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; // 可连接
	adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; // 可发现

	int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, gap_event_handler, NULL);
	if (rc != 0) {
		ESP_LOGE(TAG, "adv_start failed: %d", rc);
	} else {
		ESP_LOGI(TAG, "BLE advertising as \"%s\"", DEVICE_NAME);
	}
}

// --------------------------------------------------------------------------
// NimBLE host 同步回调（stack 就绪后调用）
// --------------------------------------------------------------------------
static void ble_on_sync(void)
{
	ble_hs_util_ensure_addr(0);
	ble_start_advertising();
}

// --------------------------------------------------------------------------
// NimBLE host task
// --------------------------------------------------------------------------
static void nimble_host_task(void *param)
{
	nimble_port_run(); // 阻塞，直到 nimble_port_stop()
	nimble_port_freertos_deinit();
}

#if 0
static void ble_test_task(void *arg)
{
	uint32_t count = 0;
	while (1) {
		if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
			char buf[32];
			int len = snprintf(buf, sizeof(buf), "AirMic tick %lu\n", count++);
			ble_nus_send((uint8_t *)buf, len);
		}
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
#endif

static void fc_uart_init(void)
{
	uart_config_t cfg = {
		.baud_rate = FC_UART_BAUD,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
	};
	uart_driver_install(FC_UART_PORT, FC_UART_BUF, FC_UART_BUF, 0, NULL, 0);
	uart_param_config(FC_UART_PORT, &cfg);
	uart_set_pin(FC_UART_PORT, FC_UART_TX_PIN, FC_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
	ESP_LOGI(TAG, "FC UART init done");
}

static void fc_uart_rx_task(void *arg)
{
	uint8_t buf[FC_UART_BUF];
	while (1) {
		int len = uart_read_bytes(FC_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(20));
		if (len > 0) {
			ble_nus_send(buf, len);
		}
	}
}

// --------------------------------------------------------------------------
// 对外初始化入口，在 app_main 里调用一次
// --------------------------------------------------------------------------
void ble_nus_init(void)
{
	ESP_ERROR_CHECK(nvs_flash_init());

	nimble_port_init();

	ble_svc_gap_init();
	ble_svc_gatt_init();

	int rc = ble_gatts_count_cfg(g_gatt_svcs);
	ESP_LOGI(TAG, "ble_gatts_count_cfg rc=%d", rc);
	assert(rc == 0);

	rc = ble_gatts_add_svcs(g_gatt_svcs);
	ESP_LOGI(TAG, "ble_gatts_add_svcs rc=%d", rc);
	assert(rc == 0);

	ble_hs_cfg.sync_cb = ble_on_sync;

	ble_svc_gap_device_name_set(DEVICE_NAME);

	nimble_port_freertos_init(nimble_host_task);

	ESP_LOGI(TAG, "BLE NUS init done");

	fc_uart_init();
	xTaskCreate(fc_uart_rx_task, "fc_uart_rx", 4096, NULL, 5, NULL);
}
