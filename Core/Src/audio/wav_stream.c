/**
 * @file  wav_stream.c
 * @brief WAV 音频文件流式播放器实现
 *        环形缓冲区接收 TCP 下发的 PCM 数据，
 *        DMA Circular ISR 从环形缓冲区读取并送入 I2S2/MAX98357A 播放。
 *        拥有独立的 TX DMA 缓冲区，与欢乐颂播放器完全独立。
 */
#include "audio/wav_stream.h"
#include "audio/i2s_mic.h"
#include "wifi/esp8266.h"
#include "debug/debug_uart.h"
#include <string.h>

#ifndef WAV_STREAM_DEBUG
#define WAV_STREAM_DEBUG 0
#endif

#define WAV_STREAM_RX_CHUNK_SIZE 256u

#if WAV_STREAM_DEBUG
#define WAV_LOG(...) debug_printf(__VA_ARGS__)
#else
#define WAV_LOG(...) ((void)0)
#endif

/* ==================== 环形缓冲区 ==================== */

typedef struct
{
    uint8_t buf[WAV_RING_BUF_SIZE];
    volatile uint16_t head;            /* 写入位置（主循环 TCP 接收端） */
    volatile uint16_t tail;            /* 读取位置（DMA ISR 端） */
    volatile uint32_t total_expected;  /* 预期总 PCM 字节数 */
    volatile uint32_t total_written;   /* 已写入字节数 */
    volatile uint32_t total_read;      /* 已读出字节数 */
    volatile uint8_t eof;              /* 所有数据已写入 */
} wav_ring_buf_t;

/* ==================== 播放器上下文 ==================== */

typedef struct
{
    wav_stream_state_t state;
    wav_ring_buf_t ring;
    uint32_t pcm_size;         /* 总 PCM 字节数 */
    uint16_t sample_rate;      /* 采样率 */
    uint32_t dbg_last_log;     /* debug 日志节流 */
    uint32_t underrun_count;   /* DMA 填充时环形缓冲不足次数 */
    uint32_t dbg_last_underrun;
    uint32_t last_rx_tick;
    volatile uint8_t active;   /* DMA 播放中标志 */
    volatile uint8_t finished; /* 播放完成标志 */
    volatile uint8_t error;    /* 错误标志 */
} wav_stream_player_t;

/* ==================== 静态变量 ==================== */

static wav_stream_player_t s_player;
static uint16_t wav_dma_buf[WAV_DMA_BUFFER_WORDS];
static uint8_t wav_rx_tmp[WAV_STREAM_RX_CHUNK_SIZE];

/* ==================== 环形缓冲区操作 ==================== */

static uint16_t ring_available(const wav_ring_buf_t *r)
{
    uint16_t h = r->head;
    uint16_t t = r->tail;

    if (h >= t)
        return (uint16_t)(h - t);
    else
        return (uint16_t)(WAV_RING_BUF_SIZE - t + h);
}

static uint16_t ring_free(const wav_ring_buf_t *r)
{
    return (uint16_t)(WAV_RING_BUF_SIZE - 1u - ring_available(r));
}

static uint16_t ring_write(wav_ring_buf_t *r, const uint8_t *data, uint16_t len)
{
    uint16_t free_space = ring_free(r);
    uint16_t to_write;
    uint16_t first_chunk;
    uint16_t h;

    if (len > free_space)
        len = free_space;

    to_write = len;
    h = r->head;

    /* 写到缓冲区末尾 */
    first_chunk = (uint16_t)(WAV_RING_BUF_SIZE - h);
    if (first_chunk > to_write)
        first_chunk = to_write;

    memcpy(&r->buf[h], data, first_chunk);
    to_write -= first_chunk;

    if (to_write > 0u)
    {
        /* 回绕写 */
        memcpy(&r->buf[0], data + first_chunk, to_write);
        r->head = to_write;
    }
    else
    {
        r->head = (uint16_t)((h + first_chunk) % WAV_RING_BUF_SIZE);
    }

    r->total_written += len;
    return len;
}

/**
 * 从环形缓冲区读取 mono PCM16 并直接展开成 I2S stereo DMA 数据。
 * ISR 中调用，避免在中断栈上放置一整个半缓冲的临时 samples 数组。
 */
static uint16_t ring_read_to_stereo_dma(wav_ring_buf_t *r, uint16_t *dst, uint16_t frames)
{
    uint16_t avail_bytes = ring_available(r);
    uint16_t read_frames;
    uint16_t t;
    uint16_t i;

    read_frames = (uint16_t)(avail_bytes / 2u);
    if (read_frames > frames)
        read_frames = frames;
    if (read_frames == 0u)
        return 0u;

    t = r->tail;
    for (i = 0u; i < read_frames; i++)
    {
        uint16_t lo = r->buf[t];
        uint16_t sample;

        t++;
        if (t >= WAV_RING_BUF_SIZE)
            t = 0u;

        sample = (uint16_t)(lo | ((uint16_t)r->buf[t] << 8));

        t++;
        if (t >= WAV_RING_BUF_SIZE)
            t = 0u;

        dst[i * 2u] = sample;
        dst[i * 2u + 1u] = sample;
    }

    r->tail = t;
    r->total_read += (uint32_t)(read_frames * 2u);
    return read_frames;
}

/* ==================== DMA 半缓冲填充 ==================== */

/**
 * 从环形缓冲区读取 PCM 数据填充 DMA 半缓冲区
 * 每帧写 2 个 uint16_t（立体声对: L=sample, R=sample）
 * 无数据时填充静音
 */
static void fill_dma_half(uint16_t *dst, uint16_t frames)
{
    uint16_t got;
    uint16_t i;

    got = ring_read_to_stereo_dma(&s_player.ring, dst, frames);
    if (got < frames && !s_player.ring.eof)
    {
        s_player.underrun_count++;
    }

    /* 无数据部分填静音 */
    for (i = got; i < frames; i++)
    {
        dst[i * 2u] = 0u;
        dst[i * 2u + 1u] = 0u;
    }

    /* 检查是否已全部播放完毕 */
    if (s_player.ring.eof &&
        s_player.ring.total_read >= s_player.ring.total_expected)
    {
        if (got == 0u)
        {
            s_player.finished = 1u;
        }
    }
}

/* ==================== 公开接口实现 ==================== */

void wav_stream_init(uint32_t pcm_size, uint16_t sample_rate)
{
    memset(&s_player, 0, sizeof(s_player));
    memset(wav_dma_buf, 0, sizeof(wav_dma_buf));
    esp8266_rx_drop_reset();

    s_player.pcm_size = pcm_size;
    s_player.sample_rate = sample_rate;
    s_player.ring.total_expected = pcm_size;
    s_player.last_rx_tick = HAL_GetTick();
    s_player.state = WAV_STREAM_BUFFERING;

    WAV_LOG("[WAV] init: pcm_size=%lu sr=%u\r\n",
            (unsigned long)pcm_size, (unsigned)sample_rate);
}

uint16_t wav_stream_feed(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0u)
        return 0u;

    if (s_player.state != WAV_STREAM_BUFFERING &&
        s_player.state != WAV_STREAM_PLAYING &&
        s_player.state != WAV_STREAM_DRAINING)
    {
        return 0u;
    }

    return ring_write(&s_player.ring, data, len);
}

uint8_t wav_stream_start_playback(void)
{
    I2S_HandleTypeDef *hi2s;
    DMA_HandleTypeDef *hdma_tx;

    if (s_player.state != WAV_STREAM_BUFFERING)
        return 0u;

    hi2s = i2s_mic_get_handle();
    hdma_tx = i2s_mic_get_tx_dma_handle();

    if (!hi2s || !hdma_tx)
    {
        s_player.state = WAV_STREAM_ERROR;
        s_player.error = 1u;
        return 0u;
    }

    /* 预填充两个半缓冲区 */
    fill_dma_half(&wav_dma_buf[0], WAV_DMA_HALF_FRAMES);
    fill_dma_half(&wav_dma_buf[WAV_DMA_HALF_FRAMES * 2u], WAV_DMA_HALF_FRAMES);

    /* 配置 DMA1_Channel5 Circular */
    __HAL_RCC_DMA1_CLK_ENABLE();
    hdma_tx->Instance = DMA1_Channel5;
    hdma_tx->Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_tx->Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_tx->Init.MemInc = DMA_MINC_ENABLE;
    hdma_tx->Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_tx->Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_tx->Init.Mode = DMA_CIRCULAR;
    hdma_tx->Init.Priority = DMA_PRIORITY_HIGH;

    if (HAL_DMA_Init(hdma_tx) != HAL_OK)
    {
        s_player.state = WAV_STREAM_ERROR;
        s_player.error = 1u;
        return 0u;
    }

    __HAL_LINKDMA(hi2s, hdmatx, *hdma_tx);
    HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);

    s_player.active = 1u;
    s_player.state = WAV_STREAM_PLAYING;

    if (HAL_I2S_Transmit_DMA(hi2s, wav_dma_buf, WAV_DMA_BUFFER_WORDS) != HAL_OK)
    {
        s_player.active = 0u;
        s_player.state = WAV_STREAM_ERROR;
        s_player.error = 1u;
        return 0u;
    }

    WAV_LOG("[WAV] DMA playback started\r\n");
    return 1u;
}

void wav_stream_mark_eof(void)
{
    s_player.ring.eof = 1u;

    if (s_player.state == WAV_STREAM_PLAYING)
    {
        s_player.state = WAV_STREAM_DRAINING;
        WAV_LOG("[WAV] EOF marked, draining\r\n");
    }
    else if (s_player.state == WAV_STREAM_BUFFERING)
    {
        WAV_LOG("[WAV] EOF marked while buffering\r\n");
    }
}

void wav_stream_service(void)
{
    uint16_t n;
    uint8_t read_rounds = 0u;

    if (s_player.state == WAV_STREAM_IDLE ||
        s_player.state == WAV_STREAM_DONE ||
        s_player.state == WAV_STREAM_ERROR)
    {
        return;
    }

    /* 循环读取 TCP 数据直到无更多数据或环形缓冲区满 */
    if (s_player.state == WAV_STREAM_BUFFERING ||
        s_player.state == WAV_STREAM_PLAYING ||
        s_player.state == WAV_STREAM_DRAINING)
    {
        while (read_rounds < 16u && ring_free(&s_player.ring) > 0u)
        {
            uint32_t remaining;
            uint16_t free_bytes;
            uint16_t max_read;

            if (s_player.ring.total_written >= s_player.ring.total_expected)
                break;

            remaining = s_player.ring.total_expected - s_player.ring.total_written;
            free_bytes = ring_free(&s_player.ring);
            max_read = (free_bytes < sizeof(wav_rx_tmp)) ? free_bytes : (uint16_t)sizeof(wav_rx_tmp);
            if (remaining < max_read)
                max_read = (uint16_t)remaining;
            if (max_read == 0u)
                break;

            n = esp8266_tcp_read_raw(wav_rx_tmp, max_read);
            if (n == 0u)
                break;
            wav_stream_feed(wav_rx_tmp, n);
            s_player.last_rx_tick = HAL_GetTick();
            read_rounds++;
        }
    }

    if (!s_player.ring.eof &&
        s_player.ring.total_written < s_player.ring.total_expected &&
        (HAL_GetTick() - s_player.last_rx_tick) > 5000u)
    {
        s_player.active = 0u;
        s_player.error = 1u;
        s_player.finished = 1u;
    }

    /* 周期性日志：每读取 4096 字节输出一次 */
    {
        uint32_t tw = s_player.ring.total_written;
        if (tw >= s_player.dbg_last_log + 4096u || (read_rounds > 0u && tw < 512u))
        {
            WAV_LOG("[WAV] rx=%lu/%lu ring=%u rd=%u\r\n",
                    (unsigned long)tw,
                    (unsigned long)s_player.ring.total_expected,
                    (unsigned)ring_available(&s_player.ring),
                    (unsigned)read_rounds);
            s_player.dbg_last_log = tw;
        }

        if (s_player.underrun_count != s_player.dbg_last_underrun &&
            (s_player.underrun_count <= 3u ||
             s_player.underrun_count >= (s_player.dbg_last_underrun + 16u)))
        {
            WAV_LOG("[WAV] underrun=%lu ring=%u rx=%lu/%lu\r\n",
                    (unsigned long)s_player.underrun_count,
                    (unsigned)ring_available(&s_player.ring),
                    (unsigned long)s_player.ring.total_written,
                    (unsigned long)s_player.ring.total_expected);
            s_player.dbg_last_underrun = s_player.underrun_count;
        }
    }

    /* 检查是否所有数据已接收完毕 */
    if (!s_player.ring.eof &&
        s_player.ring.total_written >= s_player.ring.total_expected)
    {
        wav_stream_mark_eof();
    }

    /* 预缓冲达标后启动播放 */
    if (s_player.state == WAV_STREAM_BUFFERING)
    {
        uint16_t avail = ring_available(&s_player.ring);

        /* 如果已 EOF 或缓冲数据足够则启动 */
        if (s_player.ring.eof || avail >= WAV_PREBUFFER_THRESHOLD)
        {
            if (avail > 0u)
            {
                WAV_LOG("[WAV] prebuf done, avail=%u, starting\r\n",
                        (unsigned)avail);
                wav_stream_start_playback();
            }
            else if (s_player.ring.eof)
            {
                /* EOF 但无数据 */
                s_player.state = WAV_STREAM_DONE;
                s_player.finished = 1u;
            }
        }
    }

    /* 检查播放完成 */
    if (s_player.finished)
    {
        I2S_HandleTypeDef *hi2s = i2s_mic_get_handle();

        s_player.active = 0u;
        (void)HAL_I2S_DMAStop(hi2s);

        s_player.state = s_player.error ? WAV_STREAM_ERROR : WAV_STREAM_DONE;
        WAV_LOG("[WAV] playback %s\r\n",
                s_player.error ? "error" : "done");
    }
}

wav_stream_state_t wav_stream_get_state(void)
{
    return s_player.state;
}

uint16_t wav_stream_ring_available(void)
{
    return ring_available(&s_player.ring);
}

uint8_t wav_stream_is_done(void)
{
    return (s_player.state == WAV_STREAM_DONE) ? 1u : 0u;
}

uint8_t wav_stream_is_error(void)
{
    return (s_player.state == WAV_STREAM_ERROR) ? 1u : 0u;
}

uint8_t wav_stream_is_active(void)
{
    return s_player.active;
}

uint32_t wav_stream_total_written(void)
{
    return s_player.ring.total_written;
}

uint32_t wav_stream_total_read(void)
{
    return s_player.ring.total_read;
}

uint32_t wav_stream_underrun_count(void)
{
    return s_player.underrun_count;
}

void wav_stream_mark_error(void)
{
    s_player.active = 0u;
    s_player.error = 1u;
    s_player.finished = 1u;
    s_player.state = WAV_STREAM_ERROR;
    WAV_LOG("[WAV] I2S/DMA error\r\n");
}

void wav_stream_dma_service_half(uint8_t half_index)
{
    uint16_t *half_buf = &wav_dma_buf[half_index * WAV_DMA_HALF_FRAMES * 2u];

    if (!s_player.active)
        return;

    fill_dma_half(half_buf, WAV_DMA_HALF_FRAMES);
}

uint16_t *wav_stream_get_dma_buf(void)
{
    return wav_dma_buf;
}

uint16_t wav_stream_get_dma_buf_size(void)
{
    return WAV_DMA_BUFFER_WORDS;
}

void wav_stream_stop(void)
{
    if (s_player.active)
    {
        I2S_HandleTypeDef *hi2s = i2s_mic_get_handle();

        s_player.active = 0u;
        (void)HAL_I2S_DMAStop(hi2s);
    }

    s_player.state = WAV_STREAM_IDLE;
    s_player.finished = 0u;
    s_player.error = 0u;
    WAV_LOG("[WAV] stopped\r\n");
}
