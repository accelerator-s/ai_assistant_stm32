/**
 * @file  dsp_config.c
 * @brief CMSIS DSP配置和工具函数实现
 */

#include "audio/dsp_config.h"
#include "audio/audio_config.h"  /* 包含CMSIS DSP库定义 */
#include <string.h>
#include <math.h>

/* 内存池 */
static uint8_t dsp_memory_pool[DSP_MEMORY_POOL_SIZE];
static uint32_t dsp_memory_used = 0;
static uint32_t dsp_memory_peak_used = 0;

/* 错误状态 */
static dsp_error_t dsp_last_error = DSP_ERROR_NONE;

/* 向量运算工具函数 */
void dsp_vector_abs_q15(q15_t *pSrc, q15_t *pDst, uint32_t blockSize) {
    if (pSrc == NULL || pDst == NULL || blockSize == 0) {
        dsp_last_error = DSP_ERROR_INVALID_PARAM;
        return;
    }

    for (uint32_t i = 0; i < blockSize; i++) {
        pDst[i] = (pSrc[i] < 0) ? -pSrc[i] : pSrc[i];
    }
}

void dsp_vector_add_q15(q15_t *pSrcA, q15_t *pSrcB, q15_t *pDst, uint32_t blockSize) {
    if (pSrcA == NULL || pSrcB == NULL || pDst == NULL || blockSize == 0) {
        dsp_last_error = DSP_ERROR_INVALID_PARAM;
        return;
    }

    for (uint32_t i = 0; i < blockSize; i++) {
        q31_t sum = (q31_t)pSrcA[i] + (q31_t)pSrcB[i];
        if (sum > 32767) pDst[i] = 32767;
        else if (sum < -32768) pDst[i] = -32768;
        else pDst[i] = (q15_t)sum;
    }
}

void dsp_vector_sub_q15(q15_t *pSrcA, q15_t *pSrcB, q15_t *pDst, uint32_t blockSize) {
    if (pSrcA == NULL || pSrcB == NULL || pDst == NULL || blockSize == 0) {
        dsp_last_error = DSP_ERROR_INVALID_PARAM;
        return;
    }

    for (uint32_t i = 0; i < blockSize; i++) {
        q31_t diff = (q31_t)pSrcA[i] - (q31_t)pSrcB[i];
        if (diff > 32767) pDst[i] = 32767;
        else if (diff < -32768) pDst[i] = -32768;
        else pDst[i] = (q15_t)diff;
    }
}

void dsp_vector_scale_q15(q15_t *pSrc, q15_t *pDst, q15_t scale, uint32_t blockSize) {
    if (pSrc == NULL || pDst == NULL || blockSize == 0) {
        dsp_last_error = DSP_ERROR_INVALID_PARAM;
        return;
    }

    for (uint32_t i = 0; i < blockSize; i++) {
        q31_t product = (q31_t)pSrc[i] * (q31_t)scale;
        product = product >> 15;  /* Q15乘法需要右移15位 */
        if (product > 32767) pDst[i] = 32767;
        else if (product < -32768) pDst[i] = -32768;
        else pDst[i] = (q15_t)product;
    }
}

void dsp_vector_offset_q15(q15_t *pSrc, q15_t *pDst, q15_t offset, uint32_t blockSize) {
    if (pSrc == NULL || pDst == NULL || blockSize == 0) {
        dsp_last_error = DSP_ERROR_INVALID_PARAM;
        return;
    }

    for (uint32_t i = 0; i < blockSize; i++) {
        q31_t sum = (q31_t)pSrc[i] + (q31_t)offset;
        if (sum > 32767) pDst[i] = 32767;
        else if (sum < -32768) pDst[i] = -32768;
        else pDst[i] = (q15_t)sum;
    }
}

/* 统计函数 */
q15_t dsp_vector_mean_q15(q15_t *pSrc, uint32_t blockSize) {
    if (pSrc == NULL || blockSize == 0) {
        dsp_last_error = DSP_ERROR_INVALID_PARAM;
        return 0;
    }

    q31_t sum = 0;
    for (uint32_t i = 0; i < blockSize; i++) {
        sum += pSrc[i];
    }

    return (q15_t)(sum / blockSize);
}

q15_t dsp_vector_rms_q15(q15_t *pSrc, uint32_t blockSize) {
    if (pSrc == NULL || blockSize == 0) {
        dsp_last_error = DSP_ERROR_INVALID_PARAM;
        return 0;
    }

    q31_t sum = 0;
    for (uint32_t i = 0; i < blockSize; i++) {
        q31_t square = (q31_t)pSrc[i] * (q31_t)pSrc[i];
        sum += square >> 15;  /* Q15平方后右移15位 */
    }

    q15_t mean_square = (q15_t)(sum / blockSize);

    /* 近似计算平方根 */
    float mean_square_f = dsp_q15_to_float(mean_square);
    float rms_f = sqrtf(mean_square_f);

    return dsp_float_to_q15(rms_f);
}

q15_t dsp_vector_peak_q15(q15_t *pSrc, uint32_t blockSize) {
    if (pSrc == NULL || blockSize == 0) {
        dsp_last_error = DSP_ERROR_INVALID_PARAM;
        return 0;
    }

    q15_t peak = 0;
    for (uint32_t i = 0; i < blockSize; i++) {
        q15_t abs_val = (pSrc[i] < 0) ? -pSrc[i] : pSrc[i];
        if (abs_val > peak) {
            peak = abs_val;
        }
    }

    return peak;
}

q15_t dsp_vector_energy_q15(q15_t *pSrc, uint32_t blockSize) {
    if (pSrc == NULL || blockSize == 0) {
        dsp_last_error = DSP_ERROR_INVALID_PARAM;
        return 0;
    }

    q31_t sum = 0;
    for (uint32_t i = 0; i < blockSize; i++) {
        q31_t square = (q31_t)pSrc[i] * (q31_t)pSrc[i];
        sum += square >> 15;  /* Q15平方后右移15位 */
    }

    return (q15_t)(sum / blockSize);
}

/* 滤波器设计函数 */
bool dsp_design_hp_filter_q15(q15_t *coeffs, uint8_t order, uint32_t cutoff_freq, uint32_t sample_rate) {
    if (coeffs == NULL || order == 0 || cutoff_freq == 0 || sample_rate == 0) {
        dsp_last_error = DSP_ERROR_INVALID_PARAM;
        return false;
    }

    if (order > DSP_MAX_FILTER_ORDER) {
        dsp_last_error = DSP_ERROR_FILTER_INIT;
        return false;
    }

    /* 简单的高通滤波器设计：移动平均减去直流 */
    /* 这是一个简单的FIR高通滤波器 */
    float fc = (float)cutoff_freq / (float)sample_rate;
    float omega_c = 2.0f * M_PI * fc;

    /* 设计一个简单的窗函数法FIR高通滤波器 */
    for (int n = 0; n <= order; n++) {
        float h = 0.0f;

        if (n == order / 2) {
            h = 1.0f - 2.0f * fc;
        } else {
            float theta = M_PI * (n - order / 2);
            h = -sinf(omega_c * (n - order / 2)) / theta;
        }

        /* 汉明窗 */
        float window = 0.54f - 0.46f * cosf(2.0f * M_PI * n / order);
        h *= window;

        coeffs[n] = dsp_float_to_q15(h);
    }

    return true;
}

bool dsp_design_lp_filter_q15(q15_t *coeffs, uint8_t order, uint32_t cutoff_freq, uint32_t sample_rate) {
    if (coeffs == NULL || order == 0 || cutoff_freq == 0 || sample_rate == 0) {
        dsp_last_error = DSP_ERROR_INVALID_PARAM;
        return false;
    }

    if (order > DSP_MAX_FILTER_ORDER) {
        dsp_last_error = DSP_ERROR_FILTER_INIT;
        return false;
    }

    /* 简单的低通滤波器设计 */
    float fc = (float)cutoff_freq / (float)sample_rate;
    float omega_c = 2.0f * M_PI * fc;

    for (int n = 0; n <= order; n++) {
        float h = 0.0f;

        if (n == order / 2) {
            h = 2.0f * fc;
        } else {
            float theta = M_PI * (n - order / 2);
            h = sinf(omega_c * (n - order / 2)) / theta;
        }

        /* 汉明窗 */
        float window = 0.54f - 0.46f * cosf(2.0f * M_PI * n / order);
        h *= window;

        coeffs[n] = dsp_float_to_q15(h);
    }

    return true;
}

bool dsp_design_preemphasis_filter_q15(q15_t *coeffs, q15_t alpha) {
    if (coeffs == NULL) {
        dsp_last_error = DSP_ERROR_INVALID_PARAM;
        return false;
    }

    /* 预加重滤波器：H(z) = 1 - αz^{-1} */
    coeffs[0] = 32767;  /* 1.0 in Q15 */
    coeffs[1] = -alpha; /* -α */

    return true;
}

/* FFT相关函数 */
bool dsp_fft_init_q15(arm_rfft_instance_q15 *fft_instance, uint32_t fft_size) {
    if (fft_instance == NULL || fft_size == 0) {
        dsp_last_error = DSP_ERROR_INVALID_PARAM;
        return false;
    }

    if (fft_size > DSP_MAX_FFT_SIZE) {
        dsp_last_error = DSP_ERROR_FFT_INIT;
        return false;
    }

    /* 初始化FFT实例 */
    arm_status status = arm_rfft_init_q15(fft_instance, fft_size, 0, 1);
    if (status != ARM_MATH_SUCCESS) {
        dsp_last_error = DSP_ERROR_FFT_INIT;
        return false;
    }

    return true;
}

bool dsp_fft_compute_q15(arm_rfft_instance_q15 *fft_instance, q15_t *pSrc, q15_t *pDst) {
    if (fft_instance == NULL || pSrc == NULL || pDst == NULL) {
        dsp_last_error = DSP_ERROR_INVALID_PARAM;
        return false;
    }

    arm_rfft_q15(fft_instance, pSrc, pDst);
    return true;
}

/* 内存管理 */
void* dsp_memory_alloc(uint32_t size) {
    if (size == 0 || size > DSP_MEMORY_POOL_SIZE - dsp_memory_used) {
        dsp_last_error = DSP_ERROR_MEMORY_ALLOC;
        return NULL;
    }

    void *ptr = &dsp_memory_pool[dsp_memory_used];
    dsp_memory_used += size;

    if (dsp_memory_used > dsp_memory_peak_used) {
        dsp_memory_peak_used = dsp_memory_used;
    }

    return ptr;
}

void dsp_memory_free(void *ptr) {
    /* 简单内存池，不支持释放单个块 */
    (void)ptr;  /* 防止未使用警告 */
}

uint32_t dsp_memory_get_usage(void) {
    return dsp_memory_used;
}

uint32_t dsp_memory_get_peak_usage(void) {
    return dsp_memory_peak_used;
}

/* 错误处理 */
dsp_error_t dsp_get_last_error(void) {
    return dsp_last_error;
}

const char* dsp_get_error_string(dsp_error_t error) {
    switch (error) {
        case DSP_ERROR_NONE:           return "无错误";
        case DSP_ERROR_MEMORY_ALLOC:   return "内存分配失败";
        case DSP_ERROR_FILTER_INIT:    return "滤波器初始化失败";
        case DSP_ERROR_FFT_INIT:       return "FFT初始化失败";
        case DSP_ERROR_INVALID_PARAM:  return "无效参数";
        case DSP_ERROR_BUFFER_OVERFLOW:return "缓冲区溢出";
        case DSP_ERROR_UNDERFLOW:      return "缓冲区下溢";
        case DSP_ERROR_TIMEOUT:        return "超时";
        default:                       return "未知错误";
    }
}

void dsp_clear_error(void) {
    dsp_last_error = DSP_ERROR_NONE;
}

/* 性能监控 */
void dsp_perf_start(dsp_perf_counter_t *counter) {
    if (counter == NULL) {
        return;
    }

    counter->call_count++;
    /* 在实际实现中，这里会记录开始时间 */
}

void dsp_perf_stop(dsp_perf_counter_t *counter) {
    if (counter == NULL) {
        return;
    }

    /* 在实际实现中，这里会计算耗时并更新统计 */
    uint32_t cycles = 0;  /* 需要实际测量 */

    counter->total_cycles += cycles;

    if (cycles > counter->max_cycles) {
        counter->max_cycles = cycles;
    }

    if (counter->min_cycles == 0 || cycles < counter->min_cycles) {
        counter->min_cycles = cycles;
    }
}

void dsp_perf_reset(dsp_perf_counter_t *counter) {
    if (counter == NULL) {
        return;
    }

    memset(counter, 0, sizeof(dsp_perf_counter_t));
}

float dsp_perf_get_average_us(dsp_perf_counter_t *counter, uint32_t cpu_freq_mhz) {
    if (counter == NULL || counter->call_count == 0 || cpu_freq_mhz == 0) {
        return 0.0f;
    }

    float avg_cycles = (float)counter->total_cycles / (float)counter->call_count;
    return avg_cycles / (float)cpu_freq_mhz;
}