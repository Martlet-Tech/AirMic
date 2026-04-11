#include "button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define DEBOUNCE_MS 50
#define DOUBLE_GAP_MS 300 // 两次单击间隔小于此值判定为双击
#define LONG_PRESS_MS 1000

#define TAG "button"
static gpio_num_t s_pin;
static btn_callback_t s_cb;
static QueueHandle_t s_event_queue = NULL;

static void button_task(void *arg)
{
	typedef enum { ST_IDLE, ST_PRESSED, ST_WAIT_SECOND } state_t;
	state_t state = ST_IDLE;
	int click_cnt = 0;
	int64_t t_press = 0;
	int64_t t_release = 0;
	int last_level = 1;

	while (1) {
		vTaskDelay(pdMS_TO_TICKS(10)); // ← 每次循环必须先 yield

		int level = gpio_get_level(s_pin);
		int64_t now = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;

		// 消抖：电平和上次一样就跳过
		if (level == last_level) {
			// 等待双击超时
			if (state == ST_WAIT_SECOND && (now - t_release) > DOUBLE_GAP_MS) {
				btn_event_t evt = (click_cnt >= 2) ? BTN_EVENT_DOUBLE_CLICK : BTN_EVENT_SINGLE_CLICK;
				xQueueSend(s_event_queue, &evt, 0);
				click_cnt = 0;
				state = ST_IDLE;
			}
			continue;
		}
		last_level = level;

		if (level == 0) {
			// 按下
			t_press = now;
			state = ST_PRESSED;
		} else {
			// 松开
			int64_t held = now - t_press;
			if (held >= LONG_PRESS_MS) {
				btn_event_t evt = BTN_EVENT_LONG_PRESS;
				xQueueSend(s_event_queue, &evt, 0);
				click_cnt = 0;
				state = ST_IDLE;
			} else {
				click_cnt++;
				t_release = now;
				state = ST_WAIT_SECOND;
			}
		}
	}
}

static void button_dispatch_task(void *arg)
{
	btn_event_t evt;
	while (1) {
		if (xQueueReceive(s_event_queue, &evt, portMAX_DELAY)) {
			if (s_cb)
				s_cb(evt); // 在独立 task 里执行，栈独立
		}
	}
}

void button_init(gpio_num_t pin, btn_callback_t cb)
{
	s_pin = pin;
	s_cb = cb;
	s_event_queue = xQueueCreate(4, sizeof(btn_event_t));

	gpio_config_t cfg = {
		.pin_bit_mask = (1ULL << pin),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
	};
	gpio_config(&cfg);

	xTaskCreate(button_task, "button", 8192, NULL, 4, NULL);
	xTaskCreate(button_dispatch_task, "btn_dispatch", 8192, NULL, 4, NULL);
}
