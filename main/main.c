#include "bsp/bsp_board.h"
#include "bsp/bsp_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ui/ui_port.h"
#include "ui/interaction.h"
#include "ui/reminder.h"
#include "bsp/servo_manager.h"
static const char *TAG = "SERVO_TEST";

// 舵机循环测试任务（独立运行，不阻塞 LVGL）
static void servo_test_task(void *arg)
{
    while (1)
    {
        ESP_LOGI(TAG, "--- 第一步：头左 双臂抬起 ---");
        bsp_servo_move_smooth(CH_HEAD, 60.0f, SERVO_SPEED_MID);
        bsp_servo_move_smooth(CH_L_ARM, 60.0f, SERVO_SPEED_MID);
        bsp_servo_move_smooth(CH_R_ARM, 120.0f, SERVO_SPEED_MID);
        vTaskDelay(pdMS_TO_TICKS(500));

        ESP_LOGI(TAG, "--- 第二步：头右 双臂落下 ---");
        bsp_servo_move_smooth(CH_HEAD, 120.0f, SERVO_SPEED_MID);
        bsp_servo_move_smooth(CH_L_ARM, 120.0f, SERVO_SPEED_MID);
        bsp_servo_move_smooth(CH_R_ARM, 60.0f, SERVO_SPEED_MID);
        vTaskDelay(pdMS_TO_TICKS(500));

        ESP_LOGI(TAG, "--- 第三步：全部归中 ---");
        bsp_servo_move_smooth(CH_HEAD, 90.0f, SERVO_SPEED_SLOW);
        bsp_servo_move_smooth(CH_L_ARM, 90.0f, SERVO_SPEED_SLOW);
        bsp_servo_move_smooth(CH_R_ARM, 90.0f, SERVO_SPEED_SLOW);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    // 等待 USB-JTAG CDC 连接建立，避免打印阻塞导致看门狗触发
    vTaskDelay(pdMS_TO_TICKS(500));

    bsp_board_t *board = bsp_board_get_instance();

    // 舵机初始化（三轴缓慢归中到 90°）
    bsp_board_servo_init(board);
    ESP_LOGI(TAG, "舵机初始化完成");

    // 3. 初始化舵机管理器（队列化动作执行）
    esp_err_t err = servo_manager_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "舵机管理器初始化失败: %d", err);
    }
    else
    {
        ESP_LOGI(TAG, "舵机管理器初始化完成");
    }

    // 4. 初始化交互管理器（情绪动作矩阵）
    err = interaction_manager_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "交互管理器初始化失败: %d", err);
    }
    else
    {
        ESP_LOGI(TAG, "交互管理器初始化完成");
    }
    // LCD 初始化
    bsp_board_lcd_init(board);
    ui_init();
    bsp_board_lcd_on(board);
    ESP_LOGI(TAG, "LCD 初始化完成");

    // 6. 创建触摸扫描任务（栈分配在PSRAM，节省内部SRAM）
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        touch_scan_task,
        "touch_scan",
        4096,
        NULL,
        4, // 优先级略低于舵机和音频
        NULL,
        tskNO_AFFINITY,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "创建触摸扫描任务失败！");
    }
    else
    {
        ESP_LOGI(TAG, "触摸扫描任务创建完成");
    }
    // // 舵机测试任务（独立跑，不影响 LVGL 刷新）
    // xTaskCreatePinnedToCoreWithCaps(
    //     servo_test_task,
    //     "servo_test",
    //     4096,
    //     NULL,
    //     5,
    //     NULL,
    //     tskNO_AFFINITY,
    //     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "舵机测试任务已启动，LCD 和舵机同时运行");
}
