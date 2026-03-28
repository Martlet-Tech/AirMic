#include "rid.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include <string.h>
#include <stdio.h>
#include "nimble/nimble_port.h"
#include "ble_nus.h" // pause / resume NUS 广播

static const char *TAG = "RID";
static struct ble_npl_callout s_rid_callout;
static int s_msg_idx = 0;

// ── CAAC / ASTM F3411 常量 ────────────────────────────────────
#define ODID_SERVICE_UUID 0xFFFA // ASTM 注册的 16-bit Service UUID
#define ODID_MSG_LEN 25 // 每条报文固定 25 字节
#define ODID_PROTO_VER 0x1 // 接口版本

// 报文类型（报头高 4 位）
#define MSG_BASIC_ID 0x0
#define MSG_LOCATION 0x1
#define MSG_SYSTEM 0x4

// 基本ID - ID类型（byte1 高4位）
#define ID_TYPE_SERIAL 0x1 // 产品唯一识别码
// 基本ID - UA类型（byte1 低4位）
#define UA_TYPE_ROTORCRAFT 0x2 // 旋翼机

// ── 模块状态 ──────────────────────────────────────────────────
static bool s_enabled = true;
static bool s_running = false;

// UAS ID：20字节 ASCII HEX，从飞控 UID 生成
static char s_uas_id[20] = { 0 };
static bool s_uid_set = false;

// ── 报文构造 ─────────────────────────────────────────────────

static void build_basic_id(uint8_t *msg)
{
	memset(msg, 0, ODID_MSG_LEN);
	msg[0] = (MSG_BASIC_ID << 4) | ODID_PROTO_VER;
	// byte1: 高4位=ID类型，低4位=UA类型
	msg[1] = (ID_TYPE_SERIAL << 4) | UA_TYPE_ROTORCRAFT;
	// byte2~21: UASID，20字节
	memcpy(&msg[2], s_uas_id, 20);
	// byte22~24: 预留，保持0
}

// 位置向量报文，当前无 GPS，全部填 unknown 占位
// 将来接 GPS 后替换这里的字段
static void build_location(uint8_t *msg)
{
	memset(msg, 0, ODID_MSG_LEN);
	msg[0] = (MSG_LOCATION << 4) | ODID_PROTO_VER;

	// byte1: bits[7:4]=OperationalStatus, bit[3]=Reserved, bit[2]=HeightType,
	//        bit[1]=EWDirection, bit[0]=SpeedMultiplier
	// OperationalStatus: 0=Undeclared, 1=Ground, 2=Airborne
	// 无 GPS 时 Status=Ground(1)，让 App 知道飞机在地面
	msg[1] = (1 << 4); // OperationalStatus = 1 (Ground)

	// byte2: 航迹角 0~179，EWDirection 区分东西半球
	// 无 GPS 无航向，设为 0（App 显示 0°=正北，可接受）
	msg[2] = 0;

	// byte3: 水平速度，0=静止
	// 注：规范定义 0xFF=unknown，但 OpenDroneID App 会把它显示成 63.75 m/s
	// 地面静止时填 0 更合理
	msg[3] = 0;

	// byte4: 垂直速度，0=静止（int8, 0.5m/s 步进）
	msg[4] = 0;

	// byte5~8: 纬度 int32 小端，无 GPS 填 0（显示为赤道，App 会标在非洲）
	// byte9~12: 经度 int32 小端，无 GPS 填 0

	// byte13~14: 气压高度，0xFFFF = unknown（App 显示 "—"）
	msg[13] = 0xFF;
	msg[14] = 0xFF;

	// byte15~16: 几何高度，0xFFFF = unknown
	msg[15] = 0xFF;
	msg[16] = 0xFF;

	// byte17~18: 距地高度，0xFFFF = unknown
	msg[17] = 0xFF;
	msg[18] = 0xFF;

	// byte19: 精度字段：bits[3:0]=水平精度，bits[7:4]=垂直精度，0=unknown
	// byte20: bits[3:0]=气压精度，bits[7:4]=速度精度，0=unknown
	// byte21~22: 时间戳（十分之一秒，模3600），暂无 RTC 填 0
	// byte23: 时间戳精度，0=unknown
	// byte24: 预留
}

// 系统报文
static void build_system(uint8_t *msg)
{
	memset(msg, 0, ODID_MSG_LEN);
	msg[0] = (MSG_SYSTEM << 4) | ODID_PROTO_VER;

	// byte1: flags
	// bit6-5=坐标系类型(0=WGS84)
	// bit4-2=等级分类归属区域(2=中国)
	// bit1-0=控制站位置类型(0=起飞点)
	msg[1] = (2 << 2);

	// byte2~5: 控制站纬度，unknown=0
	// byte6~9: 控制站经度，unknown=0

	// byte10~11: 运行区域计数，填1
	msg[10] = 1;
	msg[11] = 0;

	// byte12~24: 预留
}

// ── 构造单条报文的 BLE 广播数据 ──────────────────────────────
// ASTM F3411-22a Section 6.3 Legacy BLE Advertising 格式：
//
//   svc_data_uuid16 内容（NimBLE 自动加 [len][0x16] 头）：
//     [UUID_Lo=0xFA][UUID_Hi=0xFF]          2字节
//     [App Code=0x0D]                        1字节  ← ODID 识别码
//     [Message Count=1]                      1字节  ← 必须！缺少此字节导致解析器
//                                                      把 ODID 报头(0x01/0x11)误读
//                                                      为消息数量，产生随机乱值
//     [ODID 报文=25字节]                    25字节
//     合计 = 29字节
//
//   NimBLE 输出: [len=30][type=0x16][29字节] = 31字节 = Legacy 上限 ✅
static void set_adv_data_for_msg(const uint8_t *msg)
{
	// [UUID(2)] + [AppCode(1)] + [Count(1)] + [ODID报文(25)] = 29字节
	uint8_t adv_data[2 + 1 + 1 + ODID_MSG_LEN];
	adv_data[0] = ODID_SERVICE_UUID & 0xFF; // UUID low  byte: 0xFA
	adv_data[1] = (ODID_SERVICE_UUID >> 8); // UUID high byte: 0xFF
	adv_data[2] = 0x0D; // ASTM F3411 App Code
	adv_data[3] = 1; // Message Count = 1（每包1条报文）
	memcpy(&adv_data[4], msg, ODID_MSG_LEN);

	struct ble_hs_adv_fields fields = { 0 };
	fields.svc_data_uuid16 = adv_data;
	fields.svc_data_uuid16_len = sizeof(adv_data); // 29字节

	int rc = ble_gap_adv_set_fields(&fields);
	if (rc != 0) {
		ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
	}
}

static void start_nonconn_adv(void)
{
	struct ble_gap_adv_params params = { 0 };
	params.conn_mode = BLE_GAP_CONN_MODE_NON; // non-connectable
	params.disc_mode = BLE_GAP_DISC_MODE_GEN;
	// 广播间隔：300ms，三条报文轮一圈 = 900ms < 1s，满足 CAAC ≤1s 要求
	params.itvl_min = 480; // 480 * 0.625ms = 300ms
	params.itvl_max = 480;

	int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &params, NULL, NULL);
	if (rc != 0 && rc != BLE_HS_EALREADY) {
		ESP_LOGE(TAG, "adv_start failed: %d", rc);
	}
}

// ── 对外接口 ─────────────────────────────────────────────────
void rid_init(const uint8_t *fc_uid, uint8_t uid_len)
{
	// 12字节二进制 UID → 20字节 ASCII HEX（不足补0）
	memset(s_uas_id, '0', sizeof(s_uas_id));
	uint8_t copy_len = uid_len < 10 ? uid_len : 10; // 10字节 = 20个HEX字符
	for (int i = 0; i < copy_len; i++) {
		snprintf(&s_uas_id[i * 2], 3, "%02X", fc_uid[i]);
	}
	s_uid_set = true;
	ESP_LOGI(TAG, "RID uid: %.20s", s_uas_id);
}

void rid_set_enabled(bool enabled)
{
	s_enabled = enabled;
	ESP_LOGI(TAG, "RID %s", enabled ? "enabled" : "disabled");
	if (!enabled && s_running) {
		rid_stop();
	}
}

bool rid_get_enabled(void)
{
	return s_enabled;
}

static void rid_callout_cb(struct ble_npl_event *ev)
{
	if (!s_running)
		return;

	uint8_t msg[ODID_MSG_LEN];
	switch (s_msg_idx % 3) {
	case 0:
		build_basic_id(msg);
		break;
	case 1:
		build_location(msg);
		break;
	case 2:
		build_system(msg);
		break;
	}
	s_msg_idx++;

	// 在 NimBLE host task 里调用，安全
	ble_gap_adv_stop();
	set_adv_data_for_msg(msg);
	start_nonconn_adv();

	// 300ms 后再次触发
	ble_npl_callout_reset(&s_rid_callout, ble_npl_time_ms_to_ticks32(300));
}

void rid_start(void)
{
	if (!s_enabled || !s_uid_set || s_running)
		return;
	s_running = true;

	// 先暂停 NUS 广播，从此 BLE 信道独占给 RID
	ble_nus_pause_advertising();

	// 初始化 callout，绑定到 NimBLE 默认事件队列
	ble_npl_callout_init(&s_rid_callout, nimble_port_get_dflt_eventq(), rid_callout_cb, NULL);

	// 立刻触发第一次
	ble_npl_callout_reset(&s_rid_callout, 0);
	ESP_LOGI(TAG, "RID started via callout");
}

void rid_stop(void)
{
	if (!s_running)
		return;
	s_running = false;
	ble_npl_callout_stop(&s_rid_callout);
	ble_gap_adv_stop();
	ESP_LOGI(TAG, "RID stopped");

	// RID 广播已停，把信道还给 NUS
	ble_nus_resume_advertising();
}