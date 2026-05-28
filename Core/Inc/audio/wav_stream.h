/**
 * @file  wav_stream.h
 * @brief WAV 音频文件流式播放器
 *        通过 TCP 接收 PCM 数据写入环形缓冲区，
 *        由 DMA Circular ISR 从环形缓冲区读取并通过 I2S2/MAX98357A 播放。
 *        拥有独立的 DMA 缓冲区，与欢乐颂播放器互不干扰。
 */
#ifndef __WAV_STREAM_H
#define __WAV_STREAM_H

#include "main.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* ==================== 配置常量 ==================== */

#define WAV_RING_BUF_SIZE       4096u   /* 环形缓冲区 4KB，约 128ms@16kHz/16bit */
#define WAV_DMA_HALF_FRAMES     128u    /* DMA 半缓冲区帧数 */
#define WAV_DMA_BUFFER_WORDS    (WAV_DMA_HALF_FRAMES * 4u)  /* 512 uint16_t = 1KB */
#define WAV_PREBUFFER_THRESHOLD (WAV_RING_BUF_SIZE / 2u) /* 预缓冲阈值: 50% */
#define WAV_SAMPLE_RATE         16000u  /* 播放采样率 */

    /* ==================== 播放器状态 ==================== */

    typedef enum
    {
        WAV_STREAM_IDLE = 0,    /* 空闲 */
        WAV_STREAM_BUFFERING,   /* 预缓冲阶段，接收数据中 */
        WAV_STREAM_PLAYING,     /* DMA 播放中 */
        WAV_STREAM_DRAINING,    /* 数据接收完成，等待播放完毕 */
        WAV_STREAM_DONE,        /* 播放结束 */
        WAV_STREAM_ERROR        /* 错误 */
    } wav_stream_state_t;

    /* ==================== 生命周期接口 ==================== */

    /**
     * @brief 初始化流式播放器，准备接收 PCM 数据
     *        收到 SPK_WAV 命令后调用
     * @param pcm_size    预期总 PCM 字节数
     * @param sample_rate 采样率（支持 8000/11025/16000）
     */
    void wav_stream_init(uint32_t pcm_size, uint16_t sample_rate);

    /**
     * @brief 向环形缓冲区写入 PCM 数据
     *        主循环中由 TCP 接收调用
     * @param data 数据指针
     * @param len  字节长度
     * @return 实际写入的字节数
     */
    uint16_t wav_stream_feed(const uint8_t *data, uint16_t len);

    /**
     * @brief 启动 DMA 播放
     *        预缓冲达标后调用，内部配置 I2S2 TX DMA Circular
     * @return 1=成功启动, 0=失败
     */
    uint8_t wav_stream_start_playback(void);

    /**
     * @brief 标记数据接收完毕（所有 PCM 数据已写入环形缓冲区）
     */
    void wav_stream_mark_eof(void);

    /**
     * @brief 主循环服务函数
     *        负责从 TCP 读取数据、检查预缓冲、管理播放状态
     *        在 wav_stream_active 为 1 时由主循环调用
     */
    void wav_stream_service(void);

    /* ==================== 状态查询接口 ==================== */

    /**
     * @brief 获取播放器当前状态
     */
    wav_stream_state_t wav_stream_get_state(void);

    /**
     * @brief 查询环形缓冲区中可用数据字节数
     */
    uint16_t wav_stream_ring_available(void);

    /**
     * @brief 查询播放是否完成
     * @return 1=播放完成, 0=尚未完成
     */
    uint8_t wav_stream_is_done(void);

    /**
     * @brief 查询播放是否出错
     * @return 1=出错, 0=正常
     */
    uint8_t wav_stream_is_error(void);

    /* ==================== DMA 回调接口（由 i2s_mic.c 调用） ==================== */

    /**
     * @brief DMA TX 半缓冲填充回调
     *        ISR 中调用，从环形缓冲区读取 PCM 数据填充 DMA 半缓冲区
     * @param half_index 0=前半, 1=后半
     */
    void wav_stream_dma_service_half(uint8_t half_index);

    /**
     * @brief 获取 WAV DMA 缓冲区指针（供 i2s_mic.c 启动 DMA 使用）
     */
    uint16_t *wav_stream_get_dma_buf(void);

    /**
     * @brief 获取 WAV DMA 缓冲区总大小（uint16_t 单位）
     */
    uint16_t wav_stream_get_dma_buf_size(void);

    /**
     * @brief 查询 WAV 播放器是否处于活跃状态（BUFFERING/PLAYING/DRAINING）
     * @return 1=活跃, 0=非活跃
     */
    uint8_t wav_stream_is_active(void);

    uint32_t wav_stream_total_written(void);
    uint32_t wav_stream_total_read(void);
    uint32_t wav_stream_underrun_count(void);

    /**
     * @brief 标记 I2S/DMA 播放错误
     *        由 I2S 错误回调调用，主循环随后负责停止播放并回传状态。
     */
    void wav_stream_mark_error(void);

    /**
     * @brief 停止并清理 WAV 流式播放器
     *        可在播放完成或出错后调用，恢复到 IDLE 状态
     */
    void wav_stream_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __WAV_STREAM_H */
