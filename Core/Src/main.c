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
#include "audio/microphone.h"
#include "debug/debug_uart.h"
#include "system/system_controller.h"
#include <string.h>
#include <stdio.h>

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

  /* 初始化麦克风 */
  if (!microphone_init()) {
    display_update_debug("麦克风初始化失败");
  } else {
    display_update_debug("麦克风初始化成功");

    /* 配置麦克风为混合模式，启用音频处理 */
    if (!microphone_configure(MIC_MODE_HYBRID, true)) {
      display_update_debug("麦克风配置失败");
    } else {
      display_update_debug("麦克风配置成功");
    }
  }

  /* 启动 ESP8266 非阻塞状态机（不会卡住主程序） */
  esp8266_init();

  /* 初始化系统控制器 */
  system_controller_init();

  /* 首次显示 */
  display_update_key1_str("就绪");
  display_update_key2_str("就绪");
  display_update_wifi("初始化中...", COLOR_YELLOW);
  display_update_ip("等待连接");

  /* 调试信息：显示按钮初始化状态 */
  display_update_debug("按钮初始化完成");

  /* 主循环变量 */
  esp8266_status_t last_wifi_st = ESP8266_STATUS_IDLE;
  char last_ip[20] = {0};
  uint32_t last_tcp_try_tick = 0;
  uint8_t tcp_async_started = 0;
  uint32_t last_uptime_update = 0;

  while (1)
  {
    uint32_t current_time = HAL_GetTick();

    /* 驱动 ESP8266 状态机前进（非阻塞） */
    esp8266_poll();

    /* 更新按钮状态 */
    bsp_key_update();

    /* 更新运行时间（每秒更新一次） */
    if (current_time - last_uptime_update >= 1000) {
      display_update_uptime(current_time / 1000);
      last_uptime_update = current_time;
    }

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

    /* IP 地址变化时刷新显示 */
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
        (current_time - last_tcp_try_tick) > 5000)
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
        display_update_debug("TCP连接成功");
      }
      else if (tcp_st == -1)
      {
        tcp_async_started = 0;
        /* 连接失败，5 秒后允许重试 */
        last_tcp_try_tick = current_time;
        display_update_debug("TCP连接失败");
      }
    }

    /* 麦克风轮询处理 */
    microphone_poll();

    /* 系统控制器轮询处理 */
    system_controller_poll();

    /* 刷新调试信息（变化时更新） */
    const char *dbg = esp8266_get_debug_msg();
    if (dbg && dbg[0] != '\0') {
      display_update_debug(dbg);
    }

    /* 按钮状态调试（临时） */
    static uint32_t last_btn_debug = 0;
    if (current_time - last_btn_debug >= 1000) { /* 每秒更新一次 */
        uint8_t k1_state = bsp_key_get_k1();
        uint8_t k2_state = bsp_key_get_k2();
        char btn_info[32];
        snprintf(btn_info, sizeof(btn_info), "K1:%d K2:%d", k1_state, k2_state);
        display_update_debug(btn_info);
        last_btn_debug = current_time;
    }

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
