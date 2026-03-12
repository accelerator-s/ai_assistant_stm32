#include "lcd/lcd.h"
#include "lcd/font.h"
#include "bsp/bsp_spi_flash.h"

/* FSMC 初始化: Bank1 NE1, 16位并口, 访问模式B */
static void fsmc_init(void)
{
    /* 使能 FSMC 及相关 GPIO 端口时钟 */
    __HAL_RCC_FSMC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};

    /* --- GPIOD FSMC 复用引脚 --- */
    /* PD0(D2), PD1(D3), PD4(NOE), PD5(NWE), PD7(NE1),
     * PD8(D13), PD9(D14), PD10(D15), PD11(A16), PD14(D0), PD15(D1) */
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5 |
               GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 |
               GPIO_PIN_11 | GPIO_PIN_14 | GPIO_PIN_15;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOD, &gpio);

    /* --- GPIOE FSMC 复用引脚 --- */
    /* PE7(D4) ~ PE15(D12) */
    gpio.Pin = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 |
               GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 |
               GPIO_PIN_15;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOE, &gpio);

    /* 背光 PD12 普通输出 (低电平打开背光) */
    gpio.Pin = LCD_BL_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LCD_BL_PORT, &gpio);

    /* RST PE1 普通输出 */
    gpio.Pin = LCD_RST_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LCD_RST_PORT, &gpio);

    /* 时序寄存器 BTR1: ADDSET=1, DATAST=4, 访问模式B */
    FSMC_Bank1->BTCR[1] =
        (0x01u << 28) | /* ACCMOD = 01: 访问模式B */
        (0x04u << 8) |  /* DATAST = 4 HCLK */
        (0x00u << 4) |  /* ADDHLD = 0 (模式B不使用) */
        (0x01u << 0);   /* ADDSET = 1 HCLK */

    __DSB();

    /* 控制寄存器 BCR1: NOR类型, 16位宽, 写使能 */
    FSMC_Bank1->BTCR[0] =
        FSMC_BCRx_FACCEN | /* NOR Flash 访问使能 */
        FSMC_BCRx_MTYP_1 | /* 存储器类型 = NOR (10) */
        FSMC_BCRx_MWID_0 | /* 16 位数据总线 */
        FSMC_BCRx_WREN |   /* 写操作使能 */
        FSMC_BCRx_MBKEN;   /* 使能该 Bank */

    __DSB();
    __ISB();
}

/* ILI9341 硬件复位 */
static void ili9341_rst(void)
{
    HAL_GPIO_WritePin(LCD_RST_PORT, LCD_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(5);
    HAL_GPIO_WritePin(LCD_RST_PORT, LCD_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(50);
}

static void ili9341_init_reg(void)
{
    ili9341_rst();

    /* Power control B */
    LCD_WR_CMD(0xCF);
    LCD_WR_DAT(0x00);
    LCD_WR_DAT(0x81);
    LCD_WR_DAT(0x30);

    /* Power on sequence control (EDh) */
    LCD_WR_CMD(0xED);
    LCD_WR_DAT(0x64);
    LCD_WR_DAT(0x03);
    LCD_WR_DAT(0x12);
    LCD_WR_DAT(0x81);

    /* Driver timing control A (E8h) */
    LCD_WR_CMD(0xE8);
    LCD_WR_DAT(0x85);
    LCD_WR_DAT(0x10);
    LCD_WR_DAT(0x78);

    /* Power control A (CBh) */
    LCD_WR_CMD(0xCB);
    LCD_WR_DAT(0x39);
    LCD_WR_DAT(0x2C);
    LCD_WR_DAT(0x00);
    LCD_WR_DAT(0x34);
    LCD_WR_DAT(0x02);

    /* Pump ratio control (F7h) */
    LCD_WR_CMD(0xF7);
    LCD_WR_DAT(0x20);

    /* Driver timing control B (EAh) */
    LCD_WR_CMD(0xEA);
    LCD_WR_DAT(0x00);
    LCD_WR_DAT(0x00);

    /* 帧率控制: 70Hz */
    LCD_WR_CMD(0xB1);
    LCD_WR_DAT(0x00);
    LCD_WR_DAT(0x1B);

    /* 显示功能控制 */
    LCD_WR_CMD(0xB6);
    LCD_WR_DAT(0x0A);
    LCD_WR_DAT(0xA2);

    /* 电源控制1: VRH=4.60V */
    LCD_WR_CMD(0xC0);
    LCD_WR_DAT(0x35);

    /* 电源控制2 */
    LCD_WR_CMD(0xC1);
    LCD_WR_DAT(0x11);

    /* VCOM控制1 */
    LCD_WR_CMD(0xC5);
    LCD_WR_DAT(0x45);
    LCD_WR_DAT(0x45);

    /* VCOM控制2 */
    LCD_WR_CMD(0xC7);
    LCD_WR_DAT(0xA2);

    /* 3Gamma 关闭 */
    LCD_WR_CMD(0xF2);
    LCD_WR_DAT(0x00);

    /* Gamma 曲线选择 */
    LCD_WR_CMD(0x26);
    LCD_WR_DAT(0x01);

    /* 正 Gamma 校正 (15个参数) */
    LCD_WR_CMD(0xE0);
    LCD_WR_DAT(0x0F);
    LCD_WR_DAT(0x26);
    LCD_WR_DAT(0x24);
    LCD_WR_DAT(0x0B);
    LCD_WR_DAT(0x0E);
    LCD_WR_DAT(0x09);
    LCD_WR_DAT(0x54);
    LCD_WR_DAT(0xA8);
    LCD_WR_DAT(0x46);
    LCD_WR_DAT(0x0C);
    LCD_WR_DAT(0x17);
    LCD_WR_DAT(0x09);
    LCD_WR_DAT(0x0F);
    LCD_WR_DAT(0x07);
    LCD_WR_DAT(0x00);

    /* 负 Gamma 校正 (15个参数) */
    LCD_WR_CMD(0xE1);
    LCD_WR_DAT(0x00);
    LCD_WR_DAT(0x19);
    LCD_WR_DAT(0x1B);
    LCD_WR_DAT(0x04);
    LCD_WR_DAT(0x10);
    LCD_WR_DAT(0x07);
    LCD_WR_DAT(0x2A);
    LCD_WR_DAT(0x47);
    LCD_WR_DAT(0x39);
    LCD_WR_DAT(0x03);
    LCD_WR_DAT(0x06);
    LCD_WR_DAT(0x06);
    LCD_WR_DAT(0x30);
    LCD_WR_DAT(0x38);
    LCD_WR_DAT(0x0F);

    /* 扫描方向: 正常显示
     * MADCTL = 0x08
     * MY=0, MX=0, MV=0, BGR=1 */
    LCD_WR_CMD(0x36);
    LCD_WR_DAT(0x08);

    /* 列地址范围 0~239 */
    LCD_WR_CMD(0x2A);
    LCD_WR_DAT(0x00);
    LCD_WR_DAT(0x00);
    LCD_WR_DAT(0x00);
    LCD_WR_DAT(0xEF);

    /* 行地址范围 0~319 */
    LCD_WR_CMD(0x2B);
    LCD_WR_DAT(0x00);
    LCD_WR_DAT(0x00);
    LCD_WR_DAT(0x01);
    LCD_WR_DAT(0x3F);

    /* 像素格式: 16bit/pixel (RGB565) */
    LCD_WR_CMD(0x3A);
    LCD_WR_DAT(0x55);

    /* 退出睡眠（必须在所有寄存器配置完成之后发送） */
    LCD_WR_CMD(0x11);
    HAL_Delay(120);

    /* 开启显示 */
    LCD_WR_CMD(0x29);
}

/* 初始化 LCD */
void lcd_init(void)
{
    fsmc_init();
    lcd_backlight(1);
    HAL_Delay(50);
    ili9341_init_reg();
}

/* 设置显示窗口 */
void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    LCD_WR_CMD(0x2A);
    LCD_WR_DAT(x0 >> 8);
    LCD_WR_DAT(x0 & 0xFF);
    LCD_WR_DAT(x1 >> 8);
    LCD_WR_DAT(x1 & 0xFF);

    LCD_WR_CMD(0x2B);
    LCD_WR_DAT(y0 >> 8);
    LCD_WR_DAT(y0 & 0xFF);
    LCD_WR_DAT(y1 >> 8);
    LCD_WR_DAT(y1 & 0xFF);

    LCD_WR_CMD(0x2C); /* 开始写入像素数据 */
}

/* 全屏填充 */
void lcd_clear(uint16_t color)
{
    uint32_t total = (uint32_t)LCD_WIDTH * LCD_HEIGHT;

    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    while (total--)
    {
        LCD_WR_DAT(color);
    }
}

/* 画单个像素 */
void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
    {
        return;
    }
    lcd_set_window(x, y, x, y);
    LCD_WR_DAT(color);
}

/* 填充矩形 */
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint32_t total = (uint32_t)w * h;

    lcd_set_window(x, y, x + w - 1, y + h - 1);
    while (total--)
    {
        LCD_WR_DAT(color);
    }
}

/* 绘制 ASCII 字符 (8x16) */
void lcd_draw_char(uint16_t x, uint16_t y, char ch, uint16_t fg, uint16_t bg)
{
    uint8_t idx, row, col, bit;
    uint8_t byte;

    if (ch < FONT_FIRST_CHAR || ch > FONT_LAST_CHAR)
    {
        ch = ' ';
    }

    idx = (uint8_t)(ch - FONT_FIRST_CHAR);
    lcd_set_window(x, y, x + FONT_W - 1, y + FONT_H - 1);

    for (row = 0; row < FONT_H; row++)
    {
        byte = g_font_8x16[idx][row];
        for (col = 0; col < FONT_W; col++)
        {
            bit = (byte >> (7 - col)) & 0x01;
            LCD_WR_DAT(bit ? fg : bg);
        }
    }
}

/* 绘制 ASCII 字符串，自动换行 */
void lcd_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    uint16_t cx = x;
    uint16_t cy = y;

    while (*str)
    {
        if (cx + FONT_W > LCD_WIDTH)
        {
            cx = x;
            cy += FONT_H;
        }
        if (cy + FONT_H > LCD_HEIGHT)
        {
            break;
        }
        lcd_draw_char(cx, cy, *str, fg, bg);
        cx += FONT_W;
        str++;
    }
}

/* 绘制 16x16 中文字符 */
void lcd_draw_char_cn(uint16_t x, uint16_t y, uint16_t gbk_code, uint16_t fg, uint16_t bg)
{
    uint8_t buf[CN_CHAR_BYTES];
    uint8_t row, col;
    uint16_t temp;

    lcd_set_window(x, y, x + CN_CHAR_WIDTH - 1, y + CN_CHAR_HEIGHT - 1);

    get_gbk_code(buf, gbk_code);

    for (row = 0; row < CN_CHAR_HEIGHT; row++)
    {
        /* 每行 2 字节，高位在前 */
        temp = ((uint16_t)buf[row * 2] << 8) | buf[row * 2 + 1];
        for (col = 0; col < CN_CHAR_WIDTH; col++)
        {
            if (temp & (0x8000 >> col))
                LCD_WR_DAT(fg);
            else
                LCD_WR_DAT(bg);
        }
    }
}

/* 中英文混排字符串，ASCII宽8px、中文宽16px、行高16px */
void lcd_draw_string_cn(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    uint16_t cx = x;
    uint16_t cy = y;
    uint16_t ch;

    while (*str)
    {
        if ((uint8_t)*str <= 0x7E)
        {
            /* ASCII 字符 */
            if (cx + FONT_W > LCD_WIDTH)
            {
                cx = x;
                cy += CN_CHAR_HEIGHT;
            }
            if (cy + CN_CHAR_HEIGHT > LCD_HEIGHT)
                break;
            lcd_draw_char(cx, cy, *str, fg, bg);
            cx += FONT_W;
            str++;
        }
        else
        {
            /* GBK 中文字符 (双字节) */
            if (cx + CN_CHAR_WIDTH > LCD_WIDTH)
            {
                cx = x;
                cy += CN_CHAR_HEIGHT;
            }
            if (cy + CN_CHAR_HEIGHT > LCD_HEIGHT)
                break;
            ch = ((uint8_t)str[0] << 8) | (uint8_t)str[1];
            lcd_draw_char_cn(cx, cy, ch, fg, bg);
            cx += CN_CHAR_WIDTH;
            str += 2;
        }
    }
}

/* 背光控制 */
void lcd_backlight(uint8_t on)
{
    /* 野火指南者背光为低电平有效 */
    HAL_GPIO_WritePin(LCD_BL_PORT, LCD_BL_PIN,
                      on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}
