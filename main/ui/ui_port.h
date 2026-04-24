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
 * @brief 更新设备状态文字
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
 * @brief 更新对话文本，暂时保留，后续不需要
 *
 * 在主内容区显示 STT 识别结果或 TTS 播放文本
 *
 * @param text 要显示的文本内容
 */
void ui_update_text(const char *text);

/**
 * @brief 显示通知提示框，不知道这个是什么？
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
 */
void ui_show_emotion(const char *name, const char *anim_desc);
