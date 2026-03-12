/**
 * @file  bsp_spi_flash.h
 * @brief W25Q64 SPI Flash 驱动 (野火指南者 STM32F103VET6)
 *        SPI1: SCK=PA5, MISO=PA6, MOSI=PA7, CS=PC0
 */
#ifndef __BSP_SPI_FLASH_H
#define __BSP_SPI_FLASH_H

#include "main.h"

/* SPI 外设及引脚定义 */
#define FLASH_SPI SPI1
#define FLASH_SPI_CLK_ENABLE() __HAL_RCC_SPI1_CLK_ENABLE()

#define FLASH_CS_PORT GPIOC
#define FLASH_CS_PIN GPIO_PIN_0
#define FLASH_CS_CLK_ENABLE() __HAL_RCC_GPIOC_CLK_ENABLE()

#define FLASH_SCK_PORT GPIOA
#define FLASH_SCK_PIN GPIO_PIN_5
#define FLASH_MISO_PORT GPIOA
#define FLASH_MISO_PIN GPIO_PIN_6
#define FLASH_MOSI_PORT GPIOA
#define FLASH_MOSI_PIN GPIO_PIN_7
#define FLASH_SPI_GPIO_CLK_ENABLE() __HAL_RCC_GPIOA_CLK_ENABLE()

/* W25Q64 器件标识 */
#define W25Q64_JEDEC_ID 0xEF4017u

/* W25Q64 指令集 */
#define W25X_WRITE_ENABLE 0x06
#define W25X_WRITE_DISABLE 0x04
#define W25X_READ_STATUS_REG 0x05
#define W25X_WRITE_STATUS_REG 0x01
#define W25X_READ_DATA 0x03
#define W25X_PAGE_PROGRAM 0x02
#define W25X_BLOCK_ERASE 0xD8
#define W25X_SECTOR_ERASE 0x20
#define W25X_CHIP_ERASE 0xC7
#define W25X_POWER_DOWN 0xB9
#define W25X_RELEASE_POWER_DOWN 0xAB
#define W25X_DEVICE_ID 0xAB
#define W25X_JEDEC_DEVICE_ID 0x9F

#define FLASH_WIP_FLAG 0x01
#define FLASH_DUMMY_BYTE 0xFF
#define SPI_FLASH_PAGE_SIZE 256

/* GB2312 字库在 SPI Flash 中的起始地址 (387 * 4096) */
#define GBKCODE_START_ADDRESS (387u * 4096u)

/* 中文字符点阵尺寸 */
#define CN_CHAR_WIDTH 16
#define CN_CHAR_HEIGHT 16
#define CN_CHAR_BYTES (CN_CHAR_WIDTH * CN_CHAR_HEIGHT / 8)

/* 片选控制 */
#define FLASH_CS_LOW() HAL_GPIO_WritePin(FLASH_CS_PORT, FLASH_CS_PIN, GPIO_PIN_RESET)
#define FLASH_CS_HIGH() HAL_GPIO_WritePin(FLASH_CS_PORT, FLASH_CS_PIN, GPIO_PIN_SET)

/* 接口函数 */
void spi_flash_init(void);
uint32_t spi_flash_read_id(void);
void spi_flash_read(uint8_t *buf, uint32_t addr, uint32_t size);
void spi_flash_write_page(uint8_t *buf, uint32_t addr, uint16_t size);
void spi_flash_erase_sector(uint32_t addr);
void spi_flash_erase_chip(void);
void spi_flash_wait_write_end(void);

/* 从外部 Flash 读取 GB2312 字模数据 */
int get_gbk_code(uint8_t *buf, uint16_t gbk_code);

#endif /* __BSP_SPI_FLASH_H */
