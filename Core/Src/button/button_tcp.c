#include "button/button_context.h"

uint8_t tcp_ready_for_send(void)
{
    return (esp8266_get_status() == ESP8266_STATUS_TCP_CONNECTED) ? 1u : 0u;
}


uint8_t tcp_idle_for_heartbeat(void)
{
    if (sys_state == STATE_RECORDING || mic_rec_test_state == MIC_REC_TEST_RUNNING)
        return 0u;

    if (rec_end_pending || mic_rec_done_pending)
        return 0u;

    if (speaker_wav_done_pending)
        return 0u;

    if (wav_stream_active)
        return 0u;

    if (audio_buffer_available() > 0u)
        return 0u;

    if (esp8266_tx_in_progress() || !esp8266_tx_queue_is_empty())
        return 0u;

    return 1u;
}


void handle_tcp_downlink(void)
{
    char line[160];
    uint8_t loop_guard = 6;

    /* WAV 流接收期间，所有 TCP 数据由 wav_stream_service 的 read_raw 处理 */
    if (wav_stream_active)
        return;

    while (loop_guard-- > 0 && esp8266_tcp_read_line(line, sizeof(line)))
    {
        if (strncmp(line, "STT_PART:", 9) == 0)
        {
            display_update_last_message(MSG_ROLE_USER, line + 9);
            if (sys_state == STATE_WAITING)
            {
                display_update_bottom_hint("正在识别...");
            }
        }
        else if (strncmp(line, "STT_FINAL:", 10) == 0)
        {
            display_update_last_message(MSG_ROLE_USER, line + 10);
            if (sys_state == STATE_WAITING)
            {
                display_update_bottom_hint("识别完成，思考中...");
            }
        }
        else if (strncmp(line, "STT:", 4) == 0)
        {
            display_add_message(MSG_ROLE_USER, line + 4);
            if (sys_state == STATE_WAITING)
            {
                display_update_bottom_hint("识别完成，思考中...");
            }
        }
        else if (strncmp(line, "AI:", 3) == 0)
        {
            display_add_message(MSG_ROLE_ASSISTANT, line + 3);
            display_update_bottom_hint("正在合成语音...");
        }
        else if (strcmp(line, "DIALOG_DONE") == 0)
        {
            sys_state = STATE_IDLE;
            display_update_bottom_hint("K1短按录音 K2新建");
        }
        else if (strncmp(line, "SYS:", 4) == 0)
        {
            display_show_system_hint(line + 4);
        }
        else if (strncmp(line, "TITLE:", 6) == 0)
        {
            display_update_title(line + 6);
        }
        else if (strcmp(line, "MIC_TEST") == 0)
        {
            mic_probe_start();
        }
        else if (strcmp(line, "SPK_TEST") == 0)
        {
            speaker_test_ok_pending = 1u;
            display_show_system_hint("SPK_TEST");
            try_send_speaker_test_ok();
        }
        else if (strncmp(line, "SPK_TONE:", 9) == 0)
        {
            uint16_t freq_hz = 1000u;
            uint16_t duration_ms = 1000u;
            char *duration_part = strchr(line + 9, ':');

            freq_hz = (uint16_t)atoi(line + 9);
            if (duration_part)
            {
                duration_ms = (uint16_t)atoi(duration_part + 1);
            }

            display_show_system_hint("SPK_TONE");
            speaker_tone_done_pending = 1u;
            pump_speaker_test_response(1200u);
            (void)i2s_mic_play_tone(freq_hz, duration_ms);
        }
        else if (strncmp(line, "SPK_SWEEP:", 10) == 0)
        {
            uint16_t start_hz = 300u;
            uint16_t end_hz = 3000u;
            uint16_t duration_ms = 2000u;
            char *end_part = strchr(line + 10, ':');
            char *duration_part = end_part ? strchr(end_part + 1, ':') : NULL;

            start_hz = (uint16_t)atoi(line + 10);
            if (end_part)
            {
                end_hz = (uint16_t)atoi(end_part + 1);
            }
            if (duration_part)
            {
                duration_ms = (uint16_t)atoi(duration_part + 1);
            }

            display_show_system_hint("SPK_SWEEP");
            speaker_sweep_done_pending = 1u;
            pump_speaker_test_response(1200u);
            (void)i2s_mic_play_sweep(start_hz, end_hz, duration_ms);
        }
        else if (strncmp(line, "SPK_VOLUME:", 11) == 0)
        {
            uint8_t levels[8] = {30u, 60u, 90u};
            uint8_t count = 0u;
            char *p = line + 11;

            while (p && *p != '\0' && count < (uint8_t)(sizeof(levels) / sizeof(levels[0])))
            {
                int v = atoi(p);
                char *comma = strchr(p, ',');

                if (v < 0)
                    v = 0;
                if (v > 100)
                    v = 100;
                levels[count++] = (uint8_t)v;

                if (!comma)
                    break;
                p = comma + 1;
            }

            if (count == 0u)
                count = 3u;

            display_show_system_hint("SPK_VOLUME");
            speaker_volume_done_pending = 1u;
            pump_speaker_test_response(1200u);
            (void)i2s_mic_play_volume_steps(levels, count);
        }
        else if (strcmp(line, "SPK_ODE") == 0)
        {
            display_show_system_hint("SPK_ODE");
            if (i2s_mic_play_ode_to_joy())
            {
                speaker_ode_done_pending = 1u;
                pump_speaker_test_response(1500u);
            }
            else
            {
                display_show_system_hint("SPK_ODE_FAIL");
            }
        }
        else if (strncmp(line, "SPK_WAV:", 8) == 0)
        {
            /* SPK_WAV:<pcm_size>:<sample_rate> */
            uint32_t pcm_size = 0u;
            uint16_t sample_rate = 16000u;
            char *rate_part = NULL;

            pcm_size = (uint32_t)strtoul(line + 8, NULL, 10);
            rate_part = strchr(line + 8, ':');
            if (rate_part)
            {
                sample_rate = (uint16_t)atoi(rate_part + 1);
            }

            display_show_system_hint("SPK_WAV");
            display_update_bottom_hint("正在播放AI语音...");

            if (pcm_size == 0u)
            {
                speaker_wav_done_pending = 1u;
                wav_stream_active = 0u;
            }
            else if (!i2s_mic_play_wav_stream(sample_rate))
            {
                display_show_system_hint("SPK_WAV_ERR");
                speaker_wav_done_pending = 1u;
                wav_stream_active = 0u;
            }
            else
            {
                wav_stream_init(pcm_size, sample_rate);
                wav_stream_active = 1u;
                break;   /* 立即退出，后续 PCM 数据留给 wav_stream_service */
            }
        }
        else if (strncmp(line, "MIC_REC:", 8) == 0)
        {
            uint32_t duration_sec = (uint32_t)atoi(line + 8);
            mic_rec_test_start(duration_sec);
        }
    }
}
