#include "button/button_context.h"

void audio_upload_poll(void)
{
    uint16_t available;
    uint16_t read_pos;
    uint16_t write_pos;
    uint16_t contiguous;
    esp8266_send_result_t send_ret;
    uint8_t force_flush_tail;

    if (sys_state != STATE_RECORDING &&
        mic_rec_test_state != MIC_REC_TEST_RUNNING &&
        !rec_end_pending &&
        !mic_rec_done_pending)
        return;

    if (!tcp_ready_for_send())
        return;

    if (esp8266_tx_in_progress())
        return;

    available = audio_buffer_available();
    force_flush_tail = (rec_end_pending || mic_rec_done_pending) ? 1u : 0u;
    if (available < AUDIO_UPLOAD_TRIGGER_SAMPLES && !force_flush_tail)
        return;

    __disable_irq();
    read_pos = audio_upload_read_pos;
    write_pos = audio_upload_write_pos;
    __enable_irq();

    if (write_pos >= read_pos)
    {
        contiguous = (uint16_t)(write_pos - read_pos);
    }
    else
    {
        contiguous = (uint16_t)(AUDIO_UPLOAD_BUFFER_SAMPLES - read_pos);
    }

    if (contiguous > AUDIO_UPLOAD_CHUNK_SAMPLES)
        contiguous = AUDIO_UPLOAD_CHUNK_SAMPLES;

    if (contiguous == 0)
        return;

    send_ret = esp8266_tcp_send_async(
        (const uint8_t *)&audio_upload_buffer[read_pos],
        (uint16_t)(contiguous * sizeof(int16_t)));

    if (send_ret == ESP8266_SEND_OK)
    {
        __disable_irq();
        audio_upload_read_pos = (uint16_t)((audio_upload_read_pos + contiguous) % AUDIO_UPLOAD_BUFFER_SAMPLES);
        __enable_irq();
    }
}


void try_send_rec_end(void)
{
    esp8266_send_result_t send_ret;
    uint16_t remain;

    if (!rec_end_pending)
        return;

    if (!tcp_ready_for_send())
        return;

    remain = audio_buffer_available();
    if (remain > 0u)
        return;

    if (esp8266_tx_in_progress())
        return;

    send_ret = esp8266_tcp_send_line_async("REC_END");
    if (send_ret == ESP8266_SEND_OK)
    {
        rec_end_pending = 0;
    }
}


void try_send_mic_rec_done(void)
{
    esp8266_send_result_t send_ret;
    uint16_t remain;

    if (!mic_rec_done_pending)
        return;

    /* REC_END 和 MIC_REC_DONE 的发送顺序:
     * 如果 rec_end_pending 仍在等待，先等它发完；
     * 如果无 rec_end_pending，等待音频缓冲排干后再发 */
    if (rec_end_pending)
        return;

    /* 等待音频缓冲全部发送完毕 */
    remain = audio_buffer_available();
    if (remain > 0u)
        return;

    if (!tcp_ready_for_send())
        return;

    if (esp8266_tx_in_progress())
        return;

    send_ret = esp8266_tcp_send_line_async("MIC_REC_DONE:sr=8000");
    if (send_ret == ESP8266_SEND_OK)
    {
        mic_rec_done_pending = 0;
        display_show_system_hint("录音测试数据已上传");
        if (sys_state == STATE_IDLE)
        {
            display_update_bottom_hint("K1:录音 K2:发送/新建");
        }
    }
}

