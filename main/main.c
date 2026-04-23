// #include "bsp/bsp_board.h"
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_log.h"
// #include "ui/interaction.h"
// #include "ui/reminder.h"
// #include "bsp/servo_manager.h"
// #include "ui/ui_port.h"

// static const char *TAG = "APP_MAIN";

// void app_main(void)
// {
//     // 1. 初始化底层硬件句柄
//     bsp_board_t *board = bsp_board_get_instance();

//     // 2. 初始化舵机硬件底层
//     // 舵机初始化（内部会缓慢归中到90°，约1.35秒）
//     bsp_board_servo_init(board);
//     ESP_LOGI(TAG, "舵机硬件初始化完成");

//     // 3. 初始化舵机管理器（队列化动作执行）
//     esp_err_t err = servo_manager_init();
//     if (err != ESP_OK)
//     {
//         ESP_LOGE(TAG, "舵机管理器初始化失败: %d", err);
//     }
//     else
//     {
//         ESP_LOGI(TAG, "舵机管理器初始化完成");
//     }

//     // 4. 初始化交互管理器（情绪动作矩阵）
//     err = interaction_manager_init();
//     if (err != ESP_OK)
//     {
//         ESP_LOGE(TAG, "交互管理器初始化失败: %d", err);
//     }
//     else
//     {
//         ESP_LOGI(TAG, "交互管理器初始化完成");
//     }
//     // 初始化lcd
//     bsp_board_lcd_init(board);
//     // 5. 运行正式 UI 逻辑
//     ui_init();

//     // 6. 创建触摸扫描任务（栈分配在PSRAM，节省内部SRAM）
//     BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
//         touch_scan_task,
//         "touch_scan",
//         4096,
//         NULL,
//         4, // 优先级略低于舵机和音频
//         NULL,
//         tskNO_AFFINITY,
//         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

//     if (ret != pdPASS)
//     {
//         ESP_LOGE(TAG, "创建触摸扫描任务失败！");
//     }
//     else
//     {
//         ESP_LOGI(TAG, "触摸扫描任务创建完成");
//     }

//     // 7. 最后亮屏，保证用户第一眼看到的是完美的图而不是加载过程
//     bsp_board_lcd_on(board);

//     // 8. 等待所有模块就绪
//     vTaskDelay(pdMS_TO_TICKS(2000));
//     printf("\n✅ 系统准备就绪！触摸对应位置触发动作：\n");
//     printf("铜箔位置对应：上=头部 | 前=腹部 | 后=背部\n\n");
// }

// void app_main(void)
// {
//     printf("=== 触摸铜箔控制舵机 启动 ===\n");
//     printf("铜箔位置对应：上=头部 | 前=腹部 | 后=背部\n\n");

//     // 1. 初始化BSP板级硬件（舵机硬件底层）
//     bsp_board_t *bsp = bsp_board_get_instance();
//     bsp_board_servo_init(bsp);
//     ESP_LOGI(TAG, "舵机硬件初始化完成");

//     // 2. 初始化舵机管理器（队列化动作执行）
//     esp_err_t err = servo_manager_init();
//     if (err != ESP_OK)
//     {
//         ESP_LOGE(TAG, "舵机管理器初始化失败: %d", err);
//     }
//     else
//     {
//         ESP_LOGI(TAG, "舵机管理器初始化完成");
//     }

//     // 3. 初始化交互管理器（情绪动作矩阵）
//     err = interaction_manager_init();
//     if (err != ESP_OK)
//     {
//         ESP_LOGE(TAG, "交互管理器初始化失败: %d", err);
//     }
//     else
//     {
//         ESP_LOGI(TAG, "交互管理器初始化完成");
//     }

//     // 4. 创建触摸扫描任务（栈分配在PSRAM，节省内部SRAM）
//     BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
//         touch_scan_task,
//         "touch_scan",
//         4096,
//         NULL,
//         4, // 优先级略低于舵机和音频
//         NULL,
//         tskNO_AFFINITY,
//         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

//     if (ret != pdPASS)
//     {
//         ESP_LOGE(TAG, "创建触摸扫描任务失败！");
//     }
//     else
//     {
//         ESP_LOGI(TAG, "触摸扫描任务创建完成");
//     }

//     // 等待所有模块就绪
//     vTaskDelay(pdMS_TO_TICKS(2000));
//     printf("\n✅ 系统准备就绪！触摸对应位置触发动作：\n");

//     // 主循环：处理触摸事件并触发对应情绪动作
//     touch_event_t event;
//     while (1)
//     {
//         if (bsp_touch_get_event(&event))
//         {
//             switch (event)
//             {
//             // ── 单位置短按 ─────────────────────────────
//             case TOUCH_EVENT_SHORT_HEAD:
//                 ESP_LOGI(TAG, "摸头 → 开心");
//                 ui_interaction_play(EMO_HAPPY);
//                 break;

//             case TOUCH_EVENT_SHORT_ABDOMEN:
//                 ESP_LOGI(TAG, "摸肚子 → 舒服");
//                 ui_interaction_play(EMO_COMFORTABLE);
//                 break;

//             case TOUCH_EVENT_SHORT_BACK:
//                 ESP_LOGI(TAG, "摸背 → 怕痒");
//                 ui_interaction_play(EMO_TICKLISH);
//                 break;

//             // ── 单位置长按 ─────────────────────────────
//             case TOUCH_EVENT_LONG_HEAD:
//                 ESP_LOGI(TAG, "摸头长按 → 撒娇");
//                 ui_interaction_play(EMO_ACT_CUTE);
//                 break;

//             case TOUCH_EVENT_LONG_ABDOMEN:
//                 ESP_LOGI(TAG, "摸肚子长按 → 治愈");
//                 ui_interaction_play(EMO_HEALING);
//                 break;

//             case TOUCH_EVENT_LONG_BACK:
//                 ESP_LOGI(TAG, "摸背长按 → 惊喜");
//                 ui_interaction_play(EMO_SURPRISED);
//                 break;

//             // ── 双位置组合触摸 ─────────────────────────
//             case TOUCH_EVENT_COMBO_HEAD_ABDOMEN:
//                 ESP_LOGI(TAG, "头+腹 → 害羞蹭蹭");
//                 ui_interaction_play(EMO_SHY_RUB);
//                 break;

//             case TOUCH_EVENT_COMBO_HEAD_BACK:
//                 ESP_LOGI(TAG, "头+背 → 兴奋");
//                 ui_interaction_play(EMO_EXCITED);
//                 break;

//             case TOUCH_EVENT_COMBO_ABDOMEN_BACK:
//                 ESP_LOGI(TAG, "腹+背 → 舒服到打滚");
//                 ui_interaction_play(EMO_COMFORTABLE_ROLL);
//                 break;

//             default:
//                 break;
//             }
//         }

//         vTaskDelay(pdMS_TO_TICKS(50)); // 降低主循环CPU占用
//     }
// }
#include "bsp/bsp_board.h"
#include "bsp/bsp_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ui/ui_port.h"

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
    bsp_board_t *board = bsp_board_get_instance();

    // 舵机初始化（三轴缓慢归中到 90°）
    // bsp_board_servo_init(board);
    ESP_LOGI(TAG, "舵机初始化完成");

    // LCD 初始化
    bsp_board_lcd_init(board);
    ui_init();
    bsp_board_lcd_on(board);
    ESP_LOGI(TAG, "LCD 初始化完成");

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

    // ESP_LOGI(TAG, "舵机测试任务已启动，LCD 和舵机同时运行");
}
