/**
 * @file  system_controller.h
 * @brief 系统主控制器
 */

#ifndef __SYSTEM_CONTROLLER_H
#define __SYSTEM_CONTROLLER_H

#include "main.h"
#include "system/system_state.h"
#include "bsp/button_event.h"
#include "cloud/cloud_comm.h"
#include "audio/microphone.h"
#include "lcd/display.h"

/* 显示更新标志 */
typedef enum {
    DISPLAY_UPDATE_NONE = 0,        /* 无更新 */
    DISPLAY_UPDATE_STATE,           /* 状态更新 */
    DISPLAY_UPDATE_RECORDING,       /* 录音状态更新 */
    DISPLAY_UPDATE_PLAYBACK,        /* 播放状态更新 */
    DISPLAY_UPDATE_HISTORY,         /* 历史记录更新 */
    DISPLAY_UPDATE_ERROR           /* 错误更新 */
} display_update_flag_t;

/* 系统控制器上下文 */
typedef struct {
    /* 状态管理 */
    system_context_t* system_ctx;   /* 系统状态上下文 */

    /* 外设句柄 */
    microphone_t* mic_handle;       /* 麦克风句柄 */

    /* 显示管理 */
    display_update_flag_t display_update; /* 显示更新标志 */
    uint32_t last_display_update;   /* 上次显示更新时间 */

    /* 音频缓冲区 */
    uint8_t audio_buffer[4096];     /* 音频发送缓冲区 */
    uint32_t audio_buffer_pos;      /* 音频缓冲区位置 */

    /* 定时器 */
    uint32_t last_status_check;     /* 上次状态检查时间 */
    uint32_t last_audio_send;       /* 上次音频发送时间 */

    /* 标志位 */
    uint8_t initialized : 1;        /* 已初始化 */
    uint8_t mic_enabled : 1;        /* 麦克风已启用 */
    uint8_t cloud_ready : 1;        /* 云端就绪 */
    uint8_t audio_streaming : 1;    /* 音频流传输中 */
    uint8_t playback_active : 1;    /* 播放活跃 */
} system_controller_t;

/* 函数声明 */
void system_controller_init(void);
void system_controller_poll(void);

void system_handle_button_events(void);
void system_update_display(void);

/* 状态处理函数 */
void system_handle_idle_state(void);
void system_handle_recording_state(void);
void system_handle_paused_state(void);
void system_handle_playing_state(void);
void system_handle_history_state(void);
void system_handle_error_state(void);

/* 按钮事件处理函数 */
void system_handle_k1_event(button_event_t event);
void system_handle_k2_event(button_event_t event);

/* 录音控制函数 */
void system_start_recording_session(void);
void system_pause_recording_session(void);
void system_resume_recording_session(void);
void system_stop_recording_session(void);
void system_send_recorded_audio(void);
void system_cancel_recording_session(void);

/* 播放控制函数 */
void system_start_playback_session(uint32_t duration);
void system_stop_playback_session(void);

/* 会话管理函数 */
void system_create_new_session(void);
void system_load_history_session(uint32_t history_id);

/* 显示更新函数 */
void system_update_state_display(void);
void system_update_recording_display(void);
void system_update_playback_display(void);
void system_update_history_display(void);
void system_update_error_display(void);

/* 工具函数 */
uint32_t system_get_current_time(void);
void system_set_display_update(display_update_flag_t flag);
void system_clear_display_update(void);

/* 获取控制器实例 */
system_controller_t* system_get_controller(void);

#endif /* __SYSTEM_CONTROLLER_H */