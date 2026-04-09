/**
 * @file  esp8266.h
 * @brief ESP8266 WiFi 模块驱动
 *        通过 USART3 (PB10/PB11) 与板载 ESP8266 通信
 *        使用 AT 指令集完成 WiFi 连接与 TCP 数据收发
 *
 * 设计说明:
 *  1. 对外默认提供非阻塞接口；
 *  2. 所有发送请求进入内部发送队列，由驱动状态机异步推进；
 *  3. 不再暴露阻塞式 TCP 发送接口给上层业务；
 *  4. 上层应在主循环中持续调用 esp8266_poll() 驱动连接、发送、接收与重连。
 */
#ifndef __ESP8266_H
#define __ESP8266_H

#include "main.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* ==================== 基本配置 ==================== */

#ifndef ESP8266_TX_QUEUE_CAPACITY
#define ESP8266_TX_QUEUE_CAPACITY 8U
#endif

#ifndef ESP8266_MAX_TX_PAYLOAD
#define ESP8266_MAX_TX_PAYLOAD 1024U
#endif

    /* ==================== 连接状态 ==================== */

    typedef enum
    {
        ESP8266_STATUS_IDLE = 0,        /* 未初始化 */
        ESP8266_STATUS_INITIALIZING,    /* 正在初始化模块 */
        ESP8266_STATUS_READY,           /* 模块已就绪 */
        ESP8266_STATUS_CONNECTING_WIFI, /* 正在连接 WiFi */
        ESP8266_STATUS_WIFI_CONNECTED,  /* 已连接 WiFi */
        ESP8266_STATUS_WIFI_GOT_IP,     /* 已获取 IP 地址 */
        ESP8266_STATUS_TCP_CONNECTED,   /* TCP 连接已建立 */
        ESP8266_STATUS_ERROR            /* 出错 */
    } esp8266_status_t;

    /* ==================== 非阻塞发送结果 ==================== */

    typedef enum
    {
        ESP8266_SEND_OK = 0,          /* 已成功入队 */
        ESP8266_SEND_BUSY = 1,        /* 当前忙，但请求已接受/稍后推进 */
        ESP8266_SEND_QUEUE_FULL = -1, /* 发送队列已满 */
        ESP8266_SEND_TOO_LARGE = -2,  /* 单包长度超过限制 */
        ESP8266_SEND_NOT_READY = -3,  /* 当前未建立 TCP 连接 */
        ESP8266_SEND_INVALID = -4     /* 参数无效 */
    } esp8266_send_result_t;

    /* ==================== 发送项类型 ==================== */

    typedef enum
    {
        ESP8266_TX_KIND_TCP_RAW = 0, /* 纯 TCP 原始字节流 */
        ESP8266_TX_KIND_TCP_TEXT,    /* 文本帧，驱动可按需附加换行 */
        ESP8266_TX_KIND_AT_CMD       /* 内部/调试用 AT 指令 */
    } esp8266_tx_kind_t;

    /* ==================== 发送队列项 ==================== */

    typedef struct
    {
        esp8266_tx_kind_t kind;
        uint16_t len;
        uint8_t append_crlf; /* 仅对 TEXT / AT_CMD 有意义 */
        uint8_t data[ESP8266_MAX_TX_PAYLOAD];
    } esp8266_tx_item_t;

    /* ==================== 生命周期接口 ==================== */

    /**
     * @brief 初始化 ESP8266 硬件并启动非阻塞状态机
     *        本函数不等待联网成功，返回后需持续调用 esp8266_poll()
     */
    void esp8266_init(void);

    /**
     * @brief 非阻塞轮询函数
     *        负责推进:
     *        - 模块初始化
     *        - WiFi 连接
     *        - TCP 连接
     *        - 发送队列
     *        - 接收解析
     *        - 心跳与断链检测
     */
    void esp8266_poll(void);

    /* ==================== 状态查询接口 ==================== */

    /**
     * @brief 获取当前连接状态
     */
    esp8266_status_t esp8266_get_status(void);

    /**
     * @brief 获取缓存的本机 IP 字符串（连接成功后有效）
     */
    const char *esp8266_get_ip_cached(void);

    /**
     * @brief 获取最后一次关键调试信息
     */
    const char *esp8266_get_debug_msg(void);

    /**
     * @brief 查询当前发送队列中待发送项数量
     */
    uint16_t esp8266_tx_queue_count(void);

    /**
     * @brief 查询发送队列是否为空
     * @return 1=空, 0=非空
     */
    uint8_t esp8266_tx_queue_is_empty(void);

    /**
     * @brief 查询当前是否存在正在进行的异步发送事务
     * @return 1=发送中, 0=空闲
     */
    uint8_t esp8266_tx_in_progress(void);

    /* ==================== 非阻塞 TCP 连接接口 ==================== */

    /**
     * @brief 非阻塞启动 TCP 连接
     * @param ip   服务器 IP 字符串
     * @param port 服务器端口
     */
    void esp8266_connect_tcp_async(const char *ip, uint16_t port);

    /**
     * @brief 查询非阻塞 TCP 连接结果
     * @return 0=进行中/空闲, 1=成功, -1=失败
     */
    int esp8266_tcp_connect_state(void);

    /**
     * @brief 非阻塞发起一次心跳发送
     *        仅在 TCP 已连接且发送引擎空闲时有效
     * @return 1=已发起, 0=条件不满足未发起
     */
    int esp8266_tcp_send_heartbeat(void);

    /**
     * @brief 非阻塞请求关闭 TCP 连接
     * @return 0=请求已接受, -1=当前不可关闭
     */
    int esp8266_disconnect_tcp_async(void);

    /* ==================== 非阻塞发送接口 ==================== */

    /**
     * @brief 将原始字节流入队，通过 TCP 异步发送
     * @param data 数据指针
     * @param len  字节长度
     * @return 发送结果，ESP8266_SEND_OK 表示已成功入队
     */
    esp8266_send_result_t esp8266_tcp_send_async(const uint8_t *data, uint16_t len);

    /**
     * @brief 将 C 字符串作为 TCP 文本帧入队发送
     * @param text        文本指针
     * @param append_lf   非 0 时自动在末尾附加 '\n'
     * @return 发送结果
     */
    esp8266_send_result_t esp8266_tcp_send_text_async(const char *text, uint8_t append_lf);

    /**
     * @brief 将一条已格式化控制文本帧入队发送
     *        适合 REC_START / REC_END / NEW_SESSION 等控制命令
     * @param line 文本内容，不要求自带换行
     * @return 发送结果
     */
    esp8266_send_result_t esp8266_tcp_send_line_async(const char *line);

    /**
     * @brief 清空待发送队列
     *        不影响当前已经开始发送的事务
     */
    void esp8266_tx_queue_clear(void);

    /* ==================== 接收接口 ==================== */

    /**
     * @brief 读取一条云端下发文本行（通过 +IPD 解析）
     * @param out      输出缓冲区
     * @param out_size 输出缓冲区大小
     * @return 1=读到一行, 0=无可用文本
     */
    int esp8266_tcp_read_line(char *out, uint16_t out_size);

    /* ==================== 中断接口 ==================== */

    /**
     * @brief UART 接收中断回调入口
     *        由 USART3_IRQHandler 内部调用
     */
    void esp8266_uart_irq_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* __ESP8266_H */