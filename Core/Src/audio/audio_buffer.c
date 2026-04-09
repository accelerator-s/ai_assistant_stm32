/**
 * @file  audio_buffer.c
 * @brief 音频环形缓冲区实现
 */
#include "audio/audio_buffer.h"
#include <string.h>

/* 初始化音频缓冲区 */
void audio_buffer_init(audio_buffer_t *buffer)
{
    if (buffer == NULL) return;

    memset(buffer->data, 0, AUDIO_BUFFER_SIZE);
    buffer->head = 0;
    buffer->tail = 0;
    buffer->count = 0;
    buffer->overflow_count = 0;
    buffer->is_full = false;
    buffer->is_empty = true;
}

/* 向缓冲区写入数据 */
bool audio_buffer_write(audio_buffer_t *buffer, const uint8_t *data, uint32_t size)
{
    if (buffer == NULL || data == NULL || size == 0) {
        return false;
    }

    /* 检查是否有足够空间 */
    if (size > AUDIO_BUFFER_SIZE - buffer->count) {
        buffer->overflow_count++;
        return false;  /* 空间不足 */
    }

    /* 写入数据 */
    for (uint32_t i = 0; i < size; i++) {
        buffer->data[buffer->head] = data[i];
        buffer->head = (buffer->head + 1) % AUDIO_BUFFER_SIZE;
    }

    buffer->count += size;
    buffer->is_empty = false;

    /* 检查是否已满 */
    if (buffer->count == AUDIO_BUFFER_SIZE) {
        buffer->is_full = true;
    }

    return true;
}

/* 从缓冲区读取数据 */
uint32_t audio_buffer_read(audio_buffer_t *buffer, uint8_t *data, uint32_t size)
{
    if (buffer == NULL || data == NULL || size == 0) {
        return 0;
    }

    /* 计算实际可读取的数据量 */
    uint32_t bytes_to_read = (size < buffer->count) ? size : buffer->count;

    if (bytes_to_read == 0) {
        return 0;
    }

    /* 读取数据 */
    for (uint32_t i = 0; i < bytes_to_read; i++) {
        data[i] = buffer->data[buffer->tail];
        buffer->tail = (buffer->tail + 1) % AUDIO_BUFFER_SIZE;
    }

    buffer->count -= bytes_to_read;
    buffer->is_full = false;

    /* 检查是否为空 */
    if (buffer->count == 0) {
        buffer->is_empty = true;
    }

    return bytes_to_read;
}

/* 获取缓冲区可用空间 */
uint32_t audio_buffer_available_space(const audio_buffer_t *buffer)
{
    if (buffer == NULL) return 0;
    return AUDIO_BUFFER_SIZE - buffer->count;
}

/* 获取缓冲区数据量 */
uint32_t audio_buffer_available_data(const audio_buffer_t *buffer)
{
    if (buffer == NULL) return 0;
    return buffer->count;
}

/* 清空缓冲区 */
void audio_buffer_clear(audio_buffer_t *buffer)
{
    if (buffer == NULL) return;

    buffer->head = 0;
    buffer->tail = 0;
    buffer->count = 0;
    buffer->is_full = false;
    buffer->is_empty = true;
}

/* 检查缓冲区是否为空 */
bool audio_buffer_is_empty(const audio_buffer_t *buffer)
{
    if (buffer == NULL) return true;
    return buffer->is_empty;
}

/* 检查缓冲区是否已满 */
bool audio_buffer_is_full(const audio_buffer_t *buffer)
{
    if (buffer == NULL) return false;
    return buffer->is_full;
}

/* 获取溢出计数 */
uint32_t audio_buffer_get_overflow_count(const audio_buffer_t *buffer)
{
    if (buffer == NULL) return 0;
    return buffer->overflow_count;
}