/**
 * @file  system_controller.c
 * @brief 系统主控制器实现
 */

#include "system/system_controller.h"
#include "wifi/esp8266.h"
#include "debug/debug_uart.h"
#include <string.h>
#include <stdio.h>

/* 全局控制器实例 */
static system_controller_t g_controller = {0};

/* 初始化系统控制器 */
void system_controller_init(void)
{
    memset(&g_controller, 0, sizeof(system_controller_t));

    /* 初始化系统状态 */
    system_state_init();
    g_controller.system_ctx = system_get_context();

    /* 初始化按钮事件检测 */
    button_event_init();

    /* 初始化云端通信 */
    cloud_comm_init();

    /* 设置初始状态 */
    g_controller.display_update = DISPLAY_UPDATE_STATE;
    g_controller.last_display_update = system_get_current_time();

    g_controller.initialized = 1;
    g_controller.mic_enabled = 0;
    g_controller.cloud_ready = 0;
    g_controller.audio_streaming = 0;
    g_controller.playback_active = 0;

    /* 添加示例历史记录（用于测试） */
    system_history_add_item("询问天气情况", "2026-04-08 10:30");
    system_history_add_item("设置明天闹钟", "2026-04-08 09:15");
    system_history_add_item("播放经典音乐", "2026-04-07 20:45");
    system_history_add_item("查询股票价格", "2026-04-07 14:20");
    system_history_add_item("翻译英文句子", "2026-04-06 11:10");

    /* 初始显示 */
    display_update_debug("系统初始化完成");
    display_update_wifi("初始化中...", COLOR_YELLOW);

    /* 设置初始显示 */
    system_update_state_display();
}

/* 系统轮询处理（在主循环中调用） */
void system_controller_poll(void)
{
    if (!g_controller.initialized) {
        return;
    }

    uint32_t current_time = system_get_current_time();

    /* 轮询按钮事件检测 */
    button_event_poll();

    /* 轮询云端通信 */
    cloud_comm_poll();

    /* 检查云端连接状态 */
    if (!g_controller.cloud_ready && cloud_comm_is_connected()) {
        g_controller.cloud_ready = 1;
        display_update_wifi("云端已连接", COLOR_GREEN);
        system_set_display_update(DISPLAY_UPDATE_STATE);
    }

    /* 处理按钮事件 */
    system_handle_button_events();

    /* 根据当前状态处理 */
    switch (g_controller.system_ctx->current_state) {
        case SYSTEM_STATE_IDLE:
            system_handle_idle_state();
            break;

        case SYSTEM_STATE_RECORDING:
            system_handle_recording_state();
            break;

        case SYSTEM_STATE_PAUSED:
            system_handle_paused_state();
            break;

        case SYSTEM_STATE_PLAYING:
            system_handle_playing_state();
            break;

        case SYSTEM_STATE_HISTORY_LIST:
            system_handle_history_state();
            break;

        case SYSTEM_STATE_ERROR:
            system_handle_error_state();
            break;

        default:
            break;
    }

    /* 更新显示（如果需要） */
    if (g_controller.display_update != DISPLAY_UPDATE_NONE) {
        system_update_display();
        g_controller.last_display_update = current_time;
    }

    /* 定期状态检查 */
    if (current_time - g_controller.last_status_check >= 1000) {
        g_controller.last_status_check = current_time;

        /* 检查云端心跳 */
        if (g_controller.cloud_ready) {
            cloud_comm_send_heartbeat();
        }
    }
}

/* 处理按钮事件 */
void system_handle_button_events(void)
{
    button_event_t k1_event = button_get_k1_event();
    button_event_t k2_event = button_get_k2_event();

    if (k1_event != BUTTON_EVENT_NONE) {
        system_handle_k1_event(k1_event);
    }

    if (k2_event != BUTTON_EVENT_NONE) {
        system_handle_k2_event(k2_event);
    }
}

/* 处理K1按钮事件 */
void system_handle_k1_event(button_event_t event)
{
    system_state_t current_state = g_controller.system_ctx->current_state;

    switch (current_state) {
        case SYSTEM_STATE_IDLE:
            /* 待机状态：K1按住不放开始录音 */
            if (event == BUTTON_EVENT_HOLD) {
                system_start_recording_session();
            }
            break;

        case SYSTEM_STATE_RECORDING:
            /* 录音状态：K1释放暂停录音 */
            if (event == BUTTON_EVENT_RELEASE) {
                system_pause_recording_session();
            }
            break;

        case SYSTEM_STATE_PAUSED:
            /* 暂停状态：K1按住不放继续录音 */
            if (event == BUTTON_EVENT_HOLD) {
                system_resume_recording_session();
            }
            break;

        case SYSTEM_STATE_HISTORY_LIST:
            /* 历史列表模式：
               - K1短按：向上移动光标
               - 任意键双击：确认载入选中记录
            */
            if (event == BUTTON_EVENT_SHORT_PRESS) {
                system_history_move_cursor_up();
                system_set_display_update(DISPLAY_UPDATE_HISTORY);
            } else if (event == BUTTON_EVENT_DOUBLE_CLICK) {
                history_item_t* item = system_history_get_current_item();
                if (item != NULL) {
                    system_load_history_session(item->id);
                }
            }
            break;

        default:
            break;
    }
}

/* 处理K2按钮事件 */
void system_handle_k2_event(button_event_t event)
{
    system_state_t current_state = g_controller.system_ctx->current_state;

    switch (current_state) {
        case SYSTEM_STATE_IDLE:
            /* 待机状态：
               - 短按：开启新会话
               - 双击：打开历史记录列表
            */
            if (event == BUTTON_EVENT_SHORT_PRESS) {
                system_create_new_session();
            } else if (event == BUTTON_EVENT_DOUBLE_CLICK) {
                system_state_transition(SYSTEM_STATE_HISTORY_LIST);
                /* 更新K2按钮显示为历史模式 */
                display_update_key2_str("历史");
                system_set_display_update(DISPLAY_UPDATE_HISTORY);
            }
            break;

        case SYSTEM_STATE_PAUSED:
            /* 暂停状态：
               - 短按：确认发送
               - 长按：放弃发送
            */
            if (event == BUTTON_EVENT_SHORT_PRESS) {
                system_send_recorded_audio();
            } else if (event == BUTTON_EVENT_LONG_PRESS) {
                system_cancel_recording_session();
            }
            break;

        case SYSTEM_STATE_PLAYING:
            /* 播放状态：短按打断播报 */
            if (event == BUTTON_EVENT_SHORT_PRESS) {
                system_stop_playback_session();
            }
            break;

        case SYSTEM_STATE_HISTORY_LIST:
            /* 历史列表模式：
               - K2短按：向下移动光标
               - 任意键双击：确认载入选中记录
            */
            if (event == BUTTON_EVENT_SHORT_PRESS) {
                system_history_move_cursor_down();
                system_set_display_update(DISPLAY_UPDATE_HISTORY);
            } else if (event == BUTTON_EVENT_DOUBLE_CLICK) {
                history_item_t* item = system_history_get_current_item();
                if (item != NULL) {
                    system_load_history_session(item->id);
                }
            }
            break;

        default:
            break;
    }
}

/* 开始录音会话 */
void system_start_recording_session(void)
{
    if (!g_controller.cloud_ready) {
        display_update_debug("云端未连接，无法录音");
        return;
    }

    /* 检查是否需要新会话 */
    if (g_controller.system_ctx->need_new_session ||
        g_controller.system_ctx->session_state == SESSION_STATE_NONE) {

        uint32_t session_id = 0;
        if (!cloud_comm_start_session(&session_id)) {
            display_update_debug("创建会话失败");
            return;
        }

        system_set_session_id(session_id);
        system_set_session_state(SESSION_STATE_ACTIVE);
        g_controller.system_ctx->need_new_session = 0;
    }

    /* 开始录音 */
    if (microphone_start_recording()) {
        system_state_transition(SYSTEM_STATE_RECORDING);
        system_start_recording();
        g_controller.audio_streaming = 1;
        g_controller.audio_buffer_pos = 0;

        /* 更新K1按钮显示为录音状态 */
        display_update_key1_str("录音中...");
        display_update_debug("开始录音");
        system_set_display_update(DISPLAY_UPDATE_RECORDING);
    } else {
        display_update_debug("启动录音失败");
    }
}

/* 暂停录音会话 */
void system_pause_recording_session(void)
{
    if (microphone_pause_recording()) {
        system_state_transition(SYSTEM_STATE_PAUSED);
        system_pause_recording();
        g_controller.audio_streaming = 0;

        /* 发送当前缓冲区的音频数据 */
        if (g_controller.audio_buffer_pos > 0) {
            cloud_comm_send_audio_frame(
                system_get_session_id(),
                g_controller.audio_buffer,
                g_controller.audio_buffer_pos,
                0 /* 不是最后一帧 */
            );
            g_controller.audio_buffer_pos = 0;
        }

        /* 更新K1按钮显示为暂停状态 */
        display_update_key1_str("暂停");
        display_update_debug("录音暂停");
        system_set_display_update(DISPLAY_UPDATE_STATE);
    }
}

/* 恢复录音会话 */
void system_resume_recording_session(void)
{
    if (microphone_resume_recording()) {
        system_state_transition(SYSTEM_STATE_RECORDING);
        system_start_recording();
        g_controller.audio_streaming = 1;

        /* 更新K1按钮显示为录音状态 */
        display_update_key1_str("录音中...");
        display_update_debug("恢复录音");
        system_set_display_update(DISPLAY_UPDATE_RECORDING);
    }
}

/* 停止录音会话 */
void system_stop_recording_session(void)
{
    microphone_stop_recording();
    system_stop_recording();
    g_controller.audio_streaming = 0;
    g_controller.audio_buffer_pos = 0;

    /* 更新K1按钮显示为就绪状态 */
    display_update_key1_str("就绪");
    system_state_transition(SYSTEM_STATE_IDLE);
    system_set_display_update(DISPLAY_UPDATE_STATE);
}

/* 发送录制的音频 */
void system_send_recorded_audio(void)
{
    if (!g_controller.cloud_ready) {
        display_update_debug("云端未连接");
        return;
    }

    uint32_t session_id = system_get_session_id();
    if (session_id == 0) {
        display_update_debug("无活跃会话");
        return;
    }

    /* 发送音频完成标志 */
    if (cloud_comm_send_audio_complete(session_id)) {
        /* 更新K2按钮显示 */
        display_update_key2_str("发送中...");
        display_update_debug("音频发送完成，等待响应");
        system_set_session_state(SESSION_STATE_PENDING);
        system_set_display_update(DISPLAY_UPDATE_STATE);
    } else {
        display_update_debug("发送音频完成标志失败");
    }
}

/* 取消录音会话 */
void system_cancel_recording_session(void)
{
    uint32_t session_id = system_get_session_id();
    if (session_id > 0) {
        cloud_comm_cancel_audio(session_id);
    }

    system_stop_recording_session();
    system_set_session_state(SESSION_STATE_NONE);
    system_set_session_id(0);

    display_update_debug("录音已取消");
    system_set_display_update(DISPLAY_UPDATE_STATE);
}

/* 创建新会话 */
void system_create_new_session(void)
{
    if (!g_controller.cloud_ready) {
        display_update_debug("云端未连接");
        return;
    }

    /* 如果有活跃会话，先结束 */
    uint32_t old_session_id = system_get_session_id();
    if (old_session_id > 0) {
        cloud_comm_end_session(old_session_id);
    }

    /* 创建新会话 */
    uint32_t new_session_id = 0;
    if (cloud_comm_start_session(&new_session_id)) {
        system_set_session_id(new_session_id);
        system_set_session_state(SESSION_STATE_ACTIVE);
        g_controller.system_ctx->need_new_session = 0;

        /* 更新K2按钮显示 */
        display_update_key2_str("新会话");
        display_update_debug("新会话已创建");
        system_set_display_update(DISPLAY_UPDATE_STATE);
    } else {
        display_update_debug("创建新会话失败");
    }
}

/* 加载历史会话 */
void system_load_history_session(uint32_t history_id)
{
    if (!g_controller.cloud_ready) {
        display_update_debug("云端未连接");
        return;
    }

    if (cloud_comm_load_history(history_id)) {
        display_update_debug("正在加载历史记录");

        /* 退出历史列表模式，返回待机状态 */
        system_state_transition(SYSTEM_STATE_IDLE);
        /* 更新K2按钮显示为就绪状态 */
        display_update_key2_str("就绪");
        system_set_display_update(DISPLAY_UPDATE_STATE);
    } else {
        display_update_debug("加载历史记录失败");
    }
}

/* 开始播放会话 */
void system_start_playback_session(uint32_t duration)
{
    system_state_transition(SYSTEM_STATE_PLAYING);
    system_start_playback(duration);
    g_controller.playback_active = 1;

    /* 更新K2按钮显示为播放状态 */
    display_update_key2_str("播放中");
    display_update_debug("开始播放");
    system_set_display_update(DISPLAY_UPDATE_PLAYBACK);
}

/* 停止播放会话 */
void system_stop_playback_session(void)
{
    system_stop_playback();
    g_controller.playback_active = 0;
    g_controller.system_ctx->playback_interrupted = 1;

    system_state_transition(SYSTEM_STATE_IDLE);

    /* 更新K2按钮显示 */
    display_update_key2_str("就绪");
    display_update_debug("播放已停止");
    system_set_display_update(DISPLAY_UPDATE_STATE);
}

/* 状态处理函数 */
void system_handle_idle_state(void)
{
    /* 待机状态不需要特殊处理 */
}

void system_handle_recording_state(void)
{
    uint32_t current_time = system_get_current_time();

    /* 定期发送音频数据 */
    if (g_controller.audio_streaming &&
        current_time - g_controller.last_audio_send >= 100) { /* 每100ms发送一次 */

        /* 从麦克风获取音频数据并发送 */
        uint8_t audio_data[1024]; /* 音频数据缓冲区 */
        uint32_t bytes_to_read = 1024;

        /* 获取麦克风处理后的音频缓冲区 */
        audio_buffer_t* processed_buffer = microphone_get_processed_audio_buffer();
        if (processed_buffer == NULL) {
            display_update_debug("音频缓冲区不可用");
            g_controller.last_audio_send = current_time;
            system_set_display_update(DISPLAY_UPDATE_RECORDING);
            return;
        }

        /* 从处理后的音频缓冲区读取数据 */
        uint32_t bytes_read = audio_buffer_read(processed_buffer,
                                               audio_data, bytes_to_read);

        if (bytes_read > 0) {
            /* 发送音频数据到云端 */
            if (cloud_comm_send_audio_frame(
                    system_get_session_id(),
                    audio_data,
                    bytes_read,
                    0 /* 不是最后一帧 */
                )) {
                /* 更新录音动画显示 */
                display_update_debug("音频流传输中...");
            } else {
                display_update_debug("音频发送失败");
            }
        }

        g_controller.last_audio_send = current_time;
        system_set_display_update(DISPLAY_UPDATE_RECORDING);
    }
}

void system_handle_paused_state(void)
{
    /* 暂停状态不需要特殊处理 */
}

void system_handle_playing_state(void)
{
    uint32_t playback_time = system_get_playback_time();
    uint32_t duration = g_controller.system_ctx->playback_duration;

    /* 检查播放是否完成 */
    if (duration > 0 && playback_time >= duration) {
        system_stop_playback_session();
    } else {
        /* 更新播放进度显示 */
        system_set_display_update(DISPLAY_UPDATE_PLAYBACK);
    }
}

void system_handle_history_state(void)
{
    /* 历史列表模式不需要特殊处理 */
}

void system_handle_error_state(void)
{
    /* 错误状态：显示错误信息 */
    system_set_display_update(DISPLAY_UPDATE_ERROR);
}

/* 显示更新函数 */
void system_update_display(void)
{
    switch (g_controller.display_update) {
        case DISPLAY_UPDATE_STATE:
            system_update_state_display();
            break;

        case DISPLAY_UPDATE_RECORDING:
            system_update_recording_display();
            break;

        case DISPLAY_UPDATE_PLAYBACK:
            system_update_playback_display();
            break;

        case DISPLAY_UPDATE_HISTORY:
            system_update_history_display();
            break;

        case DISPLAY_UPDATE_ERROR:
            system_update_error_display();
            break;

        default:
            break;
    }

    g_controller.display_update = DISPLAY_UPDATE_NONE;
}

void system_update_state_display(void)
{
    system_state_t state = g_controller.system_ctx->current_state;
    const char* state_text = "";
    uint16_t color = COLOR_WHITE;

    switch (state) {
        case SYSTEM_STATE_IDLE:
            state_text = "待机状态";
            color = COLOR_GREEN;
            break;

        case SYSTEM_STATE_RECORDING:
            state_text = "录音中";
            color = COLOR_BLUE;
            break;

        case SYSTEM_STATE_PAUSED:
            state_text = "已暂停";
            color = COLOR_YELLOW;
            break;

        case SYSTEM_STATE_PLAYING:
            state_text = "播放中";
            color = COLOR_CYAN;
            break;

        case SYSTEM_STATE_HISTORY_LIST:
            state_text = "历史记录";
            color = COLOR_MAGENTA;
            break;

        case SYSTEM_STATE_ERROR:
            state_text = "错误";
            color = COLOR_RED;
            break;
    }

    /* 更新状态显示 */
    display_update_wifi(state_text, color);

    /* 更新会话信息 */
    if (g_controller.system_ctx->session_state != SESSION_STATE_NONE) {
        char session_info[32];
        snprintf(session_info, sizeof(session_info), "会话:%lu",
                 g_controller.system_ctx->session_id);
        display_update_ip(session_info);
    } else {
        display_update_ip("无活跃会话");
    }
}

void system_update_recording_display(void)
{
    uint32_t recording_time = system_get_recording_time() / 1000; /* 转换为秒 */
    uint8_t segments = system_get_recording_segments();

    char info[64];
    snprintf(info, sizeof(info), "录音:%lus 段:%d", recording_time, segments);
    display_update_debug(info);

    /* 显示录音动画 */
    static uint8_t anim_counter = 0;
    anim_counter = (anim_counter + 1) % 4;

    const char* anim_chars[] = {"●", "●●", "●●●", "●●●●"};
    display_update_key1_str(anim_chars[anim_counter]);
}

void system_update_playback_display(void)
{
    uint32_t playback_time = system_get_playback_time() / 1000; /* 转换为秒 */
    uint32_t total_time = g_controller.system_ctx->playback_duration / 1000;

    char info[64];
    snprintf(info, sizeof(info), "播放:%lu/%lus", playback_time, total_time);
    display_update_debug(info);

    /* 显示播放进度条 */
    if (total_time > 0) {
        uint8_t progress = (playback_time * 100) / total_time;
        progress = (progress > 100) ? 100 : progress;

        char progress_bar[32];
        uint8_t bars = (progress * 20) / 100; /* 20个字符的进度条 */
        snprintf(progress_bar, sizeof(progress_bar), "[%-20s]",
                 "====================" + (20 - bars));
        display_update_key2_str(progress_bar);
    }
}

void system_update_history_display(void)
{
    uint8_t cursor = system_history_get_cursor();
    uint8_t count = system_history_get_count();

    if (count == 0) {
        display_update_debug("无历史记录");
        display_update_key1_str("空");
        display_update_key2_str("空");
        return;
    }

    history_item_t* item = system_history_get_current_item();
    if (item != NULL) {
        char info[64];
        /* 安全地格式化字符串，限制标题长度 */
        int max_title_len = sizeof(info) - 10; /* 保留空间给"X/Y "前缀 */
        if (max_title_len < 0) max_title_len = 0;

        char truncated_title[64];
        strncpy(truncated_title, item->title, max_title_len);
        truncated_title[max_title_len] = '\0';

        snprintf(info, sizeof(info), "%d/%d %s", cursor + 1, count, truncated_title);
        display_update_debug(info);

        display_update_key1_str(item->title);
        display_update_key2_str(item->timestamp);
    }
}

void system_update_error_display(void)
{
    const char* error_msg = system_get_error_msg();
    uint32_t error_code = system_get_error_code();

    char info[64];
    snprintf(info, sizeof(info), "错误:%lu %s", error_code, error_msg);
    display_update_debug(info);

    display_update_key1_str("错误");
    display_update_key2_str("请检查");
}

/* 工具函数 */
uint32_t system_get_current_time(void)
{
    return HAL_GetTick();
}

void system_set_display_update(display_update_flag_t flag)
{
    g_controller.display_update = flag;
}

void system_clear_display_update(void)
{
    g_controller.display_update = DISPLAY_UPDATE_NONE;
}

/* 获取控制器实例 */
system_controller_t* system_get_controller(void)
{
    return &g_controller;
}