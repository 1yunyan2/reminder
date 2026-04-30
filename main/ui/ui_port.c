#include "ui_port.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "bsp/bsp_config.h"
#include "bsp/bsp_board.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "ui/reminder.h"
#include <string.h>
#include <limits.h>

// 声明外部自定义字体
LV_FONT_DECLARE(font_cn_16);
/* 添加这三个函数的前置声明 */
static void main_clock_refresh(void);
static void main_clock_tick_cb(lv_timer_t *t);
static void gif_auto_hide_cb(lv_timer_t *t);
// 主界面 GIF 选择策略：0 = 固定 GIF（开发期默认），1 = 随机 GIF（后期开启）
#define UI_MAIN_GIF_RANDOM 0

// 功能菜单空闲超时：30s 无翻页操作自动返回主界面
#define UI_MENU_IDLE_TIMEOUT_MS 30000

// 声明一个初始化 SPIFFS 的函数
void init_spiffs(void)
{
    ESP_LOGI("SPIFFS", "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",         // 这是虚拟路径的前缀，也就是挂载点
        .partition_label = "assets",    // ✅ 必须和你 partitions.csv 里的名字一模一样
        .max_files = 5,                 // 最多同时打开的文件数
        .format_if_mount_failed = false // 既然我们已经烧录了镜像，千万别格式化它
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf); // 挂载文件系统

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL) // 挂载失败，且没有格式化（可能是分区损坏或未烧录）
        {
            ESP_LOGE("SPIFFS", "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND) // 找不到文件系统分区
        {
            ESP_LOGE("SPIFFS", "Failed to find SPIFFS partition");
        }
        else
        {
            ESP_LOGE("SPIFFS", "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret)); // 其他错误
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used); //   获取文件系统信息
    if (ret != ESP_OK)
    {
        ESP_LOGE("SPIFFS", "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI("SPIFFS", "Partition size: total: %d, used: %d", total, used);
    }
}
static const char *TAG = "UI_PORT";
// 在文件开头声明
lv_display_t *lvgl_disp = NULL;
/// LVGL9.5.0 核心类型变更：lv_disp_t 变更为 lv_display_t
static lv_obj_t *gif_obj = NULL;

// ─── 主界面时钟 UI（常驻，不随菜单隐藏）───────────────────────────
// 每个数字/冒号各占一个固定坐标的格子，格子永不移动，只有内容改变。
// 这是消除比例字体晃动的唯一可靠方案。
static lv_obj_t *s_clock_d[6];             // 6 个数字格：H1 H2  M1 M2  S1 S2
static lv_obj_t *s_clock_col[2];           // 2 个冒号格
static lv_obj_t *s_time_tz_lbl = NULL;     // 时区文字  (font 14, 灰色)
static lv_obj_t *s_time_date_lbl = NULL;   // 日期 + 星期  (font_montserrat_20)
static lv_timer_t *s_main_tick_tmr = NULL; // 1s 永久心跳，全程运行

// GIF 情绪叠加层：仅在触摸情绪时短暂弹出，N 秒后自动隐藏恢复时钟视图
static lv_timer_t *s_gif_hide_tmr = NULL; // GIF 自动隐藏定时器

// // todo 已经去除 1. 定义 LVGL 9 标准的图像描述符 (可以放在 eye_gif_show 函数外面)
// const lv_image_dsc_t gif_eye_dsc = {
//     .header.magic = LV_IMAGE_HEADER_MAGIC,
//     .header.cf = LV_COLOR_FORMAT_RAW, // 关键：告诉 LVGL 这是未解码的 RAW 数据（交由内部 GIF 解码器处理）
//     .header.flags = 0,
//     .header.w = 320, // 虽然是 RAW，但填写真实宽高有助于布局
//     .header.h = 240,
//     .header.stride = 0,
//     .data_size = 493917, // 使用你头文件里的真实大小
//     .data = gif_eye_gif, // 指向你的数据数组
// };

// void eye_gif_show(void)
// {
//     lv_obj_t *scr = lv_screen_active();
//     // 1. 设置对比底色，关闭滚动条
//     lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN); // 设置背景为黑色，突出显示 GIF
//     lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);                // 关闭滚动条，保持界面干净
//     if (gif_obj != NULL)
//     {
//         lv_obj_del(gif_obj);
//     }
//     lv_obj_t *label = lv_label_create(scr);
//     lv_label_set_text(label, "GIF Decoder OK!");
//     lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
//     lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);
//     // ✅ 2. 核心修复：换回 lv_gif_create 激活专用的动画渲染器！
//     gif_obj = lv_gif_create(scr);
//     // 继续使用描述符，这能完美避开裸数组的 "G" 字符识别 Bug
//     lv_gif_set_src(gif_obj, &gif_eye_dsc);
//     lv_obj_center(gif_obj);
//     // ✅ 3. 核心修复：强制 LVGL 立即刷新排版，扒掉 0x0 的伪装
//     lv_obj_update_layout(gif_obj);

//     // 获取并打印真实信息
//     int32_t w = lv_obj_get_width(gif_obj);
//     int32_t h = lv_obj_get_height(gif_obj);
//     int32_t x = lv_obj_get_x(gif_obj);
//     int32_t y = lv_obj_get_y(gif_obj);

//     ESP_LOGI(TAG, "--- GIF Object Real Info ---");
//     ESP_LOGI(TAG, "  Width:  %" PRId32, w);
//     ESP_LOGI(TAG, "  Height: %" PRId32, h);
//     ESP_LOGI(TAG, "  X:      %" PRId32, x);
//     ESP_LOGI(TAG, "  Y:      %" PRId32, y);
//     ESP_LOGI("UI", "GIF 动画启动，当前 PSRAM 剩余: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
// }

// 主界面 GIF 路径列表：随机模式时从中抽取一项，固定模式时永远使用第 0 项。
// 后续追加新的 GIF 资源直接在此数组末尾扩展即可。
static const char *const s_main_gif_paths[] = {
    "S:/one.gif",
    "S:/two.gif",
    "S:/three.gif",
    "S:/four.gif",
    "S:/five.gif",

};

static const char *pick_main_gif_path(void)
{
#if UI_MAIN_GIF_RANDOM
    size_t n = sizeof(s_main_gif_paths) / sizeof(s_main_gif_paths[0]); // 数组元素数量
    return s_main_gif_paths[esp_random() % n];                         // 随机选择一个路径返回，随机选择一个 GIF 资源
#else
    return s_main_gif_paths[0]; // 固定使用第 0 项 GIF 资源
#endif
}

/* 切换 GIF 内容（不重建对象，仅更新 source；需在 LVGL 锁内调用） */
static void gif_switch_source(void)
{
    if (gif_obj == NULL)
        return;
    lv_gif_set_src(gif_obj, pick_main_gif_path());
}
// // 1. 声明你的图片数据数组（确保这个名字和你在 .c 文件里定义的一致）
// extern const uint8_t gImage_picture[];

// // 2. 包装成 LVGL 图像描述符
// const lv_image_dsc_t my_raw_image = {
//     .header.magic = LV_IMAGE_HEADER_MAGIC,
//     .header.cf = LV_COLOR_FORMAT_RGB565, // 关键：设置为 RGB565
//     .header.w = 240,                     // 图片宽度
//     .header.h = 311,                     // 图片高度
//     .header.stride = 240 * 2,            // 每一行的字节数 (240像素 * 2字节)
//     .data_size = 149280,                 // 数组总大小
//     .data = gImage_picture,              // 指向数组
// };
// // 2. 创建一个显示图片的函数
// void show_my_picture(void)
// {
//     // 创建一个图像对象
//     lv_obj_t *img = lv_image_create(lv_screen_active());

//     // 设置图片源
//     lv_image_set_src(img, &my_raw_image);

//     // 居中显示
//     lv_obj_center(img);

//     // (可选) 如果背景还没设为黑色，可以顺便设一下
//     lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
// }

static esp_err_t app_lvgl_init(void)
{
    // ✅ 强制打印内存
    ESP_LOGI(TAG, "--- Memory Check ---");
    ESP_LOGI(TAG, "Free PSRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "Free SRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    bsp_board_t *board = bsp_board_get_instance();
    if (board == NULL || board->lcd_panel == NULL)
    {
        ESP_LOGE(TAG, "LCD 未初始化！");
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. 初始化 LVGL 核心任务 */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 3, // LVGL 任务优先级（根据实际情况调整，确保高于其他业务任务）
        .task_stack = 8192,
        .task_affinity = 1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 10,

    };
    esp_err_t err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK)
        return err;

    /* 2. 应用 PSRAM 双缓冲策略 */
    ESP_LOGI(TAG, "正在应用 PSRAM 双缓冲 + SRAM DMA 传输策略...");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = board->lcd_io,                          // LCD IO 句柄
        .panel_handle = board->lcd_panel,                    // LCD 面板句柄
        .buffer_size = (BSP_LCD_WIDTH * BSP_LCD_HEIGHT) / 4, // 缓冲区大小（1/4 屏）
        .double_buffer = true,                               // 启用双缓冲
        .hres = BSP_LCD_WIDTH,                               // 水平分辨率
        .vres = BSP_LCD_HEIGHT,                              // 垂直分辨率
        .monochrome = false,                                 // 非单色屏
        .color_format = LV_COLOR_FORMAT_RGB565,              // RGB565 格式
        .rotation = {
            .swap_xy = true,   // 交换 X/Y 轴
            .mirror_x = false, // X 轴镜像
            .mirror_y = true,  // Y 轴镜像
        },
        .flags = {
            .buff_dma = true,    // 使用 DMA 传输
            .swap_bytes = false, // 不交换字节
            .buff_spiram = true, // 使用 SPIRAM 分配缓冲区
        }};

    lvgl_disp = lvgl_port_add_disp(&disp_cfg); // 添加 LVGL 显示设备
    if (lvgl_disp != NULL)
    {
        lv_display_set_render_mode(lvgl_disp, LV_DISPLAY_RENDER_MODE_PARTIAL);

        if (lvgl_port_lock(1000))
        {
            lv_obj_t *screen = lv_screen_active();
            if (screen != NULL)
            {
                lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
            }
            lvgl_port_unlock();
        }
    }
    return ESP_OK;
}

// void test_red_screen(void)
// {
//     lv_obj_t *scr = lv_screen_active();
//     lv_obj_set_style_bg_color(scr, lv_color_hex(0xFF0000), LV_PART_MAIN); // 设置背景为红色，测试显示效果

//     lv_obj_t *label = lv_label_create(scr);                                   // 创建一个标签对象
//     lv_label_set_text(label, "GIF Test Ready");                               // 设置标签文本
//     lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN); // 设置标签文本颜色为白色
//     lv_obj_center(label);                                                     // 将标签居中显示
// }

/* ─── 主界面时钟 UI 创建（在 LVGL 锁内调用）────────────────────────
 * 每个数字/冒号独占一个固定坐标的格子（slot），格子永不移动。
 * 这样即使 Montserrat 是比例字体（'1' 比 '0' 窄），格子位置固定，
 * 视觉上完全消除晃动。
 *
 * 布局（320px 宽屏）：
 *   格子宽度：数字 DIGIT_W=40px，冒号 COLON_W=22px
 *   总宽：6×40 + 2×22 = 284px  起始 x = (320-284)/2 = 18px
 *   格序：[H1][H2][:][M1][M2][:][S1][S2]
 * ───────────────────────────────────────────────────────────────── */
#define CLOCK_DIGIT_W 40 // 数字格宽（含左右留白）
#define CLOCK_COLON_W 22 // 冒号格宽
#define CLOCK_H 60       // 格高（与 font_48 行高匹配）
#define CLOCK_Y 25       // 距屏幕顶部

static void make_clock_cell(lv_obj_t *scr, lv_obj_t **out,
                            int32_t x, int32_t w, const char *init_text)
{
    *out = lv_label_create(scr);
    lv_obj_set_style_text_font(*out, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(*out, lv_color_white(), 0);
    lv_obj_set_style_text_align(*out, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(*out, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(*out, w, CLOCK_H);
    lv_obj_set_pos(*out, x, CLOCK_Y);
    lv_label_set_text(*out, init_text);
}

static void main_clock_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_font(scr, &font_cn_16, 0); // 全局默认中文字体

    /* 计算起始 x，使时钟整体居中 */
    int32_t total_w = 6 * CLOCK_DIGIT_W + 2 * CLOCK_COLON_W;
    int32_t sx = (BSP_LCD_WIDTH - total_w) / 2;

    /* 格子 x 坐标（按顺序：H1 H2 : M1 M2 : S1 S2） */
    int32_t xs[8] = {
        sx,
        sx + CLOCK_DIGIT_W,
        sx + CLOCK_DIGIT_W * 2,
        sx + CLOCK_DIGIT_W * 2 + CLOCK_COLON_W,
        sx + CLOCK_DIGIT_W * 3 + CLOCK_COLON_W,
        sx + CLOCK_DIGIT_W * 4 + CLOCK_COLON_W,
        sx + CLOCK_DIGIT_W * 4 + CLOCK_COLON_W * 2,
        sx + CLOCK_DIGIT_W * 5 + CLOCK_COLON_W * 2,
    };

    /* 6 个数字格（索引 0,1,3,4,6,7 对应格序） */
    make_clock_cell(scr, &s_clock_d[0], xs[0], CLOCK_DIGIT_W, "-");
    make_clock_cell(scr, &s_clock_d[1], xs[1], CLOCK_DIGIT_W, "-");
    make_clock_cell(scr, &s_clock_d[2], xs[3], CLOCK_DIGIT_W, "-");
    make_clock_cell(scr, &s_clock_d[3], xs[4], CLOCK_DIGIT_W, "-");
    make_clock_cell(scr, &s_clock_d[4], xs[6], CLOCK_DIGIT_W, "-");
    make_clock_cell(scr, &s_clock_d[5], xs[7], CLOCK_DIGIT_W, "-");

    /* 2 个冒号格（固定内容，永不更新） */
    make_clock_cell(scr, &s_clock_col[0], xs[2], CLOCK_COLON_W, ":");
    make_clock_cell(scr, &s_clock_col[1], xs[5], CLOCK_COLON_W, ":");

    /* 时区标签 — 固定宽度居中，灰色小字 */
    s_time_tz_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(s_time_tz_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_time_tz_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_time_tz_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(s_time_tz_lbl, BSP_LCD_WIDTH, 20);
    lv_obj_set_pos(s_time_tz_lbl, 0, BSP_LCD_HEIGHT - 52);
    lv_label_set_text(s_time_tz_lbl, "中国标准时间");

    /* 日期 + 星期 — 固定宽度居中，白色中号字，内容静止不晃动 */
    s_time_date_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_time_date_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_time_date_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_time_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_time_date_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(s_time_date_lbl, BSP_LCD_WIDTH, 28);
    lv_obj_set_pos(s_time_date_lbl, 0, BSP_LCD_HEIGHT - 30);
    lv_label_set_text(s_time_date_lbl, "--月--日");

    /* GIF 情绪叠加层：全屏，默认隐藏，触发情绪时显示 5s 后自动隐藏 */
    gif_obj = lv_gif_create(scr);
    lv_gif_set_color_format(gif_obj, LV_COLOR_FORMAT_RGB565);
    lv_gif_set_src(gif_obj, pick_main_gif_path());
    lv_obj_center(gif_obj);
    lv_obj_add_flag(gif_obj, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "主界面时钟 UI 已创建");
}

void ui_init(void)
{
    init_spiffs();
    app_lvgl_init();

    if (lvgl_port_lock(1000))
    {
        main_clock_create();
        lvgl_port_unlock();
    }

    /* 启动永久 1s 心跳（在锁外创建，lv_timer 线程安全） */
    if (lvgl_port_lock(100))
    {
        s_main_tick_tmr = lv_timer_create(main_clock_tick_cb, 1000, NULL);
        main_clock_refresh(); // 立即刷一次，不等 1s
        lvgl_port_unlock();
    }
}

void ui_update_wifi(int rssi) // 更新 WIFI 信号强度
{
}

void ui_update_battery(int soc) // 根据电量百分比更新电池图标
{
}

/* 刷新主界面时钟（在 LVGL 任务上下文调用，无需加锁） */
/* 将单个数字写入对应格子（只有内容变化时才 set_text，减少重绘） */
static void set_digit(lv_obj_t *lbl, uint8_t val)
{
    char buf[2] = {'0' + val, '\0'};
    lv_label_set_text(lbl, buf);
}

static void main_clock_refresh(void)
{
    if (s_clock_d[0] == NULL)
        return;

    if (reminder_is_time_synced())
    {
        uint8_t h, m, sec;
        reminder_get_current_time(&h, &m, &sec);

        /* 逐格更新，格子坐标固定，消除晃动 */
        set_digit(s_clock_d[0], h / 10);
        set_digit(s_clock_d[1], h % 10);
        set_digit(s_clock_d[2], m / 10);
        set_digit(s_clock_d[3], m % 10);
        set_digit(s_clock_d[4], sec / 10);
        set_digit(s_clock_d[5], sec % 10);

        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        static const char *const wday_cn[] = {
            "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        char buf[32];
        snprintf(buf, sizeof(buf), "%d 月 %d 日  %s",
                 tm_now.tm_mon + 1, tm_now.tm_mday,
                 wday_cn[tm_now.tm_wday & 0x7]);
        lv_label_set_text(s_time_date_lbl, buf);
    }
    else
    {
        for (int i = 0; i < 6; i++)
            lv_label_set_text(s_clock_d[i], "-");
        lv_label_set_text(s_time_date_lbl, "等待时间同步...");
    }
}

/* 1s 永久心跳回调 */
static void main_clock_tick_cb(lv_timer_t *t)
{
    (void)t;
    main_clock_refresh();
}

/* GIF 5s 后自动隐藏，恢复时钟视图 */
static void gif_auto_hide_cb(lv_timer_t *t)
{
    (void)t;
    if (gif_obj)
        lv_obj_add_flag(gif_obj, LV_OBJ_FLAG_HIDDEN);
    s_gif_hide_tmr = NULL;
}

/**
 * @brief 公开 API：手动触发时间显示刷新
 *
 * 时间页内部已有 1s 心跳，正常无需调用此函数。
 * 仅在外部模块（如 SNTP 首次同步成功）希望立即让 UI 刷新一次时调用。
 */
void ui_update_time(void)
{
    if (!lvgl_port_lock(100))
        return;
    main_clock_refresh();
    lvgl_port_unlock();
}

// 根据LLM 返回的情绪名称来更新表情图标，待实现
void ui_update_emotion(const char *emotion)
{
}

// void ui_update_text(const char *text) // todo后续去除：在主内容区显示文本（如 STT 结果或 TTS 文本）
// {
// }

// 情绪叠加层静态对象（首次创建后复用）
// 含义：情绪叠加层面板对象，包含情绪名称、动画描述和音效描述三个标签。首次调用 ui_show_emotion 时创建并配置好样式，后续调用只更新文本内容并显示/隐藏。
static lv_obj_t *s_emo_panel = NULL;
static lv_obj_t *s_emo_name_lbl = NULL;
static lv_obj_t *s_emo_anim_lbl = NULL;
static lv_obj_t *s_emo_audio_lbl = NULL;
static lv_timer_t *s_emo_timer = NULL;

static void hide_emotion_cb(lv_timer_t *t) // 隐藏表情面板
{
    (void)t; // 定时器回调，无需加锁
    if (s_emo_panel)
        lv_obj_add_flag(s_emo_panel, LV_OBJ_FLAG_HIDDEN); // 隐藏表情面板
    s_emo_timer = NULL;
}

/**
 * @brief 在屏幕底部显示当前情绪文字（3秒后自动隐藏，GIF 继续在后面播放）
 * @param name      情绪名称，如 "开心"
 * @param anim_desc 动画描述，如 "眯眼笑+冒星星"
 * @param audio_desc 音效描述，如 "开心笑声"
 * @note 显示的文字会覆盖掉当前正在播放的 GIF
 *     1. 首次调用时创建一个半透明深色圆角面板，贴屏幕底部，内含三个标签分别显示情绪名称、动画描述和音效描述，并设置好样式。
      2. 每次调用时更新标签文本并显示面板，同时重置一个 3 秒自动隐藏的定时器（如果之前已经存在则重置，否则创建）。当定时器回调触发时隐藏面板。
       3. 这样就实现了每次调用 ui_show_emotion 都会在屏幕底部显示对应的情绪信息，并在 3 秒后自动消失，期间 GIF 动画继续在上层播放。
 *
 */
void ui_show_emotion(const char *name, const char *anim_desc, const char *audio_desc)
{
    if (!lvgl_port_lock(100)) // 锁定 LVGL
        return;

    lv_obj_t *scr = lv_screen_active(); // 获取当前屏幕对象

    if (s_emo_panel == NULL)
    {
        // 半透明深色圆角面板，贴屏幕底部
        s_emo_panel = lv_obj_create(scr);
        lv_obj_set_size(s_emo_panel, 220, 100);
        lv_obj_align(s_emo_panel, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_set_style_bg_color(s_emo_panel, lv_color_hex(0x111111), 0);
        lv_obj_set_style_bg_opa(s_emo_panel, LV_OPA_80, 0);
        lv_obj_set_style_border_color(s_emo_panel, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(s_emo_panel, LV_OPA_30, 0);
        lv_obj_set_style_border_width(s_emo_panel, 1, 0);
        lv_obj_set_style_radius(s_emo_panel, 12, 0);
        lv_obj_set_style_pad_all(s_emo_panel, 6, 0);
        lv_obj_clear_flag(s_emo_panel, LV_OBJ_FLAG_SCROLLABLE);

        // 情绪名（白色，顶部）
        s_emo_name_lbl = lv_label_create(s_emo_panel);                          // 创建一个标签
        lv_obj_set_style_text_color(s_emo_name_lbl, lv_color_hex(0xFFFFFF), 0); // 设置标签文本颜色为白色
        lv_obj_align(s_emo_name_lbl, LV_ALIGN_TOP_MID, 0, 2);                   // 将标签对齐到面板顶部中间，向下偏移 2 像素

        // 动画描述（浅蓝色，中间）
        s_emo_anim_lbl = lv_label_create(s_emo_panel);
        lv_obj_set_style_text_color(s_emo_anim_lbl, lv_color_hex(0x88CCFF), 0);
        lv_label_set_long_mode(s_emo_anim_lbl, LV_LABEL_LONG_WRAP); // 允许换行
        lv_obj_set_width(s_emo_anim_lbl, 200);                      // 设置标签宽度，超过时换行
        lv_obj_align(s_emo_anim_lbl, LV_ALIGN_CENTER, 0, 6);        // 将标签对齐到面板中心，向下偏移 6 像素

        // 音效描述（浅黄色，底部）
        s_emo_audio_lbl = lv_label_create(s_emo_panel);                          // 创建标签
        lv_obj_set_style_text_color(s_emo_audio_lbl, lv_color_hex(0xFFDD88), 0); // 设置文本颜色为浅黄色
        lv_label_set_long_mode(s_emo_audio_lbl, LV_LABEL_LONG_WRAP);             // 允许换行
        lv_obj_set_width(s_emo_audio_lbl, 200);                                  // 设置标签宽度，超过时换行
        lv_obj_align(s_emo_audio_lbl, LV_ALIGN_BOTTOM_MID, 0, -2);               // 将标签对齐到面板底部中间，向上偏移 2 像素
    }

    lv_label_set_text(s_emo_name_lbl, name);            // 更新情绪名称
    lv_label_set_text(s_emo_anim_lbl, anim_desc);       // 更新动画描述
    lv_label_set_text(s_emo_audio_lbl, audio_desc);     // 更新音效描述
    lv_obj_clear_flag(s_emo_panel, LV_OBJ_FLAG_HIDDEN); // 显示表情面板

    // 重置 3 秒自动隐藏定时器
    if (s_emo_timer != NULL)
        lv_timer_reset(s_emo_timer); // 如果定时器已存在，重置它
    else
        s_emo_timer = lv_timer_create(hide_emotion_cb, 3000, NULL); // 否则创建一个新的定时器，3 秒后调用 hide_emotion_cb 隐藏表情面板
    lv_timer_set_repeat_count(s_emo_timer, 1);                      // 设置定时器只执行一次

    lvgl_port_unlock();
}

// ────────────────────────────────────────────────────────────────────────
// 页面 / 功能菜单（由触摸翻页键驱动）
//
// 视图状态机：
//   UI_VIEW_MAIN          —— 主界面（背景 GIF）。短按翻页暂为空操作。
//   UI_VIEW_FUNCTION_MENU —— 功能菜单：在子页之间循环；空闲 30s 自动退出。
//
// 子页定义：FN_PAGE_* 在此追加新条目即可（如后续接入闹钟、时间、设置等）。
// ────────────────────────────────────────────────────────────────────────
typedef enum
{
    UI_VIEW_MAIN = 0,
    UI_VIEW_FUNCTION_MENU,
} ui_view_t;

typedef enum
{
    FN_PAGE_WEATHER = 0,
    FN_PAGE_ALARM,
    // ↓ 后续如需扩展，在此追加，并补全 s_fn_page_titles[]
    FN_PAGE_COUNT
} fn_page_t;

static const char *const s_fn_page_titles[FN_PAGE_COUNT] = {
    [FN_PAGE_WEATHER] = "天气查看",
    [FN_PAGE_ALARM] = "闹钟",
};

static ui_view_t s_view = UI_VIEW_MAIN;
static fn_page_t s_fn_page = FN_PAGE_WEATHER;
static lv_obj_t *s_menu_panel = NULL;
static lv_obj_t *s_menu_title = NULL;
static lv_obj_t *s_menu_body = NULL;
static lv_timer_t *s_menu_idle_tmr = NULL;

// LVGL 定时器回调，已在 LVGL 任务上下文中执行，无需再加锁
static void menu_idle_timeout_cb(lv_timer_t *t)
{
    (void)t;
    s_menu_idle_tmr = NULL;
    if (s_view != UI_VIEW_FUNCTION_MENU)
        return;
    if (s_menu_panel)
        lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
    s_view = UI_VIEW_MAIN;
    ESP_LOGI(TAG, "功能菜单空闲超时，返回主界面");
}

// 重置/启动 30s 空闲计时（调用方需已持有 lvgl 锁，或运行在 LVGL 任务内）
static void menu_kick_idle_timer(void)
{
    if (s_menu_idle_tmr != NULL)
    {
        lv_timer_reset(s_menu_idle_tmr);
        return;
    }
    s_menu_idle_tmr = lv_timer_create(menu_idle_timeout_cb, UI_MENU_IDLE_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(s_menu_idle_tmr, 1);
}

static void menu_cancel_idle_timer(void)
{
    if (s_menu_idle_tmr != NULL)
    {
        lv_timer_del(s_menu_idle_tmr);
        s_menu_idle_tmr = NULL;
    }
}

// 首次进入菜单时按需创建覆盖层；后续仅切换 hidden / 文本
static void ensure_menu_panel(void)
{
    if (s_menu_panel != NULL)
        return;

    lv_obj_t *scr = lv_screen_active();
    s_menu_panel = lv_obj_create(scr);
    lv_obj_set_size(s_menu_panel, BSP_LCD_WIDTH, BSP_LCD_HEIGHT);
    lv_obj_align(s_menu_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_menu_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_menu_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_menu_panel, 0, 0);
    lv_obj_set_style_pad_all(s_menu_panel, 8, 0);
    lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_menu_title = lv_label_create(s_menu_panel);
    lv_obj_set_style_text_color(s_menu_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_menu_title, LV_ALIGN_TOP_MID, 0, 4);

    s_menu_body = lv_label_create(s_menu_panel);
    lv_obj_set_style_text_color(s_menu_body, lv_color_hex(0x88CCFF), 0);
    lv_label_set_long_mode(s_menu_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_menu_body, BSP_LCD_WIDTH - 24);
    lv_obj_align(s_menu_body, LV_ALIGN_CENTER, 0, 0);

    lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
}

/* ─── 闹钟页 UI（320×240 横屏布局）─────────────────────────────────────────
 *
 *  y= 4 : 标题 "闹钟"（s_menu_title，居中）
 *  y=28 : 下一个闹钟倒计时文字（灰色小字）
 *  y=50 : 闹钟卡片列表（最多 4 张，每张 40px 高，间隔 4px）
 *
 *  每张卡片：
 *    左列  — HH:MM（lv_font_montserrat_20，启用白色/禁用灰色）
 *            重复模式文字（font_cn_16，灰色）
 *    右列  — "开"/"关"（橙色/暗灰）
 * ────────────────────────────────────────────────────────────────────────── */

#define ALARM_CARD_H 40
#define ALARM_CARD_GAP 4
#define ALARM_MAX_SHOW 4 /* 最多同屏显示 4 条 */

static lv_obj_t *s_alarm_next_lbl = NULL;   /* "XX小时后响起" */
static lv_obj_t *s_alarm_cards_cont = NULL; /* 卡片容器 */

/* 重复模式 → 中文 */
static const char *s_alarm_repeat_cn(alarm_repeat_t r)
{
    switch (r)
    {
    case ALARM_REPEAT_ONCE:
        return "仅一次";
    case ALARM_REPEAT_DAILY:
        return "每天";
    case ALARM_REPEAT_WEEKDAY:
        return "法定工作日";
    case ALARM_REPEAT_WEEKEND:
        return "周末";
    case ALARM_REPEAT_CUSTOM:
        return "自定义";
    default:
        return "";
    }
}

/* 刷新"下一个闹钟"倒计时文字 */
static void alarm_refresh_next_lbl(void)
{
    if (!s_alarm_next_lbl)
        return;

    alarm_entry_t list[REMINDER_MAX_ALARMS];
    uint8_t count = 0;
    reminder_alarm_get_all(list, &count); /// 获取所有闹钟

    time_t now = time(NULL);                           // 获取当前时间
    struct tm tm_now;                                  // 获取当前时间结构体
    localtime_r(&now, &tm_now);                        // 将当前时间转换为结构体
    int now_min = tm_now.tm_hour * 60 + tm_now.tm_min; // 获取当前时间分钟数
    int min_remain = INT_MAX;                          // 当前时间离下一次闹钟还有多长时间

    for (uint8_t i = 0; i < count; i++)
    {
        if (!list[i].enabled) // 如果时钟得第几个钟是禁用的
            continue;
        int alarm_min = list[i].hour * 60 + list[i].minute;
        int remain = alarm_min - now_min;
        if (remain <= 0)
            remain += 24 * 60;
        if (remain < min_remain)
            min_remain = remain;
    }

    if (min_remain == INT_MAX)
    {
        lv_label_set_text(s_alarm_next_lbl, "当前无启用的闹钟");
    }
    else
    {
        char buf[64];
        int h = min_remain / 60, m = min_remain % 60;
        if (h > 0)
            snprintf(buf, sizeof(buf), "闹钟将在 %d 小时 %d 分钟后响起", h, m);
        else
            snprintf(buf, sizeof(buf), "闹钟将在 %d 分钟后响起", m);
        lv_label_set_text(s_alarm_next_lbl, buf);
    }
}

/* 在 s_menu_panel 内首次创建闹钟页专属控件 */
static void alarm_page_create(void)
{
    if (s_alarm_next_lbl)
        return; /* 已创建 */

    /* 倒计时提示标签 */
    s_alarm_next_lbl = lv_label_create(s_menu_panel);
    lv_obj_set_style_text_color(s_alarm_next_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_long_mode(s_alarm_next_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_alarm_next_lbl, BSP_LCD_WIDTH - 16);
    lv_obj_align(s_alarm_next_lbl, LV_ALIGN_TOP_LEFT, 0, 28);

    /* 卡片列表容器（透明背景，无边框，无内边距） */
    s_alarm_cards_cont = lv_obj_create(s_menu_panel);
    lv_obj_set_size(s_alarm_cards_cont, BSP_LCD_WIDTH - 16,
                    BSP_LCD_HEIGHT - 8 - 50); /* 面板顶部 padding=8，卡片起始 y=50 */
    lv_obj_align(s_alarm_cards_cont, LV_ALIGN_TOP_LEFT, 0, 50);
    lv_obj_set_style_bg_opa(s_alarm_cards_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_alarm_cards_cont, 0, 0);
    lv_obj_set_style_pad_all(s_alarm_cards_cont, 0, 0);
    lv_obj_clear_flag(s_alarm_cards_cont, LV_OBJ_FLAG_SCROLLABLE);
}

/* 读取闹钟数据，重建卡片列表 */
static void alarm_page_rebuild(void)
{
    if (!s_alarm_cards_cont)
        return;

    lv_obj_clean(s_alarm_cards_cont); /* 清除旧卡片 */

    alarm_entry_t list[REMINDER_MAX_ALARMS];
    uint8_t count = 0;
    reminder_alarm_get_all(list, &count);

    if (count == 0)
    {
        lv_obj_t *empty = lv_label_create(s_alarm_cards_cont);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(empty, "无闹钟");
        lv_obj_center(empty);
        alarm_refresh_next_lbl();
        return;
    }

    uint8_t shown = (count > ALARM_MAX_SHOW) ? ALARM_MAX_SHOW : count;
    for (uint8_t i = 0; i < shown; i++)
    {
        int32_t card_y = (int32_t)i * (ALARM_CARD_H + ALARM_CARD_GAP);

        /* 卡片背景 */
        lv_obj_t *card = lv_obj_create(s_alarm_cards_cont);
        lv_obj_set_size(card, lv_pct(100), ALARM_CARD_H);
        lv_obj_set_pos(card, 0, card_y);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1C1C1C), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x333333), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_hor(card, 8, 0);
        lv_obj_set_style_pad_ver(card, 4, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        /* 时间大字 */
        char tbuf[8];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", list[i].hour, list[i].minute);
        lv_obj_t *time_lbl = lv_label_create(card);
        lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(time_lbl,
                                    list[i].enabled ? lv_color_white() : lv_color_hex(0x555555), 0);
        lv_label_set_text(time_lbl, tbuf);
        lv_obj_align(time_lbl, LV_ALIGN_LEFT_MID, 0, -8);

        /* 重复模式小字 */
        lv_obj_t *rep_lbl = lv_label_create(card);
        lv_obj_set_style_text_color(rep_lbl, lv_color_hex(0x888888), 0);
        lv_label_set_text(rep_lbl, s_alarm_repeat_cn(list[i].repeat));
        lv_obj_align(rep_lbl, LV_ALIGN_LEFT_MID, 0, 11);

        /* 开/关状态 */
        lv_obj_t *sw_lbl = lv_label_create(card);
        lv_obj_set_style_text_color(sw_lbl,
                                    list[i].enabled ? lv_color_hex(0xFF9500) : lv_color_hex(0x555555), 0);
        lv_label_set_text(sw_lbl, list[i].enabled ? "开" : "关");
        lv_obj_align(sw_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    alarm_refresh_next_lbl();
}

static void alarm_page_show(void)
{
    alarm_page_create();
    alarm_page_rebuild();
    lv_obj_clear_flag(s_alarm_next_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_alarm_cards_cont, LV_OBJ_FLAG_HIDDEN);
}

static void alarm_page_hide(void)
{
    if (s_alarm_next_lbl)
        lv_obj_add_flag(s_alarm_next_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_alarm_cards_cont)
        lv_obj_add_flag(s_alarm_cards_cont, LV_OBJ_FLAG_HIDDEN);
}

static void render_fn_page(fn_page_t page)
{
    if (s_menu_title == NULL || s_menu_body == NULL)
        return;

    alarm_page_hide(); /* 先隐藏闹钟专属控件，按需再显示 */

    lv_label_set_text(s_menu_title, s_fn_page_titles[page]);

    switch (page)
    {
    case FN_PAGE_WEATHER:
        lv_label_set_text(s_menu_body, "(天气数据待接入)");
        lv_obj_clear_flag(s_menu_body, LV_OBJ_FLAG_HIDDEN);
        break;
    case FN_PAGE_ALARM:
        lv_obj_add_flag(s_menu_body, LV_OBJ_FLAG_HIDDEN); /* 隐藏通用文字 */
        alarm_page_show();
        break;
    default:
        lv_label_set_text(s_menu_body, "");
        lv_obj_clear_flag(s_menu_body, LV_OBJ_FLAG_HIDDEN);
        break;
    }
}

void ui_function_menu_enter(void)
{
    if (!lvgl_port_lock(100))
        return;

    ensure_menu_panel();

    if (s_view != UI_VIEW_FUNCTION_MENU)
    {
        s_view = UI_VIEW_FUNCTION_MENU;
        s_fn_page = FN_PAGE_WEATHER;
        ESP_LOGI(TAG, "进入功能菜单");
    }
    render_fn_page(s_fn_page);
    lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
    menu_kick_idle_timer();

    lvgl_port_unlock();
}

void ui_function_menu_exit(void)
{
    if (!lvgl_port_lock(100))
        return;

    if (s_view == UI_VIEW_FUNCTION_MENU)
    {
        if (s_menu_panel)
            lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
        s_view = UI_VIEW_MAIN;
        ESP_LOGI(TAG, "退出功能菜单，返回主界面");
    }
    menu_cancel_idle_timer();

    lvgl_port_unlock();
}

void ui_page_next(void)
{
    if (!lvgl_port_lock(100))
        return;

    if (s_view == UI_VIEW_FUNCTION_MENU)
    {
        s_fn_page = (fn_page_t)((s_fn_page + 1) % FN_PAGE_COUNT);
        render_fn_page(s_fn_page);
        menu_kick_idle_timer();
        ESP_LOGI(TAG, "菜单 → 下一页: %s", s_fn_page_titles[s_fn_page]);
    }
    else
    {
        // 主界面当前仅一页，留作后续多 GIF 切换的接入点
        ESP_LOGI(TAG, "主界面下一页（暂无）");
    }

    lvgl_port_unlock();
}

void ui_page_prev(void)
{
    if (!lvgl_port_lock(100))
        return;

    if (s_view == UI_VIEW_FUNCTION_MENU)
    {
        s_fn_page = (fn_page_t)((s_fn_page + FN_PAGE_COUNT - 1) % FN_PAGE_COUNT);
        render_fn_page(s_fn_page);
        menu_kick_idle_timer();
        ESP_LOGI(TAG, "菜单 → 上一页: %s", s_fn_page_titles[s_fn_page]);
    }
    else
    {
        ESP_LOGI(TAG, "主界面上一页（暂无）");
    }

    lvgl_port_unlock();
}

// ── 动画 + 音频映射表 ─────────────────────────────────────────────────────────
// 每一行 = 一个情绪的完整视听效果：GIF 函数 + 音频文件路径
// 新增情绪：① 实现 GIF 播放函数  ② 准备音频文件到 SPIFFS  ③ 此表加一行，完事
typedef struct
{
    const char *anim_id;     ///< 动画标识，对应 InteractionMatrix_t.screen_anim
    void (*play_func)(void); ///< GIF / 屏幕动画播放函数（在 LVGL 锁内调用）
    const char *audio_file;  ///< SPIFFS 音频路径，NULL = 该情绪无音效
} animation_map_t;

static const animation_map_t s_animation_map[] = {
    // anim_id                play_func          audio_file
    {"anim_happy_stars", gif_switch_source, "S:/laugh_short.mp3"}, // 情绪：开心
    // {"anim_curious_q",    play_curious_gif,   "S:/doubt.mp3"      },  //情绪：好奇
    // {"anim_tsundere",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_ticklish",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_sleepy",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_grieved",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_comfortable",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_act_cute",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_angry",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_shy",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_surprised",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_sluggish",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_healing",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_excited",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_shy_rub",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_comfortable_roll",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_tsundere_pet",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_sluggish_sit",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_surprised_hug",        play_angry_gif,     "S:/angry.mp3"      },
    // {"anim_ticklish_wiggle",        play_angry_gif,     "S:/angry.mp3"      },

};

void ui_play_animation(const char *anim_id)
{
    if (anim_id == NULL)
        return;

    static const size_t MAP_LEN = sizeof(s_animation_map) / sizeof(s_animation_map[0]);
    size_t idx;
    for (idx = 0; idx < MAP_LEN; idx++)
    {
        if (strcmp(anim_id, s_animation_map[idx].anim_id) == 0)
            break;
    }

    if (idx == MAP_LEN)
    {
        ESP_LOGW(TAG, "未知动画 ID: %s", anim_id);
        return;
    }

    const animation_map_t *entry = &s_animation_map[idx];

    /* ── GIF 叠加层：显示 gif_obj（已预创建），5s 后自动隐藏 ── */
    if (lvgl_port_lock(100))
    {
        if (gif_obj != NULL)
            lv_obj_clear_flag(gif_obj, LV_OBJ_FLAG_HIDDEN);

        /* 启动/重置 5s 自动隐藏定时器 */
        if (s_gif_hide_tmr != NULL)
        {
            lv_timer_reset(s_gif_hide_tmr);
        }
        else
        {
            s_gif_hide_tmr = lv_timer_create(gif_auto_hide_cb, 5000, NULL);
            lv_timer_set_repeat_count(s_gif_hide_tmr, 1);
        }

        lvgl_port_unlock();
    }

    /* ── 音频（LVGL 锁外调用，避免阻塞渲染）── */
    if (entry->audio_file != NULL)
    {
        ESP_LOGI(TAG, "音频: %s", entry->audio_file);
        // TODO: audio_player_play_file(entry->audio_file);
    }

    ESP_LOGI(TAG, "动画触发: %s", anim_id);
}
