#include "button/button_context.h"

static uint16_t ring_distance(uint16_t from, uint16_t to, uint16_t size)
{
    if (to >= from)
        return (uint16_t)(to - from);
    return (uint16_t)(size - from + to);
}


void audio_buffer_reset(void)
{
    __disable_irq();
    audio_upload_read_pos = 0;
    audio_upload_write_pos = 0;
    audio_upload_overflow = 0;
    audio_upload_dropped_samples = 0;
    audio_upload_downsample_acc = 0;
    audio_upload_downsample_count = 0;
    __enable_irq();
}


uint16_t audio_buffer_available(void)
{
    uint16_t read_pos, write_pos;
    __disable_irq();
    read_pos = audio_upload_read_pos;
    write_pos = audio_upload_write_pos;
    __enable_irq();
    return ring_distance(read_pos, write_pos, AUDIO_UPLOAD_BUFFER_SAMPLES);
}


int16_t audio_upload_apply_gain(int16_t sample)
{
    int32_t amplified = ((int32_t)sample * (int32_t)AUDIO_UPLOAD_GAIN_Q8) >> 8;

    if (amplified > 32767)
        return 32767;
    if (amplified < -32768)
        return -32768;

    return (int16_t)amplified;
}


void on_audio_data(const int16_t *buf, uint16_t len)
{
    uint16_t i;

    if (!buf || len == 0)
        return;

    if (mic_probe_collecting)
    {
        for (i = 0; i < len; i++)
        {
            int16_t s = buf[i];
            if (s != 0 && s != (int16_t)0xFFFF)
            {
                mic_probe_nonzero_count++;
            }
            mic_probe_sample_count++;
        }
    }

    if (sys_state != STATE_RECORDING && mic_rec_test_state != MIC_REC_TEST_RUNNING)
        return;

    /* 抽取降采样前先做简单均值滤波（抗混叠）
     * 对每 AUDIO_UPLOAD_DOWNSAMPLE 个样本取平均值，而不是直接丢弃 */
    for (i = 0; i < len; i++)
    {
        int16_t sample;
        uint16_t next;

        audio_upload_downsample_acc += (int32_t)buf[i];
        audio_upload_downsample_count++;
        if (audio_upload_downsample_count < AUDIO_UPLOAD_DOWNSAMPLE)
            continue;

        sample = (int16_t)(audio_upload_downsample_acc / (int32_t)AUDIO_UPLOAD_DOWNSAMPLE);
        sample = audio_upload_apply_gain(sample);
        audio_upload_downsample_acc = 0;
        audio_upload_downsample_count = 0;

        next = (uint16_t)((audio_upload_write_pos + 1u) % AUDIO_UPLOAD_BUFFER_SAMPLES);
        if (next == audio_upload_read_pos)
        {
            audio_upload_overflow = 1u;
            audio_upload_dropped_samples++;
            audio_upload_read_pos = (uint16_t)((audio_upload_read_pos + 1u) % AUDIO_UPLOAD_BUFFER_SAMPLES);
        }
        audio_upload_buffer[audio_upload_write_pos] = sample;
        audio_upload_write_pos = next;
    }
}

