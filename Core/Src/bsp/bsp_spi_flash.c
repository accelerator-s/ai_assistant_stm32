/**
 * @file  bsp_spi_flash.c
 * @brief W25Q64 SPI Flash 驱动实现
 */
#include "bsp/bsp_spi_flash.h"

/* SPI 句柄 */
static SPI_HandleTypeDef hspi_flash;

/* 收发单字节 */
static uint8_t spi_send_byte(uint8_t byte)
{
    uint8_t rx;
    HAL_SPI_TransmitReceive(&hspi_flash, &byte, &rx, 1, 100);
    return rx;
}

/**
 * @brief  初始化 SPI1 及片选 GPIO
 */
void spi_flash_init(void)
{
    /* 使能时钟 */
    FLASH_SPI_CLK_ENABLE();
    FLASH_SPI_GPIO_CLK_ENABLE();
    FLASH_CS_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};

    /* SCK / MOSI: 复用推挽 */
    gpio.Pin = FLASH_SCK_PIN | FLASH_MOSI_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(FLASH_SCK_PORT, &gpio);

    /* MISO: 浮空输入 */
    gpio.Pin = FLASH_MISO_PIN;
    gpio.Mode = GPIO_MODE_AF_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(FLASH_MISO_PORT, &gpio);

    /* CS: 推挽输出，默认高电平 */
    gpio.Pin = FLASH_CS_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(FLASH_CS_PORT, &gpio);
    FLASH_CS_HIGH();

    /* SPI1 配置: 主机模式, 8位, CPOL=1 CPHA=1, 软件片选 */
    hspi_flash.Instance = FLASH_SPI;
    hspi_flash.Init.Mode = SPI_MODE_MASTER;
    hspi_flash.Init.Direction = SPI_DIRECTION_2LINES;
    hspi_flash.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi_flash.Init.CLKPolarity = SPI_POLARITY_HIGH;
    hspi_flash.Init.CLKPhase = SPI_PHASE_2EDGE;
    hspi_flash.Init.NSS = SPI_NSS_SOFT;
    hspi_flash.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
    hspi_flash.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi_flash.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi_flash.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi_flash.Init.CRCPolynomial = 7;
    HAL_SPI_Init(&hspi_flash);

    __HAL_SPI_ENABLE(&hspi_flash);
}

/**
 * @brief  读取 JEDEC ID (厂商+器件)，W25Q64 应返回 0xEF4017
 */
uint32_t spi_flash_read_id(void)
{
    uint32_t id = 0;

    FLASH_CS_LOW();
    spi_send_byte(W25X_JEDEC_DEVICE_ID);
    id = (uint32_t)spi_send_byte(FLASH_DUMMY_BYTE) << 16;
    id |= (uint32_t)spi_send_byte(FLASH_DUMMY_BYTE) << 8;
    id |= (uint32_t)spi_send_byte(FLASH_DUMMY_BYTE);
    FLASH_CS_HIGH();

    return id;
}

/**
 * @brief  读取任意长度数据
 */
void spi_flash_read(uint8_t *buf, uint32_t addr, uint32_t size)
{
    FLASH_CS_LOW();
    spi_send_byte(W25X_READ_DATA);
    spi_send_byte((addr >> 16) & 0xFF);
    spi_send_byte((addr >> 8) & 0xFF);
    spi_send_byte(addr & 0xFF);

    while (size--)
    {
        *buf++ = spi_send_byte(FLASH_DUMMY_BYTE);
    }
    FLASH_CS_HIGH();
}

/**
 * @brief  发送写使能指令
 */
static void spi_flash_write_enable(void)
{
    FLASH_CS_LOW();
    spi_send_byte(W25X_WRITE_ENABLE);
    FLASH_CS_HIGH();
}

/**
 * @brief  等待内部写操作完成
 */
void spi_flash_wait_write_end(void)
{
    uint8_t status;

    FLASH_CS_LOW();
    spi_send_byte(W25X_READ_STATUS_REG);
    do
    {
        status = spi_send_byte(FLASH_DUMMY_BYTE);
    } while (status & FLASH_WIP_FLAG);
    FLASH_CS_HIGH();
}

/**
 * @brief  页编程 (单次最多 256 字节，不可跨页)
 */
void spi_flash_write_page(uint8_t *buf, uint32_t addr, uint16_t size)
{
    spi_flash_write_enable();

    FLASH_CS_LOW();
    spi_send_byte(W25X_PAGE_PROGRAM);
    spi_send_byte((addr >> 16) & 0xFF);
    spi_send_byte((addr >> 8) & 0xFF);
    spi_send_byte(addr & 0xFF);

    while (size--)
    {
        spi_send_byte(*buf++);
    }
    FLASH_CS_HIGH();

    spi_flash_wait_write_end();
}

/**
 * @brief  擦除一个扇区 (4KB)
 */
void spi_flash_erase_sector(uint32_t addr)
{
    spi_flash_write_enable();

    FLASH_CS_LOW();
    spi_send_byte(W25X_SECTOR_ERASE);
    spi_send_byte((addr >> 16) & 0xFF);
    spi_send_byte((addr >> 8) & 0xFF);
    spi_send_byte(addr & 0xFF);
    FLASH_CS_HIGH();

    spi_flash_wait_write_end();
}

/**
 * @brief  整片擦除
 */
void spi_flash_erase_chip(void)
{
    spi_flash_write_enable();

    FLASH_CS_LOW();
    spi_send_byte(W25X_CHIP_ERASE);
    FLASH_CS_HIGH();

    spi_flash_wait_write_end();
}

/**
 * @brief  从外部 Flash 读取 GB2312 16x16 字模
 * @param  buf      输出缓冲区，至少 32 字节
 * @param  gbk_code GBK 编码值 (高字节为区码，低字节为位码)
 * @return 0 成功
 */
int get_gbk_code(uint8_t *buf, uint16_t gbk_code)
{
    uint8_t high, low;
    uint32_t pos;
    static uint8_t flash_ready = 0;

    /* 首次调用时初始化 SPI Flash */
    if (!flash_ready)
    {
        spi_flash_init();
        flash_ready = 1;
    }

    high = (uint8_t)(gbk_code >> 8);
    low = (uint8_t)(gbk_code & 0xFF);

    /* GB2312 寻址: (区码-0xA1)*94 + (位码-0xA1)，每字符 32 字节 */
    pos = ((uint32_t)(high - 0xA1) * 94 + (low - 0xA1)) * CN_CHAR_BYTES;

    spi_flash_read(buf, GBKCODE_START_ADDRESS + pos, CN_CHAR_BYTES);
    return 0;
}
