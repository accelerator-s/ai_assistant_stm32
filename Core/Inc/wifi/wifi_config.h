/**
 * @file  wifi_config.h
 * @brief WiFi 连接参数配置
 */
#ifndef __WIFI_CONFIG_H
#define __WIFI_CONFIG_H

/* ===================== WiFi 热点配置 ===================== */

/* 目标 WiFi 的 SSID（热点名称） */
#define WIFI_SSID "OPPO Find X6 Pro"

/* 目标 WiFi 的密码 */
#define WIFI_PASSWORD "87654321"

/* ===================== 服务器地址配置 ===================== */

/* 云端服务器 IP 地址 */
#define SERVER_IP "10.254.59.245"

/* 云端服务器端口号 */
#define SERVER_PORT 8266

/* ===================== ESP8266 硬件配置 ===================== */

/* ESP8266 使用的串口: USART3 (PB10=TX, PB11=RX)，焊接在开发板上 */
#define ESP8266_USART USART3
#define ESP8266_USART_BAUDRATE 115200

#define ESP8266_USART_CLK_ENABLE() __HAL_RCC_USART3_CLK_ENABLE()
#define ESP8266_USART_IRQn USART3_IRQn
#define ESP8266_USART_IRQHandler USART3_IRQHandler

#define ESP8266_TX_GPIO_PORT GPIOB
#define ESP8266_TX_GPIO_PIN GPIO_PIN_10
#define ESP8266_RX_GPIO_PORT GPIOB
#define ESP8266_RX_GPIO_PIN GPIO_PIN_11
#define ESP8266_GPIO_CLK_ENABLE() __HAL_RCC_GPIOB_CLK_ENABLE()

/* ESP8266 使能引脚 CH_PD — PB8（高电平使能模块） */
#define ESP8266_CH_PD_PORT GPIOB
#define ESP8266_CH_PD_PIN GPIO_PIN_8

/* ESP8266 复位引脚 RST — PB9（低电平复位，正常工作时拉高） */
#define ESP8266_RST_PORT GPIOB
#define ESP8266_RST_PIN GPIO_PIN_9

/* ===================== 超时与缓冲区 ===================== */

/* AT 指令默认响应超时（毫秒） */
#define ESP8266_DEFAULT_TIMEOUT 3000

/* WiFi 连接超时（毫秒），热点连接较慢需要更长时间 */
#define ESP8266_WIFI_JOIN_TIMEOUT 15000

/* 串口接收环形缓冲区大小 */
#define ESP8266_RX_BUF_SIZE 512

#endif /* __WIFI_CONFIG_H */
