/**
 * @file  system_state.h
 * @brief 系统状态机定义
 */

#ifndef __SYSTEM_STATE_H
#define __SYSTEM_STATE_H

#include <stdint.h>
#include <stdbool.h>

/* 系统主状态 */
typedef enum {
    SYSTEM_STATE_IDLE = 0,          /* 待机状态 */
    SYSTEM_STATE_RECORDING,         /* 录音状态 */
    SYSTEM_STATE_PAUSED,            /* 录音暂停状态 */
    SYSTEM_STATE_PLAYING,           /* 播放状态 */
    SYSTEM_STATE_HISTORY_LIST,      /* 历史记录列表模式 */
    SYSTEM_STATE_ERROR              /* 错误状态 */
} system_state_t;

/* 按钮事件类型 */
typedef enum {
    BUTTON_EVENT_NONE = 0,          /* 无事件 */
    BUTTON_EVENT_SHORT_PRESS,       /* 短按 */
    BUTTON_EVENT_LONG_PRESS,        /* 长按 */
    BUTTON_EVENT_DOUBLE_CLICK,      /* 双击 */
    BUTTON_EVENT_HOLD,              /* 按住不放 */
    BUTTON_EVENT_RELEASE            /* 释放 */
} button_event_t;

/* 云端会话状态 */
typedef enum {
    SESSION_STATE_NONE = 0,         /* 无会话 */
    SESSION_STATE_ACTIVE,           /* 活跃会话 */
    SESSION_STATE_PENDING,          /* 等待服务器响应 */
    SESSION_STATE_COMPLETED         /* 会话完成 */
} session_state_t;

/* 历史记录项 */
typedef struct {
    uint32_t id;                    /* 记录ID */
    char title[64];                 /* 标题 */
    char timestamp[32];             /* 时间戳 */
    uint8_t unread;                 /* 未读标记 */
} history_item_t;

/* 历史记录列表 */
typedef struct {
    history_item_t items[20];       /* 最多20条记录 */
    uint8_t count;                  /* 记录数量 */
    uint8_t cursor;                 /* 当前光标位置 */
    uint8_t page;                   /* 当前页码 */
} history_list_t;

/* 系统上下文 */
typedef struct {
    system_state_t current_state;   /* 当前系统状态 */
    system_state_t previous_state;  /* 上一个系统状态 */

    session_state_t session_state;  /* 会话状态 */
    uint32_t session_id;            /* 当前会话ID */

    /* 录音相关 */
    uint32_t recording_start_time;  /* 录音开始时间 */
    uint32_t total_recording_time;  /* 总录音时间 */
    uint8_t recording_segments;     /* 录音分段数 */

    /* 播放相关 */
    uint32_t playback_start_time;   /* 播放开始时间 */
    uint32_t playback_duration;     /* 播放时长 */

    /* 历史记录 */
    history_list_t history_list;    /* 历史记录列表 */

    /* 按钮事件 */
    button_event_t k1_event;        /* K1按钮事件 */
    button_event_t k2_event;        /* K2按钮事件 */

    /* 状态标志 */
    uint8_t audio_sending : 1;      /* 音频正在发送 */
    uint8_t audio_receiving : 1;    /* 音频正在接收 */
    uint8_t need_new_session : 1;   /* 需要新会话 */
    uint8_t history_loaded : 1;     /* 历史记录已加载 */
    uint8_t playback_interrupted : 1; /* 播放被打断 */

    /* 错误信息 */
    uint32_t error_code;            /* 错误代码 */
    char error_msg[64];             /* 错误消息 */
} system_context_t;

/* 函数声明 */
void system_state_init(void);
void system_state_transition(system_state_t new_state);
system_state_t system_state_get_current(void);
system_state_t system_state_get_previous(void);

void system_set_k1_event(button_event_t event);
void system_set_k2_event(button_event_t event);
button_event_t system_get_k1_event(void);
button_event_t system_get_k2_event(void);
void system_clear_button_events(void);

void system_set_session_id(uint32_t id);
uint32_t system_get_session_id(void);
void system_set_session_state(session_state_t state);
session_state_t system_get_session_state(void);

void system_start_recording(void);
void system_pause_recording(void);
void system_stop_recording(void);
uint32_t system_get_recording_time(void);
uint8_t system_get_recording_segments(void);

void system_start_playback(uint32_t duration);
void system_stop_playback(void);
uint32_t system_get_playback_time(void);
uint8_t system_is_playback_interrupted(void);

void system_history_init(void);
void system_history_add_item(const char* title, const char* timestamp);
uint8_t system_history_get_count(void);
uint8_t system_history_get_cursor(void);
void system_history_move_cursor_up(void);
void system_history_move_cursor_down(void);
void system_history_select_item(void);
history_item_t* system_history_get_current_item(void);

void system_set_error(uint32_t code, const char* msg);
uint32_t system_get_error_code(void);
const char* system_get_error_msg(void);
void system_clear_error(void);

system_context_t* system_get_context(void);

#endif /* __SYSTEM_STATE_H */