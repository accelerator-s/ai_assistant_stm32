#include "button/button_context.h"

void handle_idle(key_event_t k2_ev)
{
    if (!bsp_key_get_k1())
    {
        k1_no_tcp_hint_latched = 0;
    }

    if (bsp_key_get_k1())
    {
        if (!tcp_ready_for_send())
        {
            if (!k1_no_tcp_hint_latched)
            {
                display_show_system_hint("未连接WebUI，无法语音对话");
                display_update_bottom_hint("等待TCP连接...");
                k1_no_tcp_hint_latched = 1;
            }
            return;
        }

        k1_no_tcp_hint_latched = 0;
        sys_state = STATE_RECORDING;
        rec_start_tick = HAL_GetTick();
        rec_end_pending = 0u;
        audio_buffer_reset();
        i2s_mic_start();
        display_start_recording();

        (void)esp8266_tcp_send_line_async("REC_START");
        return;
    }

    if (k2_ev == KEY_EVENT_K2_SHORT)
    {
        display_clear_messages();
        display_update_title("新对话1");
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


void handle_recording(void)
{
    uint32_t elapsed = HAL_GetTick() - rec_start_tick;
    display_update_recording(elapsed);

    if (!bsp_key_get_k1())
    {
        i2s_mic_stop();
        sys_state = STATE_REC_PAUSED;
        display_stop_recording();
        display_update_bottom_hint("K1续录 K2短按发送 长按取消");
        return;
    }
}


void handle_rec_paused(key_event_t k2_ev)
{
    if (bsp_key_get_k1())
    {
        sys_state = STATE_RECORDING;
        i2s_mic_start();
        display_start_recording();
        return;
    }

    if (k2_ev == KEY_EVENT_K2_SHORT)
    {
        uint16_t remain = audio_buffer_available();
        uint32_t flush_start = HAL_GetTick();

        /* 延长刷缓冲时间，确保残余音频数据尽可能发出 */
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
        rec_end_pending = 1u;
        try_send_rec_end();
        return;
    }

    if (k2_ev == KEY_EVENT_K2_LONG)
    {
        sys_state = STATE_IDLE;
        audio_buffer_reset();
        rec_end_pending = 0u;
        display_update_bottom_hint("K1:录音 K2:发送/新建");
        display_add_message(MSG_ROLE_SYSTEM, "已取消录音");
        (void)esp8266_tcp_send_line_async("REC_CANCEL");
        return;
    }
}


void handle_history(key_event_t k2_ev)
{
    if (bsp_key_k1_changed() && bsp_key_get_k1())
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

