#include "button/button_context.h"

void try_send_speaker_test_ok(void)
{
    esp8266_send_result_t send_ret;

    if (!speaker_test_ok_pending)
        return;

    if (!tcp_ready_for_send())
        return;

    if (esp8266_tx_in_progress())
        return;

    send_ret = esp8266_tcp_send_line_async("SPK_OK");
    if (send_ret == ESP8266_SEND_OK)
    {
        speaker_test_ok_pending = 0u;
        display_show_system_hint("SPK_OK");
        if (sys_state == STATE_IDLE)
        {
            display_update_bottom_hint("K1:录音 K2:发送/新建");
        }
    }
}


void try_send_speaker_tone_done(void)
{
    esp8266_send_result_t send_ret;

    if (!speaker_tone_done_pending)
        return;

    if (!tcp_ready_for_send())
        return;

    if (esp8266_tx_in_progress())
        return;

    send_ret = esp8266_tcp_send_line_async("SPK_TONE_DONE");
    if (send_ret == ESP8266_SEND_OK)
    {
        speaker_tone_done_pending = 0u;
        display_show_system_hint("SPK_TONE_DONE");
        if (sys_state == STATE_IDLE)
        {
            display_update_bottom_hint("K1:录音 K2:发送/新建");
        }
    }
}


void try_send_speaker_sweep_done(void)
{
    esp8266_send_result_t send_ret;

    if (!speaker_sweep_done_pending)
        return;

    if (!tcp_ready_for_send())
        return;

    if (esp8266_tx_in_progress())
        return;

    send_ret = esp8266_tcp_send_line_async("SPK_SWEEP_DONE");
    if (send_ret == ESP8266_SEND_OK)
    {
        speaker_sweep_done_pending = 0u;
        display_show_system_hint("SPK_SWEEP_DONE");
        if (sys_state == STATE_IDLE)
        {
            display_update_bottom_hint("K1:录音 K2:发送/新建");
        }
    }
}


void try_send_speaker_volume_done(void)
{
    esp8266_send_result_t send_ret;

    if (!speaker_volume_done_pending)
        return;

    if (!tcp_ready_for_send())
        return;

    if (esp8266_tx_in_progress())
        return;

    send_ret = esp8266_tcp_send_line_async("SPK_VOLUME_DONE");
    if (send_ret == ESP8266_SEND_OK)
    {
        speaker_volume_done_pending = 0u;
        display_show_system_hint("SPK_VOLUME_DONE");
        if (sys_state == STATE_IDLE)
        {
            display_update_bottom_hint("K1:录音 K2:发送/新建");
        }
    }
}


void try_send_speaker_ode_done(void)
{
    esp8266_send_result_t send_ret;

    if (!speaker_ode_done_pending)
        return;

    if (!tcp_ready_for_send())
        return;

    if (esp8266_tx_in_progress())
        return;

    send_ret = esp8266_tcp_send_line_async("SPK_ODE_DONE");
    if (send_ret == ESP8266_SEND_OK)
    {
        speaker_ode_done_pending = 0u;
        display_show_system_hint("SPK_ODE_DONE");
        if (sys_state == STATE_IDLE)
        {
            display_update_bottom_hint("K1:录音 K2:发送/新建");
        }
    }
}


void pump_speaker_test_response(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < timeout_ms)
    {
        try_send_speaker_test_ok();
        try_send_speaker_tone_done();
        try_send_speaker_sweep_done();
        try_send_speaker_volume_done();
        try_send_speaker_ode_done();
        esp8266_poll();

        if (!speaker_test_ok_pending &&
            !speaker_tone_done_pending &&
            !speaker_sweep_done_pending &&
            !speaker_volume_done_pending &&
            !speaker_ode_done_pending &&
            !esp8266_tx_in_progress() &&
            esp8266_tx_queue_is_empty())
        {
            break;
        }
    }
}

