/**
 * @file  i2s_mic.c
 * @brief INMP441 数字麦克风 I2S 驱动实现
 *        SPI2/I2S2 Master Receive + DMA1_Channel4 循环接收
 */
#include "audio/i2s_mic.h"
#include "debug/debug_uart.h"
#include <string.h>

/* I2S 和 DMA 句柄 */
static I2S_HandleTypeDef hi2s2;
static DMA_HandleTypeDef hdma_i2s2_rx;

/* DMA 双缓冲区 */
static uint16_t i2s_dma_buf[I2S_DMA_BUF_SIZE];

/* DMA 原始缓冲转 PCM16 的中间缓冲
 * 16B_EXTENDED 模式: 每帧 2 个 uint16_t (L, R)
 * 每个半缓冲区最多产生 I2S_HALF_BUF_SIZE / 2 个 PCM 样本 */
static int16_t i2s_pcm_half_buf[I2S_HALF_BUF_SIZE / 2];

/* 调试: 前 N 次回调打印原始采样值 */
static volatile uint16_t dbg_callback_count = 0;
#define DBG_CALLBACK_LIMIT 3u

/* INMP441 上电后需丢弃前几批数据（时钟锁定 + 内部稳定） */
static volatile uint16_t startup_discard_counter = 0;

/* 录音状态标志 */
static volatile uint8_t is_recording = 0;
static volatile uint8_t is_speaker_streaming = 0;

/* 数据就绪回调 */
static i2s_mic_callback_t user_callback = NULL;

/* 声道自动检测: 运行时确定 DMA 帧中哪个偏移包含有效音频 */
static volatile uint8_t  slot_detected = 0;       /* 是否已检测到有效声道 */
static volatile uint16_t slot_offset_runtime = 0;  /* 当前使用的偏移: 0 或 1 */
static volatile uint16_t detect_callback_count = 0;/* 自动检测期间已采集的回调次数 */
static volatile uint32_t detect_energy_pos0 = 0;   /* 偏移 0 累计绝对值 */
static volatile uint32_t detect_energy_pos1 = 0;   /* 偏移 1 累计绝对值 */

/**
 * 将 I2S 16B_EXTENDED 模式帧数据提取为单声道 16bit PCM
 *
 * 16B_EXTENDED 模式: 32 位声道帧，每声道仅读取 1 个 16 位 DR 值。
 * DMA 缓冲顺序（每帧 2 个 uint16_t）:
 *   [pos0, pos1, pos0, pos1, ...]
 *
 * 由于 STM32 I2S Master RX 启动时帧起始声道可能是左或右，
 * pos0 可能对应 L 也可能对应 R。使用运行时检测的
 * slot_offset_runtime（0 或 1）来选择有效声道。
 */
/* PB14 drives both 74HC125 1OE and MAX98357A SD.
 * Low: MIC path enabled, speaker muted. High: MIC path off, speaker enabled.
 * With this one-wire hardware, idle uses the quiet record-side level. */
static void i2s_bus_mode_idle(void)
{
    HAL_GPIO_WritePin(I2S_BUS_CTRL_PORT, I2S_BUS_CTRL_PIN, GPIO_PIN_RESET);
}

static void i2s_bus_mode_record(void)
{
    HAL_GPIO_WritePin(I2S_BUS_CTRL_PORT, I2S_BUS_CTRL_PIN, GPIO_PIN_RESET);
}

static void i2s_bus_mode_play(void)
{
    HAL_GPIO_WritePin(I2S_BUS_CTRL_PORT, I2S_BUS_CTRL_PIN, GPIO_PIN_SET);
}

static void i2s_bus_switch_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    gpio.Pin = I2S_BUS_CTRL_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);

    i2s_bus_mode_idle();
}

static uint16_t i2s_extract_pcm16(const uint16_t *src, uint16_t raw_len, int16_t *dst)
{
    uint16_t i;
    uint16_t out_idx = 0;
    uint16_t offset = slot_offset_runtime;

    /* 每帧 2 个 uint16_t */
    for (i = 0; (i + 1u) < raw_len; i += 2u)
    {
        dst[out_idx++] = (int16_t)src[i + offset];
    }

    return out_idx;
}

/* ===================== 内部函数 ===================== */

/**
 * I2S2 GPIO 初始化
 * PB13 — I2S2_CK  (SCK)  复用推挽输出
 * PB15 — I2S2_SD  (数据) 下拉输入（非活动时隙高阻，使用下拉避免悬空读到随机噪声）
 * PB12 — I2S2_WS  (WS)   复用推挽输出
 */
static void i2s_gpio_init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};

    /* PB13(CK) 和 PB12(WS): 复用推挽输出 */
    gpio.Pin = I2S_SCK_PIN | I2S_WS_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* PB15(SD): 主机接收模式下为下拉输入
     * INMP441 在非活动声道期间 SD 为高阻态，若不下拉则悬空读到随机噪声，
     * 导致声道自动检测失效且录音全是杂音。内部下拉电阻 ~40kΩ，
     * 不影响 INMP441 驱动能力。 */
    gpio.Pin = I2S_SD_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOB, &gpio);
}

static void i2s_speaker_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    gpio.Pin = I2S_SCK_PIN | I2S_WS_PIN | I2S_SD_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);
}

static HAL_StatusTypeDef i2s_config_master_rx(void)
{
    hi2s2.Instance = SPI2;
    hi2s2.Init.Mode = I2S_MODE_MASTER_RX;
    hi2s2.Init.Standard = I2S_STANDARD_PHILIPS;
    hi2s2.Init.DataFormat = I2S_DATAFORMAT_16B_EXTENDED;
    hi2s2.Init.MCLKOutput = I2S_MCLKOUTPUT_DISABLE;
    hi2s2.Init.AudioFreq = I2S_AUDIOFREQ_32K;
    hi2s2.Init.CPOL = I2S_CPOL_LOW;

    return HAL_I2S_Init(&hi2s2);
}

static HAL_StatusTypeDef i2s_config_master_tx(uint32_t audio_freq)
{
    hi2s2.Instance = SPI2;
    hi2s2.Init.Mode = I2S_MODE_MASTER_TX;
    hi2s2.Init.Standard = I2S_STANDARD_PHILIPS;
    hi2s2.Init.DataFormat = I2S_DATAFORMAT_16B;
    hi2s2.Init.MCLKOutput = I2S_MCLKOUTPUT_DISABLE;
    hi2s2.Init.AudioFreq = audio_freq;
    hi2s2.Init.CPOL = I2S_CPOL_LOW;

    return HAL_I2S_Init(&hi2s2);
}

static void i2s_dma_init(void);

static uint32_t i2s_normalize_speaker_rate(uint32_t sample_rate_hz)
{
    switch (sample_rate_hz)
    {
    case 8000u:
        return I2S_AUDIOFREQ_8K;
    case 16000u:
        return I2S_AUDIOFREQ_16K;
    case 32000u:
        return I2S_AUDIOFREQ_32K;
    case 44100u:
        return I2S_AUDIOFREQ_44K;
    case 48000u:
        return I2S_AUDIOFREQ_48K;
    default:
        return I2S_AUDIOFREQ_16K;
    }
}

static uint8_t i2s_restore_microphone_rx(void)
{
    uint8_t ok = 1u;

    (void)HAL_I2S_DeInit(&hi2s2);
    i2s_gpio_init();
    if (i2s_config_master_rx() == HAL_OK)
    {
        i2s_dma_init();
    }
    else
    {
        ok = 0u;
    }

    memset(i2s_dma_buf, 0, sizeof(i2s_dma_buf));
    dbg_callback_count = 0u;
    startup_discard_counter = 0u;
    is_recording = 0u;

    if (I2S_MIC_SLOT_SEL == I2S_MIC_SLOT_AUTO)
    {
        slot_detected = 0u;
        slot_offset_runtime = 0u;
        detect_callback_count = 0u;
        detect_energy_pos0 = 0u;
        detect_energy_pos1 = 0u;
    }

    return ok;
}

/**
 * DMA1 Channel4 初始化 — SPI2_RX 专用通道
 */
static void i2s_dma_init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();

    hdma_i2s2_rx.Instance = DMA1_Channel4;
    hdma_i2s2_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_i2s2_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_i2s2_rx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_i2s2_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_i2s2_rx.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_i2s2_rx.Init.Mode = DMA_CIRCULAR; /* 循环模式实现双缓冲 */
    hdma_i2s2_rx.Init.Priority = DMA_PRIORITY_HIGH;

    if (HAL_DMA_Init(&hdma_i2s2_rx) != HAL_OK)
    {
        Error_Handler();
    }

    /* 关联 I2S 句柄与 DMA 句柄 */
    __HAL_LINKDMA(&hi2s2, hdmarx, hdma_i2s2_rx);

    /* 配置 DMA 中断优先级并使能 */
    HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel4_IRQn);
}

/* ===================== DMA 回调函数 ===================== */

/**
 * 调试: 打印前几次回调的原始采样数据，同时显示两个声道的数据
 */
static void dbg_print_samples(const uint16_t *raw, uint16_t raw_len,
                              const int16_t *pcm, uint16_t pcm_len)
{
    if (dbg_callback_count >= DBG_CALLBACK_LIMIT)
        return;
    dbg_callback_count++;

    debug_printf("[I2S] cb#%u raw_len=%u pcm_len=%u slot=%u\r\n",
                 (unsigned)dbg_callback_count, (unsigned)raw_len,
                 (unsigned)pcm_len, (unsigned)slot_offset_runtime);

    /* 打印前 2 帧 的两个声道原始数据 */
    uint16_t n = (raw_len < 8u) ? raw_len : 8u;
    debug_printf("  RAW:");
    for (uint16_t k = 0; k < n; k++)
    {
        debug_printf(" %04X", (unsigned)raw[k]);
    }
    debug_printf("\r\n");

    /* 打印两个位置的 PCM 值，便于判断哪个声道有数据 */
    if (raw_len >= 4u)
    {
        debug_printf("  Pos0: %d %d  Pos1: %d %d\r\n",
                     (int)(int16_t)raw[0], (int)(int16_t)raw[2],
                     (int)(int16_t)raw[1], (int)(int16_t)raw[3]);
    }

    /* 打印前 4 个 PCM 样本 */
    n = (pcm_len < 4u) ? pcm_len : 4u;
    debug_printf("  PCM:");
    for (uint16_t k = 0; k < n; k++)
    {
        debug_printf(" %d", (int)pcm[k]);
    }
    debug_printf("\r\n");
}

/**
 * 声道自动检测: 计算两个位置的能量，进行若干次回调后汇总决策
 */
static void channel_detect_accumulate(const uint16_t *raw, uint16_t raw_len)
{
    uint32_t e0 = 0, e1 = 0;
    for (uint16_t i = 0; (i + 1u) < raw_len; i += 2u)
    {
        int16_t s0 = (int16_t)raw[i + 0];
        int16_t s1 = (int16_t)raw[i + 1];
        e0 += (uint32_t)((s0 < 0) ? -s0 : s0);
        e1 += (uint32_t)((s1 < 0) ? -s1 : s1);
    }
    detect_energy_pos0 += e0;
    detect_energy_pos1 += e1;
    detect_callback_count++;
}

/**
 * 声道自动检测: 汇总决策，选择能量更高的位置
 */
static void channel_detect_finish(void)
{
    if (detect_energy_pos0 == 0u && detect_energy_pos1 == 0u)
    {
        /* 两个位置都没有数据，默认使用偏移 0 */
        slot_offset_runtime = 0;
        debug_printf("[I2S] 声道检测: 无数据，默认 pos0\r\n");
    }
    else if (detect_energy_pos1 > detect_energy_pos0)
    {
        slot_offset_runtime = 1;
        debug_printf("[I2S] 声道检测: pos1 有效 (e0=%lu e1=%lu)\r\n",
                     (unsigned long)detect_energy_pos0,
                     (unsigned long)detect_energy_pos1);
    }
    else
    {
        slot_offset_runtime = 0;
        debug_printf("[I2S] 声道检测: pos0 有效 (e0=%lu e1=%lu)\r\n",
                     (unsigned long)detect_energy_pos0,
                     (unsigned long)detect_energy_pos1);
    }
    slot_detected = 1;
}

/**
 * DMA 半满中断回调 — 前半缓冲区数据就绪
 */
void HAL_I2S_RxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance != SPI2 || !user_callback)
        return;

    /* INMP441 启动稳定期丢弃 */
    if (startup_discard_counter < I2S_STARTUP_DISCARD_CALLBACKS)
    {
        startup_discard_counter++;
        return;
    }

    /* 声道自动检测阶段: 累计能量 */
    if (I2S_MIC_SLOT_SEL == I2S_MIC_SLOT_AUTO && !slot_detected)
    {
        channel_detect_accumulate(&i2s_dma_buf[0], I2S_HALF_BUF_SIZE);
        if (detect_callback_count >= I2S_CHANNEL_DETECT_CALLBACKS)
        {
            channel_detect_finish();
        }
        return;
    }

    uint16_t pcm_len = i2s_extract_pcm16(&i2s_dma_buf[0], I2S_HALF_BUF_SIZE, i2s_pcm_half_buf);
    dbg_print_samples(&i2s_dma_buf[0], I2S_HALF_BUF_SIZE, i2s_pcm_half_buf, pcm_len);
    if (pcm_len > 0u)
    {
        user_callback(i2s_pcm_half_buf, pcm_len);
    }
}

/**
 * DMA 全满中断回调 — 后半缓冲区数据就绪
 */
void HAL_I2S_RxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance != SPI2 || !user_callback)
        return;

    /* INMP441 启动稳定期丢弃 */
    if (startup_discard_counter < I2S_STARTUP_DISCARD_CALLBACKS)
    {
        startup_discard_counter++;
        return;
    }

    /* 声道自动检测阶段: 累计能量 */
    if (I2S_MIC_SLOT_SEL == I2S_MIC_SLOT_AUTO && !slot_detected)
    {
        channel_detect_accumulate(&i2s_dma_buf[I2S_HALF_BUF_SIZE], I2S_HALF_BUF_SIZE);
        if (detect_callback_count >= I2S_CHANNEL_DETECT_CALLBACKS)
        {
            channel_detect_finish();
        }
        return;
    }

    uint16_t pcm_len = i2s_extract_pcm16(&i2s_dma_buf[I2S_HALF_BUF_SIZE], I2S_HALF_BUF_SIZE, i2s_pcm_half_buf);
    dbg_print_samples(&i2s_dma_buf[I2S_HALF_BUF_SIZE], I2S_HALF_BUF_SIZE, i2s_pcm_half_buf, pcm_len);
    if (pcm_len > 0u)
    {
        user_callback(i2s_pcm_half_buf, pcm_len);
    }
}

/* ===================== 公开接口 ===================== */

void i2s_mic_init(void)
{
    /* 使能 SPI2 外设时钟 */
    __HAL_RCC_SPI2_CLK_ENABLE();

    /* 初始化 GPIO */
    i2s_gpio_init();
    i2s_bus_switch_init();

    /* 配置 I2S2 参数 */
    hi2s2.Instance = SPI2;
    hi2s2.Init.Mode = I2S_MODE_MASTER_RX;           /* 主机接收模式 */
    hi2s2.Init.Standard = I2S_STANDARD_PHILIPS;     /* 飞利浦 I2S 标准 */
    hi2s2.Init.DataFormat = I2S_DATAFORMAT_16B_EXTENDED; /* 16bit 数据 + 32bit 声道帧，每声道仅 1 个 DR 读取 */
    hi2s2.Init.MCLKOutput = I2S_MCLKOUTPUT_DISABLE; /* 不输出主时钟 */
    hi2s2.Init.AudioFreq = I2S_AUDIOFREQ_32K;       /* 32kHz 采样率，SCK≈2.06MHz */
    hi2s2.Init.CPOL = I2S_CPOL_LOW;                 /* 空闲时时钟低电平 */

    if (HAL_I2S_Init(&hi2s2) != HAL_OK)
    {
        Error_Handler();
    }

    /* 初始化 DMA */
    i2s_dma_init();

    /* 清零缓冲区 */
    memset(i2s_dma_buf, 0, sizeof(i2s_dma_buf));

    is_recording = 0;
    dbg_callback_count = 0;
    startup_discard_counter = 0;

    /* 初始化声道检测状态 */
    if (I2S_MIC_SLOT_SEL == I2S_MIC_SLOT_AUTO)
    {
        slot_detected = 0;
        slot_offset_runtime = 0;
        detect_callback_count = 0;
        detect_energy_pos0 = 0;
        detect_energy_pos1 = 0;
    }
    else
    {
        /* 静态配置模式 */
        slot_detected = 1;
        slot_offset_runtime = (I2S_MIC_SLOT_SEL == I2S_MIC_SLOT_RIGHT) ? 1u : 0u;
    }

    /* 打印 I2S 外设寄存器，便于确认时钟分频 */
    debug_printf("[I2S] I2SCFGR=0x%04X  I2SPR=0x%04X  mode=16B_EXT_AUTO\r\n",
                 (unsigned)(SPI2->I2SCFGR), (unsigned)(SPI2->I2SPR));
}

void i2s_mic_start(void)
{
    if (is_recording)
    {
        return;
    }

    memset(i2s_dma_buf, 0, sizeof(i2s_dma_buf));
    dbg_callback_count = 0;
    startup_discard_counter = 0;

    /* 每次启动录音时重新进行声道检测，因为 I2S 重启后帧对齐可能变化 */
    if (I2S_MIC_SLOT_SEL == I2S_MIC_SLOT_AUTO)
    {
        slot_detected = 0;
        detect_callback_count = 0;
        detect_energy_pos0 = 0;
        detect_energy_pos1 = 0;
    }

    /* 启动 DMA 循环接收
     * 16B_EXTENDED 模式: HAL 内部 RxXferSize = Size（不双倍）
     * 传入 Size = I2S_DMA_BUF_SIZE，
     * DMA 传输 I2S_DMA_BUF_SIZE 个 halfword，正好填满缓冲区 */
    i2s_bus_mode_record();

    if (HAL_I2S_Receive_DMA(&hi2s2, i2s_dma_buf, I2S_DMA_BUF_SIZE) == HAL_OK)
    {
        is_recording = 1;
    }
    else
    {
        i2s_bus_mode_idle();
    }
}

void i2s_mic_stop(void)
{
    if (!is_recording)
    {
        i2s_bus_mode_idle();
        return;
    }

    HAL_I2S_DMAStop(&hi2s2);
    is_recording = 0;
    i2s_bus_mode_idle();
}

uint8_t i2s_mic_is_recording(void)
{
    return is_recording;
}

void i2s_mic_set_callback(i2s_mic_callback_t cb)
{
    user_callback = cb;
}

I2S_HandleTypeDef *i2s_mic_get_handle(void)
{
    return &hi2s2;
}

DMA_HandleTypeDef *i2s_mic_get_dma_handle(void)
{
    return &hdma_i2s2_rx;
}

uint8_t i2s_mic_probe(void)
{
    /* 短暂采集一批数据，检查是否有非零样本
     * 16B_EXTENDED 模式: Size=32 -> 实际读 32 halfword -> 16 帧 (L+R) */
    uint16_t probe_raw_buf[64];
    int16_t probe_pcm_buf[16];
    uint16_t pcm_len;
    memset(probe_raw_buf, 0, sizeof(probe_raw_buf));

    /* 使用阻塞方式接收少量数据（超时 200ms）
     * 16B_EXTENDED 模式 Size=32 -> 实际读 32 halfwords -> 16 帧 (L+R) */
    i2s_bus_mode_record();
    HAL_StatusTypeDef status = HAL_I2S_Receive(&hi2s2, probe_raw_buf, 32, 200);
    if (status != HAL_OK)
    {
        i2s_bus_mode_idle();
        return 0;
    }

    pcm_len = i2s_extract_pcm16(probe_raw_buf, 32, probe_pcm_buf);

    /* 检查缓冲区中是否存在非零数据 */
    for (uint16_t i = 0; i < pcm_len; i++)
    {
        if (probe_pcm_buf[i] != 0 && probe_pcm_buf[i] != (int16_t)0xFFFF)
        {
            i2s_bus_mode_idle();
            return 1;
        }
    }

    i2s_bus_mode_idle();
    return 0;
}

uint8_t i2s_mic_play_tone(uint16_t frequency_hz, uint16_t duration_ms)
{
    enum
    {
        TONE_SAMPLE_RATE = 16000u,
        TONE_FRAMES_PER_CHUNK = 64u,
        TONE_AMPLITUDE = 9000u
    };

    uint16_t tx_buf[TONE_FRAMES_PER_CHUNK * 2u];
    uint32_t total_frames;
    uint32_t frames_sent = 0u;
    uint32_t half_period_frames;
    uint32_t wave_pos = 0u;
    uint8_t ok = 1u;

    if (frequency_hz < 20u)
        frequency_hz = 20u;
    if (frequency_hz > 4000u)
        frequency_hz = 4000u;
    if (duration_ms < 10u)
        duration_ms = 10u;
    if (duration_ms > 5000u)
        duration_ms = 5000u;

    if (is_recording)
    {
        HAL_I2S_DMAStop(&hi2s2);
        is_recording = 0u;
    }

    total_frames = ((uint32_t)duration_ms * TONE_SAMPLE_RATE) / 1000u;
    half_period_frames = TONE_SAMPLE_RATE / ((uint32_t)frequency_hz * 2u);
    if (half_period_frames == 0u)
        half_period_frames = 1u;

    i2s_speaker_gpio_init();
    i2s_bus_mode_play();
    (void)HAL_I2S_DeInit(&hi2s2);

    if (i2s_config_master_tx(I2S_AUDIOFREQ_16K) != HAL_OK)
    {
        ok = 0u;
    }

    while (ok && frames_sent < total_frames)
    {
        uint16_t frames_this_chunk = TONE_FRAMES_PER_CHUNK;
        uint16_t i;

        if ((total_frames - frames_sent) < frames_this_chunk)
        {
            frames_this_chunk = (uint16_t)(total_frames - frames_sent);
        }

        for (i = 0u; i < frames_this_chunk; i++)
        {
            int16_t sample = (wave_pos < half_period_frames) ?
                             (int16_t)TONE_AMPLITUDE : -(int16_t)TONE_AMPLITUDE;
            tx_buf[i * 2u] = (uint16_t)sample;
            tx_buf[i * 2u + 1u] = (uint16_t)sample;

            wave_pos++;
            if (wave_pos >= (half_period_frames * 2u))
            {
                wave_pos = 0u;
            }
        }

        if (HAL_I2S_Transmit(&hi2s2, tx_buf, (uint16_t)(frames_this_chunk * 2u), 200u) != HAL_OK)
        {
            ok = 0u;
            break;
        }

        frames_sent += frames_this_chunk;
    }

    (void)HAL_I2S_DeInit(&hi2s2);
    i2s_gpio_init();
    if (i2s_config_master_rx() == HAL_OK)
    {
        i2s_dma_init();
    }
    else
    {
        ok = 0u;
    }

    memset(i2s_dma_buf, 0, sizeof(i2s_dma_buf));
    dbg_callback_count = 0u;
    startup_discard_counter = 0u;
    is_recording = 0u;

    if (I2S_MIC_SLOT_SEL == I2S_MIC_SLOT_AUTO)
    {
        slot_detected = 0u;
        slot_offset_runtime = 0u;
        detect_callback_count = 0u;
        detect_energy_pos0 = 0u;
        detect_energy_pos1 = 0u;
    }

    i2s_bus_mode_idle();
    return ok;
}

uint8_t i2s_mic_play_sweep(uint16_t start_hz, uint16_t end_hz, uint16_t duration_ms)
{
    enum
    {
        SWEEP_SAMPLE_RATE = 16000u,
        SWEEP_FRAMES_PER_CHUNK = 64u,
        SWEEP_AMPLITUDE = 8000u
    };

    uint16_t tx_buf[SWEEP_FRAMES_PER_CHUNK * 2u];
    uint32_t total_frames;
    uint32_t frames_sent = 0u;
    uint32_t phase_acc = 0u;
    uint8_t ok = 1u;

    if (start_hz < 20u)
        start_hz = 20u;
    if (end_hz < 20u)
        end_hz = 20u;
    if (start_hz > 4000u)
        start_hz = 4000u;
    if (end_hz > 4000u)
        end_hz = 4000u;
    if (duration_ms < 50u)
        duration_ms = 50u;
    if (duration_ms > 8000u)
        duration_ms = 8000u;

    if (is_recording)
    {
        HAL_I2S_DMAStop(&hi2s2);
        is_recording = 0u;
    }

    total_frames = ((uint32_t)duration_ms * SWEEP_SAMPLE_RATE) / 1000u;

    i2s_speaker_gpio_init();
    i2s_bus_mode_play();
    (void)HAL_I2S_DeInit(&hi2s2);

    if (i2s_config_master_tx(I2S_AUDIOFREQ_16K) != HAL_OK)
    {
        ok = 0u;
    }

    while (ok && frames_sent < total_frames)
    {
        uint16_t frames_this_chunk = SWEEP_FRAMES_PER_CHUNK;
        uint16_t i;

        if ((total_frames - frames_sent) < frames_this_chunk)
        {
            frames_this_chunk = (uint16_t)(total_frames - frames_sent);
        }

        for (i = 0u; i < frames_this_chunk; i++)
        {
            uint32_t frame_index = frames_sent + i;
            int32_t delta_hz = (int32_t)end_hz - (int32_t)start_hz;
            uint32_t freq_hz = (uint32_t)((int32_t)start_hz +
                                ((delta_hz * (int32_t)frame_index) / (int32_t)total_frames));
            int16_t sample;

            if (freq_hz < 20u)
                freq_hz = 20u;

            phase_acc += freq_hz;
            while (phase_acc >= SWEEP_SAMPLE_RATE)
            {
                phase_acc -= SWEEP_SAMPLE_RATE;
            }

            sample = (phase_acc < (SWEEP_SAMPLE_RATE / 2u)) ?
                     (int16_t)SWEEP_AMPLITUDE : -(int16_t)SWEEP_AMPLITUDE;
            tx_buf[i * 2u] = (uint16_t)sample;
            tx_buf[i * 2u + 1u] = (uint16_t)sample;
        }

        if (HAL_I2S_Transmit(&hi2s2, tx_buf, (uint16_t)(frames_this_chunk * 2u), 200u) != HAL_OK)
        {
            ok = 0u;
            break;
        }

        frames_sent += frames_this_chunk;
    }

    (void)HAL_I2S_DeInit(&hi2s2);
    i2s_gpio_init();
    if (i2s_config_master_rx() == HAL_OK)
    {
        i2s_dma_init();
    }
    else
    {
        ok = 0u;
    }

    memset(i2s_dma_buf, 0, sizeof(i2s_dma_buf));
    dbg_callback_count = 0u;
    startup_discard_counter = 0u;
    is_recording = 0u;

    if (I2S_MIC_SLOT_SEL == I2S_MIC_SLOT_AUTO)
    {
        slot_detected = 0u;
        slot_offset_runtime = 0u;
        detect_callback_count = 0u;
        detect_energy_pos0 = 0u;
        detect_energy_pos1 = 0u;
    }

    i2s_bus_mode_idle();
    return ok;
}

uint8_t i2s_mic_play_volume_steps(const uint8_t *levels_percent, uint8_t count)
{
    enum
    {
        VOL_SAMPLE_RATE = 16000u,
        VOL_FRAMES_PER_CHUNK = 64u,
        VOL_MAX_AMPLITUDE = 10000u,
        VOL_TONE_HZ = 1000u,
        VOL_BEEP_MS = 350u,
        VOL_GAP_MS = 120u
    };

    uint16_t tx_buf[VOL_FRAMES_PER_CHUNK * 2u];
    uint8_t ok = 1u;
    uint8_t step;

    if (!levels_percent || count == 0u)
        return 0u;
    if (count > 8u)
        count = 8u;

    if (is_recording)
    {
        HAL_I2S_DMAStop(&hi2s2);
        is_recording = 0u;
    }

    i2s_speaker_gpio_init();
    i2s_bus_mode_play();
    (void)HAL_I2S_DeInit(&hi2s2);

    if (i2s_config_master_tx(I2S_AUDIOFREQ_16K) != HAL_OK)
    {
        ok = 0u;
    }

    for (step = 0u; ok && step < count; step++)
    {
        uint8_t percent = levels_percent[step];
        uint32_t beep_frames = ((uint32_t)VOL_BEEP_MS * VOL_SAMPLE_RATE) / 1000u;
        uint32_t gap_frames = ((uint32_t)VOL_GAP_MS * VOL_SAMPLE_RATE) / 1000u;
        uint32_t frames_sent = 0u;
        uint32_t half_period_frames = VOL_SAMPLE_RATE / ((uint32_t)VOL_TONE_HZ * 2u);
        uint32_t wave_pos = 0u;
        uint16_t amplitude;

        if (percent > 100u)
            percent = 100u;
        amplitude = (uint16_t)(((uint32_t)VOL_MAX_AMPLITUDE * percent) / 100u);

        while (ok && frames_sent < beep_frames)
        {
            uint16_t frames_this_chunk = VOL_FRAMES_PER_CHUNK;
            uint16_t i;

            if ((beep_frames - frames_sent) < frames_this_chunk)
            {
                frames_this_chunk = (uint16_t)(beep_frames - frames_sent);
            }

            for (i = 0u; i < frames_this_chunk; i++)
            {
                int16_t sample = (wave_pos < half_period_frames) ?
                                 (int16_t)amplitude : -(int16_t)amplitude;
                tx_buf[i * 2u] = (uint16_t)sample;
                tx_buf[i * 2u + 1u] = (uint16_t)sample;

                wave_pos++;
                if (wave_pos >= (half_period_frames * 2u))
                {
                    wave_pos = 0u;
                }
            }

            if (HAL_I2S_Transmit(&hi2s2, tx_buf, (uint16_t)(frames_this_chunk * 2u), 200u) != HAL_OK)
            {
                ok = 0u;
                break;
            }

            frames_sent += frames_this_chunk;
        }

        frames_sent = 0u;
        memset(tx_buf, 0, sizeof(tx_buf));
        while (ok && frames_sent < gap_frames)
        {
            uint16_t frames_this_chunk = VOL_FRAMES_PER_CHUNK;
            if ((gap_frames - frames_sent) < frames_this_chunk)
            {
                frames_this_chunk = (uint16_t)(gap_frames - frames_sent);
            }

            if (HAL_I2S_Transmit(&hi2s2, tx_buf, (uint16_t)(frames_this_chunk * 2u), 200u) != HAL_OK)
            {
                ok = 0u;
                break;
            }

            frames_sent += frames_this_chunk;
        }
    }

    (void)HAL_I2S_DeInit(&hi2s2);
    i2s_gpio_init();
    if (i2s_config_master_rx() == HAL_OK)
    {
        i2s_dma_init();
    }
    else
    {
        ok = 0u;
    }

    memset(i2s_dma_buf, 0, sizeof(i2s_dma_buf));
    dbg_callback_count = 0u;
    startup_discard_counter = 0u;
    is_recording = 0u;

    if (I2S_MIC_SLOT_SEL == I2S_MIC_SLOT_AUTO)
    {
        slot_detected = 0u;
        slot_offset_runtime = 0u;
        detect_callback_count = 0u;
        detect_energy_pos0 = 0u;
        detect_energy_pos1 = 0u;
    }

    i2s_bus_mode_idle();
    return ok;
}

uint8_t i2s_mic_speaker_stream_begin(uint32_t sample_rate_hz)
{
    uint32_t i2s_rate = i2s_normalize_speaker_rate(sample_rate_hz);

    if (is_speaker_streaming)
    {
        i2s_mic_speaker_stream_end();
    }

    if (is_recording)
    {
        HAL_I2S_DMAStop(&hi2s2);
        is_recording = 0u;
    }

    i2s_speaker_gpio_init();
    (void)HAL_I2S_DeInit(&hi2s2);

    if (i2s_config_master_tx(i2s_rate) != HAL_OK)
    {
        (void)i2s_restore_microphone_rx();
        return 0u;
    }

    is_speaker_streaming = 1u;
    return 1u;
}

uint8_t i2s_mic_speaker_stream_write(const int16_t *samples, uint16_t sample_count)
{
    enum
    {
        STREAM_FRAMES_PER_CHUNK = 64u
    };

    uint16_t tx_buf[STREAM_FRAMES_PER_CHUNK * 2u];
    uint16_t sent = 0u;

    if (!is_speaker_streaming || !samples || sample_count == 0u)
        return 0u;

    while (sent < sample_count)
    {
        uint16_t frames_this_chunk = (uint16_t)(sample_count - sent);
        uint16_t i;

        if (frames_this_chunk > STREAM_FRAMES_PER_CHUNK)
            frames_this_chunk = STREAM_FRAMES_PER_CHUNK;

        for (i = 0u; i < frames_this_chunk; i++)
        {
            uint16_t sample = (uint16_t)samples[sent + i];
            tx_buf[i * 2u] = sample;
            tx_buf[i * 2u + 1u] = sample;
        }

        if (HAL_I2S_Transmit(&hi2s2, tx_buf, (uint16_t)(frames_this_chunk * 2u), 500u) != HAL_OK)
        {
            return 0u;
        }

        sent = (uint16_t)(sent + frames_this_chunk);
    }

    return 1u;
}

void i2s_mic_speaker_stream_end(void)
{
    if (!is_speaker_streaming)
        return;

    is_speaker_streaming = 0u;
    (void)i2s_restore_microphone_rx();
}

uint8_t i2s_mic_speaker_stream_is_active(void)
{
    return is_speaker_streaming;
}
