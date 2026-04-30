
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
static const char *TAG = "BSP_TOUCH";
/**
 * !后续将多任务变为单任务，不需要这么多情绪，一次就显示一个情绪对应的数据，
 * 都用条件编译变为单个，动画过程触发需要可以直接打断然后切换动画和声音
 *
 * !
 * !
 * !
 * !!
 * !!
 * !
 * !
 * !!
 * !
 * !!
 * !
 *
 */
// ── 情绪显示分组表（每个触摸位置对应 6 个随机情绪）────────────────────────────
typedef struct
{
    const char *name;  // 情绪名称
    const char *anim;  // 屏幕动画描述（后续替换为真实动画）
    const char *audio; // 音效描述（后续替换为真实音频）
} emo_entry_t;

static const emo_entry_t g_emo_head[] = {
    {"开心", "眯眼笑+冒星星", "短促笑声"},
    {"好奇", "歪头眨眼+问号", "嗯？+轻微按键音"},
    {"傲娇", "挑眉+叉腰脸", "哼~+轻敲桌面声"},
    {"怕痒", "眯眼笑+躲躲闪闪", "咯咯笑+好痒好痒"},
    {"犯困", "打哈欠+眼皮下垂", "打哈欠+轻柔呼吸音"},
    {"委屈", "撇嘴+泪眼", "小声啜泣+呜~"},
};
static const emo_entry_t g_emo_abdomen[] = {
    {"舒服", "闭眼打哈欠+波浪线", "满足嗯~+舒缓呼吸"},
    {"撒娇", "泪眼汪汪+歪头", "要抱抱~+蹭蹭摩擦"},
    {"生气", "鼓脸+冒火", "哼！+跺脚声"},
    {"害羞", "捂脸+脸红", "哎呀~+害羞轻笑"},
    {"惊喜", "眼睛瞪大+闪光", "哇！+铃铛脆响"},
    {"慵懒", "半睁眼+打哈欠", "慵懒哈欠+咿呀声"},
};
static const emo_entry_t g_emo_back[] = {
    {"治愈", "眯眼+爱心", "呼噜呼噜+轻拍声"},
    {"傲娇", "鼻孔看人+叉腰", "切~+轻哼声"},
    {"委屈", "撇嘴+低头", "小声抽泣+呜~"},
    {"兴奋", "爱心眼+蹦跳", "耶~+拍手声"},
    {"好奇", "歪头眨眼+问号", "咦？+轻微摩擦声"},
    {"怕痒", "笑出眼泪+扭动", "咯咯大笑+别挠啦~"},
};
static const emo_entry_t g_emo_head_abdomen[] = {
    {"兴奋", "爱心眼+蹦跳", "哇呜~+铃铛串响"},
    {"害羞蹭蹭", "脸红+蹭脸", "嘿嘿~+蹭蹭摩擦"},
    {"舒服到打滚", "眯眼+波浪线", "呼噜~+翻身轻响"},
    {"傲娇求摸", "挑眉+歪头", "哼快摸我~+轻敲"},
    {"犯困", "打哈欠+眼皮下垂", "打哈欠+轻柔呼吸"},
    {"惊喜", "眼睛瞪大+闪光", "哇！+烟花脆响"},
};
static const emo_entry_t g_emo_head_back[] = {
    {"治愈", "眯眼+爱心", "呼噜呼噜+轻拍声"},
    {"傲娇", "鼻孔看人+叉腰", "切~+轻哼声"},
    {"委屈", "撇嘴+低头", "小声抽泣+呜~"},
    {"兴奋", "爱心眼+蹦跳", "耶~+拍手声"},
    {"好奇", "歪头眨眼+问号", "咦？+轻微摩擦声"},
    {"怕痒", "笑出眼泪+扭动", "咯咯大笑+别挠啦~"},
};
static const emo_entry_t g_emo_abdomen_back[] = {
    {"慵懒瘫坐", "半睁眼+打哈欠", "慵懒哈欠+瘫坐咚声"},
    {"惊喜抱抱", "爱心眼+张开双臂", "要抱抱~+哗啦声"},
    {"怕痒到扭动", "笑出眼泪+扭动", "大笑+救命啊~"},
    {"舒服", "闭眼打哈欠+波浪", "满足嗯~+舒缓呼吸"},
    {"生气", "鼓脸+冒火", "哼！+跺脚声"},
    {"兴奋", "爱心眼+蹦跳", "哇呜~+铃铛串响"},
};

// 从分组中随机取一个情绪，显示到屏幕并打印日志
static void show_random_emotion(const emo_entry_t *group, size_t count)
{
    const emo_entry_t *e = &group[esp_random() % count];
    ui_show_emotion(e->name, e->anim, e->audio);
    ESP_LOGI("TOUCH", "[%s] 动画:%s 音效:%s", e->name, e->anim, e->audio);
}
#define SHOW_EMO(group) show_random_emotion(group, sizeof(group) / sizeof(group[0]))

#define TOUCH_EVENT_QUEUE_LEN 8                  // todo  触摸事件队列长度，改为1
static QueueHandle_t s_touch_event_queue = NULL; // 触摸事件队列句柄

#define TOUCH_THRESH_PERCENT 0.05f // 15%相对阈值（提高防串扰）相对于基线变化增加度
#define POWER_ON_MASK_TIME 2000    // 上电屏蔽2秒
#define TOUCH_MIN_DELTA 2000       // 绝对变化量阈值（提高防串扰，需实际测量调整），裸触摸为5W，隔着外壳为2000
#define SHORT_PRESS_MIN_MS 300     // 短按最短持续时间（头/腹/背 与翻页短按 共用）
#define PAGE_LONG_PRESS_MS 3000    // 翻页键长按阈值（进入功能菜单）
#define PRESS_DEBOUNCE 2           // 连续 2 次读到按下才视为真正按下，防止瞬时尖峰误触
#define RELEASE_DEBOUNCE 4         // 连续 4 次读到未按才认为真正释放，防止按住时抖动误触发

typedef struct
{
    touch_pad_t channel;     // 触摸通道（对应 GPIO）
    uint32_t baseline;       // 基线值（初始化时测量，后续动态调整可选）
    bool is_pressed;         // 已通过按下防抖、视为真正按下
    uint32_t press_start_ms; // 按下时间，用于短/长按计时
    uint8_t press_count;     // 连续按下次数，达到 PRESS_DEBOUNCE 才认按下
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

// 翻页键互斥占用：两键同时触摸时只保留先感应到的那一个
typedef enum
{
    PAGE_OWNER_NONE = 0,
    PAGE_OWNER_PREV,
    PAGE_OWNER_NEXT
} page_owner_t;
static page_owner_t s_page_owner = PAGE_OWNER_NONE;

static void send_touch_event(touch_event_t event) // 往事件队列发送触摸事件
{
    xQueueSend(s_touch_event_queue, &event, 0);
}

void bsp_motor_pulse(void) // 震动马达单次脉冲
{
    gpio_set_level(BSP_MOTOR_VIB_PIN, 1); // 震动马达接在 GPIO 上，输出高电平时震动
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

    touch_pad_init(); // 初始化触摸控制器
    // 配置触摸参数：高电压 2.7V，低电压 0.5V，无衰减（根据实际情况调整）
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_0V);
    // 配置触摸按键：引脚、中断类型、中断优先级
    touch_pad_config(btn_head.channel);
    touch_pad_config(btn_abdomen.channel);
    touch_pad_config(btn_back.channel);
    touch_pad_config(btn_prev_page.channel);
    touch_pad_config(btn_next_page.channel);
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_SW); // 由软件触发测量，灵活控制测量时机
    touch_pad_filter_disable();                // 关闭滤波器，减少测量延迟（根据实际情况调整）

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

bool bsp_touch_get_event(touch_event_t *out_event) // 获取触摸事件
{
    return xQueueReceive(s_touch_event_queue, out_event, 0) == pdTRUE;
}

static int32_t read_btn_delta(touch_btn_t *btn) // 读取当前值与基线的差值，正数表示按下，负数表示未按下
{
    uint32_t val;
    touch_pad_read_raw_data(btn->channel, &val); // 读原始值，包含基线和噪声
    return (int32_t)(val - btn->baseline);
}

static bool read_btn_pressed(touch_btn_t *btn) // 判断按钮是否被按下，基于绝对阈值和相对阈值双重判定，提高抗干扰能力
{
    int32_t delta = read_btn_delta(btn); // 正数表示按下，负数表示未按下
    return (delta > TOUCH_MIN_DELTA) &&
           (delta > (int32_t)(btn->baseline * TOUCH_THRESH_PERCENT));
}

/**
 * @brief 单体（头/腹/背）按钮：仅短按，按下需达 SHORT_PRESS_MIN_MS，释放时才触发。
 *
 * 触发时机：以"抬起"为结束时间——按住期间不触发，释放后通过 RELEASE_DEBOUNCE
 * 防抖再判定持续时长是否达到短按阈值。参与组合（in_combo）的按键不发独立事件。
 */
static void update_body_btn(touch_btn_t *btn, bool pressed, bool in_combo,
                            uint32_t now_ms, const char *name,
                            touch_event_t short_evt)
{
    if (in_combo)
    {
        // 参与组合：清空状态，避免组合释放后误判为短按
        btn->is_pressed = false;
        btn->press_count = 0;
        btn->release_count = 0;
        btn->press_start_ms = 0;
        return;
    }

    if (pressed)
    {
        btn->release_count = 0;
        if (!btn->is_pressed)
        {
            btn->press_count++;
            if (btn->press_count >= PRESS_DEBOUNCE)
            {
                btn->is_pressed = true;
                btn->press_start_ms = now_ms;
            }
        }
    }
    else
    {
        btn->press_count = 0;
        if (btn->is_pressed)
        {
            btn->release_count++;
            if (btn->release_count >= RELEASE_DEBOUNCE)
            {
                uint32_t held = now_ms - btn->press_start_ms;
                if (held >= SHORT_PRESS_MIN_MS)
                {
                    ESP_LOGI(TAG, ">>>> %s 短按 (%lums) <<<<", name, (unsigned long)held);
                    send_touch_event(short_evt);
                }
                btn->is_pressed = false;
                btn->press_start_ms = 0;
                btn->release_count = 0;
            }
        }
    }
}

/**
 * @brief 翻页按钮：以释放为结束时间，根据按住时长分发短按 / 长按。
 *
 * 按住期间不触发任何事件；释放后做防抖：
 *   ≥ PAGE_LONG_PRESS_MS  → 长按事件（进入功能菜单）
 *   ≥ SHORT_PRESS_MIN_MS  → 短按事件（翻页）
 *   小于以上阈值          → 视为误触，丢弃
 *
 * blocked 参数用于互斥锁定——当另一个翻页键已先被感应到，本键本轮被屏蔽。
 */
static void update_page_btn(touch_btn_t *btn, bool pressed_raw, bool blocked,
                            uint32_t now_ms, const char *name,
                            touch_event_t short_evt, touch_event_t long_evt)
{
    bool pressed = pressed_raw && !blocked;

    if (pressed)
    {
        btn->release_count = 0;
        if (!btn->is_pressed)
        {
            btn->press_count++;
            if (btn->press_count >= PRESS_DEBOUNCE)
            {
                btn->is_pressed = true;
                btn->press_start_ms = now_ms;
            }
        }
        // 按住期间一律不触发（"触摸时候一直不触发"）
    }
    else
    {
        btn->press_count = 0;
        if (btn->is_pressed)
        {
            btn->release_count++;
            if (btn->release_count >= RELEASE_DEBOUNCE)
            {
                uint32_t held = now_ms - btn->press_start_ms;
                touch_event_t evt = TOUCH_EVENT_NONE;
                const char *kind = NULL;
                if (held >= PAGE_LONG_PRESS_MS)
                {
                    evt = long_evt;
                    kind = "长按";
                }
                else if (held >= SHORT_PRESS_MIN_MS)
                {
                    evt = short_evt;
                    kind = "短按";
                }
                if (evt != TOUCH_EVENT_NONE)
                {
                    ESP_LOGI(TAG, ">>>> %s %s (%lums) <<<<", name, kind, (unsigned long)held);
                    send_touch_event(evt);
                }
                btn->is_pressed = false;
                btn->press_start_ms = 0;
                btn->release_count = 0;
            }
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
        int32_t dp = read_btn_delta(&btn_prev_page);
        int32_t dn = read_btn_delta(&btn_next_page);

        // todo调试：每 ~500ms 打印一次 delta，便于现场标定阈值,后续删除
        // static uint32_t s_last_log_ms = 0;
        // if (now_ms - s_last_log_ms >= 1000)
        // {
        //     s_last_log_ms = now_ms;
        //     ESP_LOGI(TAG, "delta: 头=%ld 腹=%ld 背=%ld 前=%ld 后=%ld",
        //              (long)dh, (long)da, (long)db, (long)dp, (long)dn);
        // }

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

        // ── 单按钮短按（参与组合时跳过；不再有长按）─────────────────────────
        update_body_btn(&btn_head, h, combo_ha || combo_hb, now_ms,
                        "头部", TOUCH_EVENT_SHORT_HEAD);
        update_body_btn(&btn_abdomen, a, combo_ha || combo_ab, now_ms,
                        "腹部", TOUCH_EVENT_SHORT_ABDOMEN);
        update_body_btn(&btn_back, b, combo_hb || combo_ab, now_ms,
                        "背部", TOUCH_EVENT_SHORT_BACK);

        // ── 翻页按钮 ──────────────────────────────────────────────────────
        // 互斥锁定：两键同时被触摸时只保留先感应到的；占用方释放后才释放锁
        bool prev_raw = read_btn_pressed(&btn_prev_page);
        bool next_raw = read_btn_pressed(&btn_next_page);

        if (s_page_owner == PAGE_OWNER_NONE)
        {
            // 仅在恰好只有一个被按下时确权；同帧两键齐起则均屏蔽，等下一帧解决
            if (prev_raw && !next_raw)
                s_page_owner = PAGE_OWNER_PREV;
            else if (next_raw && !prev_raw)
                s_page_owner = PAGE_OWNER_NEXT;
        }

        bool prev_blocked = (s_page_owner != PAGE_OWNER_NONE && s_page_owner != PAGE_OWNER_PREV);
        bool next_blocked = (s_page_owner != PAGE_OWNER_NONE && s_page_owner != PAGE_OWNER_NEXT);

        update_page_btn(&btn_prev_page, prev_raw, prev_blocked, now_ms,
                        "前一页", TOUCH_EVENT_SHORT_PREV_PAGE, TOUCH_EVENT_LONG_PREV_PAGE);
        update_page_btn(&btn_next_page, next_raw, next_blocked, now_ms,
                        "后一页", TOUCH_EVENT_SHORT_NEXT_PAGE, TOUCH_EVENT_LONG_NEXT_PAGE);

        // 占用方完成释放（is_pressed 已被 update_page_btn 清零）后释放锁
        if (s_page_owner == PAGE_OWNER_PREV && !btn_prev_page.is_pressed && !prev_raw)
            s_page_owner = PAGE_OWNER_NONE;
        if (s_page_owner == PAGE_OWNER_NEXT && !btn_next_page.is_pressed && !next_raw)
            s_page_owner = PAGE_OWNER_NONE;

        // ── 事件 → 情绪矩阵 ──────────────────────────────────────────────────
        if (bsp_touch_get_event(&event))
        {
            switch (event)
            {
            case TOUCH_EVENT_SHORT_HEAD:
                SHOW_EMO(g_emo_head);
                ui_interaction_play(EMO_HAPPY);
                break;
            case TOUCH_EVENT_SHORT_ABDOMEN:
                SHOW_EMO(g_emo_abdomen);
                ui_interaction_play(EMO_COMFORTABLE);
                break;
            case TOUCH_EVENT_SHORT_BACK:
                SHOW_EMO(g_emo_back);
                ui_interaction_play(EMO_TICKLISH);
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
                ui_page_prev();
                break;
            case TOUCH_EVENT_SHORT_NEXT_PAGE:
                ui_page_next();
                break;
            case TOUCH_EVENT_LONG_PREV_PAGE:
            case TOUCH_EVENT_LONG_NEXT_PAGE:
                ui_function_menu_enter();
                break;
            default:
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
