/**
 * @file  audio_processor.c
 * @brief 音频处理器实现
 */

#include "audio/audio_processor.h"
#include "audio/dsp_config.h"
#include <string.h>
#include <math.h>

/* 默认配置 - 使用编译时常量 */
static const audio_processing_config_t DEFAULT_AUDIO_CONFIG = {
    .vad = {
        .energy_threshold = 1036,      /* -30dB in Q15: 0.0316227766 * 32767 = 1036 */
        .silence_duration_ms = 300,
        .speech_duration_ms = 100,
        .hangover_duration_ms = 200,
        .enable_adaptive_threshold = true,
        .noise_floor = 32              /* -60dB in Q15: 0.001 * 32767 = 32 */
    },
    .agc = {
        .target_level = 3276,          /* -20dBFS in Q15: 0.1 * 32767 = 3276 */
        .max_gain = 32767,             /* 20dB in Q15: 10.0 * 32767 = 327670, clamped to 32767 (1.0) */
        .min_gain = 10362,             /* -10dB in Q15: 0.316227766 * 32767 = 10362 */
        .attack_time_ms = 10,
        .release_time_ms = 100,
        .enable_limiter = true,
        .limiter_threshold = 23200,    /* -3dBFS in Q15: 0.707945784 * 32767 = 23200 */
        .agc_mode = 1                  /* RMS AGC */
    },
    .filter = {
        .enable_preemphasis = true,
        .preemphasis_coeff = 31784,    /* 0.97 in Q15: 0.97 * 32767 = 31784 */
        .enable_hp_filter = true,
        .hp_filter_order = 32,
        .hp_cutoff_freq = 80,          /* 80Hz高通 */
        .enable_noise_gate = true,
        .noise_gate_threshold = 327,   /* -40dB in Q15: 0.01 * 32767 = 327 */
        .noise_gate_attack_ms = 10,
        .noise_gate_release_ms = 100
    },
    .quality = {
        .overload_threshold = 23200,   /* -3dBFS in Q15: 0.707945784 * 32767 = 23200 */
        .stats_window_ms = 1000,
        .enable_snr_estimation = true,
        .enable_distortion_detect = true,
        .distortion_threshold = 5
    },
    .enable_audio_processing = true,
    .processing_priority = 10,
    .input_gain = 100,                 /* 100% */
    .output_gain = 100,                /* 100% */
    .max_cpu_usage_percent = 30,
    .max_memory_kb = 40,
    .max_processing_delay_ms = 5
};

/* 私有函数声明 */
static bool audio_processor_init_vad(audio_processor_t *processor);
static bool audio_processor_init_agc(audio_processor_t *processor);
static bool audio_processor_init_filter(audio_processor_t *processor);
static void audio_processor_update_vad(audio_processor_t *processor, q15_t sample);
static q15_t audio_processor_update_agc(audio_processor_t *processor, q15_t sample);
static q15_t audio_processor_update_filter(audio_processor_t *processor, q15_t sample);

/* 初始化音频处理器 */
bool audio_processor_init(audio_processor_t *processor, const audio_processing_config_t *config) {
    if (processor == NULL) {
        return false;
    }

    /* 重置处理器 */
    memset(processor, 0, sizeof(audio_processor_t));

    /* 使用默认配置或传入配置 */
    if (config != NULL) {
        processor->config = *config;
    } else {
        processor->config = DEFAULT_AUDIO_CONFIG;
    }

    /* 初始化质量监控器 */
    if (!audio_quality_init(&processor->quality_monitor, &processor->config.quality)) {
        return false;
    }

    /* 初始化VAD模块 */
    if (!audio_processor_init_vad(processor)) {
        return false;
    }

    /* 初始化AGC模块 */
    if (!audio_processor_init_agc(processor)) {
        return false;
    }

    /* 初始化滤波器模块 */
    if (!audio_processor_init_filter(processor)) {
        return false;
    }

    /* 设置模块状态 */
    for (int i = 0; i < MODULE_COUNT; i++) {
        processor->modules[i].is_initialized = true;
        processor->modules[i].processed_samples = 0;
        processor->modules[i].error_count = 0;
    }

    /* 根据配置启用模块 */
    processor->modules[MODULE_PREEMPHASIS].is_enabled = processor->config.filter.enable_preemphasis;
    processor->modules[MODULE_HP_FILTER].is_enabled = processor->config.filter.enable_hp_filter;
    processor->modules[MODULE_NOISE_GATE].is_enabled = processor->config.filter.enable_noise_gate;
    processor->modules[MODULE_AGC].is_enabled = true;  /* AGC总是启用 */
    processor->modules[MODULE_VAD].is_enabled = true;  /* VAD总是启用 */

    processor->is_initialized = true;
    processor->is_processing = true;
    processor->is_paused = false;
    processor->has_error = false;

    return true;
}

/* 初始化VAD模块 */
static bool audio_processor_init_vad(audio_processor_t *processor) {
    if (processor == NULL) {
        return false;
    }

    vad_state_t *vad = &processor->vad;
    const vad_config_t *config = &processor->config.vad;

    /* 初始化VAD参数 */
    vad->energy_threshold = config->energy_threshold;
    vad->silence_duration_ms = config->silence_duration_ms;
    vad->speech_duration_ms = config->speech_duration_ms;
    vad->hangover_duration_ms = config->hangover_duration_ms;
    vad->noise_floor = config->noise_floor;

    vad->current_energy = 0;
    vad->silence_counter = 0;
    vad->speech_counter = 0;
    vad->hangover_counter = 0;

    vad->is_speech_active = false;
    vad->is_speech_started = false;
    vad->is_speech_ended = false;

    vad->sample_counter = 0;
    vad->energy_index = 0;

    /* 初始化高通滤波器（去除直流） */
    if (config->enable_adaptive_threshold) {
        /* 设计一个简单的高通滤波器 */
        q15_t hp_coeffs[32];
        if (!dsp_design_hp_filter_q15(hp_coeffs, 32, 80, AUDIO_SAMPLE_RATE)) {
            return false;
        }

        arm_fir_init_q15(&vad->hp_filter, 32, hp_coeffs, vad->hp_filter_state, 64);
    }

    return true;
}

/* 初始化AGC模块 */
static bool audio_processor_init_agc(audio_processor_t *processor) {
    if (processor == NULL) {
        return false;
    }

    agc_state_t *agc = &processor->agc;
    const agc_config_t *config = &processor->config.agc;

    /* 初始化AGC参数 */
    agc->target_level = config->target_level;
    agc->max_gain = config->max_gain;
    agc->min_gain = config->min_gain;
    agc->enable_limiter = config->enable_limiter;
    agc->limiter_threshold = config->limiter_threshold;
    agc->agc_mode = config->agc_mode;

    /* 计算攻击和释放系数 */
    float attack_alpha = 1.0f - expf(-1.0f / (config->attack_time_ms * AUDIO_SAMPLE_RATE / 1000.0f));
    float release_alpha = 1.0f - expf(-1.0f / (config->release_time_ms * AUDIO_SAMPLE_RATE / 1000.0f));

    agc->attack_coeff = dsp_float_to_q15(attack_alpha);
    agc->release_coeff = dsp_float_to_q15(release_alpha);

    /* 初始化状态 */
    agc->current_gain = dsp_db_to_q15(0.0f);  /* 0dB初始增益 */
    agc->envelope = 0;

    agc->rms_index = 0;
    agc->peak_index = 0;

    return true;
}

/* 初始化滤波器模块 */
static bool audio_processor_init_filter(audio_processor_t *processor) {
    if (processor == NULL) {
        return false;
    }

    filter_state_t *filter = &processor->filter;
    const filter_config_t *config = &processor->config.filter;

    /* 初始化预加重 */
    filter->enable_preemphasis = config->enable_preemphasis;
    filter->preemphasis_coeff = config->preemphasis_coeff;
    filter->preemphasis_state = 0;

    /* 初始化高通滤波器 */
    filter->enable_hp_filter = config->enable_hp_filter;
    if (config->enable_hp_filter) {
        /* 设计高通滤波器 */
        if (!dsp_design_hp_filter_q15(filter->hp_filter_coeffs, config->hp_filter_order,
                                     config->hp_cutoff_freq, AUDIO_SAMPLE_RATE)) {
            return false;
        }

        arm_fir_init_q15(&filter->hp_filter, config->hp_filter_order,
                        filter->hp_filter_coeffs, filter->hp_filter_state, 128);
    }

    /* 初始化噪声门 */
    filter->enable_noise_gate = config->enable_noise_gate;
    if (config->enable_noise_gate) {
        filter->noise_gate_threshold = config->noise_gate_threshold;
        filter->noise_gate_attack_ms = config->noise_gate_attack_ms;
        filter->noise_gate_release_ms = config->noise_gate_release_ms;

        filter->noise_gate_gain = dsp_float_to_q15(1.0f);  /* 初始增益1.0 */
        filter->noise_gate_envelope = 0;
    }

    return true;
}

/* 处理单个样本 */
q15_t audio_processor_process_sample(audio_processor_t *processor, q15_t input_sample) {
    if (processor == NULL || !processor->is_initialized || !processor->is_processing || processor->is_paused) {
        return input_sample;
    }

    q15_t processed_sample = input_sample;

    /* 应用输入增益 */
    if (processor->config.input_gain != 100) {
        float gain = processor->config.input_gain / 100.0f;
        processed_sample = dsp_saturate_mul_q15(processed_sample, dsp_float_to_q15(gain));
    }

    /* 更新VAD（在处理前，使用原始信号） */
    audio_processor_update_vad(processor, processed_sample);

    /* 滤波器处理 */
    if (processor->modules[MODULE_PREEMPHASIS].is_enabled ||
        processor->modules[MODULE_HP_FILTER].is_enabled ||
        processor->modules[MODULE_NOISE_GATE].is_enabled) {
        processed_sample = audio_processor_update_filter(processor, processed_sample);
        processor->modules[MODULE_PREEMPHASIS].processed_samples++;
        processor->modules[MODULE_HP_FILTER].processed_samples++;
        processor->modules[MODULE_NOISE_GATE].processed_samples++;
    }

    /* AGC处理 */
    if (processor->modules[MODULE_AGC].is_enabled) {
        processed_sample = audio_processor_update_agc(processor, processed_sample);
        processor->modules[MODULE_AGC].processed_samples++;
    }

    /* 应用输出增益 */
    if (processor->config.output_gain != 100) {
        float gain = processor->config.output_gain / 100.0f;
        processed_sample = dsp_saturate_mul_q15(processed_sample, dsp_float_to_q15(gain));
    }

    /* 更新质量监控 */
    audio_quality_update_sample(&processor->quality_monitor, processed_sample);

    /* 更新统计 */
    processor->total_samples_processed++;

    return processed_sample;
}

/* 更新VAD状态 */
static void audio_processor_update_vad(audio_processor_t *processor, q15_t sample) {
    if (processor == NULL || !processor->modules[MODULE_VAD].is_enabled) {
        return;
    }

    vad_state_t *vad = &processor->vad;

    /* 去除直流偏移 */
    q15_t filtered_sample = sample;
    if (processor->config.vad.enable_adaptive_threshold) {
        q15_t input = sample;
        q15_t output;
        arm_fir_q15(&vad->hp_filter, &input, &output, 1);
        filtered_sample = output;
    }

    /* 计算能量 */
    q15_t abs_sample = (filtered_sample < 0) ? -filtered_sample : filtered_sample;
    vad->energy_buffer[vad->energy_index] = abs_sample;
    vad->energy_index = (vad->energy_index + 1) % 32;

    /* 每32个样本更新一次能量 */
    if (vad->sample_counter % 32 == 0) {
        q15_t sum = 0;
        for (int i = 0; i < 32; i++) {
            sum += vad->energy_buffer[i];
        }
        vad->current_energy = sum / 32;

        /* 自适应阈值 */
        if (processor->config.vad.enable_adaptive_threshold) {
            /* 更新噪声底噪估计 */
            if (vad->current_energy < vad->noise_floor) {
                vad->noise_floor = vad->current_energy;
            } else {
                /* 缓慢上升 */
                vad->noise_floor += (vad->current_energy - vad->noise_floor) >> 4;
            }

            /* 设置阈值为噪声底噪+12dB */
            q15_t threshold = dsp_saturate_mul_q15(vad->noise_floor, dsp_db_to_q15(12.0f));
            if (threshold > vad->energy_threshold) {
                vad->energy_threshold = threshold;
            }
        }
    }

    /* 语音活动检测 */
    bool is_speech = (vad->current_energy > vad->energy_threshold);

    if (is_speech) {
        vad->silence_counter = 0;
        vad->speech_counter++;

        /* 检查是否开始语音 */
        if (!vad->is_speech_active) {
            if (vad->speech_counter * 32 >= (vad->speech_duration_ms * AUDIO_SAMPLE_RATE / 1000)) {
                vad->is_speech_active = true;
                vad->is_speech_started = true;
                vad->hangover_counter = vad->hangover_duration_ms * AUDIO_SAMPLE_RATE / 1000;
            }
        } else {
            vad->is_speech_started = false;
            /* 重置拖尾计数器 */
            vad->hangover_counter = vad->hangover_duration_ms * AUDIO_SAMPLE_RATE / 1000;
        }
    } else {
        vad->speech_counter = 0;
        vad->silence_counter++;

        if (vad->is_speech_active) {
            if (vad->hangover_counter > 0) {
                vad->hangover_counter--;
            } else {
                /* 检查是否结束语音 */
                if (vad->silence_counter * 32 >= (vad->silence_duration_ms * AUDIO_SAMPLE_RATE / 1000)) {
                    vad->is_speech_active = false;
                    vad->is_speech_ended = true;
                }
            }
        } else {
            vad->is_speech_ended = false;
        }
    }

    vad->sample_counter++;
    processor->modules[MODULE_VAD].processed_samples++;
}

/* 更新AGC */
static q15_t audio_processor_update_agc(audio_processor_t *processor, q15_t sample) {
    if (processor == NULL) {
        return sample;
    }

    agc_state_t *agc = &processor->agc;

    /* 计算信号电平 */
    q15_t signal_level;
    if (agc->agc_mode == 0) {
        /* 峰值检测 */
        q15_t abs_sample = (sample < 0) ? -sample : sample;
        agc->peak_buffer[agc->peak_index] = abs_sample;
        agc->peak_index = (agc->peak_index + 1) % 32;

        /* 查找峰值 */
        signal_level = 0;
        for (int i = 0; i < 32; i++) {
            if (agc->peak_buffer[i] > signal_level) {
                signal_level = agc->peak_buffer[i];
            }
        }
    } else {
        /* RMS检测 */
        q15_t square = dsp_saturate_mul_q15(sample, sample);
        agc->rms_buffer[agc->rms_index] = square;
        agc->rms_index = (agc->rms_index + 1) % 32;

        /* 计算RMS */
        q31_t sum = 0;
        for (int i = 0; i < 32; i++) {
            sum += agc->rms_buffer[i];
        }
        signal_level = (q15_t)(sum / 32);
        signal_level = (q15_t)sqrtf(dsp_q15_to_float(signal_level)) * 32767.0f;
    }

    /* 更新包络 */
    if (signal_level > agc->envelope) {
        /* 攻击 */
        q15_t diff = signal_level - agc->envelope;
        agc->envelope += dsp_saturate_mul_q15(diff, agc->attack_coeff);
    } else {
        /* 释放 */
        q15_t diff = agc->envelope - signal_level;
        agc->envelope -= dsp_saturate_mul_q15(diff, agc->release_coeff);
    }

    /* 计算所需增益 */
    q15_t desired_gain;
    if (agc->envelope > 0) {
        q31_t gain_ratio = (q31_t)agc->target_level * 32767;
        gain_ratio = gain_ratio / (q31_t)agc->envelope;
        desired_gain = (q15_t)gain_ratio;
    } else {
        desired_gain = agc->max_gain;
    }

    /* 限制增益范围 */
    if (desired_gain > agc->max_gain) {
        desired_gain = agc->max_gain;
    }
    if (desired_gain < agc->min_gain) {
        desired_gain = agc->min_gain;
    }

    /* 平滑增益变化 */
    if (desired_gain > agc->current_gain) {
        q15_t diff = desired_gain - agc->current_gain;
        agc->current_gain += dsp_saturate_mul_q15(diff, agc->attack_coeff);
    } else {
        q15_t diff = agc->current_gain - desired_gain;
        agc->current_gain -= dsp_saturate_mul_q15(diff, agc->release_coeff);
    }

    /* 应用增益 */
    q15_t output = dsp_saturate_mul_q15(sample, agc->current_gain);

    /* 限幅器 */
    if (agc->enable_limiter) {
        q15_t abs_output = (output < 0) ? -output : output;
        if (abs_output > agc->limiter_threshold) {
            q31_t gain = (q31_t)agc->limiter_threshold * 32767;
            gain = gain / (q31_t)abs_output;
            output = dsp_saturate_mul_q15(output, (q15_t)gain);
        }
    }

    return output;
}

/* 更新滤波器 */
static q15_t audio_processor_update_filter(audio_processor_t *processor, q15_t sample) {
    if (processor == NULL) {
        return sample;
    }

    filter_state_t *filter = &processor->filter;
    q15_t processed_sample = sample;

    /* 预加重滤波 */
    if (filter->enable_preemphasis) {
        q15_t output = processed_sample - dsp_saturate_mul_q15(filter->preemphasis_state, filter->preemphasis_coeff);
        filter->preemphasis_state = processed_sample;
        processed_sample = output;
    }

    /* 高通滤波 */
    if (filter->enable_hp_filter) {
        q15_t input = processed_sample;
        q15_t output;
        arm_fir_q15(&filter->hp_filter, &input, &output, 1);
        processed_sample = output;
    }

    /* 噪声门 */
    if (filter->enable_noise_gate) {
        q15_t abs_sample = (processed_sample < 0) ? -processed_sample : processed_sample;

        /* 更新包络 */
        if (abs_sample > filter->noise_gate_envelope) {
            /* 攻击 */
            q15_t diff = abs_sample - filter->noise_gate_envelope;
            float attack_alpha = 1.0f - expf(-1.0f / (filter->noise_gate_attack_ms * AUDIO_SAMPLE_RATE / 1000.0f));
            filter->noise_gate_envelope += dsp_saturate_mul_q15(diff, dsp_float_to_q15(attack_alpha));
        } else {
            /* 释放 */
            q15_t diff = filter->noise_gate_envelope - abs_sample;
            float release_alpha = 1.0f - expf(-1.0f / (filter->noise_gate_release_ms * AUDIO_SAMPLE_RATE / 1000.0f));
            filter->noise_gate_envelope -= dsp_saturate_mul_q15(diff, dsp_float_to_q15(release_alpha));
        }

        /* 计算噪声门增益 */
        if (filter->noise_gate_envelope < filter->noise_gate_threshold) {
            /* 低于阈值，应用衰减 */
            q31_t gain_ratio = (q31_t)filter->noise_gate_envelope * 32767;
            gain_ratio = gain_ratio / (q31_t)filter->noise_gate_threshold;
            filter->noise_gate_gain = (q15_t)gain_ratio;
        } else {
            /* 高于阈值，增益为1 */
            filter->noise_gate_gain = 32767;  /* Q15的1.0 */
        }

        /* 应用噪声门增益 */
        processed_sample = dsp_saturate_mul_q15(processed_sample, filter->noise_gate_gain);
    }

    return processed_sample;
}

/* 处理一批样本 */
uint32_t audio_processor_process_block(audio_processor_t *processor,
                                      q15_t *input_samples,
                                      q15_t *output_samples,
                                      uint32_t sample_count) {
    if (processor == NULL || input_samples == NULL || output_samples == NULL || sample_count == 0) {
        return 0;
    }

    if (!processor->is_initialized || !processor->is_processing || processor->is_paused) {
        /* 如果处理器未启用，直接复制数据 */
        memcpy(output_samples, input_samples, sample_count * sizeof(q15_t));
        return sample_count;
    }

    uint32_t processed_count = 0;

    for (uint32_t i = 0; i < sample_count; i++) {
        output_samples[i] = audio_processor_process_sample(processor, input_samples[i]);
        processed_count++;
    }

    return processed_count;
}

/* 处理16位PCM数据 */
uint32_t audio_processor_process_pcm16(audio_processor_t *processor,
                                      int16_t *input_pcm,
                                      int16_t *output_pcm,
                                      uint32_t sample_count) {
    if (processor == NULL || input_pcm == NULL || output_pcm == NULL || sample_count == 0) {
        return 0;
    }

    /* 检查缓冲区大小 */
    if (sample_count > 32) {
        /* 分批处理 */
        uint32_t total_processed = 0;
        uint32_t remaining = sample_count;

        while (remaining > 0) {
            uint32_t chunk_size = (remaining > 32) ? 32 : remaining;

            /* 转换到Q15格式 */
            for (uint32_t i = 0; i < chunk_size; i++) {
                processor->input_buffer[i] = (q15_t)input_pcm[total_processed + i];
            }

            /* 处理 */
            uint32_t processed = audio_processor_process_block(processor,
                                                              processor->input_buffer,
                                                              processor->output_buffer,
                                                              chunk_size);

            /* 转换回int16 */
            for (uint32_t i = 0; i < processed; i++) {
                output_pcm[total_processed + i] = (int16_t)processor->output_buffer[i];
            }

            total_processed += processed;
            remaining -= chunk_size;

            if (processed < chunk_size) {
                /* 处理失败或部分处理 */
                break;
            }
        }

        return total_processed;
    } else {
        /* 单次处理 */
        /* 转换到Q15格式 */
        for (uint32_t i = 0; i < sample_count; i++) {
            processor->input_buffer[i] = (q15_t)input_pcm[i];
        }

        /* 处理 */
        uint32_t processed = audio_processor_process_block(processor,
                                                          processor->input_buffer,
                                                          processor->output_buffer,
                                                          sample_count);

        /* 转换回int16 */
        for (uint32_t i = 0; i < processed; i++) {
            output_pcm[i] = (int16_t)processor->output_buffer[i];
        }

        return processed;
    }
}

/* 获取VAD状态 */
bool audio_processor_is_speech_active(const audio_processor_t *processor) {
    if (processor == NULL || !processor->is_initialized) {
        return false;
    }
    return processor->vad.is_speech_active;
}

bool audio_processor_is_speech_started(const audio_processor_t *processor) {
    if (processor == NULL || !processor->is_initialized) {
        return false;
    }
    return processor->vad.is_speech_started;
}

bool audio_processor_is_speech_ended(const audio_processor_t *processor) {
    if (processor == NULL || !processor->is_initialized) {
        return false;
    }
    return processor->vad.is_speech_ended;
}

/* 获取AGC增益 */
q15_t audio_processor_get_current_gain(const audio_processor_t *processor) {
    if (processor == NULL || !processor->is_initialized) {
        return 32767;  /* Q15的1.0 */
    }
    return processor->agc.current_gain;
}

/* 获取处理统计 */
void audio_processor_get_stats(const audio_processor_t *processor,
                              uint32_t *processed,
                              uint32_t *dropped,
                              uint32_t *errors) {
    if (processor == NULL) {
        if (processed) *processed = 0;
        if (dropped) *dropped = 0;
        if (errors) *errors = 0;
        return;
    }

    if (processed) *processed = processor->total_samples_processed;
    if (dropped) *dropped = processor->total_samples_dropped;
    if (errors) *errors = processor->total_errors;
}

/* 重置处理器 */
void audio_processor_reset(audio_processor_t *processor) {
    if (processor == NULL) {
        return;
    }

    /* 保存配置 */
    audio_processing_config_t config = processor->config;

    /* 重新初始化 */
    audio_processor_init(processor, &config);
}

/* 暂停/恢复处理 */
void audio_processor_pause(audio_processor_t *processor) {
    if (processor == NULL || !processor->is_initialized) {
        return;
    }
    processor->is_paused = true;
}

void audio_processor_resume(audio_processor_t *processor) {
    if (processor == NULL || !processor->is_initialized) {
        return;
    }
    processor->is_paused = false;
}

/* 更新配置 */
bool audio_processor_update_config(audio_processor_t *processor,
                                  const audio_processing_config_t *config) {
    if (processor == NULL || config == NULL) {
        return false;
    }

    /* 保存当前状态 */
    bool was_processing = processor->is_processing;
    bool was_paused = processor->is_paused;

    /* 暂停处理 */
    processor->is_processing = false;

    /* 更新配置 */
    processor->config = *config;

    /* 重新初始化模块 */
    if (!audio_processor_init_vad(processor) ||
        !audio_processor_init_agc(processor) ||
        !audio_processor_init_filter(processor)) {
        return false;
    }

    /* 更新质量监控器配置 */
    audio_quality_update_config(&processor->quality_monitor, &config->quality);

    /* 恢复处理状态 */
    processor->is_processing = was_processing && !was_paused;
    processor->is_paused = was_paused;

    return true;
}

/* 获取模块启用状态 */
bool audio_processor_is_module_enabled(const audio_processor_t *processor, audio_module_type_t module) {
    if (processor == NULL || module >= MODULE_COUNT) {
        return false;
    }
    return processor->modules[module].is_enabled;
}

/* 启用/禁用模块 */
bool audio_processor_enable_module(audio_processor_t *processor, audio_module_type_t module, bool enable) {
    if (processor == NULL || module >= MODULE_COUNT) {
        return false;
    }

    processor->modules[module].is_enabled = enable;
    return true;
}

/* 获取错误信息 */
const char* audio_processor_get_error_string(const audio_processor_t *processor) {
    if (processor == NULL) {
        return "处理器未初始化";
    }

    if (!processor->is_initialized) {
        return "处理器初始化失败";
    }

    if (processor->has_error) {
        return "处理器内部错误";
    }

    return "无错误";
}

/* 获取处理延迟估计（样本数） */
uint32_t audio_processor_get_processing_delay(const audio_processor_t *processor) {
    if (processor == NULL || !processor->is_initialized) {
        return 0;
    }

    /* 估计各模块延迟 */
    uint32_t delay = 0;

    if (processor->modules[MODULE_HP_FILTER].is_enabled) {
        delay += processor->config.filter.hp_filter_order / 2;
    }

    /* VAD和AGC的延迟较小，主要是缓冲区延迟 */
    delay += 32;  /* 能量计算缓冲区 */

    return delay;
}

/* 调试输出 */
void audio_processor_print_status(const audio_processor_t *processor) {
    if (processor == NULL || !processor->is_initialized) {
        return;
    }

    /* 这里可以输出到串口或日志 */
    /* 示例：输出VAD状态、AGC增益、质量状态等 */
    (void)processor;  /* 防止未使用警告 */
}

/* 性能测试 */
void audio_processor_start_perf_test(audio_processor_perf_t *perf) {
    if (perf == NULL) {
        return;
    }

    memset(perf, 0, sizeof(audio_processor_perf_t));
    /* 在实际实现中，这里会记录开始时间 */
}

void audio_processor_stop_perf_test(audio_processor_perf_t *perf, uint32_t cycles) {
    if (perf == NULL) {
        return;
    }

    perf->total_cycles += cycles;
    perf->sample_count++;

    if (cycles > perf->max_cycles_per_sample) {
        perf->max_cycles_per_sample = cycles;
    }

    if (perf->min_cycles_per_sample == 0 || cycles < perf->min_cycles_per_sample) {
        perf->min_cycles_per_sample = cycles;
    }
}

void audio_processor_print_perf_report(const audio_processor_perf_t *perf, uint32_t cpu_freq_mhz) {
    if (perf == NULL || perf->sample_count == 0) {
        return;
    }

    uint32_t avg_cycles = perf->total_cycles / perf->sample_count;
    float avg_us = (float)avg_cycles / (float)cpu_freq_mhz;
    float max_us = (float)perf->max_cycles_per_sample / (float)cpu_freq_mhz;
    float min_us = (float)perf->min_cycles_per_sample / (float)cpu_freq_mhz;

    /* 这里可以输出到串口或日志 */
    (void)avg_us;
    (void)max_us;
    (void)min_us;
}