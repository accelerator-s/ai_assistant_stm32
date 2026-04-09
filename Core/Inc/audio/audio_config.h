/**
 * @file  audio_config.h
 * @brief 音频处理配置定义
 *        包含音频处理流水线的所有可配置参数
 */

#ifndef __AUDIO_CONFIG_H
#define __AUDIO_CONFIG_H

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/* CMSIS DSP库配置 - 定义Cortex-M3核心 */
#define ARM_MATH_CM3
#include "../../Drivers/CMSIS/DSP/Include/arm_math.h"

/* 音频基本参数 - 使用audio_buffer.h中的定义 */
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE         16000     /* 采样率: 16kHz */
#endif

#ifndef AUDIO_BITS_PER_SAMPLE
#define AUDIO_BITS_PER_SAMPLE     16        /* 位深度: 16位 */
#endif

#ifndef AUDIO_CHANNELS
#define AUDIO_CHANNELS            1         /* 单声道 */
#endif

#ifndef AUDIO_BYTES_PER_SAMPLE
#define AUDIO_BYTES_PER_SAMPLE    (AUDIO_BITS_PER_SAMPLE / 8)  /* 16位 = 2字节 */
#endif

/* 定点数格式配置 */
#define AUDIO_Q15_SCALE           32767.0f  /* Q15最大值 */
#define AUDIO_Q15_HALF_SCALE      16384.0f  /* Q15半量程 */
#define AUDIO_DB_TO_Q15(db)       ((q15_t)(AUDIO_Q15_SCALE * powf(10.0f, (db) / 20.0f)))
#define AUDIO_Q15_TO_DB(q15)      (20.0f * log10f((float)(q15) / AUDIO_Q15_SCALE))

/* 处理块大小配置 */
#define AUDIO_PROCESS_BLOCK_SIZE  64        /* 每次处理的样本数 */
#define AUDIO_FRAME_SIZE_MS       20        /* 帧时长: 20ms (320样本) */
#define AUDIO_FRAME_SIZE          ((AUDIO_SAMPLE_RATE * AUDIO_FRAME_SIZE_MS) / 1000)

/* VAD配置 */
typedef struct {
    q15_t energy_threshold;         /* 能量阈值 (Q15, 0-32767) */
    uint16_t silence_duration_ms;   /* 静音检测时长 (ms) */
    uint16_t speech_duration_ms;    /* 语音最小时长 (ms) */
    uint16_t hangover_duration_ms;  /* 拖尾时长 (ms) */
    bool enable_adaptive_threshold; /* 启用自适应阈值 */
    q15_t noise_floor;              /* 噪声底噪估计 (Q15) */
} vad_config_t;

/* AGC配置 */
typedef struct {
    q15_t target_level;             /* 目标电平 (Q15, -20dBFS对应) */
    q15_t max_gain;                 /* 最大增益 (Q15, 对应dB) */
    q15_t min_gain;                 /* 最小增益 (Q15) */
    uint16_t attack_time_ms;        /* 攻击时间 (ms) */
    uint16_t release_time_ms;       /* 释放时间 (ms) */
    bool enable_limiter;            /* 启用限幅器 */
    q15_t limiter_threshold;        /* 限幅器阈值 (Q15) */
    uint8_t agc_mode;               /* 0:峰值AGC, 1:RMS AGC */
} agc_config_t;

/* 滤波器配置 */
typedef struct {
    bool enable_preemphasis;        /* 启用预加重滤波 */
    q15_t preemphasis_coeff;        /* 预加重系数 (Q15, 通常0.97) */

    bool enable_hp_filter;          /* 启用高通滤波 */
    uint8_t hp_filter_order;        /* 高通滤波器阶数 */
    q15_t hp_cutoff_freq;           /* 高通截止频率 (Hz) */

    bool enable_noise_gate;         /* 启用噪声门 */
    q15_t noise_gate_threshold;     /* 噪声门阈值 (Q15) */
    uint16_t noise_gate_attack_ms;  /* 噪声门攻击时间 */
    uint16_t noise_gate_release_ms; /* 噪声门释放时间 */
} filter_config_t;

/* 音频质量监控配置 */
typedef struct {
    q15_t overload_threshold;       /* 过载阈值 (Q15, -3dBFS) */
    uint16_t stats_window_ms;       /* 统计窗口时长 (ms) */
    bool enable_snr_estimation;     /* 启用SNR估计 */
    bool enable_distortion_detect;  /* 启用失真检测 */
    uint8_t distortion_threshold;   /* 失真阈值 (%) */
} quality_config_t;

/* 完整音频处理配置 */
typedef struct {
    vad_config_t vad;               /* VAD配置 */
    agc_config_t agc;               /* AGC配置 */
    filter_config_t filter;         /* 滤波器配置 */
    quality_config_t quality;       /* 质量监控配置 */

    bool enable_audio_processing;   /* 启用音频处理 */
    uint8_t processing_priority;    /* 处理优先级 (0-255) */
    uint16_t input_gain;            /* 输入增益 (0-100%) */
    uint16_t output_gain;           /* 输出增益 (0-100%) */

    /* 性能限制 */
    uint32_t max_cpu_usage_percent; /* 最大CPU使用率 (%) */
    uint32_t max_memory_kb;         /* 最大内存使用 (KB) */
    uint16_t max_processing_delay_ms; /* 最大处理延迟 (ms) */
} audio_processing_config_t;

/* 预设配置 */
typedef enum {
    AUDIO_PRESET_DEFAULT = 0,       /* 默认配置 */
    AUDIO_PRESET_QUIET_ENV,         /* 安静环境 */
    AUDIO_PRESET_NOISY_ENV,         /* 嘈杂环境 */
    AUDIO_PRESET_VOICE_RECOGNITION, /* 语音识别优化 */
    AUDIO_PRESET_MUSIC_RECORDING,   /* 音乐录制 */
    AUDIO_PRESET_CUSTOM             /* 自定义配置 */
} audio_preset_t;

/* 默认配置 */
extern const audio_processing_config_t AUDIO_DEFAULT_CONFIG;

/* 预设配置获取函数 */
const audio_processing_config_t* audio_config_get_preset(audio_preset_t preset);

/* 配置验证函数 */
bool audio_config_validate(const audio_processing_config_t* config);
const char* audio_config_get_validation_error(void);

/* 配置保存/加载函数 */
bool audio_config_save_to_flash(const audio_processing_config_t* config);
bool audio_config_load_from_flash(audio_processing_config_t* config);

/* 配置调试输出 */
void audio_config_print(const audio_processing_config_t* config);

#endif /* __AUDIO_CONFIG_H */