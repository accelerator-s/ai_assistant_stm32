#include "button/button_context.h"

static void mic_probe_reset_stats(void)
{
    __disable_irq();
    mic_probe_nonzero_count = 0;
    mic_probe_sample_count = 0;
    __enable_irq();
}


void mic_probe_start(void)
{
    if (mic_probe_state != MIC_PROBE_IDLE)
        return;

    mic_probe_reset_stats();
    mic_probe_collecting = 1;
    mic_probe_start_tick = HAL_GetTick();
    mic_probe_state = MIC_PROBE_RUNNING;

    if (!i2s_mic_is_recording())
    {
        i2s_mic_start();
    }

    display_show_system_hint("硬件检测中...");
    display_update_bottom_hint("正在检测麦克风连接...");
}


static void mic_probe_finish(uint8_t success)
{
    mic_probe_collecting = 0;
    if (i2s_mic_is_recording() && sys_state != STATE_RECORDING)
    {
        i2s_mic_stop();
    }

    mic_probe_state = MIC_PROBE_IDLE;

    if (success)
    {
        (void)esp8266_tcp_send_line_async("MIC_OK");
        display_show_system_hint("麦克风硬件检测通过");
    }
    else
    {
        (void)esp8266_tcp_send_line_async("MIC_FAIL");
        display_show_system_hint("麦克风硬件检测失败");
    }

    if (sys_state == STATE_IDLE)
    {
        display_update_bottom_hint("K1:录音 K2:发送/新建");
    }
}


void mic_probe_poll(void)
{
    uint16_t nonzero_count, sample_count;

    if (mic_probe_state != MIC_PROBE_RUNNING)
        return;

    __disable_irq();
    nonzero_count = mic_probe_nonzero_count;
    sample_count = mic_probe_sample_count;
    __enable_irq();

    if (nonzero_count >= MIC_PROBE_MIN_NONZERO_SAMPLES &&
        sample_count >= MIC_PROBE_MIN_TOTAL_SAMPLES)
    {
        mic_probe_finish(1u);
        return;
    }

    if ((HAL_GetTick() - mic_probe_start_tick) >= MIC_PROBE_TIMEOUT_MS)
    {
        mic_probe_finish(0u);
    }
}


void mic_rec_test_start(uint32_t duration_sec)
{
    if (mic_rec_test_state != MIC_REC_TEST_IDLE)
        return;

    if (duration_sec < 1u)
        duration_sec = 1u;
    if (duration_sec > 10u)
        duration_sec = 10u;

    mic_rec_test_duration_ms = duration_sec * 1000u;
    mic_rec_test_start_tick = HAL_GetTick();
    mic_rec_test_state = MIC_REC_TEST_RUNNING;
    rec_end_pending = 0u;
    mic_rec_done_pending = 0u;
    audio_buffer_reset();

    (void)esp8266_tcp_send_line_async("REC_START");

    if (!i2s_mic_is_recording())
    {
        i2s_mic_start();
    }

    display_show_system_hint("录音测试中...");
    display_update_bottom_hint("请稍候，正在采集测试音频...");
}


static void mic_rec_test_finish(void)
{
    if (mic_rec_test_state != MIC_REC_TEST_RUNNING)
        return;

    if (i2s_mic_is_recording() && sys_state != STATE_RECORDING)
    {
        i2s_mic_stop();
    }

    mic_rec_test_state = MIC_REC_TEST_IDLE;
    /* 不在此处阻塞发送，由主循环的 audio_upload_poll() 排干缓冲，
     * 排干后 try_send_mic_rec_done() 自动发送 MIC_REC_DONE:sr=8000 */
    mic_rec_done_pending = 1u;
    display_show_system_hint("录音测试完成，上传中...");

    if (sys_state == STATE_IDLE)
    {
        display_update_bottom_hint("数据上传中...");
    }
}


void mic_rec_test_poll(void)
{
    if (mic_rec_test_state != MIC_REC_TEST_RUNNING)
        return;

    if ((HAL_GetTick() - mic_rec_test_start_tick) >= mic_rec_test_duration_ms)
    {
        mic_rec_test_finish();
    }
}

