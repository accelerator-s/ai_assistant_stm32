#include "button/button_context.h"

static void start_recording(void)
{
    sys_state = STATE_RECORDING;
    rec_start_tick = HAL_GetTick();
    rec_end_pending = 0u;
    audio_buffer_reset();
    i2s_mic_start();
    display_start_recording();
    display_update_bottom_hint("K1短按发送");
    (void)esp8266_tcp_send_line_async("REC_START");
}

static void finish_recording_and_send(void)
{
    uint16_t remain;
    uint32_t flush_start;

    i2s_mic_stop();
    display_stop_recording();
    rec_end_pending = 1u;

    remain = audio_buffer_available();
    flush_start = HAL_GetTick();
    while (remain > 0u &&
           tcp_ready_for_send() &&
           (HAL_GetTick() - flush_start) < 500u)
    {
        if (esp8266_tx_in_progress())
        {
            esp8266_poll();
            continue;
        }
        audio_upload_poll();
        remain = audio_buffer_available();
    }

    sys_state = STATE_WAITING;
    display_update_bottom_hint("等待AI回复...");
    display_add_message(
        MSG_ROLE_SYSTEM,
        audio_upload_overflow ? "音频已发送，存在缓冲溢出" : "语音已发送，等待回复...");
    try_send_rec_end();
}

static void cancel_recording(void)
{
    i2s_mic_stop();
    sys_state = STATE_IDLE;
    audio_buffer_reset();
    rec_end_pending = 0u;
    display_stop_recording();
    display_update_bottom_hint("K1短按录音 K2新建");
    display_add_message(MSG_ROLE_SYSTEM, "已取消录音");
    (void)esp8266_tcp_send_line_async("REC_CANCEL");
}

void handle_idle(key_event_t k1_ev, key_event_t k2_ev)
{
    if (k1_ev == KEY_EVENT_K1_RELEASE)
    {
        k1_no_tcp_hint_latched = 0u;
    }

    if (k1_ev == KEY_EVENT_K1_PRESS)
    {
        if (wav_stream_active)
        {
            display_show_system_hint("语音播放中，请稍候");
            return;
        }

        if (!tcp_ready_for_send())
        {
            if (!k1_no_tcp_hint_latched)
            {
                display_show_system_hint("未连接WebUI，无法语音对话");
                display_update_bottom_hint("等待TCP连接...");
                k1_no_tcp_hint_latched = 1u;
            }
            return;
        }

        k1_no_tcp_hint_latched = 0u;
        start_recording();
        return;
    }

    if (k2_ev == KEY_EVENT_K2_SHORT)
    {
        display_clear_messages();
        display_update_title("新对话");
        display_show_system_hint("新会话已开启");
        (void)esp8266_tcp_send_line_async("NEW_SESSION");
        return;
    }

    if (k2_ev == KEY_EVENT_K2_DOUBLE)
    {
        sys_state = STATE_HISTORY;
        (void)esp8266_tcp_send_line_async("GET_HISTORY");
        display_show_history(NULL, 0);
        return;
    }
}

void handle_recording(key_event_t k1_ev, key_event_t k2_ev)
{
    uint32_t elapsed = HAL_GetTick() - rec_start_tick;
    display_update_recording(elapsed);

    if (k1_ev == KEY_EVENT_K1_PRESS)
    {
        finish_recording_and_send();
        return;
    }

    if (k2_ev == KEY_EVENT_K2_LONG)
    {
        cancel_recording();
        return;
    }
}

void handle_rec_paused(key_event_t k1_ev, key_event_t k2_ev)
{
    if (k1_ev == KEY_EVENT_K1_PRESS || k2_ev == KEY_EVENT_K2_SHORT)
    {
        finish_recording_and_send();
        return;
    }

    if (k2_ev == KEY_EVENT_K2_LONG)
    {
        cancel_recording();
        return;
    }
}

void handle_history(key_event_t k1_ev, key_event_t k2_ev)
{
    if (k1_ev == KEY_EVENT_K1_PRESS)
    {
        display_history_up();
        return;
    }

    if (k2_ev == KEY_EVENT_K2_SHORT)
    {
        display_history_down();
        return;
    }

    if (k2_ev == KEY_EVENT_K2_DOUBLE)
    {
        uint8_t sel = display_history_get_selected();
        char cmd[32];

        sys_state = STATE_IDLE;
        display_set_state(DISPLAY_STATE_CHAT);

        snprintf(cmd, sizeof(cmd), "LOAD_SESSION:%d", sel);
        (void)esp8266_tcp_send_line_async(cmd);
        return;
    }
}
