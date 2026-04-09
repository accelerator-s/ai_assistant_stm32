/**
 * @file  microphone.h
 * @brief INMP441 数字麦克风驱动
 *        通过 I2S2 接口连接，使用 DMA 接收音频数据
 */
#ifndef __MICROPHONE_H
#define __MICROPHONE_H

#include "main.h"
#include "audio/audio_buffer.h"
#include "audio/audio_processor.h"
#include "audio/audio_quality.h"
#include <stdbool.h>
#include <stdint.h>

/* I2S 硬件配置 - 使用 I2S2 */
#define MIC_I2S_INSTANCE          SPI2
#define MIC_I2S_CLK_ENABLE()      __HAL_RCC_SPI2_CLK_ENABLE()
#define MIC_I2S_CLK_DISABLE()     __HAL_RCC_SPI2_CLK_ENABLE()
#define MIC_I2S_IRQn              SPI2_IRQn
#define MIC_I2S_IRQHandler        SPI2_IRQHandler
#define MIC_I2S_DMA_CLK_ENABLE()  __HAL_RCC_DMA1_CLK_ENABLE()

/* I2S 引脚配置 */
#define MIC_I2S_WS_PIN           GPIO_PIN_12   /* PB12 - WS (LRCLK) */
#define MIC_I2S_WS_PORT          GPIOB
#define MIC_I2S_CK_PIN           GPIO_PIN_13   /* PB13 - CK (BCLK) */
#define MIC_I2S_CK_PORT          GPIOB
#define MIC_I2S_SD_PIN           GPIO_PIN_15   /* PB15 - SD (DATA) */
#define MIC_I2S_SD_PORT          GPIOB
#define MIC_I2S_GPIO_CLK_ENABLE() __HAL_RCC_GPIOB_CLK_ENABLE()

/* DMA 配置 */
#define MIC_I2S_DMA_INSTANCE     DMA1_Channel4
#define MIC_I2S_DMA_IRQn         DMA1_Channel4_IRQn
#define MIC_I2S_DMA_IRQHandler   DMA1_Channel4_IRQHandler

/* 音频采集配置 */
#define MIC_DMA_BUFFER_SIZE      512           /* DMA缓冲区大小（样本数） */
#define MIC_SAMPLE_BUFFER_SIZE   (MIC_DMA_BUFFER_SIZE * 2) /* 字节数 */

/* 麦克风状态 */
typedef enum {
    MIC_STATUS_STOPPED = 0,      /* 停止状态 */
    MIC_STATUS_INITIALIZING,     /* 初始化中 */
    MIC_STATUS_READY,           /* 就绪状态 */
    MIC_STATUS_RECORDING,       /* 正在录音 */
    MIC_STATUS_PAUSED,          /* 暂停状态 */
    MIC_STATUS_WAITING_SPEECH,  /* 等待语音开始（VAD模式） */
    MIC_STATUS_SPEECH_ACTIVE,   /* 语音活动中 */
    MIC_STATUS_ERROR            /* 错误状态 */
} microphone_status_t;

/* 录音模式 */
typedef enum {
    MIC_MODE_MANUAL = 0,        /* 手动模式：按钮控制 */
    MIC_MODE_VAD_AUTO,          /* VAD自动模式：语音活动检测 */
    MIC_MODE_HYBRID             /* 混合模式：按钮+VAD */
} microphone_mode_t;

/* 麦克风控制结构体 */
typedef struct {
    microphone_status_t status;          /* 当前状态 */
    microphone_mode_t mode;              /* 录音模式 */

    audio_buffer_t audio_buffer;         /* 音频缓冲区（原始数据） */
    audio_buffer_t processed_buffer;     /* 处理后的音频缓冲区 */

    audio_processor_t audio_processor;   /* 音频处理器 */
    audio_quality_monitor_t quality_monitor; /* 质量监控器 */

    uint16_t dma_buffer[MIC_DMA_BUFFER_SIZE]; /* DMA双缓冲区 */
    uint32_t sample_count;               /* 已采集样本数 */
    uint32_t processed_sample_count;     /* 已处理样本数 */
    uint32_t overflow_count;             /* 缓冲区溢出计数 */

    bool is_dma_half_complete;           /* DMA半传输完成标志 */
    bool is_dma_complete;                /* DMA传输完成标志 */
    bool enable_audio_processing;        /* 启用音频处理 */
    bool enable_vad_auto_mode;           /* 启用VAD自动模式 */

    uint32_t last_error;                 /* 最后错误代码 */
    uint32_t vad_silence_timeout;        /* VAD静音超时（样本数） */
    uint32_t vad_silence_counter;        /* VAD静音计数器 */

    /* 统计信息 */
    uint32_t total_recordings;           /* 总录音次数 */
    uint32_t total_processing_time_ms;   /* 总处理时间（ms） */
    uint32_t max_processing_delay_ms;    /* 最大处理延迟（ms） */
} microphone_t;

/* 初始化麦克风 */
bool microphone_init(void);

/* 配置麦克风 */
bool microphone_configure(microphone_mode_t mode, bool enable_processing);

/* 开始录音 */
bool microphone_start_recording(void);

/* 停止录音 */
bool microphone_stop_recording(void);

/* 暂停录音 */
bool microphone_pause_recording(void);

/* 恢复录音 */
bool microphone_resume_recording(void);

/* 获取当前状态 */
microphone_status_t microphone_get_status(void);

/* 获取录音模式 */
microphone_mode_t microphone_get_mode(void);

/* 获取音频缓冲区（原始数据） */
audio_buffer_t* microphone_get_raw_audio_buffer(void);

/* 获取处理后的音频缓冲区 */
audio_buffer_t* microphone_get_processed_audio_buffer(void);

/* 获取已采集样本数 */
uint32_t microphone_get_sample_count(void);

/* 获取已处理样本数 */
uint32_t microphone_get_processed_sample_count(void);

/* 获取缓冲区溢出计数 */
uint32_t microphone_get_overflow_count(void);

/* 清空音频缓冲区 */
void microphone_clear_buffer(void);

/* 清空处理后的缓冲区 */
void microphone_clear_processed_buffer(void);

/* 处理DMA中断 */
void microphone_dma_half_complete_callback(void);
void microphone_dma_complete_callback(void);

/* 麦克风轮询函数（在主循环中调用） */
void microphone_poll(void);

/* 错误处理 */
void microphone_error_handler(uint32_t error_code);

/* 获取错误信息 */
const char* microphone_get_error_string(uint32_t error_code);

/* 获取音频处理器指针 */
audio_processor_t* microphone_get_audio_processor(void);

/* 获取质量监控器指针 */
audio_quality_monitor_t* microphone_get_quality_monitor(void);

/* 检查是否有语音活动 */
bool microphone_is_speech_active(void);

/* 检查语音是否开始 */
bool microphone_is_speech_started(void);

/* 检查语音是否结束 */
bool microphone_is_speech_ended(void);

/* 获取VAD静音超时时间（ms） */
uint32_t microphone_get_vad_silence_timeout(void);

/* 设置VAD静音超时时间（ms） */
void microphone_set_vad_silence_timeout(uint32_t timeout_ms);

/* 获取音频质量状态 */
audio_quality_state_t microphone_get_audio_quality_state(void);

/* 获取音频质量报告 */
void microphone_get_audio_quality_report(char *buffer, uint32_t buffer_size);

/* 启用/禁用音频处理 */
bool microphone_enable_audio_processing(bool enable);

/* 启用/禁用VAD自动模式 */
bool microphone_enable_vad_auto_mode(bool enable);

/* 获取统计信息 */
void microphone_get_stats(uint32_t *total_recordings,
                         uint32_t *total_processing_time_ms,
                         uint32_t *max_processing_delay_ms);

#endif /* __MICROPHONE_H */