/**
 * @file ui.h
 * @brief 用户界面模块头文件
 *
 * 基于 LVGL 图形库实现 LCD 显示屏的用户界面：
 * - 状态栏显示（时间、电量、WiFi 信号）
 * - 主内容区显示（状态文字、表情符号、对话文本）
 * - 通知提示框
 * - 二维码显示
 *
 * 所有 UI 操作都是线程安全的，可以在不同任务中调用
 */

#pragma once

#include <stdint.h>

/**
 * @brief 初始化 UI 系统
 *
 * 配置 LVGL 图形库和显示屏驱动，
 * 创建状态栏和内容区等 UI 元素
 */
void ui_init(void);

/**
 * @brief 更新时间显示（格式：HH:MM）
 *
 * 在状态栏显示当前时间，精确到分钟
 */
void ui_update_time(void);

/**
 * @brief 更新 WiFi 信号强度图标
 *
 * 根据 RSSI 值显示相应的信号格数
 *
 * @param rssi WiFi 信号强度（负数，单位 dBm）
 */
void ui_update_wifi(int rssi);

/**
 * @brief 更新电池电量图标
 *
 * 根据电量百分比显示相应的电池图标
 *
 * @param soc 电池电量百分比（0-100）
 */
void ui_update_battery(int soc);

/**
 *todo @brief 更新设备状态文字
 *
 * 在状态栏显示当前设备状态
 *
 * @param status 状态文字字符串
 */
void ui_update_status(const char *status);

/**
 * @brief 更新表情符号
 *
 * 根据 LLM 返回的情绪显示对应的 emoji 表情
 *
 * @param emotion 情绪名称（如"happy", "sad", "angry"等）
 */
void ui_update_emotion(const char *emotion);

/**
 * todo@brief 更新对话文本，暂时保留，后续不需要
 *
 * 在主内容区显示 STT 识别结果或 TTS 播放文本
 *
 * @param text 要显示的文本内容
 */
void ui_update_text(const char *text);

/**
 *todo @brief 显示通知提示框，不知道这个是什么？
 *
 * 弹出临时通知消息，指定时间后自动消失
 *
 * @param title 通知标题
 * @param message 通知内容
 * @param timeout_ms 显示时长（毫秒）
 */
void ui_show_notification(const char *title, const char *message, uint32_t timeout_ms);
/**
 * @brief 在屏幕底部显示当前情绪文字（3秒后自动隐藏，GIF 继续在后面播放）
 * @param name      情绪名称，如 "开心"
 * @param anim_desc 动画描述，如 "眯眼笑+冒星星"
 * @param audio_desc 音效描述，如 "开心笑声"
 */
void ui_show_emotion(const char *name, const char *anim_desc, const char *audio_desc);

// ─── 页面/功能菜单（由触摸翻页键驱动）─────────────────────────────
/**
 * @brief 短按翻页：上一页 / 下一页。
 *
 * 主界面：当前仅一页，调用为空操作（保留接口供后续扩展多张主页 GIF）。
 * 功能菜单：在 天气 / 闹钟 / 时间 等子页之间循环。
 * 任意一次调用都会重置功能菜单的 30s 空闲计时。
 */
void ui_page_prev(void);
void ui_page_next(void);

/**
 * @brief 长按任一翻页键：进入功能菜单（默认落到第一项）。
 *        重复调用相当于"刷新"——重置 30s 空闲计时。
 */
void ui_function_menu_enter(void);

/**
 * @brief 退出功能菜单，回到主界面。
 *        既可由空闲超时触发，也可显式调用。
 */
void ui_function_menu_exit(void);

/**
 * @brief 根据动画标识播放对应屏幕动画（由 interaction 层调用）
 *
 * @param anim_id  动画标识字符串，对应 InteractionMatrix_t.screen_anim
 *                 "anim_happy_stars" → eye_gif_show1()
 */
void ui_play_animation(const char *anim_id);