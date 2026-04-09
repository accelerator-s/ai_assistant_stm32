/**
 * @file  audio_quality.h
 * @brief 音频质量监控模块
 *        实时监控音频信号质量，检测过载、失真等问题
 */

#ifndef __AUDIO_QUALITY_H
#define __AUDIO_QUALITY_H

#include "main.h"
#include "audio/audio_config.h"
#include "audio/dsp_config.h"
#include <stdint.h>
#include <stdbool.h>

/* 质量指标 */
typedef struct {
    q15_t peak_level;               /* 峰值电平 (Q15) */
    q15_t rms_level;                /* RMS电平 (Q15) */
    q15_t crest_factor;             /* 峰值因子 (Q15, 峰值/RMS) */
    q15_t snr_estimate;             /* 信噪比估计 (Q15, 线性) */
    uint32_t overload_count;        /* 过载计数 */
    uint32_t distortion_count;      /* 失真计数 */
    uint32_t dropout_count;         /* 信号丢失计数 */
    uint32_t total_samples;         /* 总样本数 */
    uint32_t processed_samples;     /* 已处理样本数 */
} audio_quality_metrics_t;

/* 质量状态 */
typedef enum {
    QUALITY_STATE_EXCELLENT = 0,    /* 优秀：无问题 */
    QUALITY_STATE_GOOD,             /* 良好：轻微问题 */
    QUALITY_STATE_FAIR,             /* 一般：需要注意 */
    QUALITY_STATE_POOR,             /* 较差：需要调整 */
    QUALITY_STATE_BAD,              /* 差：严重问题 */
    QUALITY_STATE_ERROR             /* 错误：无法工作 */
} audio_quality_state_t;

/* 质量监控器 */
typedef struct {
    audio_quality_metrics_t metrics;    /* 当前指标 */
    audio_quality_metrics_t history[10]; /* 历史记录（环形缓冲区） */
    audio_quality_state_t state;        /* 当前状态 */

    quality_config_t config;            /* 配置 */

    /* 内部状态 */
    uint32_t sample_counter;            /* 样本计数器 */
    uint32_t window_counter;            /* 窗口计数器 */
    q15_t window_buffer[256];           /* 统计窗口缓冲区 */
    uint32_t window_index;              /* 窗口缓冲区索引 */

    q15_t noise_floor_estimate;         /* 噪声底噪估计 */
    q15_t signal_energy_accum;          /* 信号能量累加器 */
    q15_t noise_energy_accum;           /* 噪声能量累加器 */

    uint32_t last_overload_time;        /* 最后过载时间 */
    uint32_t last_distortion_time;      /* 最后失真时间 */

    bool is_initialized;                /* 初始化标志 */
} audio_quality_monitor_t;

/* 初始化质量监控器 */
bool audio_quality_init(audio_quality_monitor_t *monitor, const quality_config_t *config);

/* 更新质量监控（处理单个样本） */
void audio_quality_update_sample(audio_quality_monitor_t *monitor, q15_t sample);

/* 更新质量监控（处理一批样本） */
void audio_quality_update_block(audio_quality_monitor_t *monitor, q15_t *samples, uint32_t count);

/* 获取当前质量指标 */
const audio_quality_metrics_t* audio_quality_get_metrics(const audio_quality_monitor_t *monitor);

/* 获取质量状态 */
audio_quality_state_t audio_quality_get_state(const audio_quality_monitor_t *monitor);

/* 获取质量状态字符串 */
const char* audio_quality_get_state_string(audio_quality_state_t state);

/* 检查是否有严重问题 */
bool audio_quality_has_critical_issue(const audio_quality_monitor_t *monitor);

/* 获取问题描述 */
const char* audio_quality_get_issue_description(const audio_quality_monitor_t *monitor);

/* 重置质量监控器 */
void audio_quality_reset(audio_quality_monitor_t *monitor);

/* 更新配置 */
bool audio_quality_update_config(audio_quality_monitor_t *monitor, const quality_config_t *config);

/* 获取统计报告 */
void audio_quality_get_report(const audio_quality_monitor_t *monitor, char *buffer, uint32_t buffer_size);

/* 保存质量数据到日志 */
bool audio_quality_save_log(const audio_quality_monitor_t *monitor);

/* 质量评估函数 */
typedef struct {
    bool has_overload;                 /* 是否有过载 */
    bool has_distortion;               /* 是否有失真 */
    bool has_dropout;                  /* 是否有信号丢失 */
    bool snr_too_low;                  /* 信噪比是否过低 */
    bool level_too_low;                /* 电平是否过低 */
    bool level_too_high;               /* 电平是否过高 */
    bool crest_factor_too_high;        /* 峰值因子是否过高 */

    uint8_t overall_score;             /* 总体评分 (0-100) */
    const char *recommendation;        /* 改进建议 */
} audio_quality_assessment_t;

/* 执行质量评估 */
audio_quality_assessment_t audio_quality_assess(const audio_quality_monitor_t *monitor);

/* 调试输出 */
void audio_quality_print_stats(const audio_quality_monitor_t *monitor);

#endif /* __AUDIO_QUALITY_H */