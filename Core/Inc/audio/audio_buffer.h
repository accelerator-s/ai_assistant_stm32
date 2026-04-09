/**
 * @file  audio_buffer.h
 * @brief 音频环形缓冲区管理
 *        用于存储从麦克风采集的PCM音频数据
 */
#ifndef __AUDIO_BUFFER_H
#define __AUDIO_BUFFER_H

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/* 音频参数配置 */
#define AUDIO_SAMPLE_RATE     16000     /* 采样率: 16kHz */
#define AUDIO_BITS_PER_SAMPLE 16        /* 位深度: 16位 */
#define AUDIO_CHANNELS        1         /* 单声道 */

/* 计算音频数据大小 */
#define AUDIO_BYTES_PER_SAMPLE (AUDIO_BITS_PER_SAMPLE / 8)
#define AUDIO_BYTES_PER_FRAME  (AUDIO_BYTES_PER_SAMPLE * AUDIO_CHANNELS)
#define AUDIO_SAMPLES_PER_SEC  (AUDIO_SAMPLE_RATE * AUDIO_CHANNELS)
#define AUDIO_BYTES_PER_SEC    (AUDIO_SAMPLES_PER_SEC * AUDIO_BYTES_PER_SAMPLE)

/* 缓冲区配置 */
#define AUDIO_BUFFER_SIZE_MS  500       /* 缓冲区存储0.5秒音频（STM32F103只有64KB RAM） */
#define AUDIO_BUFFER_SIZE     ((AUDIO_BUFFER_SIZE_MS * AUDIO_BYTES_PER_SEC) / 1000)

/* 音频缓冲区结构体 */
typedef struct {
    uint8_t data[AUDIO_BUFFER_SIZE];    /* 缓冲区数据 */
    uint32_t head;                      /* 写指针 */
    uint32_t tail;                      /* 读指针 */
    uint32_t count;                     /* 当前数据量 */
    uint32_t overflow_count;            /* 溢出计数 */
    bool is_full;                       /* 缓冲区满标志 */
    bool is_empty;                      /* 缓冲区空标志 */
} audio_buffer_t;

/* 初始化音频缓冲区 */
void audio_buffer_init(audio_buffer_t *buffer);

/* 向缓冲区写入数据 */
bool audio_buffer_write(audio_buffer_t *buffer, const uint8_t *data, uint32_t size);

/* 从缓冲区读取数据 */
uint32_t audio_buffer_read(audio_buffer_t *buffer, uint8_t *data, uint32_t size);

/* 获取缓冲区可用空间 */
uint32_t audio_buffer_available_space(const audio_buffer_t *buffer);

/* 获取缓冲区数据量 */
uint32_t audio_buffer_available_data(const audio_buffer_t *buffer);

/* 清空缓冲区 */
void audio_buffer_clear(audio_buffer_t *buffer);

/* 检查缓冲区是否为空 */
bool audio_buffer_is_empty(const audio_buffer_t *buffer);

/* 检查缓冲区是否已满 */
bool audio_buffer_is_full(const audio_buffer_t *buffer);

/* 获取溢出计数 */
uint32_t audio_buffer_get_overflow_count(const audio_buffer_t *buffer);

#endif /* __AUDIO_BUFFER_H */