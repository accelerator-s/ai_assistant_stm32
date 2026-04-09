/**
 * @file  audio_quality.c
 * @brief 音频质量监控实现
 */

#include "audio/audio_quality.h"
#include "audio/dsp_config.h"
#include <string.h>
#include <math.h>
#include <stdio.h>  /* 用于snprintf */

/* 私有函数声明 */
static void audio_quality_update_state(audio_quality_monitor_t *monitor);

/* 默认配置 */
static const quality_config_t DEFAULT_QUALITY_CONFIG = {
    .overload_threshold = 23200,  /* -3dBFS in Q15: 0.707945784 * 32767 = 23200 */
    .stats_window_ms = 1000,                     /* 1秒统计窗口 */
    .enable_snr_estimation = true,
    .enable_distortion_detect = true,
    .distortion_threshold = 5                    /* 5%失真阈值 */
};

/* 状态描述字符串 */
static const char* QUALITY_STATE_STRINGS[] = {
    "优秀",
    "良好",
    "一般",
    "较差",
    "差",
    "错误"
};

/* 初始化质量监控器 */
bool audio_quality_init(audio_quality_monitor_t *monitor, const quality_config_t *config) {
    if (monitor == NULL) {
        return false;
    }

    /* 使用默认配置或传入配置 */
    if (config != NULL) {
        monitor->config = *config;
    } else {
        monitor->config = DEFAULT_QUALITY_CONFIG;
    }

    /* 初始化状态 */
    memset(&monitor->metrics, 0, sizeof(audio_quality_metrics_t));
    memset(monitor->history, 0, sizeof(monitor->history));

    monitor->state = QUALITY_STATE_EXCELLENT;
    monitor->sample_counter = 0;
    monitor->window_counter = 0;
    monitor->window_index = 0;

    monitor->noise_floor_estimate = 32;  /* 初始噪声底噪 -60dB in Q15: 0.001 * 32767 = 32 */
    monitor->signal_energy_accum = 0;
    monitor->noise_energy_accum = 0;

    monitor->last_overload_time = 0;
    monitor->last_distortion_time = 0;

    monitor->is_initialized = true;

    return true;
}

/* 更新单个样本的质量统计 */
void audio_quality_update_sample(audio_quality_monitor_t *monitor, q15_t sample) {
    if (monitor == NULL || !monitor->is_initialized) {
        return;
    }

    /* 更新样本计数 */
    monitor->sample_counter++;
    monitor->metrics.total_samples++;

    /* 更新峰值电平 */
    q15_t abs_sample = (sample < 0) ? -sample : sample;
    if (abs_sample > monitor->metrics.peak_level) {
        monitor->metrics.peak_level = abs_sample;
    }

    /* 检查过载 */
    if (abs_sample > monitor->config.overload_threshold) {
        monitor->metrics.overload_count++;
        monitor->last_overload_time = monitor->sample_counter;
    }

    /* 检查失真（削波） */
    if (abs_sample >= 32767) {  /* Q15最大值 */
        monitor->metrics.distortion_count++;
        monitor->last_distortion_time = monitor->sample_counter;
    }

    /* 检查信号丢失（接近0） */
    if (abs_sample < 100) {  /* 很小的值 */
        monitor->metrics.dropout_count++;
    }

    /* 更新窗口缓冲区 */
    monitor->window_buffer[monitor->window_index] = abs_sample;
    monitor->window_index = (monitor->window_index + 1) % 256;
    monitor->window_counter++;

    /* 每256个样本更新一次统计 */
    if (monitor->window_counter >= 256) {
        /* 计算RMS */
        q15_t sum = 0;
        for (uint32_t i = 0; i < 256; i++) {
            q15_t sample_q15 = monitor->window_buffer[i];
            q31_t square = (q31_t)sample_q15 * (q31_t)sample_q15;
            sum += (q15_t)(square >> 15);  /* Q15平方后右移15位 */
        }
        monitor->metrics.rms_level = (q15_t)(sum / 256);

        /* 计算峰值因子 */
        if (monitor->metrics.rms_level > 0) {
            q31_t crest = (q31_t)monitor->metrics.peak_level * 32767;
            crest = crest / (q31_t)monitor->metrics.rms_level;
            monitor->metrics.crest_factor = (q15_t)crest;
        }

        /* 更新SNR估计 */
        if (monitor->config.enable_snr_estimation) {
            /* 简单SNR估计：信号能量/噪声能量 */
            q15_t signal_power = monitor->metrics.rms_level;

            /* 更新噪声底噪估计（使用最小值跟踪） */
            if (signal_power < monitor->noise_floor_estimate) {
                /* 缓慢跟踪下降 */
                monitor->noise_floor_estimate = signal_power;
            } else {
                /* 缓慢上升 */
                monitor->noise_floor_estimate += (signal_power - monitor->noise_floor_estimate) >> 4;
            }

            if (monitor->noise_floor_estimate > 0) {
                q31_t snr = (q31_t)signal_power * 32767;
                snr = snr / (q31_t)monitor->noise_floor_estimate;
                monitor->metrics.snr_estimate = (q15_t)snr;
            }
        }

        /* 保存到历史记录 */
        uint32_t history_index = (monitor->sample_counter / 256) % 10;
        monitor->history[history_index] = monitor->metrics;

        /* 重置窗口计数器 */
        monitor->window_counter = 0;
        monitor->metrics.peak_level = 0;  /* 重置峰值 */
    }

    /* 更新质量状态 */
    audio_quality_update_state(monitor);
}

/* 更新一批样本的质量统计 */
void audio_quality_update_block(audio_quality_monitor_t *monitor, q15_t *samples, uint32_t count) {
    if (monitor == NULL || samples == NULL || count == 0) {
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        audio_quality_update_sample(monitor, samples[i]);
    }
}

/* 更新质量状态 */
static void audio_quality_update_state(audio_quality_monitor_t *monitor) {
    if (monitor == NULL) {
        return;
    }

    /* 检查错误状态 */
    if (!monitor->is_initialized) {
        monitor->state = QUALITY_STATE_ERROR;
        return;
    }

    /* 评估各项指标 */
    bool has_critical_issue = false;
    bool has_major_issue = false;
    bool has_minor_issue = false;

    /* 检查过载 */
    if (monitor->metrics.overload_count > 10) {
        has_major_issue = true;
    } else if (monitor->metrics.overload_count > 0) {
        has_minor_issue = true;
    }

    /* 检查失真 */
    if (monitor->metrics.distortion_count > 5) {
        has_critical_issue = true;
    } else if (monitor->metrics.distortion_count > 0) {
        has_major_issue = true;
    }

    /* 检查信号丢失 */
    if (monitor->metrics.dropout_count > 100) {
        has_major_issue = true;
    } else if (monitor->metrics.dropout_count > 10) {
        has_minor_issue = true;
    }

    /* 检查SNR */
    if (monitor->config.enable_snr_estimation) {
        float snr_db = dsp_q15_to_db(monitor->metrics.snr_estimate);
        if (snr_db < 10.0f) {  /* SNR < 10dB */
            has_critical_issue = true;
        } else if (snr_db < 20.0f) {  /* SNR < 20dB */
            has_major_issue = true;
        } else if (snr_db < 30.0f) {  /* SNR < 30dB */
            has_minor_issue = true;
        }
    }

    /* 确定最终状态 */
    if (has_critical_issue) {
        monitor->state = QUALITY_STATE_BAD;
    } else if (has_major_issue) {
        monitor->state = QUALITY_STATE_POOR;
    } else if (has_minor_issue) {
        monitor->state = QUALITY_STATE_FAIR;
    } else {
        /* 检查电平是否合适 */
        float rms_db = dsp_q15_to_db(monitor->metrics.rms_level);
        if (rms_db > -6.0f && rms_db < -12.0f) {  /* -6dBFS 到 -12dBFS 为最佳 */
            monitor->state = QUALITY_STATE_EXCELLENT;
        } else {
            monitor->state = QUALITY_STATE_GOOD;
        }
    }
}

/* 获取当前质量指标 */
const audio_quality_metrics_t* audio_quality_get_metrics(const audio_quality_monitor_t *monitor) {
    if (monitor == NULL || !monitor->is_initialized) {
        return NULL;
    }
    return &monitor->metrics;
}

/* 获取质量状态 */
audio_quality_state_t audio_quality_get_state(const audio_quality_monitor_t *monitor) {
    if (monitor == NULL || !monitor->is_initialized) {
        return QUALITY_STATE_ERROR;
    }
    return monitor->state;
}

/* 获取质量状态字符串 */
const char* audio_quality_get_state_string(audio_quality_state_t state) {
    if (state >= QUALITY_STATE_EXCELLENT && state <= QUALITY_STATE_ERROR) {
        return QUALITY_STATE_STRINGS[state];
    }
    return "未知";
}

/* 检查是否有严重问题 */
bool audio_quality_has_critical_issue(const audio_quality_monitor_t *monitor) {
    if (monitor == NULL || !monitor->is_initialized) {
        return true;
    }
    return (monitor->state == QUALITY_STATE_BAD || monitor->state == QUALITY_STATE_ERROR);
}

/* 获取问题描述 */
const char* audio_quality_get_issue_description(const audio_quality_monitor_t *monitor) {
    if (monitor == NULL || !monitor->is_initialized) {
        return "监控器未初始化";
    }

    static char description[256];
    description[0] = '\0';

    if (monitor->metrics.overload_count > 0) {
        strcat(description, "过载 ");
    }
    if (monitor->metrics.distortion_count > 0) {
        strcat(description, "失真 ");
    }
    if (monitor->metrics.dropout_count > 0) {
        strcat(description, "信号丢失 ");
    }

    if (monitor->config.enable_snr_estimation) {
        float snr_db = dsp_q15_to_db(monitor->metrics.snr_estimate);
        if (snr_db < 20.0f) {
            char snr_str[32];
            snprintf(snr_str, sizeof(snr_str), "SNR低(%.1fdB) ", snr_db);
            strcat(description, snr_str);
        }
    }

    if (description[0] == '\0') {
        strcpy(description, "无问题");
    }

    return description;
}

/* 重置质量监控器 */
void audio_quality_reset(audio_quality_monitor_t *monitor) {
    if (monitor == NULL) {
        return;
    }

    memset(&monitor->metrics, 0, sizeof(audio_quality_metrics_t));
    monitor->sample_counter = 0;
    monitor->window_counter = 0;
    monitor->window_index = 0;
    monitor->noise_floor_estimate = dsp_db_to_q15(-60.0f);
    monitor->signal_energy_accum = 0;
    monitor->noise_energy_accum = 0;
    monitor->last_overload_time = 0;
    monitor->last_distortion_time = 0;
    monitor->state = QUALITY_STATE_EXCELLENT;
}

/* 更新配置 */
bool audio_quality_update_config(audio_quality_monitor_t *monitor, const quality_config_t *config) {
    if (monitor == NULL || config == NULL) {
        return false;
    }

    monitor->config = *config;
    return true;
}

/* 获取统计报告 */
void audio_quality_get_report(const audio_quality_monitor_t *monitor, char *buffer, uint32_t buffer_size) {
    if (monitor == NULL || buffer == NULL || buffer_size == 0) {
        return;
    }

    float peak_db = dsp_q15_to_db(monitor->metrics.peak_level);
    float rms_db = dsp_q15_to_db(monitor->metrics.rms_level);
    float snr_db = dsp_q15_to_db(monitor->metrics.snr_estimate);

    snprintf(buffer, buffer_size,
             "质量状态: %s\n"
             "峰值: %.1f dBFS\n"
             "RMS: %.1f dBFS\n"
             "SNR: %.1f dB\n"
             "过载: %lu\n"
             "失真: %lu\n"
             "丢失: %lu\n"
             "总样本: %lu",
             audio_quality_get_state_string(monitor->state),
             peak_db, rms_db, snr_db,
             (unsigned long)monitor->metrics.overload_count,
             (unsigned long)monitor->metrics.distortion_count,
             (unsigned long)monitor->metrics.dropout_count,
             (unsigned long)monitor->metrics.total_samples);
}

/* 执行质量评估 */
audio_quality_assessment_t audio_quality_assess(const audio_quality_monitor_t *monitor) {
    audio_quality_assessment_t assessment = {0};

    if (monitor == NULL || !monitor->is_initialized) {
        assessment.overall_score = 0;
        assessment.recommendation = "监控器未初始化";
        return assessment;
    }

    /* 检查各项问题 */
    assessment.has_overload = (monitor->metrics.overload_count > 0);
    assessment.has_distortion = (monitor->metrics.distortion_count > 0);
    assessment.has_dropout = (monitor->metrics.dropout_count > 100);

    /* 检查SNR */
    if (monitor->config.enable_snr_estimation) {
        float snr_db = dsp_q15_to_db(monitor->metrics.snr_estimate);
        assessment.snr_too_low = (snr_db < 20.0f);
    }

    /* 检查电平 */
    float rms_db = dsp_q15_to_db(monitor->metrics.rms_level);
    assessment.level_too_low = (rms_db < -30.0f);
    assessment.level_too_high = (rms_db > -6.0f);

    /* 检查峰值因子 */
    float crest_factor = dsp_q15_to_float(monitor->metrics.crest_factor);
    assessment.crest_factor_too_high = (crest_factor > 4.0f);  /* 峰值因子 > 4 */

    /* 计算总体评分 (0-100) */
    uint8_t score = 100;

    if (assessment.has_distortion) score -= 30;
    if (assessment.has_overload) score -= 20;
    if (assessment.has_dropout) score -= 15;
    if (assessment.snr_too_low) score -= 15;
    if (assessment.level_too_low || assessment.level_too_high) score -= 10;
    if (assessment.crest_factor_too_high) score -= 10;

    assessment.overall_score = score;

    /* 生成建议 */
    if (score >= 90) {
        assessment.recommendation = "音频质量优秀，无需调整";
    } else if (score >= 70) {
        assessment.recommendation = "音频质量良好，可考虑微调";
    } else if (score >= 50) {
        assessment.recommendation = "音频质量一般，建议调整增益或位置";
    } else {
        assessment.recommendation = "音频质量差，需要检查硬件和设置";
    }

    return assessment;
}

/* 调试输出 */
void audio_quality_print_stats(const audio_quality_monitor_t *monitor) {
    if (monitor == NULL || !monitor->is_initialized) {
        return;
    }

    char report[512];
    audio_quality_get_report(monitor, report, sizeof(report));
    /* 这里可以输出到串口或日志 */
    (void)report;  /* 防止未使用警告 */
}