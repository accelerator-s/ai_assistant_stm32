/**
 * @file  microphone.c
 * @brief INMP441 数字麦克风驱动实现
 */
#include "audio/microphone.h"
#include "audio/audio_config.h"
#include "stm32f1xx_hal.h"
#include <string.h>

/* 全局麦克风实例 */
static microphone_t g_microphone = {0};

/* I2S 和 DMA 句柄 */
static I2S_HandleTypeDef hi2s2;
static DMA_HandleTypeDef hdma_i2s_rx;

/* 私有函数声明 */
static bool microphone_init_i2s(void);
static bool microphone_init_dma(void);
static bool microphone_init_gpio(void);
static void microphone_process_dma_data(void);
static void microphone_process_audio_data(uint8_t *data, uint32_t size);
static void microphone_update_vad_state(void);
static void microphone_handle_vad_auto_mode(void);

/* 初始化麦克风 */
bool microphone_init(void)
{
    /* 初始化状态 */
    g_microphone.status = MIC_STATUS_INITIALIZING;
    g_microphone.mode = MIC_MODE_MANUAL;
    g_microphone.sample_count = 0;
    g_microphone.processed_sample_count = 0;
    g_microphone.overflow_count = 0;
    g_microphone.is_dma_half_complete = false;
    g_microphone.is_dma_complete = false;
    g_microphone.enable_audio_processing = true;
    g_microphone.enable_vad_auto_mode = false;
    g_microphone.last_error = 0;
    g_microphone.vad_silence_timeout = 3000;  /* 3秒静音超时 */
    g_microphone.vad_silence_counter = 0;

    /* 初始化统计信息 */
    g_microphone.total_recordings = 0;
    g_microphone.total_processing_time_ms = 0;
    g_microphone.max_processing_delay_ms = 0;

    /* 初始化音频缓冲区 */
    audio_buffer_init(&g_microphone.audio_buffer);
    audio_buffer_init(&g_microphone.processed_buffer);

    /* 初始化音频处理器 */
    if (!audio_processor_init(&g_microphone.audio_processor, NULL)) {
        g_microphone.status = MIC_STATUS_ERROR;
        g_microphone.last_error = 100;
        return false;
    }

    /* 初始化质量监控器 */
    if (!audio_quality_init(&g_microphone.quality_monitor, NULL)) {
        g_microphone.status = MIC_STATUS_ERROR;
        g_microphone.last_error = 101;
        return false;
    }

    /* 初始化DMA缓冲区 */
    memset(g_microphone.dma_buffer, 0, sizeof(g_microphone.dma_buffer));

    /* 初始化GPIO */
    if (!microphone_init_gpio()) {
        g_microphone.status = MIC_STATUS_ERROR;
        g_microphone.last_error = 1;
        return false;
    }

    /* 初始化I2S */
    if (!microphone_init_i2s()) {
        g_microphone.status = MIC_STATUS_ERROR;
        g_microphone.last_error = 2;
        return false;
    }

    /* 初始化DMA */
    if (!microphone_init_dma()) {
        g_microphone.status = MIC_STATUS_ERROR;
        g_microphone.last_error = 3;
        return false;
    }

    g_microphone.status = MIC_STATUS_READY;
    return true;
}

/* 配置麦克风 */
bool microphone_configure(microphone_mode_t mode, bool enable_processing)
{
    if (g_microphone.status == MIC_STATUS_ERROR) {
        return false;
    }

    g_microphone.mode = mode;
    g_microphone.enable_audio_processing = enable_processing;

    /* 根据模式设置VAD自动模式 */
    if (mode == MIC_MODE_VAD_AUTO) {
        g_microphone.enable_vad_auto_mode = true;
        g_microphone.status = MIC_STATUS_WAITING_SPEECH;
    } else if (mode == MIC_MODE_HYBRID) {
        g_microphone.enable_vad_auto_mode = true;
    } else {
        g_microphone.enable_vad_auto_mode = false;
    }

    return true;
}

/* 初始化GPIO */
static bool microphone_init_gpio(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* 使能GPIO时钟 */
    MIC_I2S_GPIO_CLK_ENABLE();

    /* 配置I2S引脚 */
    GPIO_InitStruct.Pin = MIC_I2S_WS_PIN | MIC_I2S_CK_PIN | MIC_I2S_SD_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(MIC_I2S_WS_PORT, &GPIO_InitStruct);

    return true;
}

/* 初始化I2S */
static bool microphone_init_i2s(void)
{
    /* 使能I2S时钟 */
    MIC_I2S_CLK_ENABLE();

    /* 配置I2S - STM32F1配置 */
    hi2s2.Instance = MIC_I2S_INSTANCE;
    hi2s2.Init.Mode = I2S_MODE_MASTER_RX;
    hi2s2.Init.Standard = I2S_STANDARD_PHILIPS;
    hi2s2.Init.DataFormat = I2S_DATAFORMAT_16B;
    hi2s2.Init.MCLKOutput = I2S_MCLKOUTPUT_DISABLE;
    hi2s2.Init.AudioFreq = I2S_AUDIOFREQ_16K;
    hi2s2.Init.CPOL = I2S_CPOL_LOW;

    if (HAL_I2S_Init(&hi2s2) != HAL_OK) {
        return false;
    }

    return true;
}

/* 初始化DMA */
static bool microphone_init_dma(void)
{
    /* 使能DMA时钟 */
    MIC_I2S_DMA_CLK_ENABLE();

    /* 配置DMA */
    hdma_i2s_rx.Instance = MIC_I2S_DMA_INSTANCE;
    hdma_i2s_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_i2s_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_i2s_rx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_i2s_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_i2s_rx.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_i2s_rx.Init.Mode = DMA_CIRCULAR;
    hdma_i2s_rx.Init.Priority = DMA_PRIORITY_HIGH;

    if (HAL_DMA_Init(&hdma_i2s_rx) != HAL_OK) {
        return false;
    }

    /* 关联DMA到I2S */
    __HAL_LINKDMA(&hi2s2, hdmarx, hdma_i2s_rx);

    /* 使能DMA中断 */
    HAL_NVIC_SetPriority(MIC_I2S_DMA_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(MIC_I2S_DMA_IRQn);

    return true;
}

/* 开始录音 */
bool microphone_start_recording(void)
{
    if (g_microphone.status != MIC_STATUS_READY &&
        g_microphone.status != MIC_STATUS_PAUSED &&
        g_microphone.status != MIC_STATUS_WAITING_SPEECH) {
        return false;
    }

    /* 启动DMA接收 */
    if (HAL_I2S_Receive_DMA(&hi2s2,
                           (uint16_t*)g_microphone.dma_buffer,
                           MIC_DMA_BUFFER_SIZE) != HAL_OK) {
        g_microphone.status = MIC_STATUS_ERROR;
        g_microphone.last_error = 4;
        return false;
    }

    if (g_microphone.mode == MIC_MODE_VAD_AUTO) {
        g_microphone.status = MIC_STATUS_WAITING_SPEECH;
    } else {
        g_microphone.status = MIC_STATUS_RECORDING;
    }

    g_microphone.total_recordings++;
    return true;
}

/* 停止录音 */
bool microphone_stop_recording(void)
{
    if (g_microphone.status != MIC_STATUS_RECORDING &&
        g_microphone.status != MIC_STATUS_PAUSED) {
        return false;
    }

    /* 停止DMA传输 */
    if (HAL_I2S_DMAStop(&hi2s2) != HAL_OK) {
        g_microphone.status = MIC_STATUS_ERROR;
        g_microphone.last_error = 5;
        return false;
    }

    g_microphone.status = MIC_STATUS_READY;
    return true;
}

/* 暂停录音 */
bool microphone_pause_recording(void)
{
    if (g_microphone.status != MIC_STATUS_RECORDING) {
        return false;
    }

    /* 暂停DMA传输 */
    if (HAL_I2S_DMAPause(&hi2s2) != HAL_OK) {
        g_microphone.status = MIC_STATUS_ERROR;
        g_microphone.last_error = 6;
        return false;
    }

    g_microphone.status = MIC_STATUS_PAUSED;
    return true;
}

/* 恢复录音 */
bool microphone_resume_recording(void)
{
    if (g_microphone.status != MIC_STATUS_PAUSED) {
        return false;
    }

    /* 恢复DMA传输 */
    if (HAL_I2S_DMAResume(&hi2s2) != HAL_OK) {
        g_microphone.status = MIC_STATUS_ERROR;
        g_microphone.last_error = 7;
        return false;
    }

    g_microphone.status = MIC_STATUS_RECORDING;
    return true;
}

/* 获取当前状态 */
microphone_status_t microphone_get_status(void)
{
    return g_microphone.status;
}

/* 获取音频缓冲区 */
audio_buffer_t* microphone_get_audio_buffer(void)
{
    return &g_microphone.audio_buffer;
}

/* 获取已采集样本数 */
uint32_t microphone_get_sample_count(void)
{
    return g_microphone.sample_count;
}

/* 获取缓冲区溢出计数 */
uint32_t microphone_get_overflow_count(void)
{
    return g_microphone.overflow_count +
           audio_buffer_get_overflow_count(&g_microphone.audio_buffer);
}

/* 清空音频缓冲区 */
void microphone_clear_buffer(void)
{
    audio_buffer_clear(&g_microphone.audio_buffer);
    g_microphone.sample_count = 0;
    g_microphone.overflow_count = 0;
}

/* 处理DMA半传输完成 */
void microphone_dma_half_complete_callback(void)
{
    g_microphone.is_dma_half_complete = true;
}

/* 处理DMA传输完成 */
void microphone_dma_complete_callback(void)
{
    g_microphone.is_dma_complete = true;
}

/* 处理DMA数据（在主循环中调用） */
static void microphone_process_dma_data(void)
{
    uint32_t samples_to_process = 0;
    uint8_t *buffer_start = NULL;

    if (g_microphone.is_dma_half_complete) {
        /* 处理前半部分缓冲区 */
        buffer_start = (uint8_t*)g_microphone.dma_buffer;
        samples_to_process = MIC_DMA_BUFFER_SIZE / 2;
        g_microphone.is_dma_half_complete = false;
    }
    else if (g_microphone.is_dma_complete) {
        /* 处理后半部分缓冲区 */
        buffer_start = (uint8_t*)(g_microphone.dma_buffer + (MIC_DMA_BUFFER_SIZE / 2));
        samples_to_process = MIC_DMA_BUFFER_SIZE / 2;
        g_microphone.is_dma_complete = false;
    }

    if (samples_to_process > 0 && buffer_start != NULL) {
        /* 处理音频数据 */
        microphone_process_audio_data(buffer_start, samples_to_process * 2);
    }
}

/* 处理音频数据 */
static void microphone_process_audio_data(uint8_t *data, uint32_t size)
{
    if (data == NULL || size == 0) {
        return;
    }

    uint32_t samples = size / 2;  /* 16位样本 */

    /* 写入原始音频缓冲区 */
    if (!audio_buffer_write(&g_microphone.audio_buffer, data, size)) {
        g_microphone.overflow_count++;
    }

    g_microphone.sample_count += samples;

    /* 音频处理 */
    if (g_microphone.enable_audio_processing &&
        (g_microphone.status == MIC_STATUS_RECORDING ||
         g_microphone.status == MIC_STATUS_SPEECH_ACTIVE)) {

        /* 处理每个样本 */
        for (uint32_t i = 0; i < samples; i++) {
            int16_t raw_sample = ((int16_t*)data)[i];
            q15_t q15_sample = (q15_t)raw_sample;

            /* 音频处理 */
            q15_t processed_sample = audio_processor_process_sample(&g_microphone.audio_processor, q15_sample);

            /* 写入处理后的缓冲区 */
            int16_t processed_pcm = (int16_t)processed_sample;
            if (!audio_buffer_write(&g_microphone.processed_buffer,
                                   (uint8_t*)&processed_pcm,
                                   sizeof(int16_t))) {
                g_microphone.overflow_count++;
            }

            g_microphone.processed_sample_count++;
        }

        /* 更新VAD状态 */
        microphone_update_vad_state();

        /* 处理VAD自动模式 */
        if (g_microphone.enable_vad_auto_mode) {
            microphone_handle_vad_auto_mode();
        }
    } else {
        /* 如果不处理，直接复制到处理后的缓冲区 */
        if (!audio_buffer_write(&g_microphone.processed_buffer, data, size)) {
            g_microphone.overflow_count++;
        }
        g_microphone.processed_sample_count += samples;
    }
}

/* 更新VAD状态 */
static void microphone_update_vad_state(void)
{
    /* 检查语音状态变化 */
    bool speech_started = audio_processor_is_speech_started(&g_microphone.audio_processor);
    bool speech_ended = audio_processor_is_speech_ended(&g_microphone.audio_processor);
    bool speech_active = audio_processor_is_speech_active(&g_microphone.audio_processor);

    /* 更新静音计数器 */
    if (speech_active) {
        g_microphone.vad_silence_counter = 0;
    } else {
        g_microphone.vad_silence_counter++;
    }

    /* 处理语音开始 */
    if (speech_started && g_microphone.mode == MIC_MODE_VAD_AUTO) {
        g_microphone.status = MIC_STATUS_SPEECH_ACTIVE;
    }

    /* 处理语音结束 */
    if (speech_ended && g_microphone.mode == MIC_MODE_VAD_AUTO) {
        /* 检查是否超时 */
        uint32_t silence_samples = g_microphone.vad_silence_timeout * AUDIO_SAMPLE_RATE / 1000;
        if (g_microphone.vad_silence_counter >= silence_samples) {
            g_microphone.status = MIC_STATUS_WAITING_SPEECH;
        }
    }
}

/* 处理VAD自动模式 */
static void microphone_handle_vad_auto_mode(void)
{
    if (g_microphone.mode != MIC_MODE_VAD_AUTO) {
        return;
    }

    /* 检查是否需要自动停止录音（静音超时） */
    if (g_microphone.status == MIC_STATUS_SPEECH_ACTIVE) {
        uint32_t silence_samples = g_microphone.vad_silence_timeout * AUDIO_SAMPLE_RATE / 1000;
        if (g_microphone.vad_silence_counter >= silence_samples) {
            /* 自动停止录音 */
            microphone_stop_recording();
            g_microphone.status = MIC_STATUS_WAITING_SPEECH;
        }
    }
}

/* 麦克风轮询函数（在主循环中调用） */
void microphone_poll(void)
{
    microphone_process_dma_data();
}

/* 错误处理 */
void microphone_error_handler(uint32_t error_code)
{
    g_microphone.last_error = error_code;
    g_microphone.status = MIC_STATUS_ERROR;

    /* 停止所有传输 */
    HAL_I2S_DMAStop(&hi2s2);
}

/* 获取错误信息 */
/*const char* microphone_get_error_string(uint32_t error_code)
{
    switch (error_code) {
        case 1: return "GPIO初始化失败";
        case 2: return "I2S初始化失败";
        case 3: return "DMA初始化失败";
        case 4: return "启动录音失败";
        case 5: return "停止录音失败";
        case 6: return "暂停录音失败";
        case 7: return "恢复录音失败";
        default: return "未知错误";
    }
}*/

/* DMA中断处理 */
void MIC_I2S_DMA_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_i2s_rx);
}

/* I2S中断处理 */
void MIC_I2S_IRQHandler(void)
{
    HAL_I2S_IRQHandler(&hi2s2);
}

/* DMA传输完成回调 */
void HAL_I2S_RxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == MIC_I2S_INSTANCE) {
        microphone_dma_half_complete_callback();
    }
}

/* DMA传输完成回调 */
void HAL_I2S_RxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == MIC_I2S_INSTANCE) {
        microphone_dma_complete_callback();
    }
}

/* DMA错误回调 */
void HAL_I2S_ErrorCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == MIC_I2S_INSTANCE) {
        microphone_error_handler(100 + hi2s->ErrorCode);
    }
}

/* 新增函数实现 */

/* 获取录音模式 */
microphone_mode_t microphone_get_mode(void)
{
    return g_microphone.mode;
}

/* 获取音频缓冲区（原始数据） */
audio_buffer_t* microphone_get_raw_audio_buffer(void)
{
    return &g_microphone.audio_buffer;
}

/* 获取处理后的音频缓冲区 */
audio_buffer_t* microphone_get_processed_audio_buffer(void)
{
    return &g_microphone.processed_buffer;
}

/* 获取已处理样本数 */
uint32_t microphone_get_processed_sample_count(void)
{
    return g_microphone.processed_sample_count;
}

/* 清空处理后的缓冲区 */
void microphone_clear_processed_buffer(void)
{
    audio_buffer_clear(&g_microphone.processed_buffer);
    g_microphone.processed_sample_count = 0;
}

/* 获取音频处理器指针 */
audio_processor_t* microphone_get_audio_processor(void)
{
    return &g_microphone.audio_processor;
}

/* 获取质量监控器指针 */
audio_quality_monitor_t* microphone_get_quality_monitor(void)
{
    return &g_microphone.quality_monitor;
}

/* 检查是否有语音活动 */
bool microphone_is_speech_active(void)
{
    return audio_processor_is_speech_active(&g_microphone.audio_processor);
}

/* 检查语音是否开始 */
bool microphone_is_speech_started(void)
{
    return audio_processor_is_speech_started(&g_microphone.audio_processor);
}

/* 检查语音是否结束 */
bool microphone_is_speech_ended(void)
{
    return audio_processor_is_speech_ended(&g_microphone.audio_processor);
}

/* 获取VAD静音超时时间（ms） */
uint32_t microphone_get_vad_silence_timeout(void)
{
    return g_microphone.vad_silence_timeout;
}

/* 设置VAD静音超时时间（ms） */
void microphone_set_vad_silence_timeout(uint32_t timeout_ms)
{
    g_microphone.vad_silence_timeout = timeout_ms;
}

/* 获取音频质量状态 */
audio_quality_state_t microphone_get_audio_quality_state(void)
{
    return audio_quality_get_state(&g_microphone.quality_monitor);
}

/* 获取音频质量报告 */
void microphone_get_audio_quality_report(char *buffer, uint32_t buffer_size)
{
    audio_quality_get_report(&g_microphone.quality_monitor, buffer, buffer_size);
}

/* 启用/禁用音频处理 */
bool microphone_enable_audio_processing(bool enable)
{
    if (g_microphone.status == MIC_STATUS_RECORDING ||
        g_microphone.status == MIC_STATUS_SPEECH_ACTIVE) {
        return false;  /* 录音中不能更改 */
    }

    g_microphone.enable_audio_processing = enable;
    return true;
}

/* 启用/禁用VAD自动模式 */
bool microphone_enable_vad_auto_mode(bool enable)
{
    if (g_microphone.status == MIC_STATUS_RECORDING ||
        g_microphone.status == MIC_STATUS_SPEECH_ACTIVE) {
        return false;  /* 录音中不能更改 */
    }

    g_microphone.enable_vad_auto_mode = enable;

    if (enable && g_microphone.mode == MIC_MODE_MANUAL) {
        g_microphone.mode = MIC_MODE_HYBRID;
    } else if (!enable && g_microphone.mode == MIC_MODE_HYBRID) {
        g_microphone.mode = MIC_MODE_MANUAL;
    }

    return true;
}

/* 获取统计信息 */
void microphone_get_stats(uint32_t *total_recordings,
                         uint32_t *total_processing_time_ms,
                         uint32_t *max_processing_delay_ms)
{
    if (total_recordings) {
        *total_recordings = g_microphone.total_recordings;
    }
    if (total_processing_time_ms) {
        *total_processing_time_ms = g_microphone.total_processing_time_ms;
    }
    if (max_processing_delay_ms) {
        *max_processing_delay_ms = g_microphone.max_processing_delay_ms;
    }
}

/* 扩展错误代码处理 */
const char* microphone_get_error_string(uint32_t error_code)
{
    switch (error_code) {
        case 1: return "GPIO初始化失败";
        case 2: return "I2S初始化失败";
        case 3: return "DMA初始化失败";
        case 4: return "启动录音失败";
        case 5: return "停止录音失败";
        case 6: return "暂停录音失败";
        case 7: return "恢复录音失败";
        case 100: return "音频处理器初始化失败";
        case 101: return "质量监控器初始化失败";
        case 200: return "VAD检测失败";
        case 201: return "音频处理失败";
        default: return "未知错误";
    }
}