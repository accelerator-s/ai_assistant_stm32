/**
 * @file  dsp_config.h
 * @brief CMSIS DSP库配置和工具函数
 *        提供DSP相关配置和便捷函数
 */

#ifndef __DSP_CONFIG_H
#define __DSP_CONFIG_H

#include "main.h"
#include "audio/audio_config.h"  /* 包含CMSIS DSP库定义 */
#include <stdint.h>
#include <stdbool.h>
#include <math.h>  /* 包含数学函数 */

/* DSP库启用配置 */
#define DSP_USE_FLOAT             0  /* 禁用浮点运算（节省内存） */
#define DSP_USE_Q15               1  /* 启用Q15定点运算 */
#define DSP_USE_Q31               0  /* 禁用Q31运算（除非需要高精度） */
#define DSP_USE_MATRIX            0  /* 禁用矩阵运算 */
#define DSP_USE_COMPLEX           0  /* 禁用复数运算 */

/* 性能优化配置 */
#define DSP_OPTIMIZE_FOR_SPEED    1  /* 优化速度而非代码大小 */
#define DSP_USE_UNALIGNED_ACCESS  0  /* 禁用非对齐访问（Cortex-M3不支持） */

/* 内存池配置 */
#define DSP_MEMORY_POOL_SIZE      8192  /* DSP内存池大小（字节） */
#define DSP_MAX_FILTER_ORDER      64    /* 最大滤波器阶数 */
#define DSP_MAX_FFT_SIZE          256   /* 最大FFT点数 */

/* 错误处理配置 */
#define DSP_ENABLE_ERROR_CHECKING 1    /* 启用错误检查 */
#define DSP_MAX_ERROR_COUNT       10   /* 最大错误计数 */

/* Q15工具函数 */
static inline q15_t dsp_float_to_q15(float value) {
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    return (q15_t)(value * 32767.0f);
}

static inline float dsp_q15_to_float(q15_t value) {
    return (float)value / 32767.0f;
}

static inline q15_t dsp_db_to_q15(float db) {
    float linear = powf(10.0f, db / 20.0f);
    return dsp_float_to_q15(linear);
}

static inline float dsp_q15_to_db(q15_t value) {
    float linear = dsp_q15_to_float(value);
    if (linear <= 0.0f) return -96.0f;  /* 最小dB值 */
    return 20.0f * log10f(linear);
}

/* 饱和运算工具函数 */
static inline q15_t dsp_saturate_add_q15(q15_t a, q15_t b) {
    q31_t sum = (q31_t)a + (q31_t)b;
    if (sum > 32767) return 32767;
    if (sum < -32768) return -32768;
    return (q15_t)sum;
}

static inline q15_t dsp_saturate_sub_q15(q15_t a, q15_t b) {
    q31_t diff = (q31_t)a - (q31_t)b;
    if (diff > 32767) return 32767;
    if (diff < -32768) return -32768;
    return (q15_t)diff;
}

static inline q15_t dsp_saturate_mul_q15(q15_t a, q15_t b) {
    q31_t product = (q31_t)a * (q31_t)b;
    product = product >> 15;  /* Q15乘法需要右移15位 */
    if (product > 32767) return 32767;
    if (product < -32768) return -32768;
    return (q15_t)product;
}

/* 向量运算工具函数 */
void dsp_vector_abs_q15(q15_t *pSrc, q15_t *pDst, uint32_t blockSize);
void dsp_vector_add_q15(q15_t *pSrcA, q15_t *pSrcB, q15_t *pDst, uint32_t blockSize);
void dsp_vector_sub_q15(q15_t *pSrcA, q15_t *pSrcB, q15_t *pDst, uint32_t blockSize);
void dsp_vector_scale_q15(q15_t *pSrc, q15_t *pDst, q15_t scale, uint32_t blockSize);
void dsp_vector_offset_q15(q15_t *pSrc, q15_t *pDst, q15_t offset, uint32_t blockSize);

/* 统计函数 */
q15_t dsp_vector_mean_q15(q15_t *pSrc, uint32_t blockSize);
q15_t dsp_vector_rms_q15(q15_t *pSrc, uint32_t blockSize);
q15_t dsp_vector_peak_q15(q15_t *pSrc, uint32_t blockSize);
q15_t dsp_vector_energy_q15(q15_t *pSrc, uint32_t blockSize);

/* 滤波器设计函数 */
bool dsp_design_hp_filter_q15(q15_t *coeffs, uint8_t order, uint32_t cutoff_freq, uint32_t sample_rate);
bool dsp_design_lp_filter_q15(q15_t *coeffs, uint8_t order, uint32_t cutoff_freq, uint32_t sample_rate);
bool dsp_design_preemphasis_filter_q15(q15_t *coeffs, q15_t alpha);

/* FFT相关函数 */
bool dsp_fft_init_q15(arm_rfft_instance_q15 *fft_instance, uint32_t fft_size);
bool dsp_fft_compute_q15(arm_rfft_instance_q15 *fft_instance, q15_t *pSrc, q15_t *pDst);

/* 内存管理 */
void* dsp_memory_alloc(uint32_t size);
void dsp_memory_free(void *ptr);
uint32_t dsp_memory_get_usage(void);
uint32_t dsp_memory_get_peak_usage(void);

/* 错误处理 */
typedef enum {
    DSP_ERROR_NONE = 0,
    DSP_ERROR_MEMORY_ALLOC,
    DSP_ERROR_FILTER_INIT,
    DSP_ERROR_FFT_INIT,
    DSP_ERROR_INVALID_PARAM,
    DSP_ERROR_BUFFER_OVERFLOW,
    DSP_ERROR_UNDERFLOW,
    DSP_ERROR_TIMEOUT
} dsp_error_t;

dsp_error_t dsp_get_last_error(void);
const char* dsp_get_error_string(dsp_error_t error);
void dsp_clear_error(void);

/* 性能监控 */
typedef struct {
    uint32_t total_cycles;
    uint32_t max_cycles;
    uint32_t min_cycles;
    uint32_t call_count;
    uint32_t error_count;
} dsp_perf_counter_t;

void dsp_perf_start(dsp_perf_counter_t *counter);
void dsp_perf_stop(dsp_perf_counter_t *counter);
void dsp_perf_reset(dsp_perf_counter_t *counter);
float dsp_perf_get_average_us(dsp_perf_counter_t *counter, uint32_t cpu_freq_mhz);

#endif /* __DSP_CONFIG_H */