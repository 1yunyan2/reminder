
#include "bsp/bsp_board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/touch_pad.h" //触摸
#include "driver/gpio.h"
#include "esp_log.h"
#include "bsp/bsp_config.h"
#include "ui/interaction.h"

static const char *TAG = "BSP_TOUCH";

#define TOUCH_EVENT_QUEUE_LEN 8
static QueueHandle_t s_touch_event_queue = NULL; // 触摸事件队列句柄

// S3触摸逻辑：触摸时数值增大
#define TOUCH_THRESH_PERCENT 0.01f // 3%阈值，适合杜邦线
#define POWER_ON_MASK_TIME 2000    // 上电屏蔽2秒
#define TOUCH_MIN_DELTA 1000       // 最小变化量，防止误触

typedef struct
{
    touch_pad_t channel;
    uint32_t baseline; // 触摸基线（未触摸时的平均读数）
    bool is_pressed;   // 当前状态
} touch_btn_t;

static touch_btn_t btn_head = {.channel = BSP_TOUCH_1_PIN};
static touch_btn_t btn_abdomen = {.channel = BSP_TOUCH_3_PIN};
static touch_btn_t btn_back = {.channel = BSP_TOUCH_2_PIN};
static touch_btn_t btn_prev_page = {.channel = BSP_TOUCH_PREV_PIN}; // 前一页翻页按钮
static touch_btn_t btn_next_page = {.channel = BSP_TOUCH_NEXT_PIN}; // 后一页翻页按钮

// 创建触摸事件队列并初始化触摸传感器
static void send_touch_event(touch_event_t event)
{
    xQueueSend(s_touch_event_queue, &event, 0);
}

// 震动马达脉冲
void bsp_motor_pulse(void)
{
    gpio_set_level(BSP_MOTOR_VIB_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(BSP_MOTOR_VIB_PIN, 0);
}

// 多次采样取稳定基线
static uint32_t touch_get_baseline(touch_pad_t ch)
{
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++)
    {
        uint32_t val;                      // 采样
        touch_pad_read_raw_data(ch, &val); // 含义：读取触摸通道的原始数据，存储在val中
        sum += val;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return sum / 10;
}

void bsp_touch_init(void)
{
    s_touch_event_queue = xQueueCreate(TOUCH_EVENT_QUEUE_LEN, sizeof(touch_event_t)); // 创建触摸事件队列

    // 电机初始化，强制低电平
    gpio_config_t motor_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << BSP_MOTOR_VIB_PIN),
    };
    gpio_config(&motor_conf);
    gpio_set_level(BSP_MOTOR_VIB_PIN, 0);

    // S3触摸初始化（高灵敏度）
    touch_pad_init();                                                              // 触摸初始化
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_0V); // 设置触摸电压参数，提升灵敏度
    touch_pad_config(btn_head.channel);                                            // 配置触摸通道
    touch_pad_config(btn_abdomen.channel);
    touch_pad_config(btn_back.channel);
    touch_pad_config(btn_prev_page.channel); // 配置前一页翻页通道
    touch_pad_config(btn_next_page.channel); // 配置后一页翻页通道
    touch_pad_filter_disable();              // 关闭滤波，快速响应

    vTaskDelay(pdMS_TO_TICKS(100));

    // 采集基线
    btn_head.baseline = touch_get_baseline(btn_head.channel);
    btn_abdomen.baseline = touch_get_baseline(btn_abdomen.channel);
    btn_back.baseline = touch_get_baseline(btn_back.channel);
    btn_prev_page.baseline = touch_get_baseline(btn_prev_page.channel); // 采集前一页基线
    btn_next_page.baseline = touch_get_baseline(btn_next_page.channel); // 采集后一页基线

    ESP_LOGI(TAG, "✅ S3触摸初始化完成");
    ESP_LOGI(TAG, "基线: 头=%lu 腹=%lu 背=%lu 前页=%lu 后页=%lu",
             btn_head.baseline, btn_abdomen.baseline, btn_back.baseline, btn_prev_page.baseline, btn_next_page.baseline);
}

// 获取触摸事件队列中的事件，如果有则返回true并将事件存储在out_event指向的变量中，否则返回false
bool bsp_touch_get_event(touch_event_t *out_event)
{
    return xQueueReceive(s_touch_event_queue, out_event, 0) == pdTRUE;
}

/**
 * @brief 处理单个触摸按钮的状态变化，触发事件并控制震动马达
 * @param btn 触摸按钮结构体指针，包含通道、基线和状态
 * @param name 按钮名称（用于日志输出）
 * @param short_evt 触发的短按事件类型
 * @param now_ms 当前时间（毫秒），用于上电屏蔽逻辑
 */
void bsp_touch_process_btn(touch_btn_t *btn, const char *name, touch_event_t short_evt, uint32_t now_ms)
{
    // 上电屏蔽期内不响应
    if (now_ms < POWER_ON_MASK_TIME)
        return;

    uint32_t val;
    touch_pad_read_raw_data(btn->channel, &val);
    int32_t delta = val - btn->baseline; // 触摸时数值增大，所以是 val - baseline

    // 触发条件：数值增大超过阈值// 触摸时数值增大，所以是 delta > 0
    bool pressed = (delta > TOUCH_MIN_DELTA) || (delta > (int32_t)(btn->baseline * TOUCH_THRESH_PERCENT));

    if (pressed && !btn->is_pressed)
    {
        btn->is_pressed = true;
        ESP_LOGI(TAG, ">>>> %s 触发 <<<<", name);
        send_touch_event(short_evt); // 发送触摸事件到队列
        bsp_motor_pulse();           // 触觉反馈
    }
    if (!pressed)
    {
        btn->is_pressed = false;
    }
}

void touch_scan_task(void *pvParameters)
{
    bsp_touch_init();
    printf("\n✅ 准备就绪！手捏杜邦线金属端触发\n");

    touch_event_t event;
    while (1)
    {
        uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());

        // 触摸扫描
        bsp_touch_process_btn(&btn_head, "头部", TOUCH_EVENT_SHORT_HEAD, now_ms);
        bsp_touch_process_btn(&btn_abdomen, "腹部", TOUCH_EVENT_SHORT_ABDOMEN, now_ms);
        bsp_touch_process_btn(&btn_back, "背部", TOUCH_EVENT_SHORT_BACK, now_ms);
        bsp_touch_process_btn(&btn_prev_page, "前一页", TOUCH_EVENT_SHORT_PREV_PAGE, now_ms);
        bsp_touch_process_btn(&btn_next_page, "后一页", TOUCH_EVENT_SHORT_NEXT_PAGE, now_ms);

        // 事件执行
        if (bsp_touch_get_event(&event))
        {
            switch (event)
            {
            case TOUCH_EVENT_SHORT_HEAD:
                ui_interaction_play(EMO_HAPPY);
                break;
            case TOUCH_EVENT_SHORT_ABDOMEN:
                ui_interaction_play(EMO_COMFORTABLE);
                break;
            case TOUCH_EVENT_SHORT_BACK:
                ui_interaction_play(EMO_TICKLISH);
                break;
            case TOUCH_EVENT_LONG_HEAD:
                ui_interaction_play(EMO_ACT_CUTE);
                break;
            case TOUCH_EVENT_LONG_ABDOMEN:
                ui_interaction_play(EMO_HEALING);
                break;
            case TOUCH_EVENT_LONG_BACK:
                ui_interaction_play(EMO_SURPRISED);
                break;
            case TOUCH_EVENT_COMBO_HEAD_ABDOMEN:
                ui_interaction_play(EMO_SHY_RUB);
                break;
            case TOUCH_EVENT_COMBO_HEAD_BACK:
                ui_interaction_play(EMO_EXCITED);
                break;
            case TOUCH_EVENT_COMBO_ABDOMEN_BACK:
                ui_interaction_play(EMO_COMFORTABLE_ROLL);
                break;
            case TOUCH_EVENT_SHORT_PREV_PAGE:
                ESP_LOGI(TAG, ">>>> 前一页翻页触发 <<<<");
                // TODO: 实现前一页翻页逻辑
                break;
            case TOUCH_EVENT_SHORT_NEXT_PAGE:
                ESP_LOGI(TAG, ">>>> 后一页翻页触发 <<<<");
                // TODO: 实现后一页翻页逻辑
                break;
            default:
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // 无看门狗
    }
}