/* USER CODE BEGIN Header */
/**
 * @file  main.c
 * @brief 主程序入口
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "bsp/bsp_key.h"
#include "lcd/display.h"
#include "wifi/esp8266.h"
#include "wifi/wifi_config.h"
#include "debug/debug_uart.h"
#include <string.h>

/* Private function prototypes -----------------------------------------------*/
/* 配置系统时钟 72 MHz */
void SystemClock_Config(void);

int main(void)
{
  HAL_Init();
  SystemClock_Config();

  /* 外设初始化 */
  debug_uart_init();
  bsp_key_init();
  display_init();

  /* 启动 ESP8266 非阻塞状态机（不会卡住主程序） */
  esp8266_init();

  /* 首次显示 */
  display_update_key1(bsp_key_get_k1());
  display_update_key2(bsp_key_get_k2());
  display_update_wifi("初始化中...", COLOR_YELLOW);
  display_update_ip(NULL);

  /* 主循环 */
  esp8266_status_t last_wifi_st = ESP8266_STATUS_IDLE;
  char last_debug[64] = {0};
  char last_ip[20] = {0}; /* 缓存上次显示的 IP，用于检测 IP 变化 */
  uint32_t last_tcp_try_tick = 0;
  uint8_t tcp_async_started = 0;    /* 非阻塞 TCP 连接已触发标志 */
  uint32_t last_hb_tick = 0;        /* 上次心跳发送时间戳 */
#define HEARTBEAT_INTERVAL_MS 15000 /* 心跳间隔 15 秒 */

  while (1)
  {
    /* 驱动 ESP8266 状态机前进（非阻塞） */
    esp8266_poll();

    /* 刷新运行时间 */
    display_update_uptime(HAL_GetTick() / 1000);

    /* WiFi 状态变化时刷新显示 */
    esp8266_status_t wifi_st = esp8266_get_status();
    if (wifi_st != last_wifi_st)
    {
      switch (wifi_st)
      {
      case ESP8266_STATUS_IDLE:
      case ESP8266_STATUS_INITIALIZING:
        display_update_wifi("初始化中...", COLOR_YELLOW);
        break;
      case ESP8266_STATUS_READY:
      case ESP8266_STATUS_CONNECTING_WIFI:
        display_update_wifi("连接WiFi...", COLOR_YELLOW);
        break;
      case ESP8266_STATUS_WIFI_CONNECTED:
      case ESP8266_STATUS_WIFI_GOT_IP:
        display_update_wifi("WiFi已连接", COLOR_GREEN);
        break;
      case ESP8266_STATUS_TCP_CONNECTED:
        display_update_wifi("TCP已连接", COLOR_GREEN);
        break;
      case ESP8266_STATUS_ERROR:
        display_update_wifi("连接失败", COLOR_RED);
        break;
      }
      last_wifi_st = wifi_st;
    }

    /* IP 地址变化时刷新显示（CIFSR 返回后 IP 才可用） */
    const char *cur_ip = esp8266_get_ip_cached();
    if (cur_ip && cur_ip[0] != '\0' && strcmp(cur_ip, last_ip) != 0)
    {
      strncpy(last_ip, cur_ip, sizeof(last_ip) - 1);
      last_ip[sizeof(last_ip) - 1] = '\0';
      display_update_ip(cur_ip);
    }

    /* WiFi 获取 IP 后自动建立 TCP 连接到后端设备服务（非阻塞） */
    if ((wifi_st == ESP8266_STATUS_WIFI_CONNECTED || wifi_st == ESP8266_STATUS_WIFI_GOT_IP) &&
        last_ip[0] != '\0' &&
        !tcp_async_started &&
        (HAL_GetTick() - last_tcp_try_tick) > 5000)
    {
      /* 触发非阻塞 TCP 连接，主循环不会被卡住 */
      esp8266_connect_tcp_async(SERVER_IP, SERVER_PORT);
      tcp_async_started = 1;
    }

    /* 非阻塞查询 TCP 连接结果 */
    if (tcp_async_started)
    {
      int tcp_st = esp8266_tcp_connect_state();
      if (tcp_st == 1)
      {
        tcp_async_started = 0;
        display_update_debug("TCP Connect OK");
      }
      else if (tcp_st == -1)
      {
        tcp_async_started = 0;
        /* 连接失败，5 秒后允许重试 */
        last_tcp_try_tick = HAL_GetTick();
        display_update_debug("TCP Connect Fail");
      }
    }

    /* TCP 已连接时定期发送心跳，保持链路活性 */
    if (wifi_st == ESP8266_STATUS_TCP_CONNECTED &&
        (HAL_GetTick() - last_hb_tick) >= HEARTBEAT_INTERVAL_MS)
    {
      esp8266_tcp_send_heartbeat();
      last_hb_tick = HAL_GetTick();
    }

    /* 刷新调试信息（变化时更新） */
    const char *dbg = esp8266_get_debug_msg();
    if (dbg && strcmp(dbg, last_debug) != 0)
    {
      strncpy(last_debug, dbg, sizeof(last_debug) - 1);
      last_debug[sizeof(last_debug) - 1] = '\0';
      display_update_debug(dbg);
    }

    /* 按键变化时刷新显示 */
    if (bsp_key_k1_changed())
      display_update_key1(bsp_key_get_k1());
    if (bsp_key_k2_changed())
      display_update_key2(bsp_key_get_k2());

    HAL_Delay(50);
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

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
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
      ;
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  (void)file;
  (void)line;
}
#endif
