/**
 * @file  esp8266.h
 * @brief ESP8266 WiFi 模块驱动
 *        通过 USART3 (PB10/PB11) 与板载 ESP8266 通信
 *        使用 AT 指令集完成 WiFi 连接与 TCP 数据收发
 */
#ifndef __ESP8266_H
#define __ESP8266_H

#include "main.h"
#include <stdint.h>

/* ESP8266 连接状态 */
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

/* 初始化 ESP8266 硬件并启动非阻塞状态机（不阻塞） */
void esp8266_init(void);

/* 非阻塞轮询，在主循环中调用以推进连接流程 */
void esp8266_poll(void);

/* 获取当前连接状态 */
esp8266_status_t esp8266_get_status(void);

/* 获取缓存的本机 IP 字符串（连接成功后有效） */
const char *esp8266_get_ip_cached(void);

/* 查询当前 IP 地址（阻塞），结果写入 buf，返回 0 表示成功 */
int esp8266_get_ip(char *buf, uint16_t buf_size);

/* 建立 TCP 连接到指定服务器（阻塞） */
int esp8266_connect_tcp(const char *ip, uint16_t port);

/* 关闭当前 TCP 连接（阻塞） */
int esp8266_disconnect_tcp(void);

/* 通过 TCP 发送数据（阻塞） */
int esp8266_tcp_send(const uint8_t *data, uint16_t len);

/* 获取调试信息字符串（最后一次关键事件的描述） */
const char *esp8266_get_debug_msg(void);

/* UART 接收中断回调，由 USART3_IRQHandler 内部调用 */
void esp8266_uart_irq_handler(void);

#endif /* __ESP8266_H */
