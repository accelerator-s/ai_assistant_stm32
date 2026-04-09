/**
 * @file  audio_processor.h
 * @brief 音频处理核心模块
 *        实现音频处理流水线，包括VAD、AGC、滤波等功能
 */

#ifndef __AUDIO_PROCESSOR_H
#define __AUDIO_PROCESSOR_H

#include "main.h"
#include "audio/audio_config.h"
#include "audio/audio_quality.h"
#include "audio/dsp_config.h"
#include <stdint.h>
#include <stdbool.h>

/* 处理模块类型 */
typedef enum {
    MODULE_PREEMPHASIS = 0,   /* 预加重滤波 */
    MODULE_HP_FILTER,         /* 高通滤波 */
    MODULE_NOISE_GATE,        /* 噪声门 */
    MODULE_AGC,               /* 自动增益控制 */
    MODULE_VAD,               /* 语音活动检测 */
    MODULE_COUNT              /* 模块总数 */
} audio_module_type_t;

/* 处理模块状态 */
typedef struct {
    bool is_enabled;          /* 模块是否启用 */
    bool is_initialized;      /* 模块是否已初始化 */
    uint32_t processed_samples; /* 已处理样本数 */
    uint32_t error_count;     /* 错误计数 */
    void *module_data;        /* 模块私有数据 */
} audio_module_state_t;

/* VAD模块状态 */
typedef struct {
    q15_t energy_threshold;         /* 能量阈值 */
    uint32_t silence_duration_ms;   /* 静音检测时长 */
    uint32_t speech_duration_ms;    /* 语音最小时长 */
    uint32_t hangover_duration_ms;  /* 拖尾时长 */

    q15_t current_energy;           /* 当前能量 */
    q15_t noise_floor;              /* 噪声底噪估计 */
    uint32_t silence_counter;       /* 静音计数器 */
    uint32_t speech_counter;        /* 语音计数器 */
    uint32_t hangover_counter;      /* 拖尾计数器 */

    bool is_speech_active;          /* 语音活动标志 */
    bool is_speech_started;         /* 语音开始标志 */
    bool is_speech_ended;           /* 语音结束标志 */

    arm_fir_instance_q15 hp_filter; /* 高通滤波器（去除直流） */
    q15_t hp_filter_state[64];      /* 滤波器状态缓冲区 */
    q15_t hp_filter_coeffs[32];     /* 滤波器系数 */

    uint32_t sample_counter;        /* 样本计数器 */
    q15_t energy_buffer[32];        /* 能量计算缓冲区 */
    uint32_t energy_index;          /* 能量缓冲区索引 */
} vad_state_t;

/* AGC模块状态 */
typedef struct {
    q15_t target_level;             /* 目标电平 */
    q15_t max_gain;                 /* 最大增益 */
    q15_t min_gain;                 /* 最小增益 */
    q15_t attack_coeff;             /* 攻击系数 */
    q15_t release_coeff;            /* 释放系数 */

    q15_t current_gain;             /* 当前增益 */
    q15_t envelope;                 /* 包络值 */

    bool enable_limiter;            /* 启用限幅器 */
    q15_t limiter_threshold;        /* 限幅器阈值 */
    uint8_t agc_mode;               /* AGC模式 */

    q15_t rms_buffer[32];           /* RMS计算缓冲区 */
    uint32_t rms_index;             /* RMS缓冲区索引 */
    q15_t peak_buffer[32];          /* 峰值计算缓冲区 */
    uint32_t peak_index;            /* 峰值缓冲区索引 */
} agc_state_t;

/* 滤波器模块状态 */
typedef struct {
    bool enable_preemphasis;        /* 启用预加重 */
    q15_t preemphasis_coeff;        /* 预加重系数 */
    q15_t preemphasis_state;        /* 预加重状态 */

    bool enable_hp_filter;          /* 启用高通滤波 */
    arm_fir_instance_q15 hp_filter; /* 高通滤波器实例 */
    q15_t hp_filter_state[128];     /* 滤波器状态缓冲区 */
    q15_t hp_filter_coeffs[64];     /* 滤波器系数 */

    bool enable_noise_gate;         /* 启用噪声门 */
    q15_t noise_gate_threshold;     /* 噪声门阈值 */
    q15_t noise_gate_gain;          /* 噪声门增益 */
    uint32_t noise_gate_attack_ms;  /* 攻击时间 */
    uint32_t noise_gate_release_ms; /* 释放时间 */
    q15_t noise_gate_envelope;      /* 噪声门包络 */
} filter_state_t;

/* 音频处理流水线 */
typedef struct {
    /* 配置 */
    audio_processing_config_t config;

    /* 模块状态 */
    audio_module_state_t modules[MODULE_COUNT];

    /* 具体模块状态 */
    vad_state_t vad;
    agc_state_t agc;
    filter_state_t filter;

    /* 质量监控 */
    audio_quality_monitor_t quality_monitor;

    /* 处理统计 */
    uint32_t total_samples_processed;
    uint32_t total_samples_dropped;
    uint32_t total_errors;

    /* 缓冲区 */
    q15_t input_buffer[32];  /* 减小缓冲区大小以节省RAM */
    q15_t output_buffer[32];
    q15_t temp_buffer[32];

    uint32_t input_index;
    uint32_t output_index;

    /* 状态标志 */
    bool is_initialized;
    bool is_processing;
    bool is_paused;
    bool has_error;
} audio_processor_t;

/* 初始化音频处理器 */
bool audio_processor_init(audio_processor_t *processor, const audio_processing_config_t *config);

/* 处理单个样本 */
q15_t audio_processor_process_sample(audio_processor_t *processor, q15_t input_sample);

/* 处理一批样本 */
uint32_t audio_processor_process_block(audio_processor_t *processor,
                                      q15_t *input_samples,
                                      q15_t *output_samples,
                                      uint32_t sample_count);

/* 处理16位PCM数据 */
uint32_t audio_processor_process_pcm16(audio_processor_t *processor,
                                      int16_t *input_pcm,
                                      int16_t *output_pcm,
                                      uint32_t sample_count);

/* 获取VAD状态 */
bool audio_processor_is_speech_active(const audio_processor_t *processor);
bool audio_processor_is_speech_started(const audio_processor_t *processor);
bool audio_processor_is_speech_ended(const audio_processor_t *processor);

/* 获取AGC增益 */
q15_t audio_processor_get_current_gain(const audio_processor_t *processor);

/* 获取处理统计 */
void audio_processor_get_stats(const audio_processor_t *processor,
                              uint32_t *processed,
                              uint32_t *dropped,
                              uint32_t *errors);

/* 重置处理器 */
void audio_processor_reset(audio_processor_t *processor);

/* 暂停/恢复处理 */
void audio_processor_pause(audio_processor_t *processor);
void audio_processor_resume(audio_processor_t *processor);

/* 更新配置 */
bool audio_processor_update_config(audio_processor_t *processor,
                                  const audio_processing_config_t *config);

/* 获取模块启用状态 */
bool audio_processor_is_module_enabled(const audio_processor_t *processor, audio_module_type_t module);

/* 启用/禁用模块 */
bool audio_processor_enable_module(audio_processor_t *processor, audio_module_type_t module, bool enable);

/* 获取错误信息 */
const char* audio_processor_get_error_string(const audio_processor_t *processor);

/* 获取处理延迟估计（样本数） */
uint32_t audio_processor_get_processing_delay(const audio_processor_t *processor);

/* 调试输出 */
void audio_processor_print_status(const audio_processor_t *processor);

/* 性能测试 */
typedef struct {
    uint32_t total_cycles;
    uint32_t max_cycles_per_sample;
    uint32_t min_cycles_per_sample;
    uint32_t sample_count;
} audio_processor_perf_t;

void audio_processor_start_perf_test(audio_processor_perf_t *perf);
void audio_processor_stop_perf_test(audio_processor_perf_t *perf, uint32_t cycles);
void audio_processor_print_perf_report(const audio_processor_perf_t *perf, uint32_t cpu_freq_mhz);

#endif /* __AUDIO_PROCESSOR_H */