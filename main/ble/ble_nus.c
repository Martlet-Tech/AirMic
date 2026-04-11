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
#include "airmicprotocol.h"
#include "rgb_led.h"

// --------------------------------------------------------------------------
// 设备名，将来改这一行就够了
// --------------------------------------------------------------------------
#define DEVICE_NAME "Martlet AirMic"

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

static ble_uuid128_t nus_readCharacteristic_uuid = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
								    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

static ble_uuid128_t nus_writeCharacteristic_uuid = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
								     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static ble_uuid128_t airmic_svc_uuid = BLE_UUID128_INIT(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
							0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10);

static ble_uuid128_t airmic_ctrl_uuid = BLE_UUID128_INIT(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
							 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x12);

static ble_uuid128_t airmic_resp_uuid = BLE_UUID128_INIT(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
							 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x13);

static const char *TAG = "AirMic_BLE";

// --------------------------------------------------------------------------
// 全局状态
// --------------------------------------------------------------------------
uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_nus_rx_handle = 0; // Notify handle（发给手机）
static uint16_t g_airmic_resp_handle = 0;

// RID 模式标志：true 时 NUS 广播完全让位给 RID 广播
static bool s_rid_mode = false;

static void ble_start_advertising(void);

// --------------------------------------------------------------------------
// 对外接口：把数据透传给已连接的 BLE Central（Betaflight Configurator）
// 在 UART RX task 里调用这个函数
// --------------------------------------------------------------------------
void ble_nus_send(const uint8_t *data, uint16_t len)
{
	if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || g_nus_rx_handle == 0) {
		ESP_LOGW(TAG, "ble_nus_send: no active connection %d or RX handle %d", g_conn_handle, g_nus_rx_handle);
		return;
	}
	struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
	if (!om) {
		ESP_LOGW(TAG, "ble_nus_send: failed to create mbuf");
		return;
	}
	int rc = ble_gatts_notify_custom(g_conn_handle, g_nus_rx_handle, om);
	if (rc != 0) {
		ESP_LOGW(TAG, "notify failed: %d", rc); // rc=12 是 BLE_HS_ENOMEM，队列满
	}
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

		/*ESP_LOGI(TAG, "Received from Bluetooth %d bytes", len);
		for (int i = 0; i < len; i++) {
			printf("0x%02X ", buf[i]);
		}
		printf("\n");
		fflush(stdout);*/
		// 实际 UART 发送函数
		uart_write_bytes(FC_UART_PORT, buf, len);
	}
	return 0;
}

static int dummy_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	return 0;
}

// ctrl write 回调
static int airmic_ctrl_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
		uint8_t buf[64];
		uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
		if (len > sizeof(buf))
			len = sizeof(buf);
		os_mbuf_copydata(ctxt->om, 0, len, buf);
		airmic_protocol_on_write(conn_handle, buf, len);
	}
	return 0;
}

// notify 接口，给 airmicprotocol.c 调用
void ble_airmic_notify(uint16_t conn_handle, uint16_t attr_handle, const uint8_t *data, uint16_t len)
{
	struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
	if (!om)
		return;
	ble_gatts_notify_custom(conn_handle, g_airmic_resp_handle, om);
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
				// https://github.com/betaflight/betaflight-configurator/blob/master/src/js/protocols/devices.js
				// 参考这里的uuid与读写的关系, 跟正常的情况不同
				// TX Char：Central 写数据过来（MSP 命令 → FC）
				{
					.uuid = &nus_readCharacteristic_uuid.u,
					.access_cb = dummy_access_cb,
					.flags = BLE_GATT_CHR_F_NOTIFY,
					.val_handle = &g_nus_rx_handle,
				},

				// RX Char：我们 Notify 给 Central（FC 返回数据 → BF Configurator）
				{
					.uuid = &nus_writeCharacteristic_uuid.u,
					.access_cb = nus_tx_chr_access,
					.flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
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

				// Control Point：手机写命令过来
				{
					.uuid = &airmic_ctrl_uuid.u,
					.access_cb = airmic_ctrl_access,
					.flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
				},

				// Response：板子 notify 回手机
				{
					.uuid = &airmic_resp_uuid.u,
					.access_cb = dummy_access_cb,
					.flags = BLE_GATT_CHR_F_NOTIFY,
					.val_handle = &g_airmic_resp_handle,
				},

				{ 0 } },
	},

	{ 0 } // GATT 表结束符
};

static const struct ble_gap_upd_params fast_conn_params = {
	.itvl_min = 0x0006, // 0x0006 * 1.25ms = 7.5ms
	.itvl_max = 0x000C, // 0x000C * 1.25ms = 15ms
	.latency = 0, // 0 延迟，保证实时性
	.supervision_timeout = 0x0100, // 100 * 10ms = 1000ms (超时时间)
	.min_ce_len = 0x0000,
	.max_ce_len = 0x0000,
};

void ble_bridge_request_fast_connection(uint16_t conn_handle)
{
	int rc;
	ESP_LOGI(TAG, "Requesting fast connection params...");

	// 发起连接参数更新请求
	// 注意：这只是“请求”，手机可能会拒绝，但大多数现代手机 (iOS/Android) 对于 7.5ms-15ms 都会接受
	rc = ble_gap_update_params(conn_handle, &fast_conn_params);

	if (rc != 0) {
		ESP_LOGE(TAG, "Failed to request params update: %d", rc);
	}
}

// --------------------------------------------------------------------------
// GAP 事件回调
// --------------------------------------------------------------------------
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
	switch (event->type) {
	case BLE_GAP_EVENT_CONNECT:
		ESP_LOGI(TAG, "GAP CONNECT event: status=%d conn_handle=%d", event->connect.status,
			 event->connect.conn_handle);

		if (event->connect.status == 0) {
			g_conn_handle = event->connect.conn_handle;
			ESP_LOGI(TAG, "BLE connected, handle=%d", g_conn_handle);

			// 启动RGB LED呼吸变色效果
			rgb_led_set_mode(RGB_LED_MODE_BREATHING);

			vTaskDelay(pdMS_TO_TICKS(200));
			ble_bridge_request_fast_connection(g_conn_handle);
		} else {
			// status=26 时连接可能仍然有效，conn_handle 也是真实的
			if (event->connect.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
				g_conn_handle = event->connect.conn_handle; // ← 关键
				ESP_LOGW(TAG, "connect param negotiation failed(%d), but using handle=%d",
					 event->connect.status, g_conn_handle);
				// 启动RGB LED呼吸变色效果
				rgb_led_set_mode(RGB_LED_MODE_BREATHING);
			} else {
				ESP_LOGW(TAG, "connect truly failed, status=%d, restarting adv", event->connect.status);
				ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, NULL, gap_event_handler,
					  NULL);
			}
		}
		break;
	case BLE_GAP_EVENT_SUBSCRIBE:
		ESP_LOGI(TAG, "subscribe: handle=%d reason=%d cur_notify=%d nus_rx_handle=%d",
			 event->subscribe.attr_handle, event->subscribe.reason, event->subscribe.cur_notify,
			 g_nus_rx_handle);
		break;

	case BLE_GAP_EVENT_DISCONNECT:
		ESP_LOGI(TAG, "BLE disconnected, reason=%d", event->disconnect.reason);
		g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
		// 关闭RGB LED
		rgb_led_set_mode(RGB_LED_MODE_OFF);
		// RID 模式期间不恢复 NUS 广播，让 RID 独占信道
		if (!s_rid_mode) {
			ble_start_advertising();
		}
		break;

	case BLE_GAP_EVENT_MTU:
		ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
		break;

	case BLE_GAP_EVENT_CONN_UPDATE:
		// 可选：打印更新结果，确认是否成功
		if (event->conn_update.status == 0) {
			ESP_LOGI(TAG, "Connection params updated successfully!");
			//ESP_LOGI(TAG, "Interval: %d ms", event->conn_update.conn_itvl * 1.25);
		} else {
			ESP_LOGW(TAG, "Connection params update rejected by peer (status=%d)",
				 event->conn_update.status);
			// 如果被拒绝，可能是手机不支持这么快的速度，或者需要先配对
		}
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
	ble_gap_adv_set_fields(&fields);

	struct ble_hs_adv_fields rsp_fields = { 0 };
	rsp_fields.name = (uint8_t *)DEVICE_NAME;
	rsp_fields.name_len = strlen(DEVICE_NAME);
	rsp_fields.name_is_complete = 1;
	ble_gap_adv_rsp_set_fields(&rsp_fields);

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

static void disable_nimble_verbose_logs()
{
	// "NimBLE" 是你在日志里看到的标签
	esp_log_level_set("NimBLE", ESP_LOG_WARN);

	// 如果有其他蓝牙相关标签，也可以一起关掉
	esp_log_level_set("BLE_GATT", ESP_LOG_WARN);
	esp_log_level_set("BLE_GAP", ESP_LOG_WARN);
	esp_log_level_set("BLE_HS", ESP_LOG_WARN);

	ESP_LOGI("TAG", "Verbose NimBLE logs disabled.");
}
// --------------------------------------------------------------------------
// 对外初始化入口，在 app_main 里调用一次
// --------------------------------------------------------------------------
void ble_nus_init(void)
{
	ESP_ERROR_CHECK(nvs_flash_init());

	disable_nimble_verbose_logs();
	nimble_port_init();

	// ← 加这几行，关掉 SC 避免协商失败
	ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
	//ble_hs_cfg.sm_bonding = 0;
	//ble_hs_cfg.sm_mitm = 0;
	//ble_hs_cfg.sm_sc = 0; // 关掉 LE Secure Connections

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

	// 初始化RGB LED（IO40引脚）
	rgb_led_init(GPIO_NUM_40);
	// 初始状态为关闭
	rgb_led_set_mode(RGB_LED_MODE_OFF);

	nimble_port_freertos_init(nimble_host_task);

	ESP_LOGI(TAG, "BLE NUS init done");
}

// --------------------------------------------------------------------------
// RID 模式广播控制
// 注意：必须在 NimBLE host task 上下文（callout / gap event）中调用，
//       或通过 ble_npl_callout 转发，不可直接在普通 FreeRTOS task 中调用。
// --------------------------------------------------------------------------
void ble_nus_pause_advertising(void)
{
	if (s_rid_mode) {
		return; // 已在 RID 模式，幂等
	}
	s_rid_mode = true;
	ESP_LOGI(TAG, "NUS adv paused (RID mode ON)");

	// 如果有已连接的 BF Configurator，先主动断开
	if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
		ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
		// g_conn_handle 会在 DISCONNECT 事件里被清零
	}

	// 停止当前广播（不管是 NUS 还是其他）
	ble_gap_adv_stop();
}

void ble_nus_resume_advertising(void)
{
	if (!s_rid_mode) {
		return; // 已在 NUS 模式，幂等
	}
	s_rid_mode = false;
	ESP_LOGI(TAG, "NUS adv resumed (RID mode OFF)");

	// 停掉 RID 广播，重新启动 NUS 可连接广播
	ble_gap_adv_stop();
	ble_start_advertising();
}
