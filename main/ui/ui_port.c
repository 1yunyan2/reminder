#include "ui_port.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "bsp/bsp_config.h"
#include "bsp/bsp_board.h"
#include "gif_eye.h"
#include "esp_spiffs.h"
#include "esp_log.h"

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

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE("SPIFFS", "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE("SPIFFS", "Failed to find SPIFFS partition");
        }
        else
        {
            ESP_LOGE("SPIFFS", "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
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
// 1. 定义 LVGL 9 标准的图像描述符 (可以放在 eye_gif_show 函数外面)
const lv_image_dsc_t gif_eye_dsc = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RAW, // 关键：告诉 LVGL 这是未解码的 RAW 数据（交由内部 GIF 解码器处理）
    .header.flags = 0,
    .header.w = 320, // 虽然是 RAW，但填写真实宽高有助于布局
    .header.h = 240,
    .header.stride = 0,
    .data_size = 493917, // 使用你头文件里的真实大小
    .data = gif_eye_gif, // 指向你的数据数组
};

void eye_gif_show(void)
{
    lv_obj_t *scr = lv_screen_active();
    // 1. 设置对比底色，关闭滚动条
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN); // 设置背景为黑色，突出显示 GIF
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);                // 关闭滚动条，保持界面干净
    if (gif_obj != NULL)
    {
        lv_obj_del(gif_obj);
    }
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "GIF Decoder OK!");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);
    // ✅ 2. 核心修复：换回 lv_gif_create 激活专用的动画渲染器！
    gif_obj = lv_gif_create(scr);
    // 继续使用描述符，这能完美避开裸数组的 "G" 字符识别 Bug
    lv_gif_set_src(gif_obj, &gif_eye_dsc);
    lv_obj_center(gif_obj);
    // ✅ 3. 核心修复：强制 LVGL 立即刷新排版，扒掉 0x0 的伪装
    lv_obj_update_layout(gif_obj);

    // 获取并打印真实信息
    int32_t w = lv_obj_get_width(gif_obj);
    int32_t h = lv_obj_get_height(gif_obj);
    int32_t x = lv_obj_get_x(gif_obj);
    int32_t y = lv_obj_get_y(gif_obj);

    ESP_LOGI(TAG, "--- GIF Object Real Info ---");
    ESP_LOGI(TAG, "  Width:  %" PRId32, w);
    ESP_LOGI(TAG, "  Height: %" PRId32, h);
    ESP_LOGI(TAG, "  X:      %" PRId32, x);
    ESP_LOGI(TAG, "  Y:      %" PRId32, y);
    ESP_LOGI("UI", "GIF 动画启动，当前 PSRAM 剩余: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void eye_gif_show2(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    if (gif_obj != NULL)
    {
        lv_obj_del(gif_obj);
    }

    // 创建 GIF 对象
    gif_obj = lv_gif_create(scr);

    // ✅ 核心改变：不再传入描述符或数组，直接传入文件路径！
    // 这里的 S: 对应 lv_conf.h 里的盘符，eye.gif 是你放在 data 文件夹里的文件名
    lv_gif_set_src(gif_obj, "S:eye.gif");

    lv_obj_center(gif_obj);

    ESP_LOGI("UI", "GIF 从外部 Flash 启动，当前 PSRAM 剩余: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
// 1. 声明你的图片数据数组（确保这个名字和你在 .c 文件里定义的一致）
extern const uint8_t gImage_picture[];

// 2. 包装成 LVGL 图像描述符
const lv_image_dsc_t my_raw_image = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RGB565, // 关键：设置为 RGB565
    .header.w = 240,                     // 图片宽度
    .header.h = 311,                     // 图片高度
    .header.stride = 240 * 2,            // 每一行的字节数 (240像素 * 2字节)
    .data_size = 149280,                 // 数组总大小
    .data = gImage_picture,              // 指向数组
};
void show_my_picture(void)
{
    // 创建一个图像对象
    lv_obj_t *img = lv_image_create(lv_screen_active());

    // 设置图片源
    lv_image_set_src(img, &my_raw_image);

    // 居中显示
    lv_obj_center(img);

    // (可选) 如果背景还没设为黑色，可以顺便设一下
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
}
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
        .buffer_size = (BSP_LCD_WIDTH * BSP_LCD_HEIGHT) / 2, // 缓冲区大小（1/2 屏）
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

    lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    if (lvgl_disp != NULL)
    {
        // ──────────────────────────────────────────────────────
        // ✅ 修改点 3：强制关闭差分刷新，开启全局/全屏刷新 (核心代码)
        // ──────────────────────────────────────────────────────
        lv_display_set_render_mode(lvgl_disp, LV_DISPLAY_RENDER_MODE_FULL);
        // 关键：现在才可以调用 lv_screen_active()
        lv_obj_t *screen = lv_screen_active();
        if (screen != NULL)
        {
            lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        }
    }
    return ESP_OK;
}

void test_red_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFF0000), LV_PART_MAIN); // 设置背景为红色，测试显示效果

    lv_obj_t *label = lv_label_create(scr);                                   // 创建一个标签对象
    lv_label_set_text(label, "GIF Test Ready");                               // 设置标签文本
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN); // 设置标签文本颜色为白色
    lv_obj_center(label);                                                     // 将标签居中显示
}

void ui_init(void)
{
    init_spiffs();
    app_lvgl_init();

    if (lvgl_port_lock(1000))
    {
        // ✅ 先测试 GIF，注释掉红屏
        // eye_gif_show();
        eye_gif_show2();
        // show_my_picture();
        // test_red_screen();

        lvgl_port_unlock();
    }
}