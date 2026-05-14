/**
 * @file  esp8266.c
 * @brief ESP8266 WiFi 模块驱动实现（队列式全异步非阻塞版本）
 *
 * 设计目标:
 *   1. 不对上层暴露阻塞式 TCP 发送接口
 *   2. 所有 TCP 发送都先进入发送队列，再由状态机逐步推进
 *   3. 主循环仅通过 esp8266_poll() 驱动，不依赖 HAL_Delay 忙等
 *   4. 保留 WiFi/TCP 自动连接、心跳、断链检测、文本下行解析能力
 *
 * 注意:
 *   - 本实现仍允许硬件上电/复位阶段存在最小必要的时序等待，但不会在业务收发路径阻塞
 *   - 发送事务为“单飞”模型：同一时刻只推进一个 AT+CIPSEND 事务
 */

#include "wifi/esp8266.h"
#include "wifi/wifi_config.h"
#include "debug/debug_uart.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==================== 私有变量 ==================== */

/* USART3 句柄 */
static UART_HandleTypeDef huart_esp8266;

/* 接收缓冲区 */
static uint8_t rx_ring_buf[ESP8266_RX_BUF_SIZE];
static volatile uint16_t rx_write_idx;
static volatile uint16_t rx_read_idx;

/* 单字节接收缓存，中断逐字节接收 */
static uint8_t rx_byte;

/* UART 发送完成标志：所有异步发送动作都必须等上一次真正发完 */
static volatile uint8_t s_uart_tx_busy = 0u;

/* 持久化 UART 发送缓冲区，避免 HAL_UART_Transmit_IT 使用栈内存 */
static uint8_t s_uart_tx_buf[ESP8266_MAX_TX_PAYLOAD + 8u];
static uint16_t s_uart_tx_len = 0u;

/* 线性应答缓冲区，用于解析 AT 响应 */
static char resp_buf[ESP8266_RX_BUF_SIZE];
static uint16_t s_tcp_parse_offset;
#define ESP8266_TCP_LINE_BUF_SIZE 1536u
static char s_tcp_line_buf[ESP8266_TCP_LINE_BUF_SIZE];
static uint16_t s_tcp_line_len;

/* 模块对外状态 */
static esp8266_status_t s_status = ESP8266_STATUS_IDLE;

/* 本机 IP 地址缓存 */
static char s_ip_addr[20];

/* 调试信息：最后一次关键事件描述（用于 LCD 显示） */
static char s_debug_msg[64];

/* ==================== 初始化状态机 ==================== */

typedef enum
{
    INIT_POWERON = 0, /* 等待上电稳定 */
    INIT_RST_ASSERT,  /* RST 拉低 */
    INIT_RST_RELEASE, /* RST 拉高 */
    INIT_RST_WAIT,    /* 等待 ready */
    INIT_AT_SEND,     /* 发送 AT */
    INIT_AT_WAIT,     /* 等待 AT 响应 */
    INIT_ATE0_SEND,   /* 发送关回显 */
    INIT_ATE0_WAIT,
    INIT_CWMODE_SEND, /* 设置 Station 模式 */
    INIT_CWMODE_WAIT,
    INIT_UART_DEF_SEND, /* 切换 ESP8266 到目标波特率 */
    INIT_UART_DEF_WAIT,
    INIT_CWJAP_SEND,       /* 连接 WiFi */
    INIT_CWJAP_WAIT,
    INIT_CIFSR_SEND, /* 查询 IP */
    INIT_CIFSR_WAIT,
    INIT_COMPLETE,
    INIT_FAIL
} init_phase_t;

static init_phase_t s_phase = INIT_POWERON;
static uint32_t s_phase_tick;
static uint8_t s_retry_count;
static uint8_t s_use_cwjap_def;

/* 时序常量 */
#define AT_RETRIES 3u
#define AT_TEST_TIMEOUT 1000u
#define AT_CMD_TIMEOUT 3000u
#define RST_TIMEOUT 5000u
#define CWJAP_TIMEOUT 20000u
#define CWJAP_RETRIES 3u
#define POWERON_DELAY 2000u
#define HW_RST_LOW_MS 200u
#define HW_RST_POST_MS 50u
#define TCP_CONNECT_TIMEOUT 10000u

/* ==================== TCP 连接状态机 ==================== */

typedef enum
{
    TCP_PHASE_IDLE = 0,
    TCP_PHASE_CIPMUX_SEND,
    TCP_PHASE_CIPMUX_WAIT,
    TCP_PHASE_CIPSTART_SEND,
    TCP_PHASE_CIPSTART_WAIT,
    TCP_PHASE_DONE_OK,
    TCP_PHASE_DONE_FAIL,
    TCP_PHASE_HB_CIPSEND,
    TCP_PHASE_HB_PROMPT_WAIT,
    TCP_PHASE_HB_DATA,
    TCP_PHASE_HB_ACK_WAIT,
    TCP_PHASE_CLOSE_SEND,
    TCP_PHASE_CLOSE_WAIT
} tcp_phase_t;

static tcp_phase_t s_tcp_phase = TCP_PHASE_IDLE;
static uint32_t s_tcp_phase_tick;
static char s_tcp_server_ip[20];
static uint16_t s_tcp_server_port;

/* ==================== 发送队列与发送事务 ==================== */

typedef enum
{
    TX_ENGINE_IDLE = 0,
    TX_ENGINE_CIPSEND,
    TX_ENGINE_PROMPT_WAIT,
    TX_ENGINE_DATA_SEND,
    TX_ENGINE_ACK_WAIT
} tx_engine_phase_t;

typedef struct
{
    uint8_t used;
    esp8266_tx_item_t item;
} tx_slot_t;

static tx_slot_t s_tx_queue[ESP8266_TX_QUEUE_CAPACITY];
static volatile uint16_t s_tx_head = 0;
static volatile uint16_t s_tx_tail = 0;
static volatile uint16_t s_tx_count = 0;

static tx_engine_phase_t s_tx_phase = TX_ENGINE_IDLE;
static uint32_t s_tx_phase_tick;
static esp8266_tx_item_t s_tx_current;
static uint8_t s_tx_has_current = 0;

/* ==================== 工具函数 ==================== */

static int esp8266_phase_timeout(uint32_t ms)
{
    return (HAL_GetTick() - s_phase_tick) >= ms;
}

static int esp8266_tcp_phase_timeout(uint32_t ms)
{
    return (HAL_GetTick() - s_tcp_phase_tick) >= ms;
}

static int esp8266_tx_phase_timeout(uint32_t ms)
{
    return (HAL_GetTick() - s_tx_phase_tick) >= ms;
}

static void esp8266_set_phase(init_phase_t phase)
{
    s_phase = phase;
    s_phase_tick = HAL_GetTick();
}

static void esp8266_set_tcp_phase(tcp_phase_t phase)
{
    s_tcp_phase = phase;
    s_tcp_phase_tick = HAL_GetTick();
}

static void esp8266_set_tx_phase(tx_engine_phase_t phase)
{
    s_tx_phase = phase;
    s_tx_phase_tick = HAL_GetTick();
}

static void esp8266_save_debug(const char *msg)
{
    if (!msg)
    {
        s_debug_msg[0] = '\0';
        return;
    }
    strncpy(s_debug_msg, msg, sizeof(s_debug_msg) - 1);
    s_debug_msg[sizeof(s_debug_msg) - 1] = '\0';
}

static void esp8266_save_baud_debug(const char *prefix)
{
    char msg[32];

    snprintf(msg, sizeof(msg), "%s%lu", prefix, (unsigned long)ESP8266_USART_BAUDRATE);
    esp8266_save_debug(msg);
}

static void esp8266_clear_rx(void)
{
    __disable_irq();
    rx_write_idx = 0;
    memset(rx_ring_buf, 0, sizeof(rx_ring_buf));
    s_tcp_parse_offset = 0;
    __enable_irq();
}

static void esp8266_clear_tcp_line_buf(void)
{
    s_tcp_line_len = 0u;
    memset(s_tcp_line_buf, 0, sizeof(s_tcp_line_buf));
}

static int esp8266_pop_tcp_line(char *out, uint16_t out_size)
{
    char *line_end;
    uint16_t line_len;
    uint16_t consume_len;
    uint16_t remain;

    if (!out || out_size < 2u || s_tcp_line_len == 0u)
        return 0;

    line_end = (char *)memchr(s_tcp_line_buf, '\n', s_tcp_line_len);
    if (!line_end)
        return 0;

    line_len = (uint16_t)(line_end - s_tcp_line_buf);
    consume_len = (uint16_t)(line_len + 1u);
    if (line_len > 0u && s_tcp_line_buf[line_len - 1u] == '\r')
        line_len--;

    if (line_len >= out_size)
        line_len = (uint16_t)(out_size - 1u);

    if (line_len > 0u)
        memcpy(out, s_tcp_line_buf, line_len);
    out[line_len] = '\0';

    remain = (uint16_t)(s_tcp_line_len - consume_len);
    if (remain > 0u)
        memmove(s_tcp_line_buf, &s_tcp_line_buf[consume_len], remain);
    memset(&s_tcp_line_buf[remain], 0, (size_t)(sizeof(s_tcp_line_buf) - remain));
    s_tcp_line_len = remain;

    return (line_len > 0u) ? 1 : esp8266_pop_tcp_line(out, out_size);
}

static void esp8266_append_tcp_payload(const char *payload, uint16_t payload_len)
{
    uint16_t copy_len;

    if (!payload || payload_len == 0u)
        return;

    if ((uint32_t)s_tcp_line_len + payload_len >= sizeof(s_tcp_line_buf))
    {
        esp8266_clear_tcp_line_buf();
    }

    copy_len = payload_len;
    if ((uint32_t)s_tcp_line_len + copy_len >= sizeof(s_tcp_line_buf))
        copy_len = (uint16_t)(sizeof(s_tcp_line_buf) - s_tcp_line_len - 1u);

    if (copy_len > 0u)
    {
        memcpy(&s_tcp_line_buf[s_tcp_line_len], payload, copy_len);
        s_tcp_line_len = (uint16_t)(s_tcp_line_len + copy_len);
    }
}

static void esp8266_consume_rx(uint16_t count)
{
    __disable_irq();
    if (count >= rx_write_idx)
    {
        rx_write_idx = 0u;
        memset(rx_ring_buf, 0, sizeof(rx_ring_buf));
    }
    else if (count > 0u)
    {
        uint16_t remain = (uint16_t)(rx_write_idx - count);
        memmove(rx_ring_buf, &rx_ring_buf[count], remain);
        memset(&rx_ring_buf[remain], 0, (size_t)(sizeof(rx_ring_buf) - remain));
        rx_write_idx = remain;
    }
    s_tcp_parse_offset = 0u;
    __enable_irq();
}

static uint16_t esp8266_snapshot_resp(void)
{
    uint16_t len;

    __disable_irq();
    len = rx_write_idx;
    if (len > (sizeof(resp_buf) - 1u))
        len = (uint16_t)(sizeof(resp_buf) - 1u);
    memcpy(resp_buf, rx_ring_buf, len);
    resp_buf[len] = '\0';
    __enable_irq();

    return len;
}

static int esp8266_check_resp(const char *keyword)
{
    esp8266_snapshot_resp();
    return (strstr(resp_buf, keyword) != NULL) ? 1 : 0;
}

static int esp8266_uart_tx_ready(void)
{
    return (s_uart_tx_busy == 0u) &&
           (huart_esp8266.gState == HAL_UART_STATE_READY);
}

static int esp8266_send_raw_buf(const uint8_t *data, uint16_t len)
{
    HAL_StatusTypeDef hal_ret;

    if (!data || len == 0u)
        return 0;

    if (len > sizeof(s_uart_tx_buf))
        return 0;

    if (!esp8266_uart_tx_ready())
        return 0;

    memcpy(s_uart_tx_buf, data, len);
    s_uart_tx_len = len;
    s_uart_tx_busy = 1u;

    hal_ret = HAL_UART_Transmit_IT(&huart_esp8266, s_uart_tx_buf, s_uart_tx_len);
    if (hal_ret != HAL_OK)
    {
        s_uart_tx_busy = 0u;
        s_uart_tx_len = 0u;
        return 0;
    }

    return 1;
}

static int esp8266_send_raw_str(const char *str)
{
    if (!str)
        return 0;
    return esp8266_send_raw_buf((const uint8_t *)str, (uint16_t)strlen(str));
}

static int esp8266_send_cmd_now(const char *cmd)
{
    int n;

    if (!cmd)
        return 0;

    if (!esp8266_uart_tx_ready())
        return 0;

    esp8266_clear_rx();
    n = snprintf((char *)s_uart_tx_buf, sizeof(s_uart_tx_buf), "%s\r\n", cmd);
    if (n <= 0 || (uint16_t)n >= sizeof(s_uart_tx_buf))
        return 0;

    s_uart_tx_len = (uint16_t)n;
    s_uart_tx_busy = 1u;

    if (HAL_UART_Transmit_IT(&huart_esp8266, s_uart_tx_buf, s_uart_tx_len) != HAL_OK)
    {
        s_uart_tx_busy = 0u;
        s_uart_tx_len = 0u;
        return 0;
    }

    return 1;
}

static void format_tx_item_payload(const esp8266_tx_item_t *item, uint8_t *out, uint16_t *out_len)
{
    uint16_t len = 0;

    if (!item || !out || !out_len)
        return;

    if (item->kind == ESP8266_TX_KIND_TCP_RAW)
    {
        len = item->len;
        memcpy(out, item->data, len);
    }
    else
    {
        len = item->len;
        memcpy(out, item->data, len);

        if (item->append_crlf)
        {
            if (item->kind == ESP8266_TX_KIND_AT_CMD)
            {
                out[len++] = '\r';
                out[len++] = '\n';
            }
            else
            {
                out[len++] = '\n';
            }
        }
    }

    *out_len = len;
}

/* ==================== GPIO / UART 初始化 ==================== */

static void esp8266_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    ESP8266_GPIO_CLK_ENABLE();

    gpio.Pin = ESP8266_CH_PD_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(ESP8266_CH_PD_PORT, &gpio);

    gpio.Pin = ESP8266_RST_PIN;
    HAL_GPIO_Init(ESP8266_RST_PORT, &gpio);

    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(ESP8266_CH_PD_PORT, ESP8266_CH_PD_PIN, GPIO_PIN_SET);
}

static void esp8266_uart_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    ESP8266_USART_CLK_ENABLE();
    ESP8266_GPIO_CLK_ENABLE();

    gpio.Pin = ESP8266_TX_GPIO_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(ESP8266_TX_GPIO_PORT, &gpio);

    gpio.Pin = ESP8266_RX_GPIO_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(ESP8266_RX_GPIO_PORT, &gpio);

    huart_esp8266.Instance = ESP8266_USART;
    huart_esp8266.Init.BaudRate = ESP8266_USART_BAUDRATE;
    huart_esp8266.Init.WordLength = UART_WORDLENGTH_8B;
    huart_esp8266.Init.StopBits = UART_STOPBITS_1;
    huart_esp8266.Init.Parity = UART_PARITY_NONE;
    huart_esp8266.Init.Mode = UART_MODE_TX_RX;
    huart_esp8266.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart_esp8266.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart_esp8266);

    HAL_NVIC_SetPriority(ESP8266_USART_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(ESP8266_USART_IRQn);

    rx_write_idx = 0;
    rx_read_idx = 0;
    HAL_UART_Receive_IT(&huart_esp8266, &rx_byte, 1);
}

static void esp8266_uart_set_baud(uint32_t baud)
{
    HAL_UART_Abort(&huart_esp8266);
    huart_esp8266.Init.BaudRate = baud;
    HAL_UART_Init(&huart_esp8266);

    rx_write_idx = 0;
    memset(rx_ring_buf, 0, sizeof(rx_ring_buf));
    HAL_UART_Receive_IT(&huart_esp8266, &rx_byte, 1);
}

static void esp8266_uart_recover(void)
{
    __HAL_UART_CLEAR_OREFLAG(&huart_esp8266);
    __HAL_UART_CLEAR_NEFLAG(&huart_esp8266);
    __HAL_UART_CLEAR_FEFLAG(&huart_esp8266);
    __HAL_UART_CLEAR_PEFLAG(&huart_esp8266);

    if (huart_esp8266.RxState != HAL_UART_STATE_READY &&
        huart_esp8266.RxState != HAL_UART_STATE_BUSY_RX)
    {
        HAL_UART_Abort(&huart_esp8266);
        HAL_UART_Init(&huart_esp8266);
    }

    rx_write_idx = 0;
    memset(rx_ring_buf, 0, sizeof(rx_ring_buf));
    HAL_UART_Receive_IT(&huart_esp8266, &rx_byte, 1);
}

/* ==================== 发送队列实现 ==================== */

static void esp8266_tx_queue_reset(void)
{
    uint16_t i;

    __disable_irq();
    s_tx_head = 0;
    s_tx_tail = 0;
    s_tx_count = 0;
    __enable_irq();

    for (i = 0; i < ESP8266_TX_QUEUE_CAPACITY; i++)
    {
        s_tx_queue[i].used = 0u;
        memset(&s_tx_queue[i].item, 0, sizeof(s_tx_queue[i].item));
    }

    memset(&s_tx_current, 0, sizeof(s_tx_current));
    s_tx_has_current = 0u;
    esp8266_set_tx_phase(TX_ENGINE_IDLE);
}

static int esp8266_tx_queue_push_internal(const esp8266_tx_item_t *item)
{
    if (!item)
        return 0;

    __disable_irq();
    if (s_tx_count >= ESP8266_TX_QUEUE_CAPACITY)
    {
        __enable_irq();
        return 0;
    }

    s_tx_queue[s_tx_tail].used = 1u;
    memcpy(&s_tx_queue[s_tx_tail].item, item, sizeof(esp8266_tx_item_t));
    s_tx_tail = (uint16_t)((s_tx_tail + 1u) % ESP8266_TX_QUEUE_CAPACITY);
    s_tx_count++;
    __enable_irq();

    return 1;
}

static int esp8266_tx_queue_pop_internal(esp8266_tx_item_t *item)
{
    if (!item)
        return 0;

    __disable_irq();
    if (s_tx_count == 0u)
    {
        __enable_irq();
        return 0;
    }

    memcpy(item, &s_tx_queue[s_tx_head].item, sizeof(esp8266_tx_item_t));
    s_tx_queue[s_tx_head].used = 0u;
    memset(&s_tx_queue[s_tx_head].item, 0, sizeof(esp8266_tx_item_t));

    s_tx_head = (uint16_t)((s_tx_head + 1u) % ESP8266_TX_QUEUE_CAPACITY);
    s_tx_count--;
    __enable_irq();

    return 1;
}

uint16_t esp8266_tx_queue_count(void)
{
    uint16_t count;

    __disable_irq();
    count = s_tx_count;
    __enable_irq();

    return count;
}

uint8_t esp8266_tx_queue_is_empty(void)
{
    return (esp8266_tx_queue_count() == 0u) ? 1u : 0u;
}

uint8_t esp8266_tx_in_progress(void)
{
    return (s_tx_phase != TX_ENGINE_IDLE) ? 1u : 0u;
}

void esp8266_tx_queue_clear(void)
{
    esp8266_tx_queue_reset();
}

static esp8266_send_result_t esp8266_queue_item(esp8266_tx_kind_t kind,
                                                const uint8_t *data,
                                                uint16_t len,
                                                uint8_t append_crlf)
{
    esp8266_tx_item_t item;

    if (!data || len == 0u)
        return ESP8266_SEND_INVALID;

    if (s_status != ESP8266_STATUS_TCP_CONNECTED)
        return ESP8266_SEND_NOT_READY;

    if (kind == ESP8266_TX_KIND_TCP_RAW)
    {
        if (len > ESP8266_MAX_TX_PAYLOAD)
            return ESP8266_SEND_TOO_LARGE;
    }
    else
    {
        uint16_t extra = append_crlf ? ((kind == ESP8266_TX_KIND_AT_CMD) ? 2u : 1u) : 0u;
        if ((uint32_t)len + (uint32_t)extra > ESP8266_MAX_TX_PAYLOAD)
            return ESP8266_SEND_TOO_LARGE;
    }

    memset(&item, 0, sizeof(item));
    item.kind = kind;
    item.len = len;
    item.append_crlf = append_crlf;
    memcpy(item.data, data, len);

    if (!esp8266_tx_queue_push_internal(&item))
        return ESP8266_SEND_QUEUE_FULL;

    return ESP8266_SEND_OK;
}

esp8266_send_result_t esp8266_tcp_send_async(const uint8_t *data, uint16_t len)
{
    return esp8266_queue_item(ESP8266_TX_KIND_TCP_RAW, data, len, 0u);
}

esp8266_send_result_t esp8266_tcp_send_text_async(const char *text, uint8_t append_lf)
{
    if (!text)
        return ESP8266_SEND_INVALID;

    return esp8266_queue_item(ESP8266_TX_KIND_TCP_TEXT,
                              (const uint8_t *)text,
                              (uint16_t)strlen(text),
                              append_lf ? 1u : 0u);
}

esp8266_send_result_t esp8266_tcp_send_line_async(const char *line)
{
    return esp8266_tcp_send_text_async(line, 1u);
}

/* ==================== 链路监测 ==================== */

static void tcp_link_monitor(void)
{
    if (s_status != ESP8266_STATUS_TCP_CONNECTED)
        return;

    if (s_tcp_phase != TCP_PHASE_DONE_OK)
        return;

    if (s_tx_phase != TX_ENGINE_IDLE)
        return;

    esp8266_snapshot_resp();
    if (strstr(resp_buf, "CLOSED") != NULL ||
        strstr(resp_buf, "CONNECT FAIL") != NULL ||
        strstr(resp_buf, "link is not valid") != NULL)
    {
        char snip[96];
        size_t n = strlen(resp_buf);
        size_t i;

        if (n > 80u)
            n = 80u;

        memcpy(snip, resp_buf, n);
        snip[n] = '\0';

        for (i = 0; snip[i]; i++)
        {
            if (snip[i] == '\r' || snip[i] == '\n')
                snip[i] = ' ';
        }

        debug_printf("[ESP] link lost: %s\r\n", snip);

        s_status = ESP8266_STATUS_WIFI_GOT_IP;
        esp8266_set_tcp_phase(TCP_PHASE_IDLE);
        esp8266_save_debug("TCP:Disconnected");
        esp8266_clear_rx();
        esp8266_tx_queue_reset();
    }
}

/* ==================== IP 地址解析 ==================== */

static void parse_cifsr_ip(void)
{
    char *p;

    esp8266_snapshot_resp();

    p = strstr(resp_buf, "STAIP,\"");
    if (p)
    {
        char *end;
        uint16_t len;

        p += 7;
        end = strchr(p, '"');
        if (end)
        {
            len = (uint16_t)(end - p);
            if (len >= sizeof(s_ip_addr))
                len = (uint16_t)(sizeof(s_ip_addr) - 1u);

            memcpy(s_ip_addr, p, len);
            s_ip_addr[len] = '\0';
            return;
        }
    }

    p = resp_buf;
    while (*p)
    {
        if (*p >= '1' && *p <= '9')
        {
            int dots = 0;
            char *scan = p;

            while ((*scan >= '0' && *scan <= '9') || *scan == '.')
            {
                if (*scan == '.')
                    dots++;
                scan++;
            }

            if (dots == 3 && (scan - p) >= 7)
            {
                uint16_t len = (uint16_t)(scan - p);

                if (len >= sizeof(s_ip_addr))
                    len = (uint16_t)(sizeof(s_ip_addr) - 1u);

                memcpy(s_ip_addr, p, len);
                s_ip_addr[len] = '\0';

                if (strcmp(s_ip_addr, "0.0.0.0") != 0)
                    return;
            }
        }
        p++;
    }
}

/* ==================== TCP 连接状态机 ==================== */

static void tcp_poll(void)
{
    char cmd[128];

    switch (s_tcp_phase)
    {
    case TCP_PHASE_IDLE:
    case TCP_PHASE_DONE_OK:
    case TCP_PHASE_DONE_FAIL:
        break;

    case TCP_PHASE_CIPMUX_SEND:
        if (esp8266_send_cmd_now("AT+CIPMUX=0"))
        {
            esp8266_set_tcp_phase(TCP_PHASE_CIPMUX_WAIT);
        }
        break;

    case TCP_PHASE_CIPMUX_WAIT:
        if (esp8266_check_resp("OK") || esp8266_tcp_phase_timeout(AT_CMD_TIMEOUT))
        {
            esp8266_set_tcp_phase(TCP_PHASE_CIPSTART_SEND);
        }
        break;

    case TCP_PHASE_CIPSTART_SEND:
        snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u",
                 s_tcp_server_ip, s_tcp_server_port);
        if (esp8266_send_cmd_now(cmd))
        {
            esp8266_save_debug("TCP:Connecting..");
            debug_printf("[ESP] %s\r\n", cmd);
            esp8266_set_tcp_phase(TCP_PHASE_CIPSTART_WAIT);
        }
        break;

    case TCP_PHASE_CIPSTART_WAIT:
        esp8266_snapshot_resp();
        {
            int connect_fail = (strstr(resp_buf, "CONNECT FAIL") != NULL);
            int got_connect = (strstr(resp_buf, "CONNECT") != NULL) ||
                              (strstr(resp_buf, "Linked") != NULL);
            int got_error = (strstr(resp_buf, "ERROR") != NULL) ||
                            (strstr(resp_buf, "CLOSED") != NULL);

            if (connect_fail)
            {
                esp8266_save_debug("TCP:fail");
                esp8266_set_tcp_phase(TCP_PHASE_DONE_FAIL);
            }
            else if (got_connect)
            {
                s_status = ESP8266_STATUS_TCP_CONNECTED;
                esp8266_save_debug("TCP:Connected");
                esp8266_set_tcp_phase(TCP_PHASE_DONE_OK);
            }
            else if (got_error || esp8266_tcp_phase_timeout(TCP_CONNECT_TIMEOUT))
            {
                esp8266_save_debug("TCP:fail");
                esp8266_set_tcp_phase(TCP_PHASE_DONE_FAIL);
            }
        }
        break;

    case TCP_PHASE_HB_CIPSEND:
        if (s_tx_phase == TX_ENGINE_IDLE &&
            esp8266_send_cmd_now("AT+CIPSEND=3"))
        {
            esp8266_set_tcp_phase(TCP_PHASE_HB_PROMPT_WAIT);
        }
        break;

    case TCP_PHASE_HB_PROMPT_WAIT:
        if (esp8266_check_resp(">"))
        {
            esp8266_set_tcp_phase(TCP_PHASE_HB_DATA);
        }
        else if (esp8266_check_resp("ERROR") ||
                 esp8266_check_resp("CLOSED") ||
                 esp8266_tcp_phase_timeout(AT_CMD_TIMEOUT))
        {
            s_status = ESP8266_STATUS_WIFI_GOT_IP;
            esp8266_save_debug("HB:link lost");
            esp8266_set_tcp_phase(TCP_PHASE_DONE_FAIL);
            esp8266_clear_rx();
        }
        break;

    case TCP_PHASE_HB_DATA:
        if (esp8266_uart_tx_ready())
        {
            esp8266_clear_rx();
            if (esp8266_send_raw_str("HB\n"))
            {
                esp8266_set_tcp_phase(TCP_PHASE_HB_ACK_WAIT);
            }
        }
        break;

    case TCP_PHASE_HB_ACK_WAIT:
        esp8266_snapshot_resp();
        if (strstr(resp_buf, "SEND OK") != NULL)
        {
            esp8266_clear_rx();
            esp8266_set_tcp_phase(TCP_PHASE_DONE_OK);
        }
        else if (strstr(resp_buf, "ERROR") != NULL ||
                 strstr(resp_buf, "CLOSED") != NULL ||
                 esp8266_tcp_phase_timeout(AT_CMD_TIMEOUT))
        {
            s_status = ESP8266_STATUS_WIFI_GOT_IP;
            esp8266_save_debug("HB:send fail");
            esp8266_set_tcp_phase(TCP_PHASE_DONE_FAIL);
            esp8266_clear_rx();
        }
        break;

    case TCP_PHASE_CLOSE_SEND:
        if (esp8266_send_cmd_now("AT+CIPCLOSE"))
        {
            esp8266_set_tcp_phase(TCP_PHASE_CLOSE_WAIT);
        }
        break;

    case TCP_PHASE_CLOSE_WAIT:
        if (esp8266_check_resp("OK") || esp8266_check_resp("CLOSED") ||
            esp8266_tcp_phase_timeout(AT_CMD_TIMEOUT))
        {
            if (s_status == ESP8266_STATUS_TCP_CONNECTED)
                s_status = ESP8266_STATUS_WIFI_GOT_IP;

            esp8266_set_tcp_phase(TCP_PHASE_IDLE);
            esp8266_tx_queue_reset();
        }
        break;

    default:
        break;
    }
}

/* ==================== 发送事务状态机 ==================== */

static void tx_engine_start_next(void)
{
    if (s_status != ESP8266_STATUS_TCP_CONNECTED)
        return;

    if (s_tcp_phase != TCP_PHASE_DONE_OK)
        return;

    if (s_tx_phase != TX_ENGINE_IDLE)
        return;

    if (!esp8266_tx_queue_pop_internal(&s_tx_current))
        return;

    s_tx_has_current = 1u;

    {
        char cmd[32];
        uint8_t payload[ESP8266_MAX_TX_PAYLOAD];
        uint16_t payload_len = 0;

        format_tx_item_payload(&s_tx_current, payload, &payload_len);
        snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u", payload_len);
        if (esp8266_send_cmd_now(cmd))
        {
            esp8266_set_tx_phase(TX_ENGINE_PROMPT_WAIT);
        }
    }
}

static void tx_engine_finish_current(int success, const char *dbg)
{
    if (!success)
    {
        int link_lost;

        esp8266_snapshot_resp();
        link_lost = (strstr(resp_buf, "CLOSED") != NULL) ||
                    (strstr(resp_buf, "link is not valid") != NULL) ||
                    (strstr(resp_buf, "CONNECT FAIL") != NULL);

        esp8266_save_debug(dbg ? dbg : "TX:fail");
        if (link_lost)
        {
            s_status = ESP8266_STATUS_WIFI_GOT_IP;
            esp8266_set_tcp_phase(TCP_PHASE_DONE_FAIL);
        }
        else
        {
            esp8266_set_tcp_phase(TCP_PHASE_DONE_OK);
        }
    }

    memset(&s_tx_current, 0, sizeof(s_tx_current));
    s_tx_has_current = 0u;
    esp8266_set_tx_phase(TX_ENGINE_IDLE);
    esp8266_clear_rx();
}

static void tx_engine_poll(void)
{
    if (s_status != ESP8266_STATUS_TCP_CONNECTED)
        return;

    if (s_tcp_phase != TCP_PHASE_DONE_OK)
        return;

    if (s_tx_phase == TX_ENGINE_IDLE)
    {
        tx_engine_start_next();
        return;
    }

    switch (s_tx_phase)
    {
    case TX_ENGINE_PROMPT_WAIT:
        if (esp8266_check_resp(">"))
        {
            esp8266_set_tx_phase(TX_ENGINE_DATA_SEND);
        }
        else if (esp8266_check_resp("ERROR") ||
                 esp8266_check_resp("CLOSED") ||
                 esp8266_tx_phase_timeout(AT_CMD_TIMEOUT))
        {
            tx_engine_finish_current(0, "TX:prompt fail");
        }
        break;

    case TX_ENGINE_DATA_SEND:
        if (s_tx_has_current)
        {
            uint8_t payload[ESP8266_MAX_TX_PAYLOAD];
            uint16_t payload_len = 0;

            format_tx_item_payload(&s_tx_current, payload, &payload_len);
            if (esp8266_uart_tx_ready())
            {
                esp8266_clear_rx();
                if (esp8266_send_raw_buf(payload, payload_len))
                {
                    esp8266_set_tx_phase(TX_ENGINE_ACK_WAIT);
                }
            }
        }
        else
        {
            esp8266_set_tx_phase(TX_ENGINE_IDLE);
        }
        break;

    case TX_ENGINE_ACK_WAIT:
        esp8266_snapshot_resp();
        if (strstr(resp_buf, "SEND OK") != NULL)
        {
            tx_engine_finish_current(1, "TX:ok");
        }
        else if (strstr(resp_buf, "ERROR") != NULL ||
                 strstr(resp_buf, "CLOSED") != NULL ||
                 esp8266_tx_phase_timeout(AT_CMD_TIMEOUT))
        {
            tx_engine_finish_current(0, "TX:ack fail");
        }
        break;

    default:
        break;
    }
}

/* ==================== 初始化状态机 ==================== */

static void init_poll(void)
{
    char cmd_buf[128];

    switch (s_phase)
    {
    case INIT_POWERON:
        if (esp8266_phase_timeout(POWERON_DELAY))
        {
            esp8266_set_phase(INIT_RST_ASSERT);
        }
        break;

    case INIT_RST_ASSERT:
        HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_RESET);
        esp8266_save_debug("HW RST...");
        esp8266_set_phase(INIT_RST_RELEASE);
        break;

    case INIT_RST_RELEASE:
        if (esp8266_phase_timeout(HW_RST_LOW_MS))
        {
            HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_SET);
            esp8266_set_phase(INIT_RST_WAIT);
        }
        break;

    case INIT_RST_WAIT:
        if (esp8266_check_resp("ready") || esp8266_check_resp("Ready"))
        {
            esp8266_uart_recover();
            s_retry_count = 0u;
            esp8266_set_phase(INIT_AT_SEND);
        }
        else if (esp8266_phase_timeout(RST_TIMEOUT))
        {
            esp8266_uart_recover();
            s_retry_count = 0u;
            esp8266_set_phase(INIT_AT_SEND);
        }
        break;

    case INIT_AT_SEND:
        if (huart_esp8266.Init.BaudRate != ESP8266_USART_BAUDRATE)
            esp8266_uart_set_baud(ESP8266_USART_BAUDRATE);

        esp8266_save_baud_debug("AT@");
        if (esp8266_send_cmd_now("AT"))
        {
            esp8266_set_phase(INIT_AT_WAIT);
        }
        break;

    case INIT_AT_WAIT:
        if (esp8266_check_resp("OK"))
        {
            s_retry_count = 0u;
            esp8266_save_baud_debug("UART=");
            esp8266_set_phase(INIT_UART_DEF_SEND);
        }
        else if (esp8266_phase_timeout(AT_TEST_TIMEOUT))
        {
            if (++s_retry_count < AT_RETRIES)
            {
                esp8266_uart_recover();
                esp8266_set_phase(INIT_AT_SEND);
            }
            else
            {
                esp8266_save_debug("AT:baud fail");
                s_status = ESP8266_STATUS_ERROR;
                esp8266_set_phase(INIT_FAIL);
            }
        }
        break;

    case INIT_ATE0_SEND:
        if (esp8266_send_cmd_now("ATE0"))
        {
            esp8266_set_phase(INIT_ATE0_WAIT);
        }
        break;

    case INIT_ATE0_WAIT:
        if (esp8266_check_resp("OK") || esp8266_phase_timeout(AT_CMD_TIMEOUT))
        {
            esp8266_set_phase(INIT_CWMODE_SEND);
        }
        break;

    case INIT_UART_DEF_SEND:
        snprintf(cmd_buf, sizeof(cmd_buf), "AT+UART_DEF=%lu,8,1,0,0",
                 (unsigned long)ESP8266_USART_BAUDRATE);
        if (esp8266_send_cmd_now(cmd_buf))
        {
            esp8266_set_phase(INIT_UART_DEF_WAIT);
        }
        break;

    case INIT_UART_DEF_WAIT:
        if (esp8266_check_resp("OK"))
        {
            s_retry_count = 0u;
            esp8266_set_phase(INIT_ATE0_SEND);
        }
        else if (esp8266_phase_timeout(AT_CMD_TIMEOUT))
        {
            esp8266_save_debug("UART_DEF fail");
            s_status = ESP8266_STATUS_ERROR;
            esp8266_set_phase(INIT_FAIL);
        }
        break;

    case INIT_CWMODE_SEND:
        if (esp8266_send_cmd_now("AT+CWMODE=1"))
        {
            esp8266_set_phase(INIT_CWMODE_WAIT);
        }
        break;

    case INIT_CWMODE_WAIT:
        if (esp8266_check_resp("OK") || esp8266_check_resp("no change") ||
            esp8266_phase_timeout(AT_CMD_TIMEOUT))
        {
            s_status = ESP8266_STATUS_READY;
            s_retry_count = 0u;
            esp8266_set_phase(INIT_CWJAP_SEND);
        }
        break;

    case INIT_CWJAP_SEND:
        s_status = ESP8266_STATUS_CONNECTING_WIFI;
        if (s_use_cwjap_def)
        {
            snprintf(cmd_buf, sizeof(cmd_buf),
                     "AT+CWJAP_DEF=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASSWORD);
        }
        else
        {
            snprintf(cmd_buf, sizeof(cmd_buf),
                     "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASSWORD);
        }
        if (esp8266_send_cmd_now(cmd_buf))
        {
            esp8266_save_debug(s_use_cwjap_def ? "CWJAP_DEF..." : "CWJAP...");
            esp8266_set_phase(INIT_CWJAP_WAIT);
        }
        break;

    case INIT_CWJAP_WAIT:
    {
        int got_ip, got_ok, got_fail, got_err, got_cwjap_err;

        esp8266_snapshot_resp();
        got_ip = (strstr(resp_buf, "WIFI GOT IP") != NULL) ||
                 (strstr(resp_buf, "GOT IP") != NULL);
        got_ok = (strstr(resp_buf, "OK") != NULL);
        got_fail = (strstr(resp_buf, "FAIL") != NULL);
        got_err = (strstr(resp_buf, "ERROR") != NULL);
        got_cwjap_err = (strstr(resp_buf, "+CWJAP:") != NULL);

        if (got_ip)
        {
            esp8266_save_debug("WiFi OK");
            s_status = ESP8266_STATUS_WIFI_GOT_IP;
            esp8266_set_phase(INIT_CIFSR_SEND);
        }
        else if (got_ok && !got_fail && !got_err)
        {
            if (esp8266_phase_timeout(3000u))
            {
                esp8266_save_debug("WiFi OK(old)");
                s_status = ESP8266_STATUS_WIFI_GOT_IP;
                esp8266_set_phase(INIT_CIFSR_SEND);
            }
        }
        else if (got_fail || got_cwjap_err)
        {
            if (++s_retry_count < CWJAP_RETRIES)
            {
                esp8266_set_phase(INIT_CWJAP_SEND);
            }
            else
            {
                esp8266_save_debug("WiFi:FAIL");
                s_status = ESP8266_STATUS_ERROR;
                esp8266_set_phase(INIT_FAIL);
            }
        }
        else if (got_err && !got_fail)
        {
            if (!s_use_cwjap_def)
            {
                s_use_cwjap_def = 1u;
                s_retry_count = 0u;
                esp8266_save_debug("try CWJAP_DEF");
                esp8266_set_phase(INIT_CWJAP_SEND);
            }
            else
            {
                esp8266_save_debug("CWJAP/DEF err");
                s_status = ESP8266_STATUS_ERROR;
                esp8266_set_phase(INIT_FAIL);
            }
        }
        else if (esp8266_phase_timeout(CWJAP_TIMEOUT))
        {
            if (++s_retry_count < CWJAP_RETRIES)
            {
                esp8266_save_debug("WiFi:timeout");
                esp8266_set_phase(INIT_CWJAP_SEND);
            }
            else
            {
                s_status = ESP8266_STATUS_ERROR;
                esp8266_set_phase(INIT_FAIL);
            }
        }
        break;
    }

    case INIT_CIFSR_SEND:
        if (esp8266_send_cmd_now("AT+CIFSR"))
        {
            esp8266_set_phase(INIT_CIFSR_WAIT);
        }
        break;

    case INIT_CIFSR_WAIT:
        if (esp8266_check_resp("OK"))
        {
            parse_cifsr_ip();
            if (s_ip_addr[0] != '\0')
            {
                snprintf(s_debug_msg, sizeof(s_debug_msg), "IP:%s", s_ip_addr);
            }
            else
            {
                esp8266_save_debug("IP:parse fail");
            }

            s_status = ESP8266_STATUS_WIFI_GOT_IP;
            esp8266_set_phase(INIT_COMPLETE);
        }
        else if (esp8266_phase_timeout(AT_CMD_TIMEOUT))
        {
            esp8266_save_debug("IP:query timeout");
            esp8266_set_phase(INIT_COMPLETE);
        }
        break;

    case INIT_COMPLETE:
        s_phase = (init_phase_t)0xFF;
        break;

    case INIT_FAIL:
        s_phase = (init_phase_t)0xFE;
        break;

    default:
        break;
    }
}

/* ==================== 公共接口 ==================== */

void esp8266_init(void)
{
    memset(rx_ring_buf, 0, sizeof(rx_ring_buf));
    memset(resp_buf, 0, sizeof(resp_buf));
    memset(s_ip_addr, 0, sizeof(s_ip_addr));
    memset(s_debug_msg, 0, sizeof(s_debug_msg));
    memset(s_tcp_server_ip, 0, sizeof(s_tcp_server_ip));
    memset(s_uart_tx_buf, 0, sizeof(s_uart_tx_buf));
    esp8266_clear_tcp_line_buf();

    esp8266_gpio_init();
    esp8266_uart_init();
    esp8266_tx_queue_reset();

    s_uart_tx_busy = 0u;
    s_uart_tx_len = 0u;
    s_status = ESP8266_STATUS_INITIALIZING;
    s_retry_count = 0u;
    s_use_cwjap_def = 0u;
    s_tcp_server_port = 0u;
    s_tcp_parse_offset = 0u;

    esp8266_set_phase(INIT_POWERON);
    esp8266_set_tcp_phase(TCP_PHASE_IDLE);
}

void esp8266_poll(void)
{
    init_poll();
    tcp_link_monitor();
    tcp_poll();
    tx_engine_poll();
}

esp8266_status_t esp8266_get_status(void)
{
    return s_status;
}

const char *esp8266_get_ip_cached(void)
{
    return s_ip_addr;
}

const char *esp8266_get_debug_msg(void)
{
    return s_debug_msg;
}

void esp8266_connect_tcp_async(const char *ip, uint16_t port)
{
    if (!ip || ip[0] == '\0')
        return;

    if (s_status < ESP8266_STATUS_WIFI_CONNECTED)
        return;

    strncpy(s_tcp_server_ip, ip, sizeof(s_tcp_server_ip) - 1u);
    s_tcp_server_ip[sizeof(s_tcp_server_ip) - 1u] = '\0';
    s_tcp_server_port = port;

    if (s_tcp_phase == TCP_PHASE_IDLE ||
        s_tcp_phase == TCP_PHASE_DONE_FAIL ||
        s_tcp_phase == TCP_PHASE_DONE_OK)
    {
        esp8266_set_tcp_phase(TCP_PHASE_CIPMUX_SEND);
    }
}

int esp8266_tcp_connect_state(void)
{
    switch (s_tcp_phase)
    {
    case TCP_PHASE_DONE_OK:
        return 1;
    case TCP_PHASE_DONE_FAIL:
        return -1;
    case TCP_PHASE_IDLE:
        return 0;
    default:
        return 0;
    }
}

int esp8266_tcp_send_heartbeat(void)
{
    if (s_status != ESP8266_STATUS_TCP_CONNECTED)
        return 0;

    if (s_tcp_phase != TCP_PHASE_DONE_OK)
        return 0;

    if (s_tx_phase != TX_ENGINE_IDLE)
        return 0;

    esp8266_set_tcp_phase(TCP_PHASE_HB_CIPSEND);
    return 1;
}

int esp8266_disconnect_tcp_async(void)
{
    if (s_status != ESP8266_STATUS_TCP_CONNECTED)
        return -1;

    if (s_tcp_phase != TCP_PHASE_DONE_OK)
        return -1;

    esp8266_set_tcp_phase(TCP_PHASE_CLOSE_SEND);
    return 0;
}

int esp8266_tcp_read_line(char *out, uint16_t out_size)
{
    char *base;
    char *ipd;
    char *comma;
    char *len_start;
    char *next_comma;
    char *colon;
    char *payload;
    uint16_t buf_len;
    uint16_t payload_len;
    uint16_t avail;
    uint16_t consume_len;

    if (!out || out_size < 2u)
        return 0;

    out[0] = '\0';

    if (esp8266_pop_tcp_line(out, out_size))
        return 1;

    esp8266_snapshot_resp();
    buf_len = (uint16_t)strlen(resp_buf);
    if (buf_len == 0u)
    {
        s_tcp_parse_offset = 0u;
        return 0;
    }

    if (s_tcp_parse_offset >= buf_len)
    {
        if (buf_len > 32u)
            s_tcp_parse_offset = (uint16_t)(buf_len - 32u);
        else
            s_tcp_parse_offset = 0u;
    }

    base = resp_buf + s_tcp_parse_offset;
    ipd = strstr(base, "+IPD,");
    if (!ipd)
        return 0;

    comma = strchr(ipd, ',');
    colon = strchr(ipd, ':');
    if (!comma || !colon || colon <= comma)
        return 0;

    len_start = comma + 1;
    next_comma = strchr(len_start, ',');
    if (next_comma && next_comma < colon)
    {
        len_start = next_comma + 1;
    }

    payload_len = (uint16_t)atoi(len_start);
    payload = colon + 1;
    avail = (uint16_t)(resp_buf + buf_len - payload);
    if (avail < payload_len)
        return 0;

    esp8266_append_tcp_payload(payload, payload_len);

    consume_len = (uint16_t)((payload - resp_buf) + payload_len);
    esp8266_consume_rx(consume_len);
    return esp8266_pop_tcp_line(out, out_size);
}

/* ==================== 中断服务 ==================== */

void esp8266_uart_irq_handler(void)
{
    HAL_UART_IRQHandler(&huart_esp8266);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == ESP8266_USART)
    {
        s_uart_tx_busy = 0u;
        s_uart_tx_len = 0u;
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == ESP8266_USART)
    {
        if (rx_write_idx < (ESP8266_RX_BUF_SIZE - 1u))
            rx_ring_buf[rx_write_idx++] = rx_byte;

        HAL_UART_Receive_IT(&huart_esp8266, &rx_byte, 1);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == ESP8266_USART)
    {
        s_uart_tx_busy = 0u;
        s_uart_tx_len = 0u;
        __HAL_UART_CLEAR_OREFLAG(huart);
        __HAL_UART_CLEAR_NEFLAG(huart);
        __HAL_UART_CLEAR_FEFLAG(huart);
        __HAL_UART_CLEAR_PEFLAG(huart);
        HAL_UART_Receive_IT(huart, &rx_byte, 1);
    }
}
