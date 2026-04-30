
/**
 * @file ui_port.c
 * @brief UI界面端口实现文件
 * 核心功能：基于LVGL实现智能玩偶的全量UI界面，包含主时钟、闹钟设置、倒计时、情绪面板、功能菜单四大模块
 * 架构特点：
 *   1. 视图状态机：根据当前界面状态分发触摸事件
 *   2. 异步渲染：所有LVGL操作均加锁保护，避免多线程冲突
 *   3. 低耦合：与底层触摸、提醒系统通过接口解耦
 */
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
#include "ui/reminder.h"
#include <string.h>
#include <limits.h>

/* ── 外部自定义中文字体声明 ── */
LV_FONT_DECLARE(font_cn_16);
typedef enum
{
    FN_PAGE_TIME = 0,  // 时间日期页
    FN_PAGE_ALARM,     // 闹钟管理页
    FN_PAGE_COUNTDOWN, // 倒计时页
    FN_PAGE_WEATHER,   // 天气查看页
    FN_PAGE_COUNT      // 页面总数（用于循环翻页）
} fn_page_t;

/* ═══════════════════════════════════════════════════════════════
 * 功能页面枚举（4 个核心功能页）
 * ═══════════════════════════════════════════════════════════════ */

// 功能页面对应的中文标题
static const char *const s_fn_page_titles[FN_PAGE_COUNT] = {
    [FN_PAGE_TIME] = "时间日期",
    [FN_PAGE_ALARM] = "闹钟",
    [FN_PAGE_COUNTDOWN] = "倒计时",
    [FN_PAGE_WEATHER] = "天气查看",
};

// 日志标签，用于ESP_LOG系列日志输出
static const char *TAG = "UI_PORT";

/* ── 函数前向声明（解决编译顺序依赖） ── */
static void main_clock_refresh(void);
static void main_clock_tick_cb(lv_timer_t *t);
static void gif_auto_hide_cb(lv_timer_t *t);
static const char *s_alarm_repeat_cn(alarm_repeat_t r);
static void countdown_tick_cb(lv_timer_t *t);
static void alarm_edit_render(void);
static void countdown_page_render(void);
static void render_fn_page(fn_page_t page);

/* ── 配置宏定义 ── */
#define UI_MAIN_GIF_RANDOM 0          // 主界面GIF是否随机播放：0=固定第一张，1=随机切换
#define UI_MENU_IDLE_TIMEOUT_MS 30000 // 功能菜单空闲超时时间（30秒无操作自动返回主界面）

/* ═══════════════════════════════════════════════════════════════
 * 闹钟编辑状态机枚举
 * 含义：定义闹钟编辑界面的3个编辑步骤，用于分步设置闹钟参数
 * ═══════════════════════════════════════════════════════════════ */
typedef enum
{
    ALARM_EDIT_HOUR,   // 编辑小时
    ALARM_EDIT_MINUTE, // 编辑分钟
    ALARM_EDIT_REPEAT, // 编辑重复模式
} alarm_edit_state_t;

// 闹钟重复模式数组（用于循环切换）
static const alarm_repeat_t s_repeat_modes[] = {
    ALARM_REPEAT_ONCE,    // 仅一次
    ALARM_REPEAT_DAILY,   // 每天
    ALARM_REPEAT_WEEKDAY, // 工作日
    ALARM_REPEAT_WEEKEND, // 周末
};
// 重复模式数量计算
#define REPEAT_MODE_COUNT (sizeof(s_repeat_modes) / sizeof(s_repeat_modes[0]))

/* ═══════════════════════════════════════════════════════════════
 * 倒计时状态枚举
 * 含义：定义倒计时功能的3种运行状态
 * ═══════════════════════════════════════════════════════════════ */
typedef enum
{
    CD_STATE_SET,     // 设置状态：设置倒计时时长
    CD_STATE_RUNNING, // 运行状态：倒计时正在计时
    CD_STATE_EXPIRED, // 到期状态：倒计时已结束
} cd_state_t;

/* ═══════════════════════════════════════════════════════════════
 * 全局 / 静态变量定义
 * ═══════════════════════════════════════════════════════════════ */
lv_display_t *lvgl_disp = NULL;  // LVGL显示设备句柄
static lv_obj_t *gif_obj = NULL; // GIF动画对象句柄

/* 主时钟 UI 相关对象 */
static lv_obj_t *s_clock_d[6];             // 时钟6位数字对象（时:分:秒，每位一个对象）
static lv_obj_t *s_clock_col[2];           // 时钟冒号分隔符对象（2个冒号）
static lv_obj_t *s_time_tz_lbl = NULL;     // 时区标签
static lv_obj_t *s_time_date_lbl = NULL;   // 日期标签
static lv_timer_t *s_main_tick_tmr = NULL; // 主时钟刷新定时器（1秒1次）
static lv_timer_t *s_gif_hide_tmr = NULL;  // GIF自动隐藏定时器

/* 功能菜单相关对象 */
static ui_view_t s_view = UI_VIEW_MAIN;    // 当前UI视图状态，默认主界面
static fn_page_t s_fn_page = FN_PAGE_TIME; // 当前功能菜单选中的页面
static lv_obj_t *s_menu_panel = NULL;      // 功能菜单根容器
static lv_obj_t *s_menu_title = NULL;      // 功能菜单标题
static lv_obj_t *s_menu_body = NULL;       // 功能菜单内容区域
static lv_timer_t *s_menu_idle_tmr = NULL; // 菜单空闲超时定时器

/* 闹钟编辑上下文 */
static struct
{
    alarm_edit_state_t state; // 当前编辑步骤
    int8_t alarm_id;          // 正在编辑的闹钟ID（-1表示新建闹钟）
    uint8_t hour;             // 编辑中的小时
    uint8_t minute;           // 编辑中的分钟
    alarm_repeat_t repeat;    // 编辑中的重复模式
} s_edit;

/* 闹钟列表选择相关 */
static int8_t s_alarm_selected = 0;   // 当前选中的闹钟索引
static uint8_t s_alarm_total_sel = 0; // 闹钟列表总可选数量

/* 倒计时上下文 */
static struct
{
    cd_state_t state;     // 倒计时当前状态
    uint8_t minutes;      // 设置的倒计时分钟数
    int timer_id;         // 底层提醒系统的倒计时ID
    lv_timer_t *tick_tmr; // 倒计时UI刷新定时器（1秒1次）
} s_cd = {.state = CD_STATE_SET, .minutes = 15, .timer_id = -1, .tick_tmr = NULL};

/* 闹钟页 UI 对象 */
static lv_obj_t *s_alarm_next_lbl = NULL;   // 下一个闹钟响铃提示标签
static lv_obj_t *s_alarm_cards_cont = NULL; // 闹钟列表卡片容器

/* 闹钟编辑 UI 对象 */
static lv_obj_t *s_edit_panel = NULL;      // 闹钟编辑界面根容器
static lv_obj_t *s_edit_hour_lbl = NULL;   // 编辑中的小时显示标签
static lv_obj_t *s_edit_colon_lbl = NULL;  // 编辑界面冒号
static lv_obj_t *s_edit_min_lbl = NULL;    // 编辑中的分钟显示标签
static lv_obj_t *s_edit_repeat_lbl = NULL; // 重复模式显示标签
static lv_obj_t *s_edit_hint_lbl = NULL;   // 操作提示标签

/* 倒计时 UI 对象 */
static lv_obj_t *s_cd_time_lbl = NULL;  // 倒计时时间显示标签
static lv_obj_t *s_cd_hint_lbl = NULL;  // 倒计时操作提示标签
static lv_obj_t *s_cd_state_lbl = NULL; // 倒计时状态标签

/* 情绪面板相关对象 */
static lv_obj_t *s_emo_panel = NULL;     // 情绪面板根容器
static lv_obj_t *s_emo_name_lbl = NULL;  // 情绪名称标签
static lv_obj_t *s_emo_anim_lbl = NULL;  // 动画描述标签
static lv_obj_t *s_emo_audio_lbl = NULL; // 音效描述标签
static lv_timer_t *s_emo_timer = NULL;   // 情绪面板自动隐藏定时器

/* ═══════════════════════════════════════════════════════════════
 * SPIFFS 文件系统初始化
 * 函数含义：挂载SPIFFS分区，用于加载GIF动画、字体、音频等资源文件
 * ═══════════════════════════════════════════════════════════════ */
void init_spiffs(void)
{
    ESP_LOGI("SPIFFS", "Initializing SPIFFS");

    // SPIFFS挂载配置结构体
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",           // 挂载根路径
        .partition_label = "assets",      // 分区标签（对应partition_table中的assets分区）
        .max_files = 5,                   // 最大同时打开文件数
        .format_if_mount_failed = false}; // 挂载失败时是否格式化分区

    // API含义：注册并挂载SPIFFS文件系统到VFS虚拟文件系统
    // API参数含义：&conf SPIFFS配置结构体指针
    // API返回值：ESP_OK=成功，其他=失败
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    // 挂载失败处理
    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
            ESP_LOGE("SPIFFS", "Failed to mount or format filesystem");
        else if (ret == ESP_ERR_NOT_FOUND)
            ESP_LOGE("SPIFFS", "Failed to find SPIFFS partition");
        else
            ESP_LOGE("SPIFFS", "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        return;
    }

    // 获取分区使用信息
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK)
        ESP_LOGE("SPIFFS", "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    else
        ESP_LOGI("SPIFFS", "Partition size: total: %d, used: %d", total, used);
}

/* ═══════════════════════════════════════════════════════════════
 * GIF 动画路径列表
 * 含义：主界面情绪GIF动画的文件路径，存储在SPIFFS的S盘根目录
 * ═══════════════════════════════════════════════════════════════ */
static const char *const s_main_gif_paths[] = {
    "S:/one.gif",
    "S:/two.gif",
    "S:/three.gif",
    "S:/four.gif",
    "S:/five.gif",
};

/**
 * @brief 选择主界面GIF路径
 * @return 返回值含义：选中的GIF文件路径
 */
static const char *pick_main_gif_path(void)
{
#if UI_MAIN_GIF_RANDOM
    // 随机模式：计算数组长度，通过随机数选择一个GIF路径
    size_t n = sizeof(s_main_gif_paths) / sizeof(s_main_gif_paths[0]);
    // API含义：获取32位硬件随机数
    return s_main_gif_paths[esp_random() % n];
#else
    // 固定模式：返回第一个GIF路径
    return s_main_gif_paths[0];
#endif
}

/**
 * @brief 切换GIF动画源
 * 函数含义：重新设置GIF对象的播放源，切换动画
 */
static void gif_switch_source(void)
{
    if (gif_obj == NULL)
        return;
    // API含义：设置GIF对象的播放源文件路径
    // API参数含义：gif_obj GIF对象句柄，pick_main_gif_path() 文件路径
    lv_gif_set_src(gif_obj, pick_main_gif_path());
}

/* ═══════════════════════════════════════════════════════════════
 * LVGL 图形库初始化
 * 函数含义：完成LVGL运行环境、显示设备、双缓冲、屏幕参数的全量配置
 * @return 返回值含义：ESP_OK=初始化成功，其他=失败
 * ═══════════════════════════════════════════════════════════════ */
static esp_err_t app_lvgl_init(void)
{
    // 打印内存信息，用于调试内存泄漏
    ESP_LOGI(TAG, "--- Memory Check ---");
    // API含义：获取指定内存类型的剩余空间
    // API参数含义：MALLOC_CAP_SPIRAM 外部PSRAM，MALLOC_CAP_INTERNAL 内部SRAM
    ESP_LOGI(TAG, "Free PSRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "Free SRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // 获取板级支持包的单例对象，包含LCD硬件句柄
    bsp_board_t *board = bsp_board_get_instance();
    if (board == NULL || board->lcd_panel == NULL)
    {
        ESP_LOGE(TAG, "LCD 未初始化！");
        return ESP_ERR_INVALID_STATE;
    }

    // LVGL端口配置结构体
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 3,       // LVGL任务优先级
        .task_stack = 8192,       // LVGL任务栈大小（8KB）
        .task_affinity = 1,       // 绑定到CPU1核心
        .task_max_sleep_ms = 500, // 任务最大休眠时间
        .timer_period_ms = 10,    // LVGL定时器周期（10ms）
    };

    // API含义：初始化LVGL端口层，创建LVGL任务
    // API参数含义：&lvgl_cfg 配置结构体指针
    esp_err_t err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK)
        return err;

    ESP_LOGI(TAG, "正在应用 PSRAM 双缓冲 + SRAM DMA 传输策略...");

    // LVGL显示设备配置结构体
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = board->lcd_io,                          // LCD IO句柄
        .panel_handle = board->lcd_panel,                    // LCD面板句柄
        .buffer_size = (BSP_LCD_WIDTH * BSP_LCD_HEIGHT) / 4, // 帧缓冲大小（1/4屏幕，部分渲染模式）
        .double_buffer = true,                               // 启用双缓冲
        .hres = BSP_LCD_WIDTH,                               // 屏幕水平分辨率
        .vres = BSP_LCD_HEIGHT,                              // 屏幕垂直分辨率
        .monochrome = false,                                 // 非单色屏
        .color_format = LV_COLOR_FORMAT_RGB565,              // 颜色格式RGB565（16位色）
        .rotation = {
            // 屏幕旋转配置
            .swap_xy = true,   // 交换XY轴（横屏）
            .mirror_x = false, // X轴不镜像
            .mirror_y = true,  // Y轴镜像
        },
        .flags = {
            // 功能标志位
            .buff_dma = true,    // 启用DMA传输
            .swap_bytes = false, // 不交换字节序
            .buff_spiram = true, // 帧缓冲使用PSRAM
        }};

    // API含义：添加显示设备到LVGL
    // API参数含义：&disp_cfg 显示配置结构体指针
    // API返回值：显示设备句柄
    lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    if (lvgl_disp != NULL)
    {
        // API含义：设置显示设备的渲染模式为部分渲染（降低内存占用）
        lv_display_set_render_mode(lvgl_disp, LV_DISPLAY_RENDER_MODE_PARTIAL);

        // 加锁：LVGL多线程操作必须加锁，避免冲突
        if (lvgl_port_lock(1000))
        {
            // API含义：获取当前活跃的屏幕对象
            lv_obj_t *screen = lv_screen_active();
            if (screen != NULL)
            {
                // API含义：设置对象的背景颜色为黑色
                lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
                // API含义：设置对象的背景不透明度为完全覆盖
                lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
            }
            // 解锁LVGL
            lvgl_port_unlock();
        }
    }
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * 主时钟 UI 相关实现
 * ═══════════════════════════════════════════════════════════════ */
// 时钟UI尺寸宏定义
#define CLOCK_DIGIT_W 40 // 单个数字宽度
#define CLOCK_COLON_W 22 // 冒号宽度
#define CLOCK_H 60       // 数字高度
#define CLOCK_Y 25       // 数字Y轴坐标

/**
 * @brief 创建时钟单个数字/符号单元格
 * @param scr 参数含义：父屏幕对象
 * @param out 参数含义：输出参数，创建完成的标签对象句柄
 * @param x 参数含义：单元格X轴坐标
 * @param w 参数含义：单元格宽度
 * @param init_text 参数含义：初始显示文本
 */
static void make_clock_cell(lv_obj_t *scr, lv_obj_t **out,
                            int32_t x, int32_t w, const char *init_text)
{
    // API含义：在父对象上创建一个标签对象
    // API参数含义：scr 父对象句柄
    *out = lv_label_create(scr);

    // API含义：设置标签的文本字体
    lv_obj_set_style_text_font(*out, &lv_font_montserrat_48, 0);
    // API含义：设置标签的文本颜色为白色
    lv_obj_set_style_text_color(*out, lv_color_white(), 0);
    // API含义：设置文本居中对齐
    lv_obj_set_style_text_align(*out, LV_TEXT_ALIGN_CENTER, 0);
    // API含义：设置标签长文本模式为裁剪
    lv_label_set_long_mode(*out, LV_LABEL_LONG_CLIP);
    // API含义：设置对象尺寸
    lv_obj_set_size(*out, w, CLOCK_H);
    // API含义：设置对象位置
    lv_obj_set_pos(*out, x, CLOCK_Y);
    // API含义：设置标签显示文本
    lv_label_set_text(*out, init_text);
}

/**
 * @brief 创建主界面时钟UI
 * 函数含义：初始化时钟的6位数字、冒号、日期标签、GIF对象等所有UI元素
 */
static void main_clock_create(void)
{
    // 获取当前活跃屏幕
    lv_obj_t *scr = lv_screen_active();

    // 设置屏幕背景为黑色，关闭滚动条
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    // 设置屏幕默认字体为16号中文字体
    lv_obj_set_style_text_font(scr, &font_cn_16, 0);

    // 计算时钟总宽度，居中显示
    int32_t total_w = 6 * CLOCK_DIGIT_W + 2 * CLOCK_COLON_W;
    int32_t sx = (BSP_LCD_WIDTH - total_w) / 2;

    // 计算每个数字/冒号的X轴坐标
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

    // 创建6位时钟数字（时十位、时个位、分十位、分个位、秒十位、秒个位）
    make_clock_cell(scr, &s_clock_d[0], xs[0], CLOCK_DIGIT_W, "-");
    make_clock_cell(scr, &s_clock_d[1], xs[1], CLOCK_DIGIT_W, "-");
    make_clock_cell(scr, &s_clock_d[2], xs[3], CLOCK_DIGIT_W, "-");
    make_clock_cell(scr, &s_clock_d[3], xs[4], CLOCK_DIGIT_W, "-");
    make_clock_cell(scr, &s_clock_d[4], xs[6], CLOCK_DIGIT_W, "-");
    make_clock_cell(scr, &s_clock_d[5], xs[7], CLOCK_DIGIT_W, "-");

    // 创建2个冒号分隔符
    make_clock_cell(scr, &s_clock_col[0], xs[2], CLOCK_COLON_W, ":");
    make_clock_cell(scr, &s_clock_col[1], xs[5], CLOCK_COLON_W, ":");

    // 创建时区标签
    s_time_tz_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(s_time_tz_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_time_tz_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_time_tz_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(s_time_tz_lbl, BSP_LCD_WIDTH, 20);
    lv_obj_set_pos(s_time_tz_lbl, 0, BSP_LCD_HEIGHT - 52);
    lv_label_set_text(s_time_tz_lbl, "中国标准时间");

    // 创建日期标签
    s_time_date_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_time_date_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_time_date_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_time_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_time_date_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(s_time_date_lbl, BSP_LCD_WIDTH, 28);
    lv_obj_set_pos(s_time_date_lbl, 0, BSP_LCD_HEIGHT - 30);
    lv_label_set_text(s_time_date_lbl, "--月--日");

    // 创建GIF动画对象
    gif_obj = lv_gif_create(scr);
    // API含义：设置GIF的颜色格式
    lv_gif_set_color_format(gif_obj, LV_COLOR_FORMAT_RGB565);
    // 设置GIF源文件
    lv_gif_set_src(gif_obj, pick_main_gif_path());
    // API含义：将对象居中对齐到父容器
    lv_obj_center(gif_obj);
    // API含义：给对象添加隐藏标志，初始不显示
    lv_obj_add_flag(gif_obj, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "主界面时钟 UI 已创建");
}

/**
 * @brief 设置时钟数字标签的显示值
 * @param lbl 参数含义：数字标签对象句柄
 * @param val 参数含义：要显示的数字（0~9）
 */
static void set_digit(lv_obj_t *lbl, uint8_t val)
{
    // 格式化数字为2字符字符串，补前导零
    char buf[2] = {'0' + val, '\0'};
    lv_label_set_text(lbl, buf);
}

/**
 * @brief 刷新主时钟显示
 * 函数含义：从提醒系统获取当前时间，更新时钟数字和日期显示
 */
static void main_clock_refresh(void)
{
    // 时钟对象未创建，直接返回
    if (s_clock_d[0] == NULL)
        return;

    // 检查时间是否已同步
    if (reminder_is_time_synced())
    {
        uint8_t h, m, sec;
        // API含义：从提醒系统获取当前时分秒
        reminder_get_current_time(&h, &m, &sec);

        // 更新6位时钟数字
        set_digit(s_clock_d[0], h / 10);   // 时十位
        set_digit(s_clock_d[1], h % 10);   // 时个位
        set_digit(s_clock_d[2], m / 10);   // 分十位
        set_digit(s_clock_d[3], m % 10);   // 分个位
        set_digit(s_clock_d[4], sec / 10); // 秒十位
        set_digit(s_clock_d[5], sec % 10); // 秒个位

        // 获取当前时间戳，转换为本地时间结构体
        time_t now = time(NULL);
        struct tm tm_now;
        // API含义：线程安全的时间戳转本地时间结构体
        localtime_r(&now, &tm_now);

        // 星期中文名称数组
        static const char *const wday_cn[] = {
            "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};

        // 格式化日期字符串
        char buf[32];
        snprintf(buf, sizeof(buf), "%d 月 %d 日  %s",
                 tm_now.tm_mon + 1, tm_now.tm_mday,
                 wday_cn[tm_now.tm_wday & 0x7]);
        // 更新日期标签
        lv_label_set_text(s_time_date_lbl, buf);
    }
    else
    {
        // 时间未同步，显示占位符
        for (int i = 0; i < 6; i++)
            lv_label_set_text(s_clock_d[i], "-");
        lv_label_set_text(s_time_date_lbl, "等待时间同步...");
    }
}

/**
 * @brief 主时钟定时器回调函数（1秒1次）
 * @param t 参数含义：定时器对象句柄（未使用）
 */
static void main_clock_tick_cb(lv_timer_t *t)
{
    (void)t; // 消除未使用参数警告
    // 刷新时钟显示
    main_clock_refresh();

    // 如果当前在功能菜单的时间日期页，同步更新页面内容
    if (s_view == UI_VIEW_FUNCTION_MENU &&
        s_fn_page == FN_PAGE_TIME &&
        s_menu_body != NULL)
    {
        if (reminder_is_time_synced())
        {
            time_t now = time(NULL);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            static const char *const wday_cn[] = {
                "星期日", "星期一", "星期二", "星期三",
                "星期四", "星期五", "星期六"};
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "%04d年%02d月%02d日\n%s\n%02d:%02d:%02d\n\n中国标准时间",
                     tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                     wday_cn[tm_now.tm_wday & 0x7],
                     tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
            lv_label_set_text(s_menu_body, buf);
        }
    }
}

/**
 * @brief GIF自动隐藏定时器回调
 * @param t 参数含义：定时器对象句柄（未使用）
 */
static void gif_auto_hide_cb(lv_timer_t *t)
{
    (void)t;
    // 隐藏GIF对象
    if (gif_obj)
        lv_obj_add_flag(gif_obj, LV_OBJ_FLAG_HIDDEN);
    s_gif_hide_tmr = NULL;
}

/**
 * @brief 手动更新时间显示（对外接口）
 */
void ui_update_time(void)
{
    // LVGL多线程操作必须加锁，超时100ms
    if (!lvgl_port_lock(100))
        return;
    main_clock_refresh();
    lvgl_port_unlock();
}

/**
 * @brief 更新情绪显示（对外接口，预留扩展）
 * @param emotion 参数含义：情绪类型字符串
 */
void ui_update_emotion(const char *emotion) {}

/* ═══════════════════════════════════════════════════════════════
 * 情绪数据定义
 * 含义：不同触摸区域对应的情绪列表，包含名称、动画、音效描述
 * ═══════════════════════════════════════════════════════════════ */
typedef struct
{
    const char *name;  // 情绪名称
    const char *anim;  // 动画描述
    const char *audio; // 音效描述
} emo_entry_t;

// // 头部触摸对应的情绪列表
// static const emo_entry_t g_emo_head[] = {
//     {"开心", "眯眼笑+冒星星", "短促笑声"},
//     {"好奇", "歪头眨眼+问号", "嗯？+轻微按键音"},
//     {"傲娇", "挑眉+叉腰脸", "哼~+轻敲桌面声"},
//     {"怕痒", "眯眼笑+躲躲闪闪", "咯咯笑+好痒好痒"},
//     {"犯困", "打哈欠+眼皮下垂", "打哈欠+轻柔呼吸音"},
//     {"委屈", "撇嘴+泪眼", "小声啜泣+呜~"},
// };

// // 腹部触摸对应的情绪列表
// static const emo_entry_t g_emo_abdomen[] = {
//     {"舒服", "闭眼打哈欠+波浪线", "满足嗯~+舒缓呼吸"},
//     {"撒娇", "泪眼汪汪+歪头", "要抱抱~+蹭蹭摩擦"},
//     {"生气", "鼓脸+冒火", "哼！+跺脚声"},
//     {"害羞", "捂脸+脸红", "哎呀~+害羞轻笑"},
//     {"惊喜", "眼睛瞪大+闪光", "哇！+铃铛脆响"},
//     {"慵懒", "半睁眼+打哈欠", "慵懒哈欠+咿呀声"},
// };

// // 背部触摸对应的情绪列表
// static const emo_entry_t g_emo_back[] = {
//     {"治愈", "眯眼+爱心", "呼噜呼噜+轻拍声"},
//     {"傲娇", "鼻孔看人+叉腰", "切~+轻哼声"},
//     {"委屈", "撇嘴+低头", "小声抽泣+呜~"},
//     {"兴奋", "爱心眼+蹦跳", "耶~+拍手声"},
//     {"好奇", "歪头眨眼+问号", "咦？+轻微摩擦声"},
//     {"怕痒", "笑出眼泪+扭动", "咯咯大笑+别挠啦~"},
// };

// // 头+腹组合触摸对应的情绪列表
// static const emo_entry_t g_emo_head_abdomen[] = {
//     {"兴奋", "爱心眼+蹦跳", "哇呜~+铃铛串响"},
//     {"害羞蹭蹭", "脸红+蹭脸", "嘿嘿~+蹭蹭摩擦"},
//     {"舒服到打滚", "眯眼+波浪线", "呼噜~+翻身轻响"},
//     {"傲娇求摸", "挑眉+歪头", "哼快摸我~+轻敲"},
//     {"犯困", "打哈欠+眼皮下垂", "打哈欠+轻柔呼吸"},
//     {"惊喜", "眼睛瞪大+闪光", "哇！+烟花脆响"},
// };

// // 头+背组合触摸对应的情绪列表
// static const emo_entry_t g_emo_head_back[] = {
//     {"治愈", "眯眼+爱心", "呼噜呼噜+轻拍声"},
//     {"傲娇", "鼻孔看人+叉腰", "切~+轻哼声"},
//     {"委屈", "撇嘴+低头", "小声抽泣+呜~"},
//     {"兴奋", "爱心眼+蹦跳", "耶~+拍手声"},
//     {"好奇", "歪头眨眼+问号", "咦？+轻微摩擦声"},
//     {"怕痒", "笑出眼泪+扭动", "咯咯大笑+别挠啦~"},
// };

// // 腹+背组合触摸对应的情绪列表
// static const emo_entry_t g_emo_abdomen_back[] = {
//     {"慵懒瘫坐", "半睁眼+打哈欠", "慵懒哈欠+瘫坐咚声"},
//     {"惊喜抱抱", "爱心眼+张开双臂", "要抱抱~+哗啦声"},
//     {"怕痒到扭动", "笑出眼泪+扭动", "大笑+救命啊~"},
//     {"舒服", "闭眼打哈欠+波浪", "满足嗯~+舒缓呼吸"},
//     {"生气", "鼓脸+冒火", "哼！+跺脚声"},
//     {"兴奋", "爱心眼+蹦跳", "哇呜~+铃铛串响"},
// };

/**
 * @brief 从情绪组中随机选择一个情绪并显示
 * @param group 参数含义：情绪组数组
 * @param count 参数含义：情绪组的元素数量
 */
static void show_random_emotion(const emo_entry_t *group, size_t count)
{
    // 随机选择一个情绪
    const emo_entry_t *e = &group[esp_random() % count];
    // 显示情绪面板
    ui_show_emotion(e->name, e->anim, e->audio);
    ESP_LOGI("TOUCH", "[%s] 动画:%s 音效:%s", e->name, e->anim, e->audio);
}

// 情绪显示宏，简化调用
#define SHOW_EMO(group) show_random_emotion(group, sizeof(group) / sizeof(group[0]))

/* ═══════════════════════════════════════════════════════════════
 * 情绪叠加层实现
 * ═══════════════════════════════════════════════════════════════ */
/**
 * @brief 情绪面板自动隐藏定时器回调
 * @param t 参数含义：定时器对象句柄（未使用）
 */
static void hide_emotion_cb(lv_timer_t *t)
{
    (void)t;
    // 隐藏情绪面板
    if (s_emo_panel)
        lv_obj_add_flag(s_emo_panel, LV_OBJ_FLAG_HIDDEN); // api含义：添加隐藏标志
    s_emo_timer = NULL;
}

/**
 * @brief 显示情绪面板（对外接口）
 * @param name 参数含义：情绪名称
 * @param anim_desc 参数含义：动画描述
 * @param audio_desc 参数含义：音效描述
 */
void ui_show_emotion(const char *name, const char *anim_desc, const char *audio_desc)
{
    // LVGL操作加锁
    if (!lvgl_port_lock(100))
        return;

    // 获取当前屏幕
    lv_obj_t *scr = lv_screen_active();

    // 情绪面板未创建时，先初始化
    if (s_emo_panel == NULL)
    {
        // 创建面板根容器
        s_emo_panel = lv_obj_create(scr);
        lv_obj_set_size(s_emo_panel, 220, 100);
        lv_obj_align(s_emo_panel, LV_ALIGN_BOTTOM_MID, 0, -8);
        // 设置面板样式
        lv_obj_set_style_bg_color(s_emo_panel, lv_color_hex(0x111111), 0);
        lv_obj_set_style_bg_opa(s_emo_panel, LV_OPA_80, 0);
        lv_obj_set_style_border_color(s_emo_panel, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(s_emo_panel, LV_OPA_30, 0);
        lv_obj_set_style_border_width(s_emo_panel, 1, 0);
        lv_obj_set_style_radius(s_emo_panel, 12, 0);
        lv_obj_set_style_pad_all(s_emo_panel, 6, 0);
        lv_obj_clear_flag(s_emo_panel, LV_OBJ_FLAG_SCROLLABLE);

        // 创建情绪名称标签
        s_emo_name_lbl = lv_label_create(s_emo_panel);
        lv_obj_set_style_text_color(s_emo_name_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(s_emo_name_lbl, LV_ALIGN_TOP_MID, 0, 2);

        // 创建动画描述标签
        s_emo_anim_lbl = lv_label_create(s_emo_panel);
        lv_obj_set_style_text_color(s_emo_anim_lbl, lv_color_hex(0x88CCFF), 0);
        lv_label_set_long_mode(s_emo_anim_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_emo_anim_lbl, 200);
        lv_obj_align(s_emo_anim_lbl, LV_ALIGN_CENTER, 0, 6);

        // 创建音效描述标签
        s_emo_audio_lbl = lv_label_create(s_emo_panel);
        lv_obj_set_style_text_color(s_emo_audio_lbl, lv_color_hex(0xFFDD88), 0);
        lv_label_set_long_mode(s_emo_audio_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_emo_audio_lbl, 200);
        lv_obj_align(s_emo_audio_lbl, LV_ALIGN_BOTTOM_MID, 0, -2);
    }

    // 更新标签内容
    lv_label_set_text(s_emo_name_lbl, name);
    lv_label_set_text(s_emo_anim_lbl, anim_desc);
    lv_label_set_text(s_emo_audio_lbl, audio_desc);

    // 显示面板
    lv_obj_clear_flag(s_emo_panel, LV_OBJ_FLAG_HIDDEN);

    // 重置/创建自动隐藏定时器
    if (s_emo_timer != NULL)
        lv_timer_reset(s_emo_timer); // 已有定时器，重置计时
    else
        s_emo_timer = lv_timer_create(hide_emotion_cb, 3000, NULL); // 3秒后自动隐藏
    lv_timer_set_repeat_count(s_emo_timer, 1);                      // 只执行一次

    // 解锁LVGL
    lvgl_port_unlock();
}

/* ═══════════════════════════════════════════════════════════════
 * 功能菜单框架实现
 * ═══════════════════════════════════════════════════════════════ */
/**
 * @brief 功能菜单空闲超时回调
 * @param t 参数含义：定时器对象句柄（未使用）
 */
static void menu_idle_timeout_cb(lv_timer_t *t)
{
    (void)t;
    s_menu_idle_tmr = NULL;

    // 不在功能菜单界面，直接返回
    if (s_view != UI_VIEW_FUNCTION_MENU)
        return;

    // 隐藏菜单面板，返回主界面
    if (s_menu_panel)
        lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
    // 超时返回主界面时恢复GIF显示
    if (gif_obj != NULL)
        lv_obj_clear_flag(gif_obj, LV_OBJ_FLAG_HIDDEN);
    s_view = UI_VIEW_MAIN;
    ESP_LOGI(TAG, "功能菜单空闲超时，返回主界面");
}

/**
 * @brief 重置菜单空闲定时器（有操作时调用，刷新超时时间）
 */
static void menu_kick_idle_timer(void)
{
    if (s_menu_idle_tmr != NULL)
    {
        lv_timer_reset(s_menu_idle_tmr);
        return;
    }
    // 定时器不存在，创建新的定时器
    s_menu_idle_tmr = lv_timer_create(menu_idle_timeout_cb, UI_MENU_IDLE_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(s_menu_idle_tmr, 1);
}

/**
 * @brief 取消菜单空闲定时器（进入编辑界面时调用，避免超时退出）
 */
static void menu_cancel_idle_timer(void)
{
    if (s_menu_idle_tmr != NULL)
    {
        lv_timer_del(s_menu_idle_tmr);
        s_menu_idle_tmr = NULL;
    }
}

/**
 * @brief 确保菜单面板已创建（懒加载）
 */
static void ensure_menu_panel(void)
{
    // 已创建，直接返回
    if (s_menu_panel != NULL)
        return;

    lv_obj_t *scr = lv_screen_active();

    // 创建菜单根容器
    s_menu_panel = lv_obj_create(scr);
    lv_obj_set_size(s_menu_panel, BSP_LCD_WIDTH, BSP_LCD_HEIGHT);
    lv_obj_align(s_menu_panel, LV_ALIGN_CENTER, 0, 0);
    // 设置全屏黑色背景
    lv_obj_set_style_bg_color(s_menu_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_menu_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_menu_panel, 0, 0);
    lv_obj_set_style_pad_all(s_menu_panel, 8, 0);
    lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_SCROLLABLE);

    // 创建菜单标题标签
    s_menu_title = lv_label_create(s_menu_panel);
    lv_obj_set_style_text_color(s_menu_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_menu_title, &font_cn_16, 0);
    lv_obj_align(s_menu_title, LV_ALIGN_TOP_MID, 0, 4);

    // 创建菜单内容标签
    s_menu_body = lv_label_create(s_menu_panel);
    lv_obj_set_style_text_color(s_menu_body, lv_color_hex(0x88CCFF), 0);
    lv_label_set_long_mode(s_menu_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_menu_body, BSP_LCD_WIDTH - 24);
    lv_obj_align(s_menu_body, LV_ALIGN_CENTER, 0, 0);

    // 初始隐藏
    lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
}

/* ═══════════════════════════════════════════════════════════════
 * 闹钟页实现
 * ═══════════════════════════════════════════════════════════════ */
// 闹钟UI尺寸宏定义
#define ALARM_CARD_H 40  // 单个闹钟卡片高度
#define ALARM_CARD_GAP 4 // 闹钟卡片间距
#define ALARM_MAX_SHOW 4 // 单页最多显示4个闹钟

/**
 * @brief 闹钟重复模式转中文
 * @param r 参数含义：闹钟重复模式枚举
 * @return 返回值含义：对应的中文描述字符串
 */
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

/**
 * @brief 刷新下一个闹钟响铃提示
 */
static void alarm_refresh_next_lbl(void)
{
    if (!s_alarm_next_lbl)
        return;

    alarm_entry_t list[REMINDER_MAX_ALARMS];
    uint8_t count = 0;
    // API含义：获取所有闹钟列表
    reminder_alarm_get_all(list, &count);

    // 获取当前时间
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int now_min = tm_now.tm_hour * 60 + tm_now.tm_min; // 当前时间转换为分钟数

    int min_remain = INT_MAX; // 最小剩余分钟数

    // 遍历所有闹钟，找到最近的一个
    for (uint8_t i = 0; i < count; i++)
    {
        if (!list[i].enabled)
            continue; // 跳过未启用的闹钟

        int alarm_min = list[i].hour * 60 + list[i].minute;
        int remain = alarm_min - now_min;
        if (remain <= 0)
            remain += 24 * 60; // 当天已过，算第二天的时间

        if (remain < min_remain)
            min_remain = remain;
    }

    // 更新提示标签
    if (min_remain == INT_MAX)
        lv_label_set_text(s_alarm_next_lbl, "当前无启用的闹钟");
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

/**
 * @brief 创建闹钟页面UI
 */
static void alarm_page_create(void)
{
    if (s_alarm_next_lbl)
        return; // 已创建，直接返回

    // 创建下一个闹钟提示标签
    s_alarm_next_lbl = lv_label_create(s_menu_panel);
    lv_obj_set_style_text_color(s_alarm_next_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(s_alarm_next_lbl, &font_cn_16, 0);
    lv_label_set_long_mode(s_alarm_next_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_alarm_next_lbl, BSP_LCD_WIDTH - 16);
    lv_obj_align(s_alarm_next_lbl, LV_ALIGN_TOP_LEFT, 0, 28);

    // 创建闹钟列表容器
    s_alarm_cards_cont = lv_obj_create(s_menu_panel);
    lv_obj_set_size(s_alarm_cards_cont, BSP_LCD_WIDTH - 16, BSP_LCD_HEIGHT - 8 - 50);
    lv_obj_align(s_alarm_cards_cont, LV_ALIGN_TOP_LEFT, 0, 50);
    // 透明背景，无边框
    lv_obj_set_style_bg_opa(s_alarm_cards_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_alarm_cards_cont, 0, 0);
    lv_obj_set_style_pad_all(s_alarm_cards_cont, 0, 0);
    lv_obj_clear_flag(s_alarm_cards_cont, LV_OBJ_FLAG_SCROLLABLE);
}

/**
 * @brief 重建闹钟列表UI
 * 函数含义：重新加载闹钟数据，刷新列表显示
 */
static void alarm_page_rebuild(void)
{
    if (!s_alarm_cards_cont)
        return;

    // 清空容器内所有对象
    lv_obj_clean(s_alarm_cards_cont);

    alarm_entry_t list[REMINDER_MAX_ALARMS];
    uint8_t count = 0;
    reminder_alarm_get_all(list, &count);

    // 判断是否可以新建闹钟
    uint8_t new_slot = (count < REMINDER_MAX_ALARMS) ? 1 : 0;
    s_alarm_total_sel = count + new_slot; // 总可选数量

    // 边界处理
    if (s_alarm_total_sel == 0)
        s_alarm_selected = -1;
    else if (s_alarm_selected >= (int8_t)s_alarm_total_sel)
        s_alarm_selected = s_alarm_total_sel - 1;

    // 无闹钟且无法新建，显示空提示
    if (count == 0 && !new_slot)
    {
        lv_obj_t *empty = lv_label_create(s_alarm_cards_cont);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(empty, "无闹钟");
        lv_obj_center(empty);
        alarm_refresh_next_lbl();
        return;
    }

    // 计算要显示的闹钟数量
    uint8_t shown = (count > ALARM_MAX_SHOW) ? ALARM_MAX_SHOW : count;

    // 循环创建闹钟卡片
    for (uint8_t i = 0; i < shown; i++)
    {
        int32_t card_y = (int32_t)i * (ALARM_CARD_H + ALARM_CARD_GAP);
        bool selected = (i == s_alarm_selected); // 是否选中

        // 创建卡片容器
        lv_obj_t *card = lv_obj_create(s_alarm_cards_cont);
        lv_obj_set_size(card, lv_pct(100), ALARM_CARD_H);
        lv_obj_set_pos(card, 0, card_y);
        // 设置卡片样式，选中状态高亮
        lv_obj_set_style_bg_color(card, lv_color_hex(selected ? 0x2C2C2C : 0x1C1C1C), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(selected ? 0xFF9500 : 0x333333), 0);
        lv_obj_set_style_border_width(card, selected ? 2 : 1, 0);
        lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_hor(card, 8, 0);
        lv_obj_set_style_pad_ver(card, 4, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // 格式化闹钟时间
        char tbuf[8];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", list[i].hour, list[i].minute);

        // 创建时间标签
        lv_obj_t *time_lbl = lv_label_create(card);
        lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(time_lbl,
                                    list[i].enabled ? lv_color_white() : lv_color_hex(0x555555), 0);
        lv_label_set_text(time_lbl, tbuf);
        lv_obj_align(time_lbl, LV_ALIGN_LEFT_MID, 0, -8);

        // 创建重复模式标签
        lv_obj_t *rep_lbl = lv_label_create(card);
        lv_obj_set_style_text_color(rep_lbl, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(rep_lbl, &font_cn_16, 0);
        lv_label_set_text(rep_lbl, s_alarm_repeat_cn(list[i].repeat));
        lv_obj_align(rep_lbl, LV_ALIGN_LEFT_MID, 0, 11);

        // 创建启用状态标签
        lv_obj_t *sw_lbl = lv_label_create(card);
        lv_obj_set_style_text_color(sw_lbl,
                                    list[i].enabled ? lv_color_hex(0xFF9500) : lv_color_hex(0x555555), 0);
        lv_label_set_text(sw_lbl, list[i].enabled ? "开" : "关");
        lv_obj_align(sw_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    // 新建闹钟槽位
    if (new_slot)
    {
        int32_t card_y = (int32_t)shown * (ALARM_CARD_H + ALARM_CARD_GAP);
        bool selected = (shown == s_alarm_selected);

        // 创建新建卡片
        lv_obj_t *card = lv_obj_create(s_alarm_cards_cont);
        lv_obj_set_size(card, lv_pct(100), ALARM_CARD_H);
        lv_obj_set_pos(card, 0, card_y);
        lv_obj_set_style_bg_color(card, lv_color_hex(selected ? 0x2C2C2C : 0x1C1C1C), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(selected ? 0xFF9500 : 0x333333), 0);
        lv_obj_set_style_border_width(card, selected ? 2 : 1, 0);
        lv_obj_set_style_border_opa(card, LV_OPA_30, 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_hor(card, 8, 0);
        lv_obj_set_style_pad_ver(card, 4, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // 新建提示标签
        lv_obj_t *plus_lbl = lv_label_create(card);
        lv_obj_set_style_text_color(plus_lbl, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(plus_lbl, &font_cn_16, 0);
        lv_label_set_text(plus_lbl, "+ 新建闹钟");
        lv_obj_center(plus_lbl);
    }

    // 刷新下一个闹钟提示
    alarm_refresh_next_lbl();
}

/**
 * @brief 显示闹钟页面
 */
static void alarm_page_show(void)
{
    alarm_page_create();
    alarm_page_rebuild();
    // 显示相关UI对象
    lv_obj_clear_flag(s_alarm_next_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_alarm_cards_cont, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 隐藏闹钟页面
 */
static void alarm_page_hide(void)
{
    if (s_alarm_next_lbl)
        lv_obj_add_flag(s_alarm_next_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_alarm_cards_cont)
        lv_obj_add_flag(s_alarm_cards_cont, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 闹钟列表向下选择
 */
static void alarm_list_select_next(void)
{
    if (s_alarm_total_sel <= 0)
        return;

    s_alarm_selected++;
    // 循环选择
    if (s_alarm_selected >= (int8_t)s_alarm_total_sel)
        s_alarm_selected = 0;

    // LVGL操作加锁
    if (lvgl_port_lock(100))
    {
        alarm_page_rebuild();
        menu_kick_idle_timer(); // 刷新空闲超时
        lvgl_port_unlock();
    }
}

/**
 * @brief 闹钟列表向上选择
 */
static void alarm_list_select_prev(void)
{
    if (s_alarm_total_sel <= 0)
        return;

    s_alarm_selected--;
    // 循环选择
    if (s_alarm_selected < 0)
        s_alarm_selected = s_alarm_total_sel - 1;

    if (lvgl_port_lock(100))
    {
        alarm_page_rebuild();
        menu_kick_idle_timer();
        lvgl_port_unlock();
    }
}

/* ═══════════════════════════════════════════════════════════════
 * 闹钟编辑界面实现
 * ═══════════════════════════════════════════════════════════════ */
/**
 * @brief 创建闹钟编辑界面UI
 */
static void alarm_edit_create(void)
{
    if (s_edit_panel != NULL)
        return;

    lv_obj_t *scr = lv_screen_active();

    // 创建编辑界面根容器
    s_edit_panel = lv_obj_create(scr);
    lv_obj_set_size(s_edit_panel, BSP_LCD_WIDTH, BSP_LCD_HEIGHT);
    lv_obj_set_style_bg_color(s_edit_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_edit_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_edit_panel, 0, 0);
    lv_obj_set_style_pad_all(s_edit_panel, 0, 0);
    lv_obj_clear_flag(s_edit_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_edit_panel, LV_OBJ_FLAG_HIDDEN); // 初始隐藏

    // 创建标题
    lv_obj_t *title = lv_label_create(s_edit_panel);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &font_cn_16, 0);
    lv_label_set_text(title, "设置闹钟");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // 创建小时标签
    s_edit_hour_lbl = lv_label_create(s_edit_panel);
    lv_obj_set_style_text_font(s_edit_hour_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_edit_hour_lbl, lv_color_hex(0xFF9500), 0);
    lv_obj_align(s_edit_hour_lbl, LV_ALIGN_CENTER, -55, -20);

    // 创建冒号
    s_edit_colon_lbl = lv_label_create(s_edit_panel);
    lv_obj_set_style_text_font(s_edit_colon_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_edit_colon_lbl, lv_color_white(), 0);
    lv_label_set_text(s_edit_colon_lbl, ":");
    lv_obj_align(s_edit_colon_lbl, LV_ALIGN_CENTER, 0, -20);

    // 创建分钟标签
    s_edit_min_lbl = lv_label_create(s_edit_panel);
    lv_obj_set_style_text_font(s_edit_min_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_edit_min_lbl, lv_color_white(), 0);
    lv_obj_align(s_edit_min_lbl, LV_ALIGN_CENTER, 55, -20);

    // 创建重复模式标签
    s_edit_repeat_lbl = lv_label_create(s_edit_panel);
    lv_obj_set_style_text_font(s_edit_repeat_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_edit_repeat_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_edit_repeat_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_edit_repeat_lbl, LV_ALIGN_CENTER, 0, 30);

    // 创建操作提示标签
    s_edit_hint_lbl = lv_label_create(s_edit_panel);
    lv_obj_set_style_text_font(s_edit_hint_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_edit_hint_lbl, lv_color_hex(0x666666), 0);
    lv_label_set_long_mode(s_edit_hint_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_edit_hint_lbl, BSP_LCD_WIDTH - 16);
    lv_obj_set_style_text_align(s_edit_hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_edit_hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -12);
}

/**
 * @brief 刷新闹钟编辑界面显示
 */
static void alarm_edit_render(void)
{
    if (!s_edit_panel)
        return;

    // 更新小时和分钟显示
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d", s_edit.hour);
    lv_label_set_text(s_edit_hour_lbl, buf);
    snprintf(buf, sizeof(buf), "%02d", s_edit.minute);
    lv_label_set_text(s_edit_min_lbl, buf);

    // 高亮当前编辑项
    lv_color_t active = lv_color_hex(0xFF9500);
    lv_color_t inactive = lv_color_white();
    lv_obj_set_style_text_color(s_edit_hour_lbl,
                                (s_edit.state == ALARM_EDIT_HOUR) ? active : inactive, 0);
    lv_obj_set_style_text_color(s_edit_min_lbl,
                                (s_edit.state == ALARM_EDIT_MINUTE) ? active : inactive, 0);
    lv_obj_set_style_text_color(s_edit_repeat_lbl,
                                (s_edit.state == ALARM_EDIT_REPEAT) ? active : inactive, 0);

    // 更新重复模式显示
    char rbuf[32];
    snprintf(rbuf, sizeof(rbuf), "重复: %s", s_alarm_repeat_cn(s_edit.repeat));
    lv_label_set_text(s_edit_repeat_lbl, rbuf);

    // 根据编辑步骤更新操作提示
    switch (s_edit.state)
    {
    case ALARM_EDIT_HOUR:
        lv_label_set_text(s_edit_hint_lbl, "后页:+1  前页:-1\n长按后页:确认  长按前页:取消");
        break;
    case ALARM_EDIT_MINUTE:
        lv_label_set_text(s_edit_hint_lbl, "后页:+1  前页:-1\n长按后页:确认  长按前页:返回");
        break;
    case ALARM_EDIT_REPEAT:
        lv_label_set_text(s_edit_hint_lbl, "后页:下一个  前页:上一个\n长按后页:保存  长按前页:返回");
        break;
    }
}

/**
 * @brief 进入闹钟编辑界面
 * @param alarm_id 参数含义：要编辑的闹钟ID，-1表示新建闹钟
 */
static void alarm_edit_enter(int8_t alarm_id)
{
    alarm_edit_create();

    // 编辑已有闹钟，加载原有数据
    if (alarm_id >= 0)
    {
        alarm_entry_t list[REMINDER_MAX_ALARMS];
        uint8_t count = 0;
        reminder_alarm_get_all(list, &count);
        if (alarm_id < count)
        {
            s_edit.hour = list[alarm_id].hour;
            s_edit.minute = list[alarm_id].minute;
            s_edit.repeat = list[alarm_id].repeat;
        }
    }
    // 新建闹钟，设置默认值
    else
    {
        s_edit.hour = 8;
        s_edit.minute = 0;
        s_edit.repeat = ALARM_REPEAT_ONCE;
    }

    s_edit.alarm_id = alarm_id;
    s_edit.state = ALARM_EDIT_HOUR; // 初始编辑小时

    if (lvgl_port_lock(100))
    {
        alarm_edit_render();
        lv_obj_clear_flag(s_edit_panel, LV_OBJ_FLAG_HIDDEN); // 显示编辑界面
        if (s_menu_panel)
            lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN); // 隐藏功能菜单
        lvgl_port_unlock();
    }

    s_view = UI_VIEW_ALARM_EDIT; // 更新视图状态
    menu_cancel_idle_timer();    // 取消空闲超时，避免编辑时退出
    ESP_LOGI(TAG, "进入闹钟编辑 (id=%d)", alarm_id);
}

/**
 * @brief 退出闹钟编辑界面
 * @param save 参数含义：true=保存修改，false=取消修改
 */
static void alarm_edit_exit(bool save)
{
    // 保存修改
    if (save)
    {
        alarm_entry_t entry = {
            .hour = s_edit.hour,
            .minute = s_edit.minute,
            .repeat = s_edit.repeat,
            .enabled = true,
        };
        memset(entry.message, 0, sizeof(entry.message));

        // 更新已有闹钟
        if (s_edit.alarm_id >= 0)
            reminder_alarm_update(s_edit.alarm_id, &entry);
        // 新建闹钟
        else
        {
            int new_id = reminder_alarm_add(&entry);
            if (new_id >= 0)
                s_alarm_selected = new_id;
        }
        ESP_LOGI(TAG, "闹钟已保存: %02d:%02d", s_edit.hour, s_edit.minute);
    }
    else
    {
        ESP_LOGI(TAG, "闹钟编辑已取消");
    }

    if (lvgl_port_lock(100))
    {
        lv_obj_add_flag(s_edit_panel, LV_OBJ_FLAG_HIDDEN); // 隐藏编辑界面
        if (s_menu_panel)
            lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN); // 显示功能菜单
        alarm_page_rebuild();                                    // 刷新闹钟列表
        lvgl_port_unlock();
    }

    s_view = UI_VIEW_FUNCTION_MENU; // 恢复视图状态
    menu_kick_idle_timer();         // 重启空闲超时
}

/**
 * @brief 闹钟编辑值增加（后页键）
 */
static void alarm_edit_value_next(void)
{
    switch (s_edit.state)
    {
    case ALARM_EDIT_HOUR:
        s_edit.hour = (s_edit.hour + 1) % 24; // 小时0~23循环
        break;
    case ALARM_EDIT_MINUTE:
        s_edit.minute = (s_edit.minute + 1) % 60; // 分钟0~59循环
        break;
    case ALARM_EDIT_REPEAT:
    {
        // 找到当前重复模式的索引
        uint8_t idx = 0;
        for (uint8_t i = 0; i < REPEAT_MODE_COUNT; i++)
            if (s_repeat_modes[i] == s_edit.repeat)
            {
                idx = i;
                break;
            }
        s_edit.repeat = s_repeat_modes[(idx + 1) % REPEAT_MODE_COUNT]; // 循环切换
        break;
    }
    }

    if (lvgl_port_lock(100))
    {
        alarm_edit_render();
        lvgl_port_unlock();
    }
}

/**
 * @brief 闹钟编辑值减少（前页键）
 */
static void alarm_edit_value_prev(void)
{
    switch (s_edit.state)
    {
    case ALARM_EDIT_HOUR:
        s_edit.hour = (s_edit.hour + 23) % 24; // 减1，循环
        break;
    case ALARM_EDIT_MINUTE:
        s_edit.minute = (s_edit.minute + 59) % 60; // 减1，循环
        break;
    case ALARM_EDIT_REPEAT:
    {
        uint8_t idx = 0;
        for (uint8_t i = 0; i < REPEAT_MODE_COUNT; i++)
            if (s_repeat_modes[i] == s_edit.repeat)
            {
                idx = i;
                break;
            }
        s_edit.repeat = s_repeat_modes[(idx + REPEAT_MODE_COUNT - 1) % REPEAT_MODE_COUNT];
        break;
    }
    }

    if (lvgl_port_lock(100))
    {
        alarm_edit_render();
        lvgl_port_unlock();
    }
}

/**
 * @brief 闹钟编辑下一步（长按后页键）
 */
static void alarm_edit_advance(void)
{
    switch (s_edit.state)
    {
    case ALARM_EDIT_HOUR:
        s_edit.state = ALARM_EDIT_MINUTE; // 小时→分钟
        break;
    case ALARM_EDIT_MINUTE:
        s_edit.state = ALARM_EDIT_REPEAT; // 分钟→重复模式
        break;
    case ALARM_EDIT_REPEAT:
        alarm_edit_exit(true); // 重复模式→保存退出
        return;
    }

    if (lvgl_port_lock(100))
    {
        alarm_edit_render();
        lvgl_port_unlock();
    }
}

/**
 * @brief 闹钟编辑返回/取消（长按前页键）
 */
static void alarm_edit_back_or_cancel(void)
{
    switch (s_edit.state)
    {
    case ALARM_EDIT_HOUR:
        alarm_edit_exit(false); // 小时编辑→取消退出
        return;
    case ALARM_EDIT_MINUTE:
        s_edit.state = ALARM_EDIT_HOUR; // 分钟→小时
        break;
    case ALARM_EDIT_REPEAT:
        s_edit.state = ALARM_EDIT_MINUTE; // 重复模式→分钟
        break;
    }

    if (lvgl_port_lock(100))
    {
        alarm_edit_render();
        lvgl_port_unlock();
    }
}

/* ═══════════════════════════════════════════════════════════════
 * 倒计时页面实现
 * ═══════════════════════════════════════════════════════════════ */
/**
 * @brief 创建倒计时页面UI
 */
static void countdown_page_create(void)
{
    if (s_cd_time_lbl != NULL)
        return;

    // 创建倒计时时间显示标签
    s_cd_time_lbl = lv_label_create(s_menu_panel);
    lv_obj_set_style_text_font(s_cd_time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_cd_time_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_cd_time_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_cd_time_lbl, LV_ALIGN_CENTER, 0, -20);

    // 创建倒计时状态标签
    s_cd_state_lbl = lv_label_create(s_menu_panel);
    lv_obj_set_style_text_font(s_cd_state_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_cd_state_lbl, lv_color_hex(0x88CCFF), 0);
    lv_obj_set_style_text_align(s_cd_state_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_cd_state_lbl, LV_ALIGN_CENTER, 0, 30);

    // 创建操作提示标签
    s_cd_hint_lbl = lv_label_create(s_menu_panel);
    lv_obj_set_style_text_font(s_cd_hint_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_cd_hint_lbl, lv_color_hex(0x666666), 0);
    lv_label_set_long_mode(s_cd_hint_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_cd_hint_lbl, BSP_LCD_WIDTH - 16);
    lv_obj_set_style_text_align(s_cd_hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_cd_hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
}

/**
 * @brief 刷新倒计时页面显示
 */
static void countdown_page_render(void)
{
    if (!s_cd_time_lbl)
        return;

    char buf[16];
    switch (s_cd.state)
    {
    case CD_STATE_SET:
        // 设置状态：显示设置的分钟数
        snprintf(buf, sizeof(buf), "%02d:00", s_cd.minutes);
        lv_label_set_text(s_cd_time_lbl, buf);
        lv_obj_set_style_text_color(s_cd_time_lbl, lv_color_white(), 0);
        lv_label_set_text(s_cd_state_lbl, "设置时长");
        lv_label_set_text(s_cd_hint_lbl, "后页:+1分  前页:-1分\n长按后页:开始");
        break;

    case CD_STATE_RUNNING:
    {
        // 运行状态：显示剩余时间
        uint32_t remain = 0;
        reminder_timer_get_remain(s_cd.timer_id, &remain);
        snprintf(buf, sizeof(buf), "%02lu:%02lu",
                 (unsigned long)(remain / 60), (unsigned long)(remain % 60));
        lv_label_set_text(s_cd_time_lbl, buf);
        lv_obj_set_style_text_color(s_cd_time_lbl, lv_color_hex(0xFF9500), 0);
        lv_label_set_text(s_cd_state_lbl, "倒计时中...");
        lv_label_set_text(s_cd_hint_lbl, "长按前页:取消");
        break;
    }

    case CD_STATE_EXPIRED:
        // 到期状态：显示00:00
        lv_label_set_text(s_cd_time_lbl, "00:00");
        lv_obj_set_style_text_color(s_cd_time_lbl, lv_color_hex(0xFF3333), 0);
        lv_label_set_text(s_cd_state_lbl, "倒计时结束!");
        lv_label_set_text(s_cd_hint_lbl, "长按前页:返回设置");
        break;
    }
}

/**
 * @brief 显示倒计时页面
 */
static void countdown_page_show(void)
{
    countdown_page_create();

    // 倒计时正在运行，检查状态
    if (s_cd.state == CD_STATE_RUNNING)
    {
        uint32_t remain = 0;
        // 检查倒计时是否已结束
        if (reminder_timer_get_remain(s_cd.timer_id, &remain) != ESP_OK || remain == 0)
        {
            s_cd.state = CD_STATE_EXPIRED;
            if (s_cd.tick_tmr != NULL)
            {
                lv_timer_pause(s_cd.tick_tmr);
                lv_timer_del(s_cd.tick_tmr);
                s_cd.tick_tmr = NULL;
            }
        }
        // 倒计时正常运行，创建刷新定时器
        else if (s_cd.tick_tmr == NULL)
        {
            s_cd.tick_tmr = lv_timer_create(countdown_tick_cb, 1000, NULL);
        }
    }
    // 倒计时已结束，清理定时器
    else if (s_cd.state == CD_STATE_EXPIRED)
    {
        if (s_cd.tick_tmr != NULL)
        {
            lv_timer_pause(s_cd.tick_tmr);
            lv_timer_del(s_cd.tick_tmr);
            s_cd.tick_tmr = NULL;
        }
    }

    countdown_page_render();
    // 显示相关UI对象
    lv_obj_clear_flag(s_cd_time_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_cd_hint_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_cd_state_lbl, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 隐藏倒计时页面（不停止后台倒计时）
 */
static void countdown_page_hide(void)
{
    if (s_cd_time_lbl)
        lv_obj_add_flag(s_cd_time_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_cd_hint_lbl)
        lv_obj_add_flag(s_cd_hint_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_cd_state_lbl)
        lv_obj_add_flag(s_cd_state_lbl, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 倒计时刷新定时器回调（1秒1次）
 * @param t 参数含义：定时器对象句柄（未使用）
 */
static void countdown_tick_cb(lv_timer_t *t)
{
    (void)t;

    // 非运行状态，直接返回
    if (s_cd.state != CD_STATE_RUNNING)
        return;

    uint32_t remain = 0;
    // 获取剩余时间，失败或剩余0则到期
    if (reminder_timer_get_remain(s_cd.timer_id, &remain) != ESP_OK || remain == 0)
    {
        s_cd.state = CD_STATE_EXPIRED;
        countdown_page_render();
        bsp_motor_pulse(); // 震动提醒
        // 停止定时器
        if (s_cd.tick_tmr != NULL)
            lv_timer_set_repeat_count(s_cd.tick_tmr, 0);
        s_cd.tick_tmr = NULL;
        return;
    }

    // 更新显示
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu",
             (unsigned long)(remain / 60), (unsigned long)(remain % 60));
    if (s_cd_time_lbl)
        lv_label_set_text(s_cd_time_lbl, buf);
}

/**
 * @brief 启动倒计时
 */
static void countdown_start(void)
{
    // 调用底层提醒系统启动倒计时
    s_cd.timer_id = reminder_timer_start(s_cd.minutes * 60, "倒计时结束");
    if (s_cd.timer_id < 0)
    {
        ESP_LOGE(TAG, "倒计时启动失败");
        return;
    }

    s_cd.state = CD_STATE_RUNNING;

    if (lvgl_port_lock(100))
    {
        // 创建1秒刷新定时器
        if (s_cd.tick_tmr == NULL)
            s_cd.tick_tmr = lv_timer_create(countdown_tick_cb, 1000, NULL);
        countdown_page_render();
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "倒计时启动: %d 分钟", s_cd.minutes);
}

/**
 * @brief 取消倒计时
 */
static void countdown_cancel(void)
{
    // 取消底层倒计时
    if (s_cd.timer_id >= 0)
    {
        reminder_timer_cancel(s_cd.timer_id);
        s_cd.timer_id = -1;
    }

    s_cd.state = CD_STATE_SET;

    if (lvgl_port_lock(100))
    {
        // 清理定时器
        if (s_cd.tick_tmr != NULL)
        {
            lv_timer_pause(s_cd.tick_tmr);
            lv_timer_del(s_cd.tick_tmr);
            s_cd.tick_tmr = NULL;
        }
        countdown_page_render();
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "倒计时已取消");
}

/* ═══════════════════════════════════════════════════════════════
 * 功能页面路由实现
 * ═══════════════════════════════════════════════════════════════ */
/**
 * @brief 渲染指定的功能页面
 * @param page 参数含义：要渲染的功能页面枚举
 */
static void render_fn_page(fn_page_t page)
{
    if (s_menu_title == NULL || s_menu_body == NULL)
        return;

    // 先隐藏所有页面的专属UI
    alarm_page_hide();
    countdown_page_hide();

    // 设置页面标题
    lv_label_set_text(s_menu_title, s_fn_page_titles[page]);

    // 根据页面类型渲染内容
    switch (page)
    {
    case FN_PAGE_TIME:
    {
        // 时间日期页
        if (reminder_is_time_synced())
        {
            time_t now = time(NULL);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            static const char *const wday_cn[] = {
                "星期日", "星期一", "星期二", "星期三",
                "星期四", "星期五", "星期六"};
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "%04d年%02d月%02d日\n%s\n%02d:%02d:%02d\n\n中国标准时间",
                     tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                     wday_cn[tm_now.tm_wday & 0x7],
                     tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
            lv_label_set_text(s_menu_body, buf);
        }
        else
            lv_label_set_text(s_menu_body, "等待时间同步...");
        lv_obj_clear_flag(s_menu_body, LV_OBJ_FLAG_HIDDEN);
        break;
    }

    case FN_PAGE_ALARM:
        // 闹钟页：隐藏通用内容，显示闹钟专属UI
        lv_obj_add_flag(s_menu_body, LV_OBJ_FLAG_HIDDEN);
        s_alarm_selected = 0;
        alarm_page_show();
        break;

    case FN_PAGE_COUNTDOWN:
        // 倒计时页：隐藏通用内容，显示倒计时专属UI
        lv_obj_add_flag(s_menu_body, LV_OBJ_FLAG_HIDDEN);
        countdown_page_show();
        break;

    case FN_PAGE_WEATHER:
        // 天气页：预留接口
        lv_label_set_text(s_menu_body, "(天气数据待接入)");
        lv_obj_clear_flag(s_menu_body, LV_OBJ_FLAG_HIDDEN);
        break;

    default:
        lv_label_set_text(s_menu_body, "");
        lv_obj_clear_flag(s_menu_body, LV_OBJ_FLAG_HIDDEN);
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * 功能菜单对外接口实现
 * ═══════════════════════════════════════════════════════════════ */
/**
 * @brief 进入功能菜单界面
 */
void ui_function_menu_enter(void)
{
    if (!lvgl_port_lock(100))
        return;

    ensure_menu_panel();

    // 不在功能菜单时，初始化页面
    if (s_view != UI_VIEW_FUNCTION_MENU)
    {
        s_view = UI_VIEW_FUNCTION_MENU;
        s_fn_page = FN_PAGE_TIME; // 默认显示时间页
        ESP_LOGI(TAG, "进入功能菜单");
    }

    // 进入菜单时隐藏主界面GIF
    if (gif_obj != NULL)
        lv_obj_add_flag(gif_obj, LV_OBJ_FLAG_HIDDEN);

    // 渲染当前页面
    render_fn_page(s_fn_page);
    lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN); // 显示菜单
    menu_kick_idle_timer();                              // 启动空闲超时

    lvgl_port_unlock();
}

/**
 * @brief 退出功能菜单，返回主界面
 */
void ui_function_menu_exit(void)
{
    if (!lvgl_port_lock(100))
        return;

    if (s_view == UI_VIEW_FUNCTION_MENU)
    {
        if (s_menu_panel)
            lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN); // 隐藏菜单
        // 返回主界面时恢复GIF显示
        if (gif_obj != NULL)
            lv_obj_clear_flag(gif_obj, LV_OBJ_FLAG_HIDDEN);
        s_view = UI_VIEW_MAIN; // 恢复主界面状态
        ESP_LOGI(TAG, "退出功能菜单，返回主界面");
    }

    menu_cancel_idle_timer(); // 取消空闲超时
    lvgl_port_unlock();
}

/**
 * @brief 功能菜单向后翻页
 */
void ui_page_next(void)
{
    if (!lvgl_port_lock(100))
        return;

    if (s_view == UI_VIEW_FUNCTION_MENU)
    {
        s_fn_page = (fn_page_t)((s_fn_page + 1) % FN_PAGE_COUNT); // 循环翻页
        render_fn_page(s_fn_page);
        menu_kick_idle_timer();
        ESP_LOGI(TAG, "菜单 → 下一页: %s", s_fn_page_titles[s_fn_page]);
    }

    lvgl_port_unlock();
}

/**
 * @brief 功能菜单向前翻页
 */
void ui_page_prev(void)
{
    if (!lvgl_port_lock(100))
        return;

    if (s_view == UI_VIEW_FUNCTION_MENU)
    {
        s_fn_page = (fn_page_t)((s_fn_page + FN_PAGE_COUNT - 1) % FN_PAGE_COUNT); // 循环翻页
        render_fn_page(s_fn_page);
        menu_kick_idle_timer();
        ESP_LOGI(TAG, "菜单 → 上一页: %s", s_fn_page_titles[s_fn_page]);
    }

    lvgl_port_unlock();
}

/* ═══════════════════════════════════════════════════════════════
 * 动画播放实现
 * ═══════════════════════════════════════════════════════════════ */
/**
 * @brief 动画映射表结构体
 */
typedef struct
{
    const char *anim_id;     // 动画ID
    void (*play_func)(void); // 动画播放函数
    const char *audio_file;  // 对应音频文件路径
} animation_map_t;

// 动画映射表
static const animation_map_t s_animation_map[] = {
    {"anim_happy_stars", gif_switch_source, "S:/laugh_short.mp3"},
};

/**
 * @brief 播放指定ID的动画
 * @param anim_id 参数含义：动画唯一ID
 */
void ui_play_animation(const char *anim_id)
{
    if (anim_id == NULL)
        return;

    // 计算映射表长度
    static const size_t MAP_LEN = sizeof(s_animation_map) / sizeof(s_animation_map[0]);
    size_t idx;

    // 匹配动画ID
    for (idx = 0; idx < MAP_LEN; idx++)
        if (strcmp(anim_id, s_animation_map[idx].anim_id) == 0)
            break;

    // 未找到匹配的动画
    if (idx == MAP_LEN)
    {
        ESP_LOGW(TAG, "未知动画 ID: %s", anim_id);
        return;
    }

    const animation_map_t *entry = &s_animation_map[idx];

    if (lvgl_port_lock(100))
    {
        // 切换GIF动画源（主界面GIF常驻，不设自动隐藏）
        if (gif_obj != NULL)
        {
            lv_obj_clear_flag(gif_obj, LV_OBJ_FLAG_HIDDEN);
            entry->play_func(); // 切换GIF源
        }
        lvgl_port_unlock();
    }

    // 打印音频信息，预留播放接口
    if (entry->audio_file != NULL)
        ESP_LOGI(TAG, "音频: %s", entry->audio_file);
    ESP_LOGI(TAG, "动画触发: %s", anim_id);
}

/* ═══════════════════════════════════════════════════════════════
 * UI系统初始化对外接口
 * ═══════════════════════════════════════════════════════════════ */
/**
 * @brief UI系统总初始化入口
 */
void ui_init(void)
{
    init_spiffs();   // 初始化SPIFFS文件系统
    app_lvgl_init(); // 初始化LVGL图形库

    // 创建主界面UI
    if (lvgl_port_lock(1000))
    {
        main_clock_create(); // 创建主时钟界面（包含GIF对象）
        alarm_edit_create(); // 创建闹钟编辑界面

        // 开机显示默认GIF图（常驻主界面，不设自动隐藏）
        if (gif_obj != NULL)
            lv_obj_clear_flag(gif_obj, LV_OBJ_FLAG_HIDDEN);

        lvgl_port_unlock();
    }

    // 创建主时钟1秒刷新定时器
    if (lvgl_port_lock(100))
    {
        s_main_tick_tmr = lv_timer_create(main_clock_tick_cb, 1000, NULL); // 创建定时器
        main_clock_refresh();                                              // 刷新时钟
        lvgl_port_unlock();                                                // 解锁
    }
}

/* ═══════════════════════════════════════════════════════════════
 * 对外接口实现
 * ═══════════════════════════════════════════════════════════════ */
/**
 * @brief 获取当前UI视图状态
 * @return 返回值含义：当前UI视图枚举
 */
ui_view_t ui_get_current_view(void)
{
    return s_view;
}

/**
 * @brief 触摸事件核心分发函数
 * @param event 参数含义：触摸事件类型
 */
void ui_dispatch_touch_event(touch_event_t event)
{
    if (event == TOUCH_EVENT_NONE)
        return;

    /* ── 最高优先级：闹钟响铃中，任意触摸关闭闹钟 ── */
    if (reminder_get_state() == REMINDER_STATE_RINGING)
    {
        reminder_alarm_dismiss(); // 关闭闹钟
        if (lvgl_port_lock(100))
        {
            if (gif_obj)
                lv_obj_add_flag(gif_obj, LV_OBJ_FLAG_HIDDEN);
            lvgl_port_unlock();
        }
        ESP_LOGI(TAG, "触摸关闭闹钟");
        return;
    }

    // 根据当前视图状态分发事件
    switch (s_view)
    {
    /* ─── 主界面：身体触摸触发情绪，耳朵长按进菜单 ─── */
    case UI_VIEW_MAIN:
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
        case TOUCH_EVENT_COMBO_HEAD_ABDOMEN:
            ui_interaction_play(EMO_SHY_RUB);
            break;
        case TOUCH_EVENT_COMBO_HEAD_BACK:
            ui_interaction_play(EMO_EXCITED);
            break;
        case TOUCH_EVENT_COMBO_ABDOMEN_BACK:
            ui_interaction_play(EMO_COMFORTABLE_ROLL);
            break;
        case TOUCH_EVENT_LONG_PREV_PAGE:
        case TOUCH_EVENT_LONG_NEXT_PAGE:
            ui_function_menu_enter(); // 长按耳朵键进入功能菜单
            break;
        default:
            break;
        }
        break;

    /* ─── 功能菜单：耳朵键导航，身体触摸无操作 ─── */
    case UI_VIEW_FUNCTION_MENU:
        switch (event)
        {
        case TOUCH_EVENT_SHORT_PREV_PAGE:
            if (s_fn_page == FN_PAGE_ALARM)
                alarm_list_select_prev();
            else if (s_fn_page == FN_PAGE_COUNTDOWN)
            {
                if (s_cd.state == CD_STATE_SET)
                {
                    s_cd.minutes = (s_cd.minutes > 1) ? s_cd.minutes - 1 : 1;
                    if (lvgl_port_lock(100))
                    {
                        countdown_page_render();
                        lvgl_port_unlock();
                    }
                }
            }
            else
                ui_page_prev();
            break;

        case TOUCH_EVENT_SHORT_NEXT_PAGE:
            // 闹钟页：下选闹钟；倒计时页：加分钟；其他页：下翻页
            if (s_fn_page == FN_PAGE_ALARM)
                alarm_list_select_next();
            else if (s_fn_page == FN_PAGE_COUNTDOWN)
            {
                if (s_cd.state == CD_STATE_SET)
                {
                    s_cd.minutes = (s_cd.minutes < 60) ? s_cd.minutes + 1 : 60;
                    if (lvgl_port_lock(100))
                    {
                        countdown_page_render();
                        lvgl_port_unlock();
                    }
                }
            }
            else
                ui_page_next();
            break;
        case TOUCH_EVENT_LONG_NEXT_PAGE:
            // 闹钟页：进入编辑；倒计时页：启动；其他页：退出菜单
            if (s_fn_page == FN_PAGE_ALARM)
            {
                if (s_alarm_selected >= 0 && s_alarm_selected < s_alarm_total_sel)
                {
                    alarm_entry_t list[REMINDER_MAX_ALARMS];
                    uint8_t count = 0;
                    reminder_alarm_get_all(list, &count);
                    if (s_alarm_selected < count)
                        alarm_edit_enter(s_alarm_selected); // 编辑已有闹钟
                    else
                        alarm_edit_enter(-1); // 新建闹钟
                }
                else
                    alarm_edit_enter(-1);
            }
            else if (s_fn_page == FN_PAGE_COUNTDOWN)
            {
                if (s_cd.state == CD_STATE_SET)
                    countdown_start(); // 启动倒计时
            }
            else
                ui_function_menu_exit(); // 退出菜单
            break;
        case TOUCH_EVENT_LONG_PREV_PAGE:
            // 倒计时页：取消；其他页：退出菜单
            if (s_fn_page == FN_PAGE_COUNTDOWN)
            {
                if (s_cd.state == CD_STATE_RUNNING)
                    countdown_cancel(); // 取消倒计时
                else if (s_cd.state == CD_STATE_EXPIRED)
                {
                    s_cd.state = CD_STATE_SET;
                    if (lvgl_port_lock(100))
                    {
                        countdown_page_render();
                        lvgl_port_unlock();
                    }
                }
                else
                    ui_function_menu_exit();
            }
            else
                ui_function_menu_exit();
            break;
        default:
            break;
        }
        break;

    /* ─── 闹钟编辑模式：耳朵键操作编辑，身体触摸无操作 ─── */
    case UI_VIEW_ALARM_EDIT:
        switch (event)
        {
        case TOUCH_EVENT_SHORT_NEXT_PAGE:
            alarm_edit_value_next(); // 值加1
            break;
        case TOUCH_EVENT_SHORT_PREV_PAGE:
            alarm_edit_value_prev(); // 值减1
            break;
        case TOUCH_EVENT_LONG_NEXT_PAGE:
            alarm_edit_advance(); // 下一步/保存
            break;
        case TOUCH_EVENT_LONG_PREV_PAGE:
            alarm_edit_back_or_cancel(); // 上一步/取消
            break;
        default:
            break;
        }
        break;
    }
}

/**
 * @brief 更新WiFi信号显示（预留接口）
 * @param rssi 参数含义：WiFi信号强度
 */
void ui_update_wifi(int rssi) {}

/**
 * @brief 更新电池电量显示（预留接口）
 * @param soc 参数含义：电池电量百分比
 */
void ui_update_battery(int soc) {}