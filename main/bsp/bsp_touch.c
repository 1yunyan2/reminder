
#include "bsp/bsp_board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/touch_pad.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "bsp/bsp_config.h"
#include "ui/interaction.h"
#include "ui/ui_port.h"

// ── 情绪显示分组表（每个触摸位置对应 6 个随机情绪）────────────────────────────
typedef struct
{
    const char *name;
    const char *anim;
} emo_entry_t;

static const emo_entry_t g_emo_head[] = {
    {"开心", "眯眼笑+冒星星"},
    {"好奇", "歪头眨眼+问号"},
    {"傲娇", "挑眉+叉腰脸"},
    {"怕痒", "眯眼笑+躲躲闪闪"},
    {"犯困", "打哈欠+眼皮下垂"},
    {"委屈", "撇嘴+泪眼"},
};
static const emo_entry_t g_emo_abdomen[] = {
    {"舒服", "闭眼打哈欠+波浪线"},
    {"撒娇", "泪眼汪汪+歪头"},
    {"生气", "鼓脸+冒火"},
    {"害羞", "捂脸+脸红"},
    {"惊喜", "眼睛瞪大+闪光"},
    {"慵懒", "半睁眼+打哈欠"},
};
static const emo_entry_t g_emo_back[] = {
    {"治愈", "眯眼+爱心"},
    {"傲娇", "鼻孔看人+叉腰"},
    {"委屈", "撇嘴+低头"},
    {"兴奋", "爱心眼+蹦跳"},
    {"好奇", "歪头眨眼+问号"},
    {"怕痒", "笑出眼泪+扭动"},
};
static const emo_entry_t g_emo_head_abdomen[] = {
    {"兴奋", "爱心眼+蹦跳"},
    {"害羞蹭蹭", "脸红+蹭脸"},
    {"舒服到打滚", "眯眼+波浪线"},
    {"傲娇求摸", "挑眉+歪头"},
    {"犯困", "打哈欠+眼皮下垂"},
    {"惊喜", "眼睛瞪大+闪光"},
};
static const emo_entry_t g_emo_head_back[] = {
    {"治愈", "眯眼+爱心"},
    {"傲娇", "鼻孔看人+叉腰"},
    {"委屈", "撇嘴+低头"},
    {"兴奋", "爱心眼+蹦跳"},
    {"好奇", "歪头眨眼+问号"},
    {"怕痒", "笑出眼泪+扭动"},
};
static const emo_entry_t g_emo_abdomen_back[] = {
    {"慵懒瘫坐", "半睁眼+打哈欠"},
    {"惊喜抱抱", "爱心眼+张开双臂"},
    {"怕痒到扭动", "笑出眼泪+扭动"},
    {"舒服", "闭眼打哈欠+波浪线"},
    {"生气", "鼓脸+冒火"},
    {"兴奋", "爱心眼+蹦跳"},
};

// 从分组中随机取一个情绪并显示
static void show_random_emotion(const emo_entry_t *group, size_t count)
{
    const emo_entry_t *e = &group[esp_random() % count];
    ui_show_emotion(e->name, e->anim);
    ESP_LOGI("TOUCH", "情绪: %s | %s", e->name, e->anim);
}
#define SHOW_EMO(group) show_random_emotion(group, sizeof(group) / sizeof(group[0]))

static const char *TAG = "BSP_TOUCH";

#define TOUCH_EVENT_QUEUE_LEN 8
static QueueHandle_t s_touch_event_queue = NULL;

#define TOUCH_THRESH_PERCENT 0.15f // 15%相对阈值（提高防串扰）
#define POWER_ON_MASK_TIME 2000    // 上电屏蔽2秒
#define TOUCH_MIN_DELTA 50000      // 绝对变化量阈值（提高防串扰，需实际测量调整）
#define SHORT_PRESS_MIN_MS 800     // 短按最短持续时间（ms）
#define LONG_PRESS_MS 1500         // 长按判定时间（ms）
#define RELEASE_DEBOUNCE 4         // 连续 4 次读到未按才认为真正释放，防止按住时抖动误触发

typedef struct
{
    touch_pad_t channel;     // 触摸通道（对应 GPIO）
    uint32_t baseline;       // 基线值（初始化时测量，后续动态调整可选）
    bool is_pressed;         // 当前物理按压状态（不考虑组合时的事件屏蔽）
    uint32_t press_start_ms; // 按下时间，用于长按计时
    bool long_sent;          // 已发过长按，释放时不再发短按
    uint8_t release_count;   // 连续未按次数，达到 RELEASE_DEBOUNCE 才真正释放
} touch_btn_t;

static touch_btn_t btn_head = {.channel = BSP_TOUCH_1_PIN};         // 头部
static touch_btn_t btn_abdomen = {.channel = BSP_TOUCH_3_PIN};      // 腹部
static touch_btn_t btn_back = {.channel = BSP_TOUCH_2_PIN};         // 背部
static touch_btn_t btn_prev_page = {.channel = BSP_TOUCH_PREV_PIN}; // 前一页
static touch_btn_t btn_next_page = {.channel = BSP_TOUCH_NEXT_PIN}; // 后一页

// 组合触摸激活状态（防止持续按住时重复触发）
static bool s_combo_ha_active = false;
static bool s_combo_hb_active = false;
static bool s_combo_ab_active = false;

static void send_touch_event(touch_event_t event)
{
    xQueueSend(s_touch_event_queue, &event, 0);
}

void bsp_motor_pulse(void)
{
    gpio_set_level(BSP_MOTOR_VIB_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(BSP_MOTOR_VIB_PIN, 0);
}

static uint32_t touch_get_baseline(touch_pad_t ch) // 取多次平均，稳定基线值
{
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++)
    {
        touch_pad_sw_start(); // 触发一次采样，更新内部状态
        vTaskDelay(pdMS_TO_TICKS(10));
        uint32_t val;
        touch_pad_read_raw_data(ch, &val); // 读原始值，包含基线和噪声
        sum += val;
    }
    return sum / 10;
}

void bsp_touch_init(void)
{
    s_touch_event_queue = xQueueCreate(TOUCH_EVENT_QUEUE_LEN, sizeof(touch_event_t));

    gpio_config_t motor_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << BSP_MOTOR_VIB_PIN),
    };
    gpio_config(&motor_conf);
    gpio_set_level(BSP_MOTOR_VIB_PIN, 0);

    touch_pad_init();
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_0V);
    touch_pad_config(btn_head.channel);
    touch_pad_config(btn_abdomen.channel);
    touch_pad_config(btn_back.channel);
    touch_pad_config(btn_prev_page.channel);
    touch_pad_config(btn_next_page.channel);
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_SW);
    touch_pad_filter_disable();

    vTaskDelay(pdMS_TO_TICKS(100));

    btn_head.baseline = touch_get_baseline(btn_head.channel);
    btn_abdomen.baseline = touch_get_baseline(btn_abdomen.channel);
    btn_back.baseline = touch_get_baseline(btn_back.channel);
    btn_prev_page.baseline = touch_get_baseline(btn_prev_page.channel);
    btn_next_page.baseline = touch_get_baseline(btn_next_page.channel);

    ESP_LOGI(TAG, "✅ 触摸初始化完成");
    ESP_LOGI(TAG, "基线: 头=%lu 腹=%lu 背=%lu 前页=%lu 后页=%lu",
             btn_head.baseline, btn_abdomen.baseline, btn_back.baseline,
             btn_prev_page.baseline, btn_next_page.baseline);
}

bool bsp_touch_get_event(touch_event_t *out_event)
{
    return xQueueReceive(s_touch_event_queue, out_event, 0) == pdTRUE;
}

static int32_t read_btn_delta(touch_btn_t *btn)
{
    uint32_t val;
    touch_pad_read_raw_data(btn->channel, &val);
    return (int32_t)(val - btn->baseline);
}

static bool read_btn_pressed(touch_btn_t *btn)
{
    int32_t delta = read_btn_delta(btn);
    return (delta > TOUCH_MIN_DELTA) &&
           (delta > (int32_t)(btn->baseline * TOUCH_THRESH_PERCENT));
}

// 更新单按钮状态，处理短按/长按
// in_combo=true 时该按钮参与了组合，本轮不产生单独事件
static void update_btn(touch_btn_t *btn, bool pressed, bool in_combo,
                       uint32_t now_ms, const char *name,
                       touch_event_t short_evt, touch_event_t long_evt)
{
    if (in_combo)
    {
        // 参与组合：跟踪物理状态，但标记为已处理，释放时不误发短按
        btn->is_pressed = pressed;
        btn->long_sent = true;
        btn->press_start_ms = 0;
        return;
    }

    if (pressed)
    {
        btn->release_count = 0; // 按住时清零释放计数
        if (!btn->is_pressed)
        {
            btn->is_pressed = true;
            btn->press_start_ms = now_ms;
            btn->long_sent = false;
        }
        else if (!btn->long_sent && (now_ms - btn->press_start_ms) >= LONG_PRESS_MS)
        {
            btn->long_sent = true;
            ESP_LOGI(TAG, ">>>> %s 长按 <<<<", name);
            send_touch_event(long_evt);
        }
    }
    else
    {
        // 释放防抖：连续 RELEASE_DEBOUNCE 次读到未按才真正释放
        // 防止按住时信号抖动导致反复触发短按
        if (btn->is_pressed)
        {
            btn->release_count++;
            if (btn->release_count >= RELEASE_DEBOUNCE)
            {
                if (!btn->long_sent &&
                    (now_ms - btn->press_start_ms) >= SHORT_PRESS_MIN_MS)
                {
                    ESP_LOGI(TAG, ">>>> %s 短按 <<<<", name);
                    send_touch_event(short_evt);
                }
                btn->is_pressed = false;
                btn->long_sent = false;
                btn->press_start_ms = 0;
                btn->release_count = 0;
            }
        }
    }
}

// 翻页按钮：仅短按，带释放防抖
static void update_page_btn(touch_btn_t *btn, bool pressed,
                            const char *name, touch_event_t short_evt)
{
    if (pressed)
    {
        btn->release_count = 0;
        if (!btn->is_pressed)
        {
            btn->is_pressed = true;
            ESP_LOGI(TAG, ">>>> %s <<<<", name);
            send_touch_event(short_evt);
        }
    }
    else if (btn->is_pressed)
    {
        btn->release_count++;
        if (btn->release_count >= RELEASE_DEBOUNCE)
        {
            btn->is_pressed = false;
            btn->release_count = 0;
        }
    }
}

void touch_scan_task(void *pvParameters)
{
    bsp_touch_init();
    printf("\n✅ 准备就绪！触摸铜箔触发情绪\n");

    touch_event_t event;
    while (1)
    {
        touch_pad_sw_start(); // 启动一次触摸测量，数据可通过 touch_pad_read_raw_data() 读取
        vTaskDelay(pdMS_TO_TICKS(5));

        uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount()); // 获取当前时间

        if (now_ms < POWER_ON_MASK_TIME)
        {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        // ── 读取三个主按钮原始状态（最强信号优先，防串扰）────────────────────
        int32_t dh = read_btn_delta(&btn_head);
        int32_t da = read_btn_delta(&btn_abdomen);
        int32_t db = read_btn_delta(&btn_back);

        int32_t thresh_abs = TOUCH_MIN_DELTA;
        bool h_raw = (dh > thresh_abs) && (dh > (int32_t)(btn_head.baseline * TOUCH_THRESH_PERCENT));
        bool a_raw = (da > thresh_abs) && (da > (int32_t)(btn_abdomen.baseline * TOUCH_THRESH_PERCENT));
        bool b_raw = (db > thresh_abs) && (db > (int32_t)(btn_back.baseline * TOUCH_THRESH_PERCENT));

        // 若多个按键同时超阈值且不是有效组合，只保留 delta 最大的那个
        int active_count = (int)h_raw + (int)a_raw + (int)b_raw;
        bool h = h_raw, a = a_raw, b = b_raw;
        if (active_count > 1)
        {
            // 允许真实组合（两个都很强：次强 > 70% 最强）
            int32_t max_d = (dh > da ? dh : da);
            max_d = (max_d > db ? max_d : db);
            int32_t combo_thresh = max_d * 7 / 10;
            h = h_raw && (dh >= combo_thresh);
            a = a_raw && (da >= combo_thresh);
            b = b_raw && (db >= combo_thresh);
        }

        // ── 组合检测（上升沿触发，持续按住不重复）────────────────────────────
        bool combo_ha = h && a;
        bool combo_hb = h && b;
        bool combo_ab = a && b;

        if (combo_ha && !s_combo_ha_active)
        {
            s_combo_ha_active = true;
            ESP_LOGI(TAG, ">>>> 头+腹 组合 <<<<");
            send_touch_event(TOUCH_EVENT_COMBO_HEAD_ABDOMEN);
        }
        if (!combo_ha)
            s_combo_ha_active = false;

        if (combo_hb && !s_combo_hb_active)
        {
            s_combo_hb_active = true;
            ESP_LOGI(TAG, ">>>> 头+背 组合 <<<<");
            send_touch_event(TOUCH_EVENT_COMBO_HEAD_BACK);
        }
        if (!combo_hb)
            s_combo_hb_active = false;

        if (combo_ab && !s_combo_ab_active)
        {
            s_combo_ab_active = true;
            ESP_LOGI(TAG, ">>>> 腹+背 组合 <<<<");
            send_touch_event(TOUCH_EVENT_COMBO_ABDOMEN_BACK);
        }
        if (!combo_ab)
            s_combo_ab_active = false;

        // ── 单按钮短按/长按（参与组合时跳过）────────────────────────────────
        update_btn(&btn_head, h, combo_ha || combo_hb, now_ms,
                   "头部", TOUCH_EVENT_SHORT_HEAD, TOUCH_EVENT_LONG_HEAD);
        update_btn(&btn_abdomen, a, combo_ha || combo_ab, now_ms,
                   "腹部", TOUCH_EVENT_SHORT_ABDOMEN, TOUCH_EVENT_LONG_ABDOMEN);
        update_btn(&btn_back, b, combo_hb || combo_ab, now_ms,
                   "背部", TOUCH_EVENT_SHORT_BACK, TOUCH_EVENT_LONG_BACK);

        // ── 翻页按钮 ─────────────────────────────────────────────────────────
        update_page_btn(&btn_prev_page, read_btn_pressed(&btn_prev_page), "前一页", TOUCH_EVENT_SHORT_PREV_PAGE);
        update_page_btn(&btn_next_page, read_btn_pressed(&btn_next_page), "后一页", TOUCH_EVENT_SHORT_NEXT_PAGE);

        // ── 事件 → 情绪矩阵 ──────────────────────────────────────────────────
        if (bsp_touch_get_event(&event))
        {
            switch (event)
            {
            case TOUCH_EVENT_SHORT_HEAD:
                SHOW_EMO(g_emo_head);
                ui_interaction_play(EMO_HAPPY);
                break;
            case TOUCH_EVENT_LONG_HEAD:
                SHOW_EMO(g_emo_head);
                ui_interaction_play(EMO_ACT_CUTE);
                break;
            case TOUCH_EVENT_SHORT_ABDOMEN:
                SHOW_EMO(g_emo_abdomen);
                ui_interaction_play(EMO_COMFORTABLE);
                break;
            case TOUCH_EVENT_LONG_ABDOMEN:
                SHOW_EMO(g_emo_abdomen);
                ui_interaction_play(EMO_HEALING);
                break;
            case TOUCH_EVENT_SHORT_BACK:
                SHOW_EMO(g_emo_back);
                ui_interaction_play(EMO_TICKLISH);
                break;
            case TOUCH_EVENT_LONG_BACK:
                SHOW_EMO(g_emo_back);
                ui_interaction_play(EMO_SURPRISED);
                break;
            case TOUCH_EVENT_COMBO_HEAD_ABDOMEN:
                SHOW_EMO(g_emo_head_abdomen);
                ui_interaction_play(EMO_SHY_RUB);
                break;
            case TOUCH_EVENT_COMBO_HEAD_BACK:
                SHOW_EMO(g_emo_head_back);
                ui_interaction_play(EMO_EXCITED);
                break;
            case TOUCH_EVENT_COMBO_ABDOMEN_BACK:
                SHOW_EMO(g_emo_abdomen_back);
                ui_interaction_play(EMO_COMFORTABLE_ROLL);
                break;
            case TOUCH_EVENT_SHORT_PREV_PAGE:
                ESP_LOGI(TAG, "前一页");
                break;
            case TOUCH_EVENT_SHORT_NEXT_PAGE:
                ESP_LOGI(TAG, "后一页");
                break;
            default:
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
