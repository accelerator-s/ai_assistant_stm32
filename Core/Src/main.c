/* USER CODE BEGIN Header */
/**
 * @file  main.c
 * @brief 主程序入口 — 全异步、无阻塞主循环版本
 *
 * 系统状态:
 *   STATE_IDLE       — 待机状态，等待用户操作
 *   STATE_RECORDING  — 录音中（K1 按住）
 *   STATE_REC_PAUSED — 录音暂停（K1 松开，可继续录或发送）
 *   STATE_WAITING    — 等待 AI 回复
 *   STATE_HISTORY    — 历史记录列表浏览
 *
 * 设计原则:
 *   1. 主循环不使用任何阻塞式网络发送
 *   2. 音频上传仅通过异步 TCP 发送队列推进
 *   3. 麦克风探测改为非阻塞状态机，不再直接同步采样等待
 *   4. 所有控制命令均通过异步文本发送接口入队
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "bsp/bsp_key.h"
#include "lcd/display.h"
#include "wifi/esp8266.h"
#include "wifi/wifi_config.h"
#include "audio/i2s_mic.h"
#include "debug/debug_uart.h"
#include "lcd/lcd.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ===================== 系统状态定义 ===================== */

typedef enum
{
    STATE_IDLE = 0,   /* 待机状态 */
    STATE_RECORDING,  /* 正在录音 */
    STATE_REC_PAUSED, /* 录音暂停，等待继续或发送 */
    STATE_WAITING,    /* 等待 AI 回复 */
    STATE_HISTORY     /* 历史记录浏览 */
} sys_state_t;

static sys_state_t sys_state = STATE_IDLE;

/* ===================== 按键事件 ===================== */

typedef enum
{
    KEY_EVENT_NONE = 0,
    KEY_EVENT_K1_PRESS,
    KEY_EVENT_K1_RELEASE,
    KEY_EVENT_K2_SHORT,
    KEY_EVENT_K2_LONG,
    KEY_EVENT_K2_DOUBLE
} key_event_t;

static uint32_t k2_press_tick = 0;
static uint8_t k2_press_count = 0;
static uint32_t k2_last_release_tick = 0;
static uint8_t k2_was_pressed = 0;

#define K2_LONG_PRESS_MS 800u
#define K2_DOUBLE_CLICK_MS 300u

/* ===================== 录音与上传缓冲 ===================== */

static uint32_t rec_start_tick = 0;
static uint8_t k1_no_tcp_hint_latched = 0;

#define AUDIO_UPLOAD_BUFFER_SAMPLES 16000u
#define AUDIO_UPLOAD_CHUNK_SAMPLES 1024u
#define AUDIO_UPLOAD_TRIGGER_SAMPLES AUDIO_UPLOAD_CHUNK_SAMPLES
#define AUDIO_UPLOAD_DOWNSAMPLE 4u
#define AUDIO_UPLOAD_GAIN_Q8 512

#define WIFI_COLOR_CONNECTED COLOR_ACCENT_GREEN
#define WIFI_COLOR_DISCONNECTED COLOR_ICON_MIC
#define WIFI_COLOR_CONNECTING COLOR_ORANGE
#define WIFI_COLOR_IP_READY COLOR_YELLOW
#define WIFI_COLOR_TCP_TRY COLOR_CYAN

static int16_t audio_upload_buffer[AUDIO_UPLOAD_BUFFER_SAMPLES];
static volatile uint16_t audio_upload_write_pos = 0;
static volatile uint16_t audio_upload_read_pos = 0;
static volatile uint8_t audio_upload_overflow = 0;
static volatile uint32_t audio_upload_dropped_samples = 0;
static int32_t audio_upload_downsample_acc = 0;
static uint16_t audio_upload_downsample_count = 0;
static volatile uint8_t rec_end_pending = 0;
static volatile uint8_t mic_rec_done_pending = 0;

/* ===================== 非阻塞麦克风探测 ===================== */

typedef enum
{
    MIC_PROBE_IDLE = 0,
    MIC_PROBE_RUNNING
} mic_probe_state_t;

typedef enum
{
    MIC_REC_TEST_IDLE = 0,
    MIC_REC_TEST_RUNNING
} mic_rec_test_state_t;

static mic_probe_state_t mic_probe_state = MIC_PROBE_IDLE;
static uint32_t mic_probe_start_tick = 0;
static volatile uint16_t mic_probe_nonzero_count = 0;
static volatile uint16_t mic_probe_sample_count = 0;
static volatile uint8_t mic_probe_collecting = 0;
static mic_rec_test_state_t mic_rec_test_state = MIC_REC_TEST_IDLE;
static uint32_t mic_rec_test_start_tick = 0;
static uint32_t mic_rec_test_duration_ms = 3000u;

#define MIC_PROBE_TIMEOUT_MS 250u
#define MIC_PROBE_MIN_NONZERO_SAMPLES 8u
#define MIC_PROBE_MIN_TOTAL_SAMPLES 64u

static void audio_upload_poll(void);
static void try_send_rec_end(void);
static void update_network_debug_display(esp8266_status_t wifi_st,
                                         uint8_t tcp_async_started,
                                         int tcp_conn_state,
                                         uint8_t tcp_fail_latched,
                                         const char *ip_text);

/* ===================== 工具函数 ===================== */

static uint16_t ring_distance(uint16_t from, uint16_t to, uint16_t size)
{
    if (to >= from)
        return (uint16_t)(to - from);
    return (uint16_t)(size - from + to);
}

static uint8_t tcp_ready_for_send(void)
{
    return (esp8266_get_status() == ESP8266_STATUS_TCP_CONNECTED) ? 1u : 0u;
}

static void audio_buffer_reset(void)
{
    __disable_irq();
    audio_upload_read_pos = 0;
    audio_upload_write_pos = 0;
    audio_upload_overflow = 0;
    audio_upload_dropped_samples = 0;
    audio_upload_downsample_acc = 0;
    audio_upload_downsample_count = 0;
    __enable_irq();
}

static uint16_t audio_buffer_available(void)
{
    uint16_t read_pos, write_pos;
    __disable_irq();
    read_pos = audio_upload_read_pos;
    write_pos = audio_upload_write_pos;
    __enable_irq();
    return ring_distance(read_pos, write_pos, AUDIO_UPLOAD_BUFFER_SAMPLES);
}

static int16_t audio_upload_apply_gain(int16_t sample)
{
    int32_t amplified = ((int32_t)sample * (int32_t)AUDIO_UPLOAD_GAIN_Q8) >> 8;

    if (amplified > 32767)
        return 32767;
    if (amplified < -32768)
        return -32768;

    return (int16_t)amplified;
}

static void mic_probe_reset_stats(void)
{
    __disable_irq();
    mic_probe_nonzero_count = 0;
    mic_probe_sample_count = 0;
    __enable_irq();
}

static void mic_probe_start(void)
{
    if (mic_probe_state != MIC_PROBE_IDLE)
        return;

    mic_probe_reset_stats();
    mic_probe_collecting = 1;
    mic_probe_start_tick = HAL_GetTick();
    mic_probe_state = MIC_PROBE_RUNNING;

    if (!i2s_mic_is_recording())
    {
        i2s_mic_start();
    }

    display_show_system_hint("硬件检测中...");
    display_update_bottom_hint("正在检测麦克风连接...");
}

static void mic_probe_finish(uint8_t success)
{
    mic_probe_collecting = 0;
    if (i2s_mic_is_recording() && sys_state != STATE_RECORDING)
    {
        i2s_mic_stop();
    }

    mic_probe_state = MIC_PROBE_IDLE;

    if (success)
    {
        (void)esp8266_tcp_send_line_async("MIC_OK");
        display_show_system_hint("麦克风硬件检测通过");
    }
    else
    {
        (void)esp8266_tcp_send_line_async("MIC_FAIL");
        display_show_system_hint("麦克风硬件检测失败");
    }

    if (sys_state == STATE_IDLE)
    {
        display_update_bottom_hint("K1:录音 K2:发送/新建");
    }
}

static void mic_probe_poll(void)
{
    uint16_t nonzero_count, sample_count;

    if (mic_probe_state != MIC_PROBE_RUNNING)
        return;

    __disable_irq();
    nonzero_count = mic_probe_nonzero_count;
    sample_count = mic_probe_sample_count;
    __enable_irq();

    if (nonzero_count >= MIC_PROBE_MIN_NONZERO_SAMPLES &&
        sample_count >= MIC_PROBE_MIN_TOTAL_SAMPLES)
    {
        mic_probe_finish(1u);
        return;
    }

    if ((HAL_GetTick() - mic_probe_start_tick) >= MIC_PROBE_TIMEOUT_MS)
    {
        mic_probe_finish(0u);
    }
}

static void mic_rec_test_start(uint32_t duration_sec)
{
    if (mic_rec_test_state != MIC_REC_TEST_IDLE)
        return;

    if (duration_sec < 1u)
        duration_sec = 1u;
    if (duration_sec > 10u)
        duration_sec = 10u;

    mic_rec_test_duration_ms = duration_sec * 1000u;
    mic_rec_test_start_tick = HAL_GetTick();
    mic_rec_test_state = MIC_REC_TEST_RUNNING;
    rec_end_pending = 0u;
    mic_rec_done_pending = 0u;
    audio_buffer_reset();

    (void)esp8266_tcp_send_line_async("REC_START");

    if (!i2s_mic_is_recording())
    {
        i2s_mic_start();
    }

    display_show_system_hint("录音测试中...");
    display_update_bottom_hint("请稍候，正在采集测试音频...");
}

static void mic_rec_test_finish(void)
{
    if (mic_rec_test_state != MIC_REC_TEST_RUNNING)
        return;

    if (i2s_mic_is_recording() && sys_state != STATE_RECORDING)
    {
        i2s_mic_stop();
    }

    mic_rec_test_state = MIC_REC_TEST_IDLE;
    /* 不在此处阻塞发送，由主循环的 audio_upload_poll() 排干缓冲，
     * 排干后 try_send_mic_rec_done() 自动发送 MIC_REC_DONE:sr=8000 */
    mic_rec_done_pending = 1u;
    display_show_system_hint("录音测试完成，上传中...");

    if (sys_state == STATE_IDLE)
    {
        display_update_bottom_hint("数据上传中...");
    }
}

static void mic_rec_test_poll(void)
{
    if (mic_rec_test_state != MIC_REC_TEST_RUNNING)
        return;

    if ((HAL_GetTick() - mic_rec_test_start_tick) >= mic_rec_test_duration_ms)
    {
        mic_rec_test_finish();
    }
}

/* ===================== 网络状态屏幕调试 ===================== */

static uint16_t network_status_color(esp8266_status_t wifi_st,
                                     uint8_t tcp_async_started,
                                     int tcp_conn_state,
                                     uint8_t tcp_fail_latched)
{
    if (wifi_st == ESP8266_STATUS_TCP_CONNECTED || tcp_conn_state == 1)
        return WIFI_COLOR_CONNECTED;

    if (tcp_async_started)
        return WIFI_COLOR_TCP_TRY;

    if (tcp_fail_latched || wifi_st == ESP8266_STATUS_ERROR)
        return WIFI_COLOR_DISCONNECTED;

    if (wifi_st == ESP8266_STATUS_WIFI_GOT_IP)
        return WIFI_COLOR_IP_READY;

    if (wifi_st == ESP8266_STATUS_CONNECTING_WIFI ||
        wifi_st == ESP8266_STATUS_WIFI_CONNECTED)
        return WIFI_COLOR_CONNECTING;

    return COLOR_TEXT_SECONDARY;
}

static const char *network_status_label(esp8266_status_t wifi_st,
                                        uint8_t tcp_async_started,
                                        int tcp_conn_state,
                                        uint8_t tcp_fail_latched)
{
    if (wifi_st == ESP8266_STATUS_TCP_CONNECTED || tcp_conn_state == 1)
        return "TCP OK";

    if (tcp_async_started)
        return "TCP TRY";

    if (tcp_fail_latched)
        return "TCP FAIL";

    switch (wifi_st)
    {
    case ESP8266_STATUS_IDLE:
        return "ESP IDLE";
    case ESP8266_STATUS_INITIALIZING:
        return "ESP INIT";
    case ESP8266_STATUS_READY:
        return "ESP READY";
    case ESP8266_STATUS_CONNECTING_WIFI:
        return "WiFi JOIN";
    case ESP8266_STATUS_WIFI_CONNECTED:
        return "WiFi OK";
    case ESP8266_STATUS_WIFI_GOT_IP:
        return "IP OK";
    case ESP8266_STATUS_ERROR:
    {
        const char *dbg = esp8266_get_debug_msg();
        if (dbg && strstr(dbg, "AT:baud fail") != NULL)
            return "AT FAIL";
        if (dbg && strstr(dbg, "UART_DEF fail") != NULL)
            return "UART FAIL";
        return "ESP ERR";
    }
    default:
        return "NET ?";
    }
}

static void network_status_detail(esp8266_status_t wifi_st,
                                  uint8_t tcp_async_started,
                                  int tcp_conn_state,
                                  uint8_t tcp_fail_latched,
                                  const char *ip_text,
                                  char *out,
                                  size_t out_size)
{
    const char *dbg = esp8266_get_debug_msg();
    const char *ip = (ip_text && ip_text[0] != '\0') ? ip_text : "-";

    if (!out || out_size == 0u)
        return;

    out[0] = '\0';

    if (wifi_st == ESP8266_STATUS_TCP_CONNECTED || tcp_conn_state == 1)
    {
        snprintf(out, out_size, "TCP OK %s:%u", SERVER_IP, (unsigned)SERVER_PORT);
        return;
    }

    if (tcp_async_started)
    {
        snprintf(out, out_size, "TCP>%s:%u", SERVER_IP, (unsigned)SERVER_PORT);
        return;
    }

    if (tcp_fail_latched)
    {
        snprintf(out, out_size, "TCP FAIL %s", (dbg && dbg[0] != '\0') ? dbg : "no resp");
        return;
    }

    switch (wifi_st)
    {
    case ESP8266_STATUS_INITIALIZING:
        snprintf(out, out_size, "ESP init %s", (dbg && dbg[0] != '\0') ? dbg : "");
        break;
    case ESP8266_STATUS_READY:
        snprintf(out, out_size, "ESP ready");
        break;
    case ESP8266_STATUS_CONNECTING_WIFI:
        snprintf(out, out_size, "WiFi join:%s", WIFI_SSID);
        break;
    case ESP8266_STATUS_WIFI_CONNECTED:
        snprintf(out, out_size, "WiFi ok, wait IP");
        break;
    case ESP8266_STATUS_WIFI_GOT_IP:
        snprintf(out, out_size, "IP:%s", ip);
        break;
    case ESP8266_STATUS_ERROR:
        snprintf(out, out_size, "ESP ERR %s", (dbg && dbg[0] != '\0') ? dbg : "");
        break;
    default:
        snprintf(out, out_size, "NET idle");
        break;
    }
}

static void update_network_debug_display(esp8266_status_t wifi_st,
                                         uint8_t tcp_async_started,
                                         int tcp_conn_state,
                                         uint8_t tcp_fail_latched,
                                         const char *ip_text)
{
    static char last_label[16] = "";
    static char last_detail[64] = "";
    static uint16_t last_color = 0u;
    static uint32_t last_idle_detail_tick = 0u;

    const char *label = network_status_label(wifi_st, tcp_async_started,
                                             tcp_conn_state, tcp_fail_latched);
    uint16_t color = network_status_color(wifi_st, tcp_async_started,
                                          tcp_conn_state, tcp_fail_latched);
    char detail[64];
    uint8_t label_changed;
    uint8_t detail_changed;

    network_status_detail(wifi_st, tcp_async_started, tcp_conn_state,
                          tcp_fail_latched, ip_text, detail, sizeof(detail));

    label_changed = (strcmp(last_label, label) != 0 || last_color != color) ? 1u : 0u;
    detail_changed = (strcmp(last_detail, detail) != 0) ? 1u : 0u;

    if (label_changed)
    {
        display_update_wifi(label, color);
        strncpy(last_label, label, sizeof(last_label) - 1u);
        last_label[sizeof(last_label) - 1u] = '\0';
        last_color = color;
    }

    if (detail_changed)
    {
        display_push_debug_line(detail);
        strncpy(last_detail, detail, sizeof(last_detail) - 1u);
        last_detail[sizeof(last_detail) - 1u] = '\0';
    }

    if (sys_state == STATE_IDLE &&
        (wifi_st != ESP8266_STATUS_TCP_CONNECTED || tcp_async_started || tcp_fail_latched) &&
        (detail_changed || (HAL_GetTick() - last_idle_detail_tick) >= 2000u))
    {
        display_update_bottom_hint(detail);
        last_idle_detail_tick = HAL_GetTick();
    }
    else if (sys_state == STATE_IDLE &&
             wifi_st == ESP8266_STATUS_TCP_CONNECTED &&
             detail_changed)
    {
        display_update_bottom_hint("K1:录音 K2:发送/新建");
        last_idle_detail_tick = HAL_GetTick();
    }
}

/* ===================== TCP 下行消息处理 ===================== */

static void handle_tcp_downlink(void)
{
    char line[160];
    uint8_t loop_guard = 6;

    while (loop_guard-- > 0 && esp8266_tcp_read_line(line, sizeof(line)))
    {
        if (strncmp(line, "STT:", 4) == 0)
        {
            display_add_message(MSG_ROLE_USER, line + 4);
            if (sys_state == STATE_WAITING)
            {
                display_update_bottom_hint("识别完成，思考中...");
            }
        }
        else if (strncmp(line, "AI:", 3) == 0)
        {
            display_add_message(MSG_ROLE_ASSISTANT, line + 3);
            sys_state = STATE_IDLE;
            display_update_bottom_hint("K1:录音 K2:发送/新建");
        }
        else if (strncmp(line, "SYS:", 4) == 0)
        {
            display_show_system_hint(line + 4);
        }
        else if (strncmp(line, "TITLE:", 6) == 0)
        {
            display_update_title(line + 6);
        }
        else if (strcmp(line, "MIC_TEST") == 0)
        {
            mic_probe_start();
        }
        else if (strncmp(line, "MIC_REC:", 8) == 0)
        {
            uint32_t duration_sec = (uint32_t)atoi(line + 8);
            mic_rec_test_start(duration_sec);
        }
    }
}

/* ===================== 音频数据回调 ===================== */

static void on_audio_data(const int16_t *buf, uint16_t len)
{
    uint16_t i;

    if (!buf || len == 0)
        return;

    if (mic_probe_collecting)
    {
        for (i = 0; i < len; i++)
        {
            int16_t s = buf[i];
            if (s != 0 && s != (int16_t)0xFFFF)
            {
                mic_probe_nonzero_count++;
            }
            mic_probe_sample_count++;
        }
    }

    if (sys_state != STATE_RECORDING && mic_rec_test_state != MIC_REC_TEST_RUNNING)
        return;

    /* 抽取降采样前先做简单均值滤波（抗混叠）
     * 对每 AUDIO_UPLOAD_DOWNSAMPLE 个样本取平均值，而不是直接丢弃 */
    for (i = 0; i < len; i++)
    {
        int16_t sample;
        uint16_t next;

        audio_upload_downsample_acc += (int32_t)buf[i];
        audio_upload_downsample_count++;
        if (audio_upload_downsample_count < AUDIO_UPLOAD_DOWNSAMPLE)
            continue;

        sample = (int16_t)(audio_upload_downsample_acc / (int32_t)AUDIO_UPLOAD_DOWNSAMPLE);
        sample = audio_upload_apply_gain(sample);
        audio_upload_downsample_acc = 0;
        audio_upload_downsample_count = 0;

        next = (uint16_t)((audio_upload_write_pos + 1u) % AUDIO_UPLOAD_BUFFER_SAMPLES);
        if (next == audio_upload_read_pos)
        {
            audio_upload_overflow = 1u;
            audio_upload_dropped_samples++;
            audio_upload_read_pos = (uint16_t)((audio_upload_read_pos + 1u) % AUDIO_UPLOAD_BUFFER_SAMPLES);
        }
        audio_upload_buffer[audio_upload_write_pos] = sample;
        audio_upload_write_pos = next;
    }
}

/* ===================== 音频异步上传 ===================== */

static void audio_upload_poll(void)
{
    uint16_t available;
    uint16_t read_pos;
    uint16_t write_pos;
    uint16_t contiguous;
    esp8266_send_result_t send_ret;
    uint8_t force_flush_tail;

    if (sys_state != STATE_RECORDING &&
        mic_rec_test_state != MIC_REC_TEST_RUNNING &&
        !rec_end_pending &&
        !mic_rec_done_pending)
        return;

    if (!tcp_ready_for_send())
        return;

    if (esp8266_tx_in_progress())
        return;

    available = audio_buffer_available();
    force_flush_tail = (rec_end_pending || mic_rec_done_pending) ? 1u : 0u;
    if (available < AUDIO_UPLOAD_TRIGGER_SAMPLES && !force_flush_tail)
        return;

    __disable_irq();
    read_pos = audio_upload_read_pos;
    write_pos = audio_upload_write_pos;
    __enable_irq();

    if (write_pos >= read_pos)
    {
        contiguous = (uint16_t)(write_pos - read_pos);
    }
    else
    {
        contiguous = (uint16_t)(AUDIO_UPLOAD_BUFFER_SAMPLES - read_pos);
    }

    if (contiguous > AUDIO_UPLOAD_CHUNK_SAMPLES)
        contiguous = AUDIO_UPLOAD_CHUNK_SAMPLES;

    if (contiguous == 0)
        return;

    send_ret = esp8266_tcp_send_async(
        (const uint8_t *)&audio_upload_buffer[read_pos],
        (uint16_t)(contiguous * sizeof(int16_t)));

    if (send_ret == ESP8266_SEND_OK)
    {
        __disable_irq();
        audio_upload_read_pos = (uint16_t)((audio_upload_read_pos + contiguous) % AUDIO_UPLOAD_BUFFER_SAMPLES);
        __enable_irq();
    }
}

static void try_send_rec_end(void)
{
    esp8266_send_result_t send_ret;
    uint16_t remain;

    if (!rec_end_pending)
        return;

    if (!tcp_ready_for_send())
        return;

    remain = audio_buffer_available();
    if (remain > 0u)
        return;

    if (esp8266_tx_in_progress())
        return;

    send_ret = esp8266_tcp_send_line_async("REC_END");
    if (send_ret == ESP8266_SEND_OK)
    {
        rec_end_pending = 0;
    }
}

static void try_send_mic_rec_done(void)
{
    esp8266_send_result_t send_ret;
    uint16_t remain;

    if (!mic_rec_done_pending)
        return;

    /* REC_END 和 MIC_REC_DONE 的发送顺序:
     * 如果 rec_end_pending 仍在等待，先等它发完；
     * 如果无 rec_end_pending，等待音频缓冲排干后再发 */
    if (rec_end_pending)
        return;

    /* 等待音频缓冲全部发送完毕 */
    remain = audio_buffer_available();
    if (remain > 0u)
        return;

    if (!tcp_ready_for_send())
        return;

    if (esp8266_tx_in_progress())
        return;

    send_ret = esp8266_tcp_send_line_async("MIC_REC_DONE:sr=8000");
    if (send_ret == ESP8266_SEND_OK)
    {
        mic_rec_done_pending = 0;
        display_show_system_hint("录音测试数据已上传");
        if (sys_state == STATE_IDLE)
        {
            display_update_bottom_hint("K1:录音 K2:发送/新建");
        }
    }
}

/* ===================== 按键事件检测 ===================== */

static key_event_t detect_k2_event(void)
{
    uint8_t pressed = bsp_key_get_k2();
    uint32_t now = HAL_GetTick();

    if (pressed && !k2_was_pressed)
    {
        k2_press_tick = now;
        k2_was_pressed = 1;
    }
    else if (!pressed && k2_was_pressed)
    {
        uint32_t hold_time;
        k2_was_pressed = 0;
        hold_time = now - k2_press_tick;

        if (hold_time >= K2_LONG_PRESS_MS)
        {
            k2_press_count = 0;
            return KEY_EVENT_K2_LONG;
        }

        if ((now - k2_last_release_tick) <= K2_DOUBLE_CLICK_MS)
        {
            k2_press_count = 0;
            k2_last_release_tick = 0;
            return KEY_EVENT_K2_DOUBLE;
        }

        k2_press_count++;
        k2_last_release_tick = now;
    }

    if (k2_press_count > 0 && !pressed &&
        ((now - k2_last_release_tick) > K2_DOUBLE_CLICK_MS))
    {
        k2_press_count = 0;
        return KEY_EVENT_K2_SHORT;
    }

    return KEY_EVENT_NONE;
}

/* ===================== 状态机处理 ===================== */

static void handle_idle(key_event_t k2_ev)
{
    if (!bsp_key_get_k1())
    {
        k1_no_tcp_hint_latched = 0;
    }

    if (bsp_key_get_k1())
    {
        if (!tcp_ready_for_send())
        {
            if (!k1_no_tcp_hint_latched)
            {
                display_show_system_hint("未连接WebUI，无法语音对话");
                display_update_bottom_hint("等待TCP连接...");
                k1_no_tcp_hint_latched = 1;
            }
            return;
        }

        k1_no_tcp_hint_latched = 0;
        sys_state = STATE_RECORDING;
        rec_start_tick = HAL_GetTick();
        rec_end_pending = 0u;
        audio_buffer_reset();
        i2s_mic_start();
        display_start_recording();

        (void)esp8266_tcp_send_line_async("REC_START");
        return;
    }

    if (k2_ev == KEY_EVENT_K2_SHORT)
    {
        display_clear_messages();
        display_update_title("新对话1");
        display_show_system_hint("新会话已开启");
        (void)esp8266_tcp_send_line_async("NEW_SESSION");
        return;
    }

    if (k2_ev == KEY_EVENT_K2_DOUBLE)
    {
        sys_state = STATE_HISTORY;
        (void)esp8266_tcp_send_line_async("GET_HISTORY");
        display_show_history(NULL, 0);
        return;
    }
}

static void handle_recording(void)
{
    uint32_t elapsed = HAL_GetTick() - rec_start_tick;
    display_update_recording(elapsed);

    if (!bsp_key_get_k1())
    {
        i2s_mic_stop();
        sys_state = STATE_REC_PAUSED;
        display_stop_recording();
        display_update_bottom_hint("K1续录 K2短按发送 长按取消");
        return;
    }
}

static void handle_rec_paused(key_event_t k2_ev)
{
    if (bsp_key_get_k1())
    {
        sys_state = STATE_RECORDING;
        i2s_mic_start();
        display_start_recording();
        return;
    }

    if (k2_ev == KEY_EVENT_K2_SHORT)
    {
        uint16_t remain = audio_buffer_available();
        uint32_t flush_start = HAL_GetTick();

        /* 延长刷缓冲时间，确保残余音频数据尽可能发出 */
        while (remain > 0u &&
               tcp_ready_for_send() &&
               (HAL_GetTick() - flush_start) < 500u)
        {
            if (esp8266_tx_in_progress())
            {
                esp8266_poll();
                continue;
            }
            audio_upload_poll();
            remain = audio_buffer_available();
        }

        sys_state = STATE_WAITING;
        display_update_bottom_hint("等待AI回复...");
        display_add_message(
            MSG_ROLE_SYSTEM,
            audio_upload_overflow ? "音频已发送，存在缓冲溢出" : "语音已发送，等待回复...");
        rec_end_pending = 1u;
        try_send_rec_end();
        return;
    }

    if (k2_ev == KEY_EVENT_K2_LONG)
    {
        sys_state = STATE_IDLE;
        audio_buffer_reset();
        rec_end_pending = 0u;
        display_update_bottom_hint("K1:录音 K2:发送/新建");
        display_add_message(MSG_ROLE_SYSTEM, "已取消录音");
        (void)esp8266_tcp_send_line_async("REC_CANCEL");
        return;
    }
}

static void handle_history(key_event_t k2_ev)
{
    if (bsp_key_k1_changed() && bsp_key_get_k1())
    {
        display_history_up();
        return;
    }

    if (k2_ev == KEY_EVENT_K2_SHORT)
    {
        display_history_down();
        return;
    }

    if (k2_ev == KEY_EVENT_K2_DOUBLE)
    {
        uint8_t sel = display_history_get_selected();
        char cmd[32];

        sys_state = STATE_IDLE;
        display_set_state(DISPLAY_STATE_CHAT);

        snprintf(cmd, sizeof(cmd), "LOAD_SESSION:%d", sel);
        (void)esp8266_tcp_send_line_async(cmd);
        return;
    }
}

/* ===================== 系统时钟配置 ===================== */

void SystemClock_Config(void);

/* ===================== 主函数 ===================== */

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    debug_uart_init();
    bsp_key_init();
    display_init();

    i2s_mic_init();
    i2s_mic_set_callback(on_audio_data);

    esp8266_init();

    display_update_wifi("ESP INIT", WIFI_COLOR_CONNECTING);
    display_show_system_hint("语音助手已启动");

    char last_ip[20] = {0};
    uint32_t last_tcp_try_tick = 0;
    uint8_t tcp_async_started = 0;
    uint8_t tcp_fail_latched = 0;
    uint32_t last_hb_tick = 0;
    int last_tcp_conn_state = 0;

#define HEARTBEAT_INTERVAL_MS 15000u

    while (1)
    {
        esp8266_poll();
        handle_tcp_downlink();
        mic_probe_poll();
        mic_rec_test_poll();

        esp8266_status_t wifi_st = esp8266_get_status();

        {
            const char *cur_ip = esp8266_get_ip_cached();
            if (cur_ip && cur_ip[0] != '\0' && strcmp(cur_ip, last_ip) != 0)
            {
                strncpy(last_ip, cur_ip, sizeof(last_ip) - 1);
                last_ip[sizeof(last_ip) - 1] = '\0';
            }
        }

        if ((wifi_st == ESP8266_STATUS_WIFI_CONNECTED ||
             wifi_st == ESP8266_STATUS_WIFI_GOT_IP) &&
            (wifi_st == ESP8266_STATUS_WIFI_GOT_IP || last_ip[0] != '\0') &&
            !tcp_async_started &&
            ((HAL_GetTick() - last_tcp_try_tick) > 5000u))
        {
            debug_printf("[TCP] async start %s:%u\r\n", SERVER_IP, (unsigned)SERVER_PORT);
            esp8266_connect_tcp_async(SERVER_IP, SERVER_PORT);
            tcp_async_started = 1;
            tcp_fail_latched = 0;
            last_tcp_try_tick = HAL_GetTick();
            last_tcp_conn_state = 0;
        }

        if (tcp_async_started)
        {
            int tcp_st = esp8266_tcp_connect_state();

            if (tcp_st == 1)
            {
                debug_printf("[TCP] async connected\r\n");
                tcp_fail_latched = 0;
                tcp_async_started = 0;
            }
            else if (tcp_st == -1)
            {
                debug_printf("[TCP] async failed: %s\r\n", esp8266_get_debug_msg());
                tcp_fail_latched = 1;
                tcp_async_started = 0;
                last_tcp_try_tick = HAL_GetTick();
            }

            last_tcp_conn_state = tcp_st;
        }

        if (wifi_st == ESP8266_STATUS_TCP_CONNECTED)
        {
            tcp_async_started = 0;
            tcp_fail_latched = 0;
            last_tcp_conn_state = 1;
        }
        else if (wifi_st == ESP8266_STATUS_WIFI_CONNECTED ||
                 wifi_st == ESP8266_STATUS_WIFI_GOT_IP)
        {
            if (!tcp_async_started)
            {
                last_tcp_conn_state = tcp_fail_latched ? -1 : 0;
            }
        }
        else
        {
            tcp_async_started = 0;
            tcp_fail_latched = 0;
            last_tcp_conn_state = 0;
        }

        update_network_debug_display(wifi_st, tcp_async_started,
                                     last_tcp_conn_state, tcp_fail_latched,
                                     last_ip);

        if (wifi_st == ESP8266_STATUS_TCP_CONNECTED &&
            ((HAL_GetTick() - last_hb_tick) >= HEARTBEAT_INTERVAL_MS))
        {
            esp8266_tcp_send_heartbeat();
            last_hb_tick = HAL_GetTick();
        }

        bsp_key_update();  /* 更新按键状态 */
        audio_upload_poll();
        try_send_rec_end();
        try_send_mic_rec_done();

        {
            key_event_t k2_ev = detect_k2_event();

            switch (sys_state)
            {
            case STATE_IDLE:
                handle_idle(k2_ev);
                break;

            case STATE_RECORDING:
                handle_recording();
                break;

            case STATE_REC_PAUSED:
                handle_rec_paused(k2_ev);
                break;

            case STATE_WAITING:
                if (k2_ev == KEY_EVENT_K2_SHORT)
                {
                    sys_state = STATE_IDLE;
                    display_update_bottom_hint("K1:录音 K2:发送/新建");
                }
                break;

            case STATE_HISTORY:
                handle_history(k2_ev);
                break;

            default:
                break;
            }
        }

        bsp_key_k1_changed();
        bsp_key_k2_changed();
    }
}

/* 系统时钟配置: HSE 8MHz -> PLL x9 -> 72MHz */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
    {
        Error_Handler();
    }
}

/* 错误处理: LED 闪烁指示 */
void Error_Handler(void)
{
    GPIO_InitTypeDef err_gpio = {0};
    err_gpio.Pin = GPIO_PIN_12;
    err_gpio.Mode = GPIO_MODE_OUTPUT_PP;
    err_gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOD, &err_gpio);

    while (1)
    {
        HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_12);
        for (volatile uint32_t i = 0; i < 500000; i++)
        {
        }
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
}
#endif
