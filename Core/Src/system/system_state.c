/**
 * @file  system_state.c
 * @brief 系统状态机实现
 */

#include "system/system_state.h"
#include "main.h"
#include <string.h>

/* 全局系统上下文 */
static system_context_t g_system_context = {0};

/* 初始化系统状态 */
void system_state_init(void)
{
    memset(&g_system_context, 0, sizeof(g_system_context));

    g_system_context.current_state = SYSTEM_STATE_IDLE;
    g_system_context.previous_state = SYSTEM_STATE_IDLE;
    g_system_context.session_state = SESSION_STATE_NONE;
    g_system_context.session_id = 0;

    g_system_context.recording_start_time = 0;
    g_system_context.total_recording_time = 0;
    g_system_context.recording_segments = 0;

    g_system_context.playback_start_time = 0;
    g_system_context.playback_duration = 0;

    g_system_context.k1_event = BUTTON_EVENT_NONE;
    g_system_context.k2_event = BUTTON_EVENT_NONE;

    g_system_context.audio_sending = 0;
    g_system_context.audio_receiving = 0;
    g_system_context.need_new_session = 0;
    g_system_context.history_loaded = 0;
    g_system_context.playback_interrupted = 0;

    g_system_context.error_code = 0;
    g_system_context.error_msg[0] = '\0';

    /* 初始化历史记录列表 */
    system_history_init();
}

/* 状态转换 */
void system_state_transition(system_state_t new_state)
{
    if (new_state == g_system_context.current_state) {
        return; /* 状态未变化 */
    }

    g_system_context.previous_state = g_system_context.current_state;
    g_system_context.current_state = new_state;

    /* 状态转换时的清理工作 */
    switch (new_state) {
        case SYSTEM_STATE_IDLE:
            /* 进入待机状态时重置录音和播放状态 */
            g_system_context.recording_segments = 0;
            g_system_context.total_recording_time = 0;
            g_system_context.playback_interrupted = 0;
            break;

        case SYSTEM_STATE_RECORDING:
            /* 进入录音状态时记录开始时间 */
            if (g_system_context.recording_start_time == 0) {
                g_system_context.recording_start_time = HAL_GetTick();
            }
            break;

        case SYSTEM_STATE_PAUSED:
            /* 进入暂停状态时更新总录音时间 */
            if (g_system_context.recording_start_time > 0) {
                uint32_t current_time = HAL_GetTick();
                g_system_context.total_recording_time +=
                    (current_time - g_system_context.recording_start_time);
                g_system_context.recording_start_time = 0;
                g_system_context.recording_segments++;
            }
            break;

        case SYSTEM_STATE_PLAYING:
            /* 进入播放状态时记录开始时间 */
            g_system_context.playback_start_time = HAL_GetTick();
            g_system_context.playback_interrupted = 0;
            break;

        case SYSTEM_STATE_HISTORY_LIST:
            /* 进入历史列表模式时重置光标 */
            g_system_context.history_list.cursor = 0;
            g_system_context.history_list.page = 0;
            break;

        default:
            break;
    }
}

/* 获取当前状态 */
system_state_t system_state_get_current(void)
{
    return g_system_context.current_state;
}

/* 获取上一个状态 */
system_state_t system_state_get_previous(void)
{
    return g_system_context.previous_state;
}

/* 按钮事件处理 */
void system_set_k1_event(button_event_t event)
{
    g_system_context.k1_event = event;
}

void system_set_k2_event(button_event_t event)
{
    g_system_context.k2_event = event;
}

button_event_t system_get_k1_event(void)
{
    button_event_t event = g_system_context.k1_event;
    g_system_context.k1_event = BUTTON_EVENT_NONE;
    return event;
}

button_event_t system_get_k2_event(void)
{
    button_event_t event = g_system_context.k2_event;
    g_system_context.k2_event = BUTTON_EVENT_NONE;
    return event;
}

void system_clear_button_events(void)
{
    g_system_context.k1_event = BUTTON_EVENT_NONE;
    g_system_context.k2_event = BUTTON_EVENT_NONE;
}

/* 会话管理 */
void system_set_session_id(uint32_t id)
{
    g_system_context.session_id = id;
}

uint32_t system_get_session_id(void)
{
    return g_system_context.session_id;
}

void system_set_session_state(session_state_t state)
{
    g_system_context.session_state = state;
}

session_state_t system_get_session_state(void)
{
    return g_system_context.session_state;
}

/* 录音管理 */
void system_start_recording(void)
{
    g_system_context.recording_start_time = HAL_GetTick();
    g_system_context.audio_sending = 1;
}

void system_pause_recording(void)
{
    if (g_system_context.recording_start_time > 0) {
        uint32_t current_time = HAL_GetTick();
        g_system_context.total_recording_time +=
            (current_time - g_system_context.recording_start_time);
        g_system_context.recording_start_time = 0;
        g_system_context.recording_segments++;
    }
    g_system_context.audio_sending = 0;
}

void system_stop_recording(void)
{
    system_pause_recording();
    g_system_context.recording_segments = 0;
    g_system_context.total_recording_time = 0;
}

uint32_t system_get_recording_time(void)
{
    uint32_t current_time = 0;

    if (g_system_context.recording_start_time > 0) {
        current_time = HAL_GetTick();
        return g_system_context.total_recording_time +
               (current_time - g_system_context.recording_start_time);
    }

    return g_system_context.total_recording_time;
}

uint8_t system_get_recording_segments(void)
{
    return g_system_context.recording_segments;
}

/* 播放管理 */
void system_start_playback(uint32_t duration)
{
    g_system_context.playback_start_time = HAL_GetTick();
    g_system_context.playback_duration = duration;
    g_system_context.audio_receiving = 1;
    g_system_context.playback_interrupted = 0;
}

void system_stop_playback(void)
{
    g_system_context.playback_start_time = 0;
    g_system_context.playback_duration = 0;
    g_system_context.audio_receiving = 0;
}

uint32_t system_get_playback_time(void)
{
    if (g_system_context.playback_start_time > 0) {
        uint32_t current_time = HAL_GetTick();
        if (current_time >= g_system_context.playback_start_time) {
            return current_time - g_system_context.playback_start_time;
        }
    }
    return 0;
}

uint8_t system_is_playback_interrupted(void)
{
    return g_system_context.playback_interrupted;
}

/* 历史记录管理 */
void system_history_init(void)
{
    memset(&g_system_context.history_list, 0, sizeof(history_list_t));
}

void system_history_add_item(const char* title, const char* timestamp)
{
    if (g_system_context.history_list.count >= 20) {
        return; /* 列表已满 */
    }

    history_item_t* item = &g_system_context.history_list.items[g_system_context.history_list.count];

    item->id = g_system_context.history_list.count + 1;
    strncpy(item->title, title, sizeof(item->title) - 1);
    item->title[sizeof(item->title) - 1] = '\0';

    strncpy(item->timestamp, timestamp, sizeof(item->timestamp) - 1);
    item->timestamp[sizeof(item->timestamp) - 1] = '\0';

    item->unread = 0;

    g_system_context.history_list.count++;
}

uint8_t system_history_get_count(void)
{
    return g_system_context.history_list.count;
}

uint8_t system_history_get_cursor(void)
{
    return g_system_context.history_list.cursor;
}

void system_history_move_cursor_up(void)
{
    if (g_system_context.history_list.cursor > 0) {
        g_system_context.history_list.cursor--;
    }
}

void system_history_move_cursor_down(void)
{
    if (g_system_context.history_list.cursor < g_system_context.history_list.count - 1) {
        g_system_context.history_list.cursor++;
    }
}

void system_history_select_item(void)
{
    /* 这里可以添加选中项的处理逻辑 */
    /* 例如：加载对应的对话上下文 */
}

history_item_t* system_history_get_current_item(void)
{
    if (g_system_context.history_list.count == 0) {
        return NULL;
    }

    if (g_system_context.history_list.cursor >= g_system_context.history_list.count) {
        g_system_context.history_list.cursor = 0;
    }

    return &g_system_context.history_list.items[g_system_context.history_list.cursor];
}

/* 错误处理 */
void system_set_error(uint32_t code, const char* msg)
{
    g_system_context.error_code = code;
    strncpy(g_system_context.error_msg, msg, sizeof(g_system_context.error_msg) - 1);
    g_system_context.error_msg[sizeof(g_system_context.error_msg) - 1] = '\0';

    /* 发生错误时切换到错误状态 */
    system_state_transition(SYSTEM_STATE_ERROR);
}

uint32_t system_get_error_code(void)
{
    return g_system_context.error_code;
}

const char* system_get_error_msg(void)
{
    return g_system_context.error_msg;
}

void system_clear_error(void)
{
    g_system_context.error_code = 0;
    g_system_context.error_msg[0] = '\0';

    /* 清除错误后返回上一个状态 */
    system_state_transition(g_system_context.previous_state);
}

/* 获取系统上下文 */
system_context_t* system_get_context(void)
{
    return &g_system_context;
}