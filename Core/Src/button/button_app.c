#include "button/button_app.h"
#include "button/button_context.h"

#define HEARTBEAT_INTERVAL_MS 15000u

void button_app_init(void)
{
    debug_uart_init();
    bsp_key_init();
    display_init();

    i2s_mic_init();
    i2s_mic_set_callback(on_audio_data);

    esp8266_init();

    display_update_wifi("wifi未连接", WIFI_COLOR_DISCONNECTED);
    display_show_system_hint("语音助手已启动");

}

void button_app_run(void)
{
    esp8266_status_t last_wifi_st = (esp8266_status_t)0xFF;
    char last_ip[20] = {0};
    uint32_t last_tcp_try_tick = 0;
    uint8_t tcp_async_started = 0;
    uint32_t last_hb_tick = 0;
    int last_tcp_conn_state = 0;

    while (1)
    {
        handle_tcp_downlink();
        esp8266_poll();
        handle_tcp_downlink();
        mic_probe_poll();
        mic_rec_test_poll();

        esp8266_status_t wifi_st = esp8266_get_status();
        if (wifi_st != last_wifi_st)
        {
            switch (wifi_st)
            {
            case ESP8266_STATUS_TCP_CONNECTED:
                display_update_wifi("已连接", WIFI_COLOR_CONNECTED);
                if (sys_state == STATE_IDLE)
                {
                    display_update_bottom_hint("K1:录音 K2:发送/新建");
                }
                break;

            default:
                display_update_wifi("未连接", WIFI_COLOR_DISCONNECTED);
                break;
            }
            last_wifi_st = wifi_st;
        }

        {
            const char *cur_ip = esp8266_get_ip_cached();
            if (cur_ip && cur_ip[0] != '\0' && strcmp(cur_ip, last_ip) != 0)
            {
                strncpy(last_ip, cur_ip, sizeof(last_ip) - 1);
                last_ip[sizeof(last_ip) - 1] = '\0';
            }
        }

        if ((wifi_st == ESP8266_STATUS_WIFI_CONNECTED ||
             wifi_st == ESP8266_STATUS_WIFI_GOT_IP) &&
            (wifi_st == ESP8266_STATUS_WIFI_GOT_IP || last_ip[0] != '\0') &&
            !tcp_async_started &&
            ((HAL_GetTick() - last_tcp_try_tick) > 5000u))
        {
            debug_printf("[TCP] async start %s:%u\r\n", SERVER_IP, (unsigned)SERVER_PORT);
            esp8266_connect_tcp_async(SERVER_IP, SERVER_PORT);
            tcp_async_started = 1;
            last_tcp_try_tick = HAL_GetTick();
            last_tcp_conn_state = 0;
            display_update_wifi("未连接", WIFI_COLOR_DISCONNECTED);
        }

        if (tcp_async_started)
        {
            int tcp_st = esp8266_tcp_connect_state();

            if (tcp_st != last_tcp_conn_state)
            {
                if (tcp_st == 1)
                {
                    display_update_wifi("已连接", WIFI_COLOR_CONNECTED);
                }
                else
                {
                    display_update_wifi("未连接", WIFI_COLOR_DISCONNECTED);
                }
                last_tcp_conn_state = tcp_st;
            }

            if (tcp_st == 1)
            {
                debug_printf("[TCP] async connected\r\n");
                tcp_async_started = 0;
            }
            else if (tcp_st == -1)
            {
                debug_printf("[TCP] async failed: %s\r\n", esp8266_get_debug_msg());
                tcp_async_started = 0;
                last_tcp_try_tick = HAL_GetTick();
            }
        }

        if (wifi_st == ESP8266_STATUS_TCP_CONNECTED)
        {
            tcp_async_started = 0;
            last_tcp_conn_state = 1;
        }
        else if (wifi_st == ESP8266_STATUS_WIFI_CONNECTED ||
                 wifi_st == ESP8266_STATUS_WIFI_GOT_IP)
        {
            if (!tcp_async_started)
            {
                last_tcp_conn_state = 0;
            }
        }
        else
        {
            tcp_async_started = 0;
            last_tcp_conn_state = 0;
        }

        if (wifi_st == ESP8266_STATUS_TCP_CONNECTED &&
            tcp_idle_for_heartbeat() &&
            ((HAL_GetTick() - last_hb_tick) >= HEARTBEAT_INTERVAL_MS))
        {
            esp8266_tcp_send_heartbeat();
            last_hb_tick = HAL_GetTick();
        }

        bsp_key_update();  /* 更新按键状态 */
        audio_upload_poll();
        try_send_rec_end();
        try_send_speaker_test_ok();
        try_send_speaker_tone_done();
        try_send_speaker_sweep_done();
        try_send_speaker_volume_done();
        try_send_speaker_ode_done();
        try_send_mic_rec_done();

        {
            key_event_t k2_ev = detect_k2_event();

            switch (sys_state)
            {
            case STATE_IDLE:
                handle_idle(k2_ev);
                break;

            case STATE_RECORDING:
                handle_recording();
                break;

            case STATE_REC_PAUSED:
                handle_rec_paused(k2_ev);
                break;

            case STATE_WAITING:
                if (k2_ev == KEY_EVENT_K2_SHORT)
                {
                    sys_state = STATE_IDLE;
                    display_update_bottom_hint("K1:录音 K2:发送/新建");
                }
                break;

            case STATE_HISTORY:
                handle_history(k2_ev);
                break;

            default:
                break;
            }
        }

        bsp_key_k1_changed();
        bsp_key_k2_changed();
    }
}
