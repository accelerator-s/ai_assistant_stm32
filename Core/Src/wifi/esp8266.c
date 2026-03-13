/**
 * @file  esp8266.c
 * @brief ESP8266 WiFi 模块驱动实现（增强版）
 *        - 启动时发送 AT+RST 硬复位，确保模块处于已知状态
 *        - 兼容新旧两版 AT 固件（AT+CWJAP / AT+CWJAP_DEF）
 *        - 兼容新旧两版 AT+CIFSR 响应格式
 *        - 非阻塞状态机，主循环调用 esp8266_poll() 驱动
 */
#include "wifi/esp8266.h"
#include "wifi/wifi_config.h"
#include <string.h>
#include <stdio.h>

/* ==================== 私有变量 ==================== */

/* USART3 句柄 */
static UART_HandleTypeDef huart_esp8266;

/* 接收缓冲区 */
static uint8_t rx_ring_buf[ESP8266_RX_BUF_SIZE];
static volatile uint16_t rx_write_idx;

/* 单字节接收缓存，中断逐字节接收 */
static uint8_t rx_byte;

/* 线性应答缓冲区，用于解析 AT 响应 */
static char resp_buf[ESP8266_RX_BUF_SIZE];

/* 模块对外状态 */
static esp8266_status_t s_status = ESP8266_STATUS_IDLE;

/* 本机 IP 地址缓存 */
static char s_ip_addr[20];

/* 调试信息：最后一次关键事件描述（用于 LCD 显示） */
static char s_debug_msg[64];

/* ==================== 非阻塞状态机 ==================== */

/* 内部初始化阶段 */
typedef enum
{
    INIT_POWERON,   /* 等待上电稳定 */
    INIT_RST_SEND,  /* 发送 AT+RST 复位 */
    INIT_RST_WAIT,  /* 等待复位完成 */
    INIT_AT_SEND,   /* 发送 AT 握手 */
    INIT_AT_WAIT,   /* 等待 AT 响应 */
    INIT_ATE0_SEND, /* 发送关回显 */
    INIT_ATE0_WAIT,
    INIT_CWMODE_SEND, /* 设置 Station 模式 */
    INIT_CWMODE_WAIT,
    INIT_UART_DEF_SEND, /* 将 ESP8266 波特率切换到 115200 */
    INIT_UART_DEF_WAIT,
    INIT_CWJAP_SEND, /* 连接 WiFi 热点 */
    INIT_CWJAP_WAIT,
    INIT_CIFSR_SEND, /* 查询 IP 地址 */
    INIT_CIFSR_WAIT,
    INIT_COMPLETE, /* 初始化完成 */
    INIT_FAIL,     /* 初始化失败 */
} init_phase_t;

static init_phase_t s_phase = INIT_POWERON;
static uint32_t s_phase_tick;   /* 进入当前阶段的时间戳 */
static uint8_t s_retry_count;   /* 当前阶段的重试计数 */
static uint8_t s_use_cwjap_def; /* 非零表示使用旧版 AT+CWJAP_DEF 指令 */

/* 自动波特率探测：依次尝试的波特率列表 */
static const uint32_t s_baud_table[] = {115200, 9600, 57600};
#define BAUD_TABLE_SIZE (sizeof(s_baud_table) / sizeof(s_baud_table[0]))
static uint8_t s_baud_idx; /* 当前正在尝试的波特率索引 */

/* 每个波特率最多重试 AT 次数 */
#define AT_PER_BAUD_RETRIES 3
#define AT_TEST_TIMEOUT 1000

/* 常规 AT 指令超时 */
#define AT_CMD_TIMEOUT 3000

/* 复位后等待 "ready" 的超时 */
#define RST_TIMEOUT 5000

/* WiFi 连接超时 20 秒，最多重试 3 次 */
#define CWJAP_TIMEOUT 20000
#define CWJAP_RETRIES 3

/* 上电等待时间 */
#define POWERON_DELAY 2000

/* ==================== 底层串口操作 ==================== */

/* ==================== CH_PD / RST 引脚操作 ==================== */

/**
 * @brief 初始化 ESP8266 的 CH_PD 和 RST 控制引脚
 *        CH_PD=PB8 高电平使能模块，RST=PB9 低电平复位
 */
static void esp8266_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    ESP8266_GPIO_CLK_ENABLE();

    /* CH_PD — PB8 推挽输出 */
    gpio.Pin = ESP8266_CH_PD_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(ESP8266_CH_PD_PORT, &gpio);

    /* RST — PB9 推挽输出 */
    gpio.Pin = ESP8266_RST_PIN;
    HAL_GPIO_Init(ESP8266_RST_PORT, &gpio);

    /* 先拉高 RST，再使能 CH_PD */
    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(ESP8266_CH_PD_PORT, ESP8266_CH_PD_PIN, GPIO_PIN_SET);
}

/**
 * @brief 硬件复位 ESP8266：RST 拉低 200ms 再拉高
 */
static void esp8266_hw_reset(void)
{
    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(200);
    HAL_GPIO_WritePin(ESP8266_RST_PORT, ESP8266_RST_PIN, GPIO_PIN_SET);
}

/* ==================== 底层串口操作 ==================== */

/**
 * @brief 初始化 USART3 外设及对应 GPIO
 */
static void esp8266_uart_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* 使能时钟 */
    ESP8266_USART_CLK_ENABLE();
    ESP8266_GPIO_CLK_ENABLE();

    /* TX — PB10 复用推挽 */
    gpio.Pin = ESP8266_TX_GPIO_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(ESP8266_TX_GPIO_PORT, &gpio);

    /* RX — PB11 浮空输入 */
    gpio.Pin = ESP8266_RX_GPIO_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(ESP8266_RX_GPIO_PORT, &gpio);

    /* UART 参数: 115200, 8N1 */
    huart_esp8266.Instance = ESP8266_USART;
    huart_esp8266.Init.BaudRate = ESP8266_USART_BAUDRATE;
    huart_esp8266.Init.WordLength = UART_WORDLENGTH_8B;
    huart_esp8266.Init.StopBits = UART_STOPBITS_1;
    huart_esp8266.Init.Parity = UART_PARITY_NONE;
    huart_esp8266.Init.Mode = UART_MODE_TX_RX;
    huart_esp8266.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart_esp8266.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart_esp8266);

    /* USART3 中断优先级 */
    HAL_NVIC_SetPriority(ESP8266_USART_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(ESP8266_USART_IRQn);

    /* 启动首次中断接收 */
    rx_write_idx = 0;
    HAL_UART_Receive_IT(&huart_esp8266, &rx_byte, 1);
}

/**
 * @brief 动态切换 USART3 波特率（用于自动探测）
 */
static void esp8266_uart_set_baud(uint32_t baud)
{
    HAL_UART_Abort(&huart_esp8266);
    huart_esp8266.Init.BaudRate = baud;
    HAL_UART_Init(&huart_esp8266);
    rx_write_idx = 0;
    memset(rx_ring_buf, 0, sizeof(rx_ring_buf));
    HAL_UART_Receive_IT(&huart_esp8266, &rx_byte, 1);
}

/**
 * @brief 清除 UART 错误标志，重新启动接收
 *        RST 后 ESP8266 启动信息以 74880 波特率输出，会导致帧错误
 */
static void esp8266_uart_recover(void)
{
    /* 清除所有错误标志 */
    __HAL_UART_CLEAR_OREFLAG(&huart_esp8266); /* 溢出错误 */
    __HAL_UART_CLEAR_NEFLAG(&huart_esp8266);  /* 噪声错误 */
    __HAL_UART_CLEAR_FEFLAG(&huart_esp8266);  /* 帧错误 */
    __HAL_UART_CLEAR_PEFLAG(&huart_esp8266);  /* 校验错误 */

    /* 如果 HAL 处于错误状态，重新初始化并启动接收 */
    if (huart_esp8266.RxState != HAL_UART_STATE_READY &&
        huart_esp8266.RxState != HAL_UART_STATE_BUSY_RX)
    {
        HAL_UART_Abort(&huart_esp8266);
        HAL_UART_Init(&huart_esp8266);
    }

    /* 清空缓冲区并重新启动接收 */
    rx_write_idx = 0;
    memset(rx_ring_buf, 0, sizeof(rx_ring_buf));
    HAL_UART_Receive_IT(&huart_esp8266, &rx_byte, 1);
}

/**
 * @brief 向 ESP8266 发送原始字节串
 */
static void esp8266_send_str(const char *str)
{
    HAL_UART_Transmit(&huart_esp8266, (uint8_t *)str, strlen(str), 500);
}

/**
 * @brief 清空接收缓冲区
 */
static void esp8266_clear_rx(void)
{
    __disable_irq();
    rx_write_idx = 0;
    memset(rx_ring_buf, 0, sizeof(rx_ring_buf));
    __enable_irq();
}

/**
 * @brief 将接收缓冲区快照到线性 resp_buf
 * @return 拷贝的字节数
 */
static uint16_t esp8266_snapshot_resp(void)
{
    uint16_t len;
    __disable_irq();
    len = rx_write_idx;
    if (len > sizeof(resp_buf) - 1)
        len = sizeof(resp_buf) - 1;
    memcpy(resp_buf, rx_ring_buf, len);
    resp_buf[len] = '\0';
    __enable_irq();
    return len;
}

/**
 * @brief 发送 AT 指令（仅发送，不等待响应）
 */
static void esp8266_send_cmd(const char *cmd)
{
    char buf[256];
    esp8266_clear_rx();
    snprintf(buf, sizeof(buf), "%s\r\n", cmd);
    esp8266_send_str(buf);
}

/**
 * @brief 非阻塞检查响应是否包含指定关键字
 */
static int esp8266_check_resp(const char *keyword)
{
    esp8266_snapshot_resp();
    return (strstr(resp_buf, keyword) != NULL) ? 1 : 0;
}

/**
 * @brief 检查当前阶段是否超时
 */
static int esp8266_phase_timeout(uint32_t ms)
{
    return (HAL_GetTick() - s_phase_tick) >= ms;
}

/**
 * @brief 切换到新的状态机阶段
 */
static void esp8266_set_phase(init_phase_t phase)
{
    s_phase = phase;
    s_phase_tick = HAL_GetTick();
}

/**
 * @brief 保存调试信息（截取前 63 字符）
 */
static void esp8266_save_debug(const char *msg)
{
    strncpy(s_debug_msg, msg, sizeof(s_debug_msg) - 1);
    s_debug_msg[sizeof(s_debug_msg) - 1] = '\0';
}

/* ==================== 阻塞式 AT（TCP 操作用） ==================== */

static int esp8266_send_at_blocking(const char *cmd, const char *expect,
                                    uint32_t timeout_ms)
{
    char at_buf[256];
    esp8266_clear_rx();
    snprintf(at_buf, sizeof(at_buf), "%s\r\n", cmd);
    esp8266_send_str(at_buf);

    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms)
    {
        esp8266_snapshot_resp();
        if (strstr(resp_buf, expect) != NULL)
            return 0;
        HAL_Delay(20);
    }
    return -1;
}

/* ==================== 中断服务 ==================== */

void esp8266_uart_irq_handler(void)
{
    HAL_UART_IRQHandler(&huart_esp8266);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == ESP8266_USART)
    {
        if (rx_write_idx < ESP8266_RX_BUF_SIZE - 1)
            rx_ring_buf[rx_write_idx++] = rx_byte;
        HAL_UART_Receive_IT(&huart_esp8266, &rx_byte, 1);
    }
}

/**
 * @brief UART 错误回调：ESP8266 复位时 74880 波特率输出会导致帧错误，
 *        必须在此重新启动接收，否则接收链永久中断
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == ESP8266_USART)
    {
        __HAL_UART_CLEAR_OREFLAG(huart);
        __HAL_UART_CLEAR_NEFLAG(huart);
        __HAL_UART_CLEAR_FEFLAG(huart);
        __HAL_UART_CLEAR_PEFLAG(huart);
        HAL_UART_Receive_IT(huart, &rx_byte, 1);
    }
}

/* ==================== IP 地址解析（兼容新旧固件） ==================== */

/**
 * @brief 从 CIFSR 响应中提取 IP 地址
 *        新固件: +CIFSR:STAIP,"192.168.x.x"
 *        旧固件: 192.168.x.x （直接返回裸 IP）
 */
static void parse_cifsr_ip(void)
{
    esp8266_snapshot_resp();

    /* 新固件格式：查找 STAIP," */
    char *p = strstr(resp_buf, "STAIP,\"");
    if (p)
    {
        p += 7;
        char *end = strchr(p, '"');
        if (end)
        {
            uint16_t len = (uint16_t)(end - p);
            if (len >= sizeof(s_ip_addr))
                len = sizeof(s_ip_addr) - 1;
            memcpy(s_ip_addr, p, len);
            s_ip_addr[len] = '\0';
            return;
        }
    }

    /* 旧固件格式：在响应文本中找形如 x.x.x.x 的 IP */
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
                    len = sizeof(s_ip_addr) - 1;
                memcpy(s_ip_addr, p, len);
                s_ip_addr[len] = '\0';
                if (strcmp(s_ip_addr, "0.0.0.0") != 0)
                    return;
            }
        }
        p++;
    }
}

/* ==================== 非阻塞状态机驱动 ==================== */

void esp8266_init(void)
{
    esp8266_gpio_init(); /* 先初始化 CH_PD/RST 引脚，使能模块 */
    esp8266_uart_init();
    s_status = ESP8266_STATUS_INITIALIZING;
    s_retry_count = 0;
    s_use_cwjap_def = 0;
    s_baud_idx = 0;
    s_ip_addr[0] = '\0';
    s_debug_msg[0] = '\0';
    esp8266_set_phase(INIT_POWERON);
}

void esp8266_poll(void)
{
    char cmd_buf[128];

    switch (s_phase)
    {
    /* -------- 上电等待 -------- */
    case INIT_POWERON:
        if (esp8266_phase_timeout(POWERON_DELAY))
            esp8266_set_phase(INIT_RST_SEND);
        break;

    /* -------- 硬件复位 -------- */
    case INIT_RST_SEND:
        esp8266_hw_reset(); /* 用 GPIO 硬复位代替 AT+RST */
        esp8266_save_debug("HW RST...");
        esp8266_set_phase(INIT_RST_WAIT);
        break;

    case INIT_RST_WAIT:
        if (esp8266_check_resp("ready") || esp8266_check_resp("Ready"))
        {
            /* 复位完成后恢复 UART 状态，清除启动时的帧错误 */
            esp8266_uart_recover();
            s_retry_count = 0;
            s_baud_idx = 0;
            esp8266_set_phase(INIT_AT_SEND);
        }
        else if (esp8266_phase_timeout(RST_TIMEOUT))
        {
            /* 超时也继续，旧固件可能不输出 "ready" */
            esp8266_uart_recover();
            s_retry_count = 0;
            s_baud_idx = 0;
            esp8266_set_phase(INIT_AT_SEND);
        }
        break;

    /* -------- AT 握手（多波特率自动探测） -------- */
    case INIT_AT_SEND:
    {
        /* 首次进入或切换波特率后，同步 USART3 波特率 */
        uint32_t target_baud = s_baud_table[s_baud_idx];
        if (huart_esp8266.Init.BaudRate != target_baud)
            esp8266_uart_set_baud(target_baud);

        char baud_msg[32];
        snprintf(baud_msg, sizeof(baud_msg), "AT@%lu..",
                 (unsigned long)target_baud);
        esp8266_save_debug(baud_msg);
        esp8266_send_cmd("AT");
        esp8266_set_phase(INIT_AT_WAIT);
        break;
    }

    case INIT_AT_WAIT:
        if (esp8266_check_resp("OK"))
        {
            uint32_t cur_baud = s_baud_table[s_baud_idx];
            s_retry_count = 0;

            /* 如果不是 115200，需要先切换 ESP8266 波特率 */
            if (cur_baud != 115200)
            {
                char msg[32];
                snprintf(msg, sizeof(msg), "AT OK@%lu",
                         (unsigned long)cur_baud);
                esp8266_save_debug(msg);
                esp8266_set_phase(INIT_UART_DEF_SEND);
            }
            else
            {
                esp8266_save_debug("AT OK");
                esp8266_set_phase(INIT_ATE0_SEND);
            }
        }
        else if (esp8266_phase_timeout(AT_TEST_TIMEOUT))
        {
            if (++s_retry_count < AT_PER_BAUD_RETRIES)
            {
                esp8266_uart_recover();
                esp8266_set_phase(INIT_AT_SEND);
            }
            else if (++s_baud_idx < BAUD_TABLE_SIZE)
            {
                /* 当前波特率不通，尝试下一个 */
                s_retry_count = 0;
                esp8266_set_phase(INIT_AT_SEND);
            }
            else
            {
                esp8266_save_debug("AT:all baud fail");
                s_status = ESP8266_STATUS_ERROR;
                esp8266_set_phase(INIT_FAIL);
            }
        }
        break;

    /* -------- 关闭回显 -------- */
    case INIT_ATE0_SEND:
        esp8266_send_cmd("ATE0");
        esp8266_set_phase(INIT_ATE0_WAIT);
        break;

    case INIT_ATE0_WAIT:
        if (esp8266_check_resp("OK") || esp8266_phase_timeout(AT_CMD_TIMEOUT))
            esp8266_set_phase(INIT_CWMODE_SEND);
        break;

    /* -------- 切换 ESP8266 到 115200 波特率（非 115200 时执行） -------- */
    case INIT_UART_DEF_SEND:
    {
        /* 用当前波特率发送改波特率指令 */
        esp8266_save_debug("set 115200...");
        esp8266_send_cmd("AT+UART_DEF=115200,8,1,0,0");
        esp8266_set_phase(INIT_UART_DEF_WAIT);
        break;
    }

    case INIT_UART_DEF_WAIT:
        if (esp8266_check_resp("OK") || esp8266_phase_timeout(AT_CMD_TIMEOUT))
        {
            /* 切换本地 USART3 波特率到 115200 */
            HAL_Delay(100); /* 等 ESP8266 切换完成 */
            esp8266_uart_set_baud(115200);
            s_baud_idx = 0;
            esp8266_save_debug("baud->115200");
            /* 验证 AT 是否在 115200 下可用 */
            s_retry_count = 0;
            esp8266_set_phase(INIT_ATE0_SEND);
        }
        break;

    /* -------- 设置 Station 模式 -------- */
    case INIT_CWMODE_SEND:
        esp8266_send_cmd("AT+CWMODE=1");
        esp8266_set_phase(INIT_CWMODE_WAIT);
        break;

    case INIT_CWMODE_WAIT:
        if (esp8266_check_resp("OK") || esp8266_check_resp("no change"))
        {
            s_status = ESP8266_STATUS_READY;
            s_retry_count = 0;
            esp8266_set_phase(INIT_CWJAP_SEND);
        }
        else if (esp8266_phase_timeout(AT_CMD_TIMEOUT))
        {
            /* 超时也继续 */
            s_status = ESP8266_STATUS_READY;
            s_retry_count = 0;
            esp8266_set_phase(INIT_CWJAP_SEND);
        }
        break;

    /* -------- 连接 WiFi（兼容新旧固件，带重试） -------- */
    case INIT_CWJAP_SEND:
        s_status = ESP8266_STATUS_CONNECTING_WIFI;
        if (s_use_cwjap_def)
            snprintf(cmd_buf, sizeof(cmd_buf),
                     "AT+CWJAP_DEF=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASSWORD);
        else
            snprintf(cmd_buf, sizeof(cmd_buf),
                     "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASSWORD);
        esp8266_send_cmd(cmd_buf);
        esp8266_save_debug(s_use_cwjap_def ? "CWJAP_DEF..." : "CWJAP...");
        esp8266_set_phase(INIT_CWJAP_WAIT);
        break;

    case INIT_CWJAP_WAIT:
    {
        /* 获取一次快照，后续多次判断用同一份数据 */
        esp8266_snapshot_resp();
        int got_ip = (strstr(resp_buf, "WIFI GOT IP") != NULL) ||
                     (strstr(resp_buf, "GOT IP") != NULL);
        int got_ok = (strstr(resp_buf, "OK") != NULL);
        int got_fail = (strstr(resp_buf, "FAIL") != NULL);
        int got_err = (strstr(resp_buf, "ERROR") != NULL);
        int got_cwjap_err = (strstr(resp_buf, "+CWJAP:") != NULL);

        /* 成功：收到 WIFI GOT IP */
        if (got_ip)
        {
            esp8266_save_debug("WiFi OK");
            s_status = ESP8266_STATUS_WIFI_GOT_IP;
            esp8266_set_phase(INIT_CIFSR_SEND);
        }
        /* 成功（旧固件兼容）：收到 OK 但没有 FAIL/ERROR */
        else if (got_ok && !got_fail && !got_err)
        {
            /* 多等 3 秒，旧固件 OK 后可能还有 WIFI GOT IP 陆续到达 */
            if (esp8266_phase_timeout(3000))
            {
                esp8266_save_debug("WiFi OK(old)");
                s_status = ESP8266_STATUS_WIFI_GOT_IP;
                esp8266_set_phase(INIT_CIFSR_SEND);
            }
        }
        /* 失败：收到 FAIL 或 +CWJAP:X 错误码 */
        else if (got_fail || got_cwjap_err)
        {
            char *err = strstr(resp_buf, "+CWJAP:");
            if (err)
            {
                char ec = *(err + 7);
                switch (ec)
                {
                case '1':
                    esp8266_save_debug("WiFi:timeout");
                    break;
                case '2':
                    esp8266_save_debug("WiFi:bad pwd");
                    break;
                case '3':
                    esp8266_save_debug("WiFi:no AP");
                    break;
                case '4':
                    esp8266_save_debug("WiFi:conn fail");
                    break;
                default:
                    esp8266_save_debug("WiFi:unknown");
                    break;
                }
            }
            else
            {
                esp8266_save_debug("WiFi:FAIL");
            }
            if (++s_retry_count < CWJAP_RETRIES)
                esp8266_set_phase(INIT_CWJAP_SEND);
            else
            {
                s_status = ESP8266_STATUS_ERROR;
                esp8266_set_phase(INIT_FAIL);
            }
        }
        /* ERROR（不伴随 FAIL）：指令可能不被支持，尝试旧版指令 */
        else if (got_err && !got_fail)
        {
            if (!s_use_cwjap_def)
            {
                s_use_cwjap_def = 1;
                s_retry_count = 0;
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
        /* 超时 */
        else if (esp8266_phase_timeout(CWJAP_TIMEOUT))
        {
            esp8266_save_debug("WiFi:timeout");
            if (++s_retry_count < CWJAP_RETRIES)
                esp8266_set_phase(INIT_CWJAP_SEND);
            else
            {
                s_status = ESP8266_STATUS_ERROR;
                esp8266_set_phase(INIT_FAIL);
            }
        }
        break;
    }

    /* -------- 查询 IP 地址 -------- */
    case INIT_CIFSR_SEND:
        esp8266_send_cmd("AT+CIFSR");
        esp8266_set_phase(INIT_CIFSR_WAIT);
        break;

    case INIT_CIFSR_WAIT:
        if (esp8266_check_resp("OK"))
        {
            parse_cifsr_ip();
            if (s_ip_addr[0] != '\0')
                snprintf(s_debug_msg, sizeof(s_debug_msg), "IP:%s", s_ip_addr);
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

    /* -------- 完成 / 失败（只执行一次） -------- */
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

/* ==================== 公共查询接口 ==================== */

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

/* ==================== 阻塞式 TCP 操作 ==================== */

int esp8266_get_ip(char *buf, uint16_t buf_size)
{
    if (esp8266_send_at_blocking("AT+CIFSR", "OK", AT_CMD_TIMEOUT) != 0)
        return -1;
    parse_cifsr_ip();
    if (s_ip_addr[0] != '\0')
    {
        strncpy(buf, s_ip_addr, buf_size - 1);
        buf[buf_size - 1] = '\0';
        return 0;
    }
    return -1;
}

int esp8266_connect_tcp(const char *ip, uint16_t port)
{
    char cmd[128];
    if (s_status < ESP8266_STATUS_WIFI_CONNECTED)
        return -1;
    esp8266_send_at_blocking("AT+CIPMUX=0", "OK", AT_CMD_TIMEOUT);
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u", ip, port);
    if (esp8266_send_at_blocking(cmd, "CONNECT", AT_CMD_TIMEOUT) == 0)
    {
        s_status = ESP8266_STATUS_TCP_CONNECTED;
        return 0;
    }
    return -1;
}

int esp8266_disconnect_tcp(void)
{
    if (esp8266_send_at_blocking("AT+CIPCLOSE", "OK", AT_CMD_TIMEOUT) == 0)
    {
        if (s_status == ESP8266_STATUS_TCP_CONNECTED)
            s_status = ESP8266_STATUS_WIFI_GOT_IP;
        return 0;
    }
    return -1;
}

int esp8266_tcp_send(const uint8_t *data, uint16_t len)
{
    char cmd[32];
    if (s_status != ESP8266_STATUS_TCP_CONNECTED)
        return -1;
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u", len);
    if (esp8266_send_at_blocking(cmd, ">", AT_CMD_TIMEOUT) != 0)
        return -1;
    HAL_UART_Transmit(&huart_esp8266, (uint8_t *)data, len, 2000);
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < AT_CMD_TIMEOUT)
    {
        esp8266_snapshot_resp();
        if (strstr(resp_buf, "SEND OK") != NULL)
            return 0;
        HAL_Delay(20);
    }
    return -1;
}
