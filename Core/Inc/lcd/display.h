/**
 * @file  display.h
 * @brief LCD 界面管理模块 — ChatGPT 风格聊天交互界面
 *
 * 界面布局 (240x320):
 *   顶部状态栏   [0,   24)   — WiFi图标 + 状态 + 会话标题
 *   聊天消息区   [24, 280)   — 消息气泡滚动区域 (256px)
 *   底部操作栏   [280, 320)  — 录音状态 + 按键提示
 *
 * 界面状态:
 *   DISPLAY_STATE_CHAT      — 主聊天界面
 *   DISPLAY_STATE_RECORDING — 录音中（覆盖层动效）
 *   DISPLAY_STATE_HISTORY   — 历史记录列表
 *   DISPLAY_STATE_CHAT_SCROLL — 聊天滚动浏览
 *   DISPLAY_STATE_DEBUG_LOG — 调试日志浏览
 */
#ifndef __DISPLAY_H
#define __DISPLAY_H

#include "main.h"
#include "lcd/lcd.h"
#include <stdint.h>

/* ===================== 界面状态枚举 ===================== */

typedef enum
{
    DISPLAY_STATE_CHAT = 0,    /* 主聊天界面 */
    DISPLAY_STATE_RECORDING,   /* 录音中覆盖层 */
    DISPLAY_STATE_HISTORY,     /* 历史记录列表 */
    DISPLAY_STATE_CHAT_SCROLL, /* 聊天滚动浏览 */
    DISPLAY_STATE_DEBUG_LOG    /* 调试日志浏览 */
} display_state_t;

/* ===================== 消息角色 ===================== */

typedef enum
{
    MSG_ROLE_USER = 0,  /* 用户消息 */
    MSG_ROLE_ASSISTANT, /* AI 助手回复 */
    MSG_ROLE_SYSTEM     /* 系统提示 */
} msg_role_t;

/* ===================== 布局常量 ===================== */

#define STATUS_BAR_H 24  /* 状态栏高度 */
#define CHAT_AREA_Y 24   /* 聊天区起始Y */
#define CHAT_AREA_H 256  /* 聊天区高度 */
#define BOTTOM_BAR_Y 280 /* 底部栏起始Y */
#define BOTTOM_BAR_H 40  /* 底部栏高度 */

#define MSG_MAX_COUNT 20 /* 最多缓存的消息条数 */
#define MSG_MAX_LEN 128  /* 单条消息最大字节数（GBK编码） */

/* ===================== 消息结构体 ===================== */

typedef struct
{
    msg_role_t role;        /* 消息角色 */
    char text[MSG_MAX_LEN]; /* 消息文本（GBK编码） */
    uint8_t text_len;       /* 实际文本长度 */
} display_msg_t;

/* ===================== 初始化与状态切换 ===================== */

/**
 * 初始化 LCD 并绘制初始聊天界面
 */
void display_init(void);

/**
 * 切换界面状态（带过渡动画）
 */
void display_set_state(display_state_t state);

/**
 * 获取当前界面状态
 */
display_state_t display_get_state(void);

/* ===================== 状态栏更新 ===================== */

/**
 * 更新 WiFi 连接状态显示
 * @param text   状态文本
 * @param color  状态颜色
 */
void display_update_wifi(const char *text, uint16_t color);

/**
 * 更新会话标题显示
 * @param title 会话标题文本
 */
void display_update_title(const char *title);

/* ===================== 聊天消息 ===================== */

/**
 * 清空所有消息（新建会话时调用）
 */
void display_clear_messages(void);

/**
 * 追加一条消息到聊天界面
 * @param role  消息角色
 * @param text  消息文本（GBK编码字符串）
 */
void display_add_message(msg_role_t role, const char *text);

/**
 * 更新最后一条同角色消息；若最后一条不是该角色，则追加新消息。
 * 用于流式 ASR 中间结果刷新同一条用户气泡。
 * @param role  消息角色
 * @param text  消息文本（GBK编码字符串）
 */
void display_update_last_message(msg_role_t role, const char *text);

/**
 * 显示系统提示信息（居中小字）
 * @param text 提示文本
 */
void display_show_system_hint(const char *text);

/* ===================== 录音界面 ===================== */

/**
 * 进入录音状态（显示麦克风动效覆盖层）
 */
void display_start_recording(void);

/**
 * 更新录音时长显示
 * @param ms 已录制毫秒数
 */
void display_update_recording(uint32_t ms);

/**
 * 退出录音状态（恢复聊天界面）
 */
void display_stop_recording(void);

/* ===================== 历史记录列表 ===================== */

/**
 * 显示历史会话列表
 * @param titles  标题字符串数组
 * @param count   标题数量
 */
void display_show_history(const char **titles, uint8_t count);

/**
 * 历史列表光标上移
 */
void display_history_up(void);

/**
 * 历史列表光标下移
 */
void display_history_down(void);

/**
 * 获取当前历史列表选中索引
 * @return 选中的索引值
 */
uint8_t display_history_get_selected(void);

/* ===================== 用户设置 ===================== */

/**
 * 设置用户名（显示在消息气泡旁）
 * @param name 用户名字符串
 */
void display_set_username(const char *name);

/* ===================== 底部操作栏 ===================== */

/**
 * 更新底部栏提示文字
 * @param text 提示文字
 */
void display_update_bottom_hint(const char *text);

/* ===================== 聊天滚动浏览 ===================== */

/**
 * 进入聊天滚动浏览模式（从最新消息开始）
 */
void display_enter_chat_scroll(void);

/**
 * 退出聊天滚动浏览模式，返回普通聊天界面
 */
void display_exit_chat_scroll(void);

/**
 * 聊天记录向上滚动（查看更早消息）
 */
void display_chat_scroll_up(void);

/**
 * 聊天记录向下滚动（查看更新消息）
 */
void display_chat_scroll_down(void);

/* ===================== 调试日志 ===================== */

#define DEBUG_LOG_MAX_COUNT 32u /* 最多保存的调试行数 */
#define DEBUG_LOG_LINE_LEN 48u  /* 每行最大字符数 */

/**
 * 向调试日志缓冲区追加一行（与聊天消息分离）
 * @param line 调试文本
 */
void display_push_debug_line(const char *line);

/**
 * 进入调试日志浏览界面（自动滚动到最新）
 */
void display_enter_debug_log(void);

/**
 * 退出调试日志浏览界面，返回聊天界面
 */
void display_exit_debug_log(void);

/**
 * 调试日志向上滚动一行
 */
void display_debug_scroll_up(void);

/**
 * 调试日志向下滚动一行
 */
void display_debug_scroll_down(void);

#endif /* __DISPLAY_H */
