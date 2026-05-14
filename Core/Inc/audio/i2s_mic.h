/**
 * @file  i2s_mic.h
 * @brief INMP441 数字麦克风 I2S 驱动
 *        使用 SPI2/I2S2 Master Receive 模式 + DMA 双缓冲采集
 *        引脚: SCK=PB13, WS=PB12, SD=PB15
 */
#ifndef __I2S_MIC_H
#define __I2S_MIC_H

#include "main.h"
#include <stdint.h>

/* ===================== I2S2 引脚配置 ===================== */

/* I2S2_CK  — 串行时钟:  PB13 */
#define I2S_SCK_PORT GPIOB
#define I2S_SCK_PIN GPIO_PIN_13

/* I2S2_WS  — 声道选择:  PB12 */
#define I2S_WS_PORT GPIOB
#define I2S_WS_PIN GPIO_PIN_12

/* I2S2_SD  — 数据输入:  PB15 (I2S2 Master RX 使用 MOSI 引脚接收) */
#define I2S_SD_PORT GPIOB
#define I2S_SD_PIN GPIO_PIN_15

/* INMP441 声道选择（与模块 L/R 引脚硬件连接一致）
 * L/R=GND → 左声道, L/R=VCC → 右声道
 * 注意: STM32 I2S Master RX 的 DMA 帧起始声道可能与预期相反，
 *       因此提供运行时自动检测功能 */
#define I2S_MIC_SLOT_LEFT  0u
#define I2S_MIC_SLOT_RIGHT 1u
#define I2S_MIC_SLOT_AUTO  2u   /* 自动检测有效声道 */
#define I2S_MIC_SLOT_SEL   I2S_MIC_SLOT_AUTO

/* ===================== 音频参数 ===================== */

/* 采样率 32kHz（SCK=2.06MHz，满足 INMP441 最低 1.024MHz 要求） */
#define I2S_SAMPLE_RATE     I2S_AUDIOFREQ_32K

/* 每个 DMA 半缓冲区大小（单位: uint16_t 原始采样字） */
#define I2S_HALF_BUF_SIZE   1024

/* 完整 DMA 缓冲区大小（双缓冲，单位: uint16_t 样本数） */
#define I2S_DMA_BUF_SIZE    (I2S_HALF_BUF_SIZE * 2)

/* INMP441 启动稳定所需丢弃的回调次数（约 40ms） */
#define I2S_STARTUP_DISCARD_CALLBACKS  5u

/* 声道自动检测期间的回调采样次数 */
#define I2S_CHANNEL_DETECT_CALLBACKS   4u

/* ===================== 回调类型 ===================== */

/**
 * 音频数据就绪回调函数类型
 * @param buf   指向就绪的 PCM 样本缓冲区（16bit 有符号，左声道数据）
 * @param len   样本数量
 */
typedef void (*i2s_mic_callback_t)(const int16_t *buf, uint16_t len);

/* ===================== 接口函数 ===================== */

/**
 * 初始化 I2S2 外设及 DMA 通道，配置为 Master Receive 模式
 * 初始化后处于停止状态，需调用 i2s_mic_start() 开始采集
 */
void i2s_mic_init(void);

/**
 * 开始录音（启动 DMA 循环接收）
 */
void i2s_mic_start(void);

/**
 * 停止录音
 */
void i2s_mic_stop(void);

/**
 * 查询当前是否正在录音
 * @return 1=录音中, 0=已停止
 */
uint8_t i2s_mic_is_recording(void);

/**
 * 注册音频数据就绪回调
 * DMA 半满/全满中断触发时调用此回调
 * @param cb 回调函数指针
 */
void i2s_mic_set_callback(i2s_mic_callback_t cb);

/**
 * 获取 I2S 句柄指针（供中断处理函数使用）
 */
I2S_HandleTypeDef *i2s_mic_get_handle(void);

/**
 * 获取 DMA 句柄指针（供中断处理函数使用）
 */
DMA_HandleTypeDef *i2s_mic_get_dma_handle(void);

/**
 * 硬件连接检测: 短暂启动 I2S 采集，检查是否读到非零数据
 * @return 1=检测到麦克风数据, 0=未检测到
 */
uint8_t i2s_mic_probe(void);

/**
 * Play a short mono test tone on I2S2 for MAX98357A speaker checks.
 * The driver temporarily switches I2S2 from microphone RX to master TX and
 * restores the microphone configuration before returning.
 * @return 1=played successfully, 0=I2S transmit failed
 */
uint8_t i2s_mic_play_tone(uint16_t frequency_hz, uint16_t duration_ms);

/**
 * Play a linear frequency sweep on I2S2 for speaker response checks.
 * @return 1=played successfully, 0=I2S transmit failed
 */
uint8_t i2s_mic_play_sweep(uint16_t start_hz, uint16_t end_hz, uint16_t duration_ms);

/**
 * Play a sequence of 1 kHz beeps with different PCM amplitudes.
 * levels_percent values are clamped to 0..100.
 * @return 1=played successfully, 0=I2S transmit failed
 */
uint8_t i2s_mic_play_volume_steps(const uint8_t *levels_percent, uint8_t count);

/**
 * Start a streaming speaker playback session on I2S2.
 * The microphone RX configuration is restored by i2s_mic_speaker_stream_end().
 *
 * @param sample_rate_hz Supported values: 8000, 16000, 32000, 44100, 48000.
 * @return 1=stream started, 0=I2S TX setup failed
 */
uint8_t i2s_mic_speaker_stream_begin(uint32_t sample_rate_hz);

/**
 * Write mono PCM16 samples to the current speaker stream.
 * Samples are duplicated to both I2S slots for MAX98357A compatibility.
 *
 * @return 1=all samples transmitted, 0=no stream or transmit failed
 */
uint8_t i2s_mic_speaker_stream_write(const int16_t *samples, uint16_t sample_count);

/**
 * Stop the current speaker stream and restore microphone RX mode.
 */
void i2s_mic_speaker_stream_end(void);

uint8_t i2s_mic_speaker_stream_is_active(void);

#endif /* __I2S_MIC_H */
